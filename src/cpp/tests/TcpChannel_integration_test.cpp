



#include "TcpChannel.h"
#include "Runtime.h"
#include "TlsContext.h"
#include "TTErrors.h"

#include <BC/BCFCodec.h>

#include <openssl/pem.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

static int g_failures = 0;
static int g_skipped  = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)





class Recorder : public ITcpChannelHandler
{
public:
    void OnChannelReady() override
    {
        std::unique_lock<std::mutex> lk(m_);
        ready_ = true;
        cv_.notify_all();
    }

    void OnChannelData(const void* data, size_t size) override
    {
        std::unique_lock<std::mutex> lk(m_);
        data_.append((const char*)data, size);
        cv_.notify_all();
    }

    void OnChannelClosed(BCRESULT result, const std::string& reason) override
    {
        std::unique_lock<std::mutex> lk(m_);
        closed_        = true;
        close_result_  = result;
        close_reason_  = reason;
        close_count_++;
        cv_.notify_all();
    }

    bool WaitReady(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return ready_ || closed_; }) && ready_;
    }

    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return closed_; });
    }

    bool WaitData(size_t n, int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this, n] { return data_.size() >= n; });
    }

    bool        Ready()       { std::unique_lock<std::mutex> lk(m_); return ready_; }
    BCRESULT    Result()      { std::unique_lock<std::mutex> lk(m_); return close_result_; }
    std::string Reason()      { std::unique_lock<std::mutex> lk(m_); return close_reason_; }
    std::string Data()        { std::unique_lock<std::mutex> lk(m_); return data_; }
    int         CloseCount()  { std::unique_lock<std::mutex> lk(m_); return close_count_; }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    ready_        = false;
    bool                    closed_       = false;
    int                     close_count_  = 0;
    BCRESULT                close_result_ = BC_R_SUCCESS;
    std::string             close_reason_;
    std::string             data_;
};






static void SettleBeforeDestroy()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}





class FakeServer
{
public:
    enum Mode {
        
        MODE_ACCEPT_THEN_CLOSE = 0,
        
        MODE_ECHO_ONCE         = 1,
        
        
        MODE_ACCEPT_AND_STALL  = 2,
    };

    bool Start(Mode mode)
    {
        mode_ = mode;
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        int on = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;      
        if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) return false;
        if (listen(listen_fd_, 1) != 0) return false;

        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, (struct sockaddr*)&addr, &len) != 0) return false;
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this] { _Run(); });
        return true;
    }

    void Stop()
    {
        stop_ = true;
        if (listen_fd_ >= 0)
        {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    uint16_t Port() const { return port_; }

private:
    void _Run()
    {
        int fd = accept(listen_fd_, NULL, NULL);
        if (fd < 0) return;

        if (mode_ == MODE_ECHO_ONCE)
        {
            char buf[512];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n > 0)
            {
                const char kPong[] = "PONG";
                (void)!send(fd, kPong, sizeof(kPong) - 1, 0);
            }
        }
        else if (mode_ == MODE_ACCEPT_AND_STALL)
        {
            
            char buf[512];
            while (!stop_)
            {
                struct timeval tv = { 0, 200 * 1000 };
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n == 0) break;      
            }
        }
        
        close(fd);
    }

    Mode              mode_      = MODE_ACCEPT_THEN_CLOSE;
    int               listen_fd_ = -1;
    uint16_t          port_      = 0;
    std::atomic<bool> stop_{false};
    std::thread       thread_;
};





static bool PortIsOpen(const char* ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    bool ok = (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    close(fd);
    return ok;
}


static std::string SpkiPinOfCertFile(const char* path)
{
    BIO* bio = BIO_new_file(path, "r");
    if (!bio) return std::string();
    X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!cert) return std::string();
    std::string pin = TlsContext::ComputeSpkiPinBase64(cert);
    X509_free(cert);
    return pin;
}

static TcpChannelConfig BaseConfig(const char* ip, uint16_t port)
{
    TcpChannelConfig cfg;
    cfg.host             = "localhost";
    cfg.resolvedIp       = ip;           
    cfg.port             = port;
    cfg.policy           = TT_VPN_POLICY_OS;   
    cfg.connectTimeoutMs = 8000;
    return cfg;
}












