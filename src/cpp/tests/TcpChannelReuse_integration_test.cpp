



#include "TcpChannel.h"
#include "Runtime.h"
#include "TTErrors.h"

#include <BC/BCFCodec.h>

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
    
    
    TcpChannel* park_on_data = NULL;

    void OnChannelReady() override
    {
        std::unique_lock<std::mutex> lk(m_);
        ready_ = true;
        ready_count_++;
        cv_.notify_all();
    }

    void OnChannelData(const void* data, size_t size) override
    {
        TcpChannel* toPark = NULL;
        {
            std::unique_lock<std::mutex> lk(m_);
            data_.append((const char*)data, size);
            toPark = park_on_data;
            park_on_data = NULL;   
        }
        if (toPark) toPark->Park();
        std::unique_lock<std::mutex> lk(m_);
        cv_.notify_all();
    }

    void OnChannelClosed(BCRESULT result, const std::string& reason) override
    {
        std::unique_lock<std::mutex> lk(m_);
        closed_       = true;
        close_result_ = result;
        close_reason_ = reason;
        cv_.notify_all();
    }

    void OnChannelDetached() override
    {
        std::unique_lock<std::mutex> lk(m_);
        detached_ = true;
        cv_.notify_all();
    }

    void OnChannelAdoptFailed() override
    {
        std::unique_lock<std::mutex> lk(m_);
        adopt_failed_ = true;
        cv_.notify_all();
    }

    bool WaitAdoptFailed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return adopt_failed_; });
    }

    bool        Detached()    { std::unique_lock<std::mutex> lk(m_); return detached_; }

    bool WaitReady(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return ready_ || closed_; }) && ready_;
    }

    bool WaitData(size_t n, int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this, n] { return data_.size() >= n || closed_; })
               && data_.size() >= n;
    }

    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return closed_; });
    }

    bool        Closed()      { std::unique_lock<std::mutex> lk(m_); return closed_; }
    BCRESULT    Result()      { std::unique_lock<std::mutex> lk(m_); return close_result_; }
    std::string Data()        { std::unique_lock<std::mutex> lk(m_); return data_; }
    int         ReadyCount()  { std::unique_lock<std::mutex> lk(m_); return ready_count_; }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    ready_        = false;
    int                     ready_count_  = 0;
    bool                    closed_       = false;
    bool                    detached_     = false;
    bool                    adopt_failed_ = false;
    BCRESULT                close_result_ = BC_R_SUCCESS;
    std::string             close_reason_;
    std::string             data_;
};



static void SettleBeforeDestroy()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}





class EchoServer
{
public:
    bool Start()
    {
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
        if (listen(listen_fd_, 4) != 0) return false;

        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, (struct sockaddr*)&addr, &len) != 0) return false;
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this] { _Run(); });
        return true;
    }

    void Stop()
    {
        stop_ = true;
        if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
        if (thread_.joinable()) thread_.join();
    }

    
    void DropPeer()
    {
        int fd = peer_fd_.exchange(-1);
        if (fd >= 0) close(fd);
    }

    
    void PushUnsolicited(const char* s)
    {
        int fd = peer_fd_.load();
        if (fd >= 0) (void)!send(fd, s, strlen(s), 0);
    }

    uint16_t Port() const { return port_; }

private:
    void _Run()
    {
        int fd = accept(listen_fd_, NULL, NULL);
        if (fd < 0) return;
        peer_fd_ = fd;

        char buf[512];
        while (!stop_)
        {
            int cur = peer_fd_.load();
            if (cur < 0) break;           

            struct timeval tv = { 0, 100 * 1000 };
            setsockopt(cur, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = recv(cur, buf, sizeof(buf), 0);
            if (n == 0) break;            
            if (n < 0) continue;          

            const char kPong[] = "PONG";
            (void)!send(cur, kPong, sizeof(kPong) - 1, 0);
        }

        int last = peer_fd_.exchange(-1);
        if (last >= 0) close(last);
    }

    int               listen_fd_ = -1;
    uint16_t          port_      = 0;
    std::atomic<int>  peer_fd_{-1};
    std::atomic<bool> stop_{false};
    std::thread       thread_;
};










class QueueGate
{
public:
    bool Arm(TcpChannel& ch)
    {
        return ch.ScheduleTask(timer_, [this](int32_t) {
            std::unique_lock<std::mutex> lk(m_);
            entered_ = true;
            cv_.notify_all();
            cv_.wait(lk, [this] { return open_; });
        }, 1) == BC_R_SUCCESS;
    }

    bool WaitEntered()
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::seconds(5),
                            [this] { return entered_; });
    }

    void Open()
    {
        std::unique_lock<std::mutex> lk(m_);
        open_ = true;
        cv_.notify_all();
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    entered_ = false;
    bool                    open_    = false;
    int32_t                 timer_   = 0;
};





static TcpChannelConfig MakeCfg(uint16_t port)
{
    TcpChannelConfig cfg;
    cfg.host              = "localhost";
    cfg.port              = port;
    cfg.tls               = false;
    cfg.resolvedIp        = "127.0.0.1";   
    cfg.policy            = TT_VPN_POLICY_OS;
    cfg.connectTimeoutMs  = 5000;
    return cfg;
}

static void Send(TcpChannel& ch, const char* s)
{
    BufferPtr buf(new BCBuffer);
    buf->Write(s, (uint32_t)strlen(s));
    ch.Send(buf);
}