static void case_a_tls12_bad_pin(const char* ip, uint16_t port)
{
    printf("[case A] TLS1.2 + 错误 spkiPin（Critical 回归，需 ASan）\n");

    Recorder rec;
    TcpChannel* ch = new TcpChannel();

    TcpChannelConfig cfg = BaseConfig(ip, port);
    cfg.tls = true;
    
    cfg.tls_cfg.spkiPin = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

    BCRESULT r = ch->Open(cfg, &rec);
    CHECK(r == BC_R_SUCCESS);
    if (r != BC_R_SUCCESS) { delete ch; return; }

    CHECK(rec.WaitClosed(10000));
    CHECK(rec.CloseCount() == 1);
    CHECK(rec.Result() == BC_R_TLS_VERIFY_FAILED);
    CHECK(!rec.Ready());          
    CHECK(rec.Data().empty());    
    printf("  result=%d reason=%s\n", (int)rec.Result(), rec.Reason().c_str());

    SettleBeforeDestroy();
    delete ch;
}







static void case_b_tls12_good_pin(const char* ip, uint16_t port,
                                  const std::string& pin)
{
    printf("[case B] TLS1.2 + 正确 spkiPin\n");

    Recorder rec;
    TcpChannel* ch = new TcpChannel();

    TcpChannelConfig cfg = BaseConfig(ip, port);
    cfg.tls             = true;
    cfg.tls_cfg.spkiPin = pin;

    BCRESULT r = ch->Open(cfg, &rec);
    CHECK(r == BC_R_SUCCESS);
    if (r != BC_R_SUCCESS) { delete ch; return; }

    CHECK(rec.WaitReady(10000));
    CHECK(ch->GetState() == TcpChannel::TCPCH_READY);
    CHECK(!ch->PeerIp().empty());

    ch->Close();
    CHECK(rec.WaitClosed(5000));
    CHECK(rec.CloseCount() == 1);
    CHECK(rec.Result() == BC_R_SUCCESS);
    printf("  peer=%s result=%d reason=%s\n", ch->PeerIp().c_str(),
           (int)rec.Result(), rec.Reason().c_str());

    SettleBeforeDestroy();
    delete ch;
}









static void case_c_eof_during_handshake()
{
    printf("[case C] 握手中途对端 FIN（Important 2 回归）\n");

    FakeServer server;
    if (!server.Start(FakeServer::MODE_ACCEPT_THEN_CLOSE))
    {
        printf("  FAIL 起假服务端失败\n");
        g_failures++;
        return;
    }

    Recorder rec;
    TcpChannel* ch = new TcpChannel();

    TcpChannelConfig cfg = BaseConfig("127.0.0.1", server.Port());
    cfg.tls = true;     

    BCRESULT r = ch->Open(cfg, &rec);
    CHECK(r == BC_R_SUCCESS);
    if (r != BC_R_SUCCESS) { delete ch; server.Stop(); return; }

    CHECK(rec.WaitClosed(10000));
    CHECK(rec.CloseCount() == 1);
    CHECK(rec.Result() != BC_R_SUCCESS);          
    CHECK(rec.Result() == BC_R_UNEXPECTEDEND);
    CHECK(!rec.Ready());
    printf("  result=%d reason=%s\n", (int)rec.Result(), rec.Reason().c_str());

    SettleBeforeDestroy();
    delete ch;
    server.Stop();
}







static void case_d_plaintext_roundtrip()
{
    printf("[case D] 明文 ready → send → recv → 对端正常关闭\n");

    FakeServer server;
    if (!server.Start(FakeServer::MODE_ECHO_ONCE))
    {
        printf("  FAIL 起假服务端失败\n");
        g_failures++;
        return;
    }

    Recorder rec;
    TcpChannel* ch = new TcpChannel();

    TcpChannelConfig cfg = BaseConfig("127.0.0.1", server.Port());
    cfg.tls = false;

    BCRESULT r = ch->Open(cfg, &rec);
    CHECK(r == BC_R_SUCCESS);
    if (r != BC_R_SUCCESS) { delete ch; server.Stop(); return; }

    CHECK(rec.WaitReady(10000));

    BufferPtr buf(new BCBuffer);
    const char kPing[] = "PING";
    buf->Write(kPing, sizeof(kPing) - 1);
    CHECK(ch->Send(buf) == BC_R_SUCCESS);

    CHECK(rec.WaitData(4, 5000));
    CHECK(rec.Data() == "PONG");

    CHECK(rec.WaitClosed(5000));
    CHECK(rec.CloseCount() == 1);
    
    CHECK(rec.Result() == BC_R_SUCCESS);
    printf("  data=%s result=%d reason=%s\n", rec.Data().c_str(),
           (int)rec.Result(), rec.Reason().c_str());

    SettleBeforeDestroy();
    delete ch;
    server.Stop();
}










static void case_e_handshake_timeout()
{
    printf("[case E] 握手黑洞 → 超时（顺带压定时器自删路径）\n");

    FakeServer server;
    if (!server.Start(FakeServer::MODE_ACCEPT_AND_STALL))
    {
        printf("  FAIL 起假服务端失败\n");
        g_failures++;
        return;
    }

    Recorder rec;
    TcpChannel* ch = new TcpChannel();

    TcpChannelConfig cfg = BaseConfig("127.0.0.1", server.Port());
    cfg.tls              = true;
    cfg.connectTimeoutMs = 1200;    

    BCRESULT r = ch->Open(cfg, &rec);
    CHECK(r == BC_R_SUCCESS);
    if (r != BC_R_SUCCESS) { delete ch; server.Stop(); return; }

    CHECK(rec.WaitClosed(8000));
    CHECK(rec.CloseCount() == 1);
    CHECK(rec.Result() == BC_R_CONNECT_TIMEOUT);
    CHECK(!rec.Ready());
    printf("  result=%d reason=%s\n", (int)rec.Result(), rec.Reason().c_str());

    SettleBeforeDestroy();
    delete ch;
    server.Stop();
}
















static void case_f_concurrent_close_must_not_lose_callback()
{
    printf("[case F] Close() 与关闭流程并发（丢回调回归，20 轮）\n");

    const int kIters = 20;
    int hangs = 0;

    for (int i = 0; i < kIters; i++)
    {
        FakeServer server;
        if (!server.Start(FakeServer::MODE_ACCEPT_AND_STALL))
        {
            printf("  FAIL 起假服务端失败\n");
            g_failures++;
            return;
        }

        Recorder rec;
        TcpChannel* ch = new TcpChannel();

        TcpChannelConfig cfg = BaseConfig("127.0.0.1", server.Port());
        cfg.tls              = false;    
        cfg.connectTimeoutMs = 8000;

        if (ch->Open(cfg, &rec) != BC_R_SUCCESS)
        {
            printf("  FAIL Open 失败\n");
            g_failures++;
            delete ch;
            server.Stop();
            return;
        }

        if (!rec.WaitReady(8000))
        {
            printf("  FAIL 第 %d 轮没能进入 READY\n", i);
            g_failures++;
            SettleBeforeDestroy();
            delete ch;
            server.Stop();
            return;
        }

        
        std::atomic<bool> stop{false};
        std::thread spinner([ch, &stop] {
            while (!stop.load())
            {
                ch->Close();
            }
        });

        bool closed = rec.WaitClosed(5000);
        stop = true;
        spinner.join();

        if (!closed)
        {
            printf("  HANG iter %d state=%d\n", i, (int)ch->GetState());
            hangs++;
        }

        SettleBeforeDestroy();
        delete ch;
        server.Stop();

        if (hangs > 0)
        {
            break;      
        }
    }

    CHECK(hangs == 0);
    printf("  done: %d iters, %d hangs\n", kIters, hangs);
}





int main(int argc, char** argv)
{
    
    
    setvbuf(stdout, NULL, _IONBF, 0);

    const char* ip       = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    port     = (argc > 2) ? (uint16_t)atoi(argv[2]) : 18444;
    const char* certPath = (argc > 3) ? argv[3] : "certs/localhost.crt";

    BCFObject runtimeCfg;
    runtimeCfg.PutInt("workerThreads", 1);
    runtimeCfg.PutInt("taskThreads", 4);
    runtimeCfg.PutInt("timerThreads", 2);
    if (Runtime::Initialize(&runtimeCfg) != BC_R_SUCCESS)
    {
        printf("Runtime::Initialize 失败\n");
        return 1;
    }

    
    case_c_eof_during_handshake();
    case_d_plaintext_roundtrip();
    case_e_handshake_timeout();
    case_f_concurrent_close_must_not_lose_callback();

    if (PortIsOpen(ip, port))
    {
        std::string pin = SpkiPinOfCertFile(certPath);
        case_a_tls12_bad_pin(ip, port);
        if (pin.empty())
        {
            printf("[case B] SKIP：读不到证书 %s，算不出 SPKI pin\n", certPath);
            g_skipped++;
        }
        else
        {
            case_b_tls12_good_pin(ip, port, pin);
        }
    }
    else
    {
        printf("[case A/B] SKIP：%s:%u 上没有 TLS 服务端。请先执行\n"
               "  openssl s_server -accept %u -cert certs/localhost.crt "
               "-key certs/localhost.key -www -tls1_2 -no_ticket -no_cache\n",
               ip, (unsigned)port, (unsigned)port);
        g_skipped += 2;
    }

    if (g_failures == 0 && g_skipped == 0)
    {
        printf("TcpChannel_integration_test: ALL PASSED\n");
        return 0;
    }
    if (g_failures == 0)
    {
        printf("TcpChannel_integration_test: PASSED（%d 个用例被跳过）\n",
               g_skipped);
        return 2;
    }
    printf("TcpChannel_integration_test: %d FAILURE(S)\n", g_failures);
    return 1;
}