static void test_parked_channel_serves_a_second_handler()
{
    printf("Case A: Park 过的通道能交给下一个 handler\n");

    EchoServer server;
    CHECK(server.Start());

    TcpChannel ch;
    Recorder   first;
    first.park_on_data = &ch;   

    CHECK(ch.Open(MakeCfg(server.Port()), &first) == BC_R_SUCCESS);
    CHECK(first.WaitReady(5000));

    Send(ch, "PING");
    CHECK(first.WaitData(4, 5000));
    CHECK(first.Data() == "PONG");

    
    CHECK(!first.Closed());
    CHECK(ch.GetState() == TcpChannel::TCPCH_READY);

    Recorder second;
    CHECK(ch.Adopt(&second) == BC_R_SUCCESS);
    
    
    CHECK(second.WaitReady(5000));

    Send(ch, "PING");
    CHECK(second.WaitData(4, 5000));
    CHECK(second.Data() == "PONG");

    
    CHECK(first.Data() == "PONG");

    ch.Close();
    CHECK(second.WaitClosed(5000));
    SettleBeforeDestroy();
    server.Stop();
}




static void test_adopt_rejects_a_channel_the_peer_closed()
{
    printf("Case B: 对端在 Park 期间关掉连接 -> Adopt 拒绝\n");

    EchoServer server;
    CHECK(server.Start());

    TcpChannel ch;
    Recorder   first;
    first.park_on_data = &ch;

    CHECK(ch.Open(MakeCfg(server.Port()), &first) == BC_R_SUCCESS);
    CHECK(first.WaitReady(5000));
    Send(ch, "PING");
    CHECK(first.WaitData(4, 5000));

    
    server.DropPeer();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Recorder second;
    BCRESULT r = ch.Adopt(&second);
    CHECK(r != BC_R_SUCCESS);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(second.ReadyCount() == 0);
    CHECK(!second.Closed());

    SettleBeforeDestroy();
    server.Stop();
}





static void test_adopt_rejects_a_channel_that_got_unexpected_bytes()
{
    printf("Case C: Park 期间收到意外字节 -> Adopt 拒绝\n");

    EchoServer server;
    CHECK(server.Start());

    TcpChannel ch;
    Recorder   first;
    first.park_on_data = &ch;

    CHECK(ch.Open(MakeCfg(server.Port()), &first) == BC_R_SUCCESS);
    CHECK(first.WaitReady(5000));
    Send(ch, "PING");
    CHECK(first.WaitData(4, 5000));

    server.PushUnsolicited("LEFTOVER");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Recorder second;
    CHECK(ch.Adopt(&second) != BC_R_SUCCESS);
    CHECK(second.ReadyCount() == 0);

    
    CHECK(first.Data() == "PONG");

    ch.Close();
    SettleBeforeDestroy();
    server.Stop();
}












static void test_adopt_fails_after_precheck_passed()
{
    printf("Case E: Adopt 预检通过、排队期间失效 -> 新 handler 不拿所有权\n");

    EchoServer server;
    CHECK(server.Start());

    TcpChannel* ch = new TcpChannel();
    Recorder    first;

    CHECK(ch->Open(MakeCfg(server.Port()), &first) == BC_R_SUCCESS);
    CHECK(first.WaitReady(5000));
    Send(*ch, "PING");
    CHECK(first.WaitData(4, 5000));

    
    Recorder watcher;
    {
        std::mutex              m;
        std::condition_variable cv;
        bool                    done = false;
        ch->PostTask([&]() {
            ch->Park(&watcher);
            std::unique_lock<std::mutex> lk(m);
            done = true;
            cv.notify_all();
        });
        std::unique_lock<std::mutex> lk(m);
        CHECK(cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; }));
    }
    CHECK(ch->IsParkedAndUsable());

    
    
    
    
    
    
    
    QueueGate gate1;
    CHECK(gate1.Arm(*ch));
    CHECK(gate1.WaitEntered());

    server.PushUnsolicited("LEFTOVER");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    QueueGate gate2;
    CHECK(gate2.Arm(*ch));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   

    gate1.Open();
    CHECK(gate2.WaitEntered());

    CHECK(ch->IsParkedAndUsable());              
    Recorder second;
    CHECK(ch->Adopt(&second) == BC_R_SUCCESS);   

    gate2.Open();

    CHECK(second.WaitAdoptFailed(5000));
    
    
    CHECK(second.ReadyCount() == 0);
    CHECK(!second.Closed());

    
    CHECK(watcher.WaitClosed(5000));
    CHECK(!watcher.Detached());
    CHECK(first.Data() == "PONG");

    
    SettleBeforeDestroy();
    delete ch;
    server.Stop();
}



static void test_park_is_rejected_before_ready()
{
    printf("Case D: 未就绪的通道 Park 不生效\n");

    TcpChannel ch;
    ch.Park();   

    CHECK(ch.GetState() == TcpChannel::TCPCH_IDLE);

    Recorder r;
    CHECK(ch.Adopt(&r) != BC_R_SUCCESS);
}



int main()
{
    BCFObject runtimeCfg;
    runtimeCfg.PutInt("workerThreads", 1);
    runtimeCfg.PutInt("taskThreads", 4);
    runtimeCfg.PutInt("timerThreads", 2);
    if (Runtime::Initialize(&runtimeCfg) != BC_R_SUCCESS)
    {
        printf("Runtime::Initialize 失败\n");
        return 1;
    }

    test_parked_channel_serves_a_second_handler();
    test_adopt_rejects_a_channel_the_peer_closed();
    test_adopt_rejects_a_channel_that_got_unexpected_bytes();
    test_adopt_fails_after_precheck_passed();
    test_park_is_rejected_before_ready();

    Runtime::Destroy();

    if (g_failures == 0)
    {
        printf("TcpChannelReuse_integration_test: all passed\n");
        return 0;
    }
    printf("TcpChannelReuse_integration_test: %d failure(s)\n", g_failures);
    return 1;
}




