



#include "WSConnector.h"
#include "Runtime.h"
#include "TTErrors.h"

#include <BC/BCFCodec.h>

#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
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
#include <vector>

using namespace WS;

static int g_failures = 0;
static int g_skipped  = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                         \
    do {                                                                       \
        std::string _a(actual);                                                \
        std::string _e(expected);                                              \
        if (_a != _e) {                                                        \
            printf("  FAIL %s:%d: 期望 \"%s\"，实际 \"%s\"\n",                 \
                   __FILE__, __LINE__, _e.c_str(), _a.c_str());                \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                         \
    do {                                                                       \
        long long _a = (long long)(actual);                                    \
        long long _e = (long long)(expected);                                  \
        if (_a != _e) {                                                        \
            printf("  FAIL %s:%d: 期望 %lld，实际 %lld\n",                     \
                   __FILE__, __LINE__, _e, _a);                                \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static bool Contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

static bool ReadFileAll(const char* path, std::string& out)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;
    char   buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    fclose(fp);
    return true;
}






static std::string MakeFrame(bool fin, uint8_t opcode,
                             const std::string& payload,
                             const uint8_t* mask = NULL)
{
    std::string out;
    out.push_back((char)((fin ? 0x80 : 0x00) | (opcode & 0x0F)));

    const size_t  len     = payload.size();
    const uint8_t maskBit = mask ? 0x80 : 0x00;
    if (len < 126)
    {
        out.push_back((char)(maskBit | (uint8_t)len));
    }
    else if (len < 65536)
    {
        out.push_back((char)(maskBit | 126));
        out.push_back((char)((len >> 8) & 0xFF));
        out.push_back((char)(len & 0xFF));
    }
    else
    {
        out.push_back((char)(maskBit | 127));
        for (int i = 7; i >= 0; i--)
        {
            out.push_back((char)((len >> (i * 8)) & 0xFF));
        }
    }
    if (mask)
    {
        out.append((const char*)mask, 4);
        for (size_t i = 0; i < len; i++)
        {
            out.push_back((char)(payload[i] ^ mask[i % 4]));
        }
    }
    else
    {
        out.append(payload);
    }
    return out;
}

static std::string MakeCloseFrame(uint16_t code)
{
    std::string p;
    p.push_back((char)((code >> 8) & 0xFF));
    p.push_back((char)(code & 0xFF));
    return MakeFrame(true, 0x8, p);
}





class Waiter : public IWSConnectionHandler
{
public:
    void OnConnectResult(BCRESULT result, const HttpHeaderMap& headers) override
    {
        std::unique_lock<std::mutex> lk(m_);
        connect_calls_++;
        connect_result_ = result;
        headers_        = headers;
        cv_.notify_all();
    }

    void OnRecvText(LPCSTR text) override
    {
        std::unique_lock<std::mutex> lk(m_);
        texts_.push_back(text ? text : "");
        cv_.notify_all();
    }

    void OnRecvData(LPCVOID data, size_t size) override
    {
        std::unique_lock<std::mutex> lk(m_);
        datas_.push_back(std::string((const char*)data, size));
        cv_.notify_all();
    }

    void OnClosed(LPCSTR reason) override
    {
        std::unique_lock<std::mutex> lk(m_);
        closed_calls_++;
        close_reason_ = reason ? reason : "";
        cv_.notify_all();
    }

    void OnException(BCException& e) override
    {
        std::unique_lock<std::mutex> lk(m_);
        exception_calls_++;
        exception_msg_ = e.GetMsg().c_str();
        cv_.notify_all();
    }

    bool WaitConnect(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return connect_calls_ > 0; });
    }
    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return closed_calls_ > 0; });
    }
    bool WaitTexts(size_t n, int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this, n] { return texts_.size() >= n; });
    }
    bool WaitDatas(size_t n, int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this, n] { return datas_.size() >= n; });
    }
    bool WaitException(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return exception_calls_ > 0; });
    }

    int      ConnectCalls()   { std::unique_lock<std::mutex> lk(m_); return connect_calls_; }
    BCRESULT ConnectResult()  { std::unique_lock<std::mutex> lk(m_); return connect_result_; }
    int      ClosedCalls()    { std::unique_lock<std::mutex> lk(m_); return closed_calls_; }
    int      ExceptionCalls() { std::unique_lock<std::mutex> lk(m_); return exception_calls_; }
    std::string CloseReason() { std::unique_lock<std::mutex> lk(m_); return close_reason_; }
    std::string ExceptionMsg(){ std::unique_lock<std::mutex> lk(m_); return exception_msg_; }
    std::vector<std::string> Texts() { std::unique_lock<std::mutex> lk(m_); return texts_; }
    std::vector<std::string> Datas() { std::unique_lock<std::mutex> lk(m_); return datas_; }
    HttpHeaderMap Headers()   { std::unique_lock<std::mutex> lk(m_); return headers_; }

private:
    std::mutex               m_;
    std::condition_variable  cv_;
    int                      connect_calls_   = 0;
    BCRESULT                 connect_result_  = BC_R_FAILURE;
    int                      closed_calls_    = 0;
    int                      exception_calls_ = 0;
    std::string              close_reason_;
    std::string              exception_msg_;
    std::vector<std::string> texts_;
    std::vector<std::string> datas_;
    HttpHeaderMap            headers_;
};





class CtorHandler : public IWSConnectorHandler
{
public:
    explicit CtorHandler(bool echoLogs = false) : echo_(echoLogs) {}

    void OnLog(int level, LPCSTR msg) override
    {
        std::unique_lock<std::mutex> lk(m_);
        logs_.append(msg ? msg : "").append("\n");
        if (echo_)
        {
            printf("    [L%d] %s\n", level, msg ? msg : "");
        }
    }

    void OnClosed() override
    {
        std::unique_lock<std::mutex> lk(m_);
        closed_calls_++;
        cv_.notify_all();
    }

    void OnException(BCException& e) override
    {
        std::unique_lock<std::mutex> lk(m_);
        exceptions_.append(e.GetMsg().c_str()).append("\n");
    }

    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return closed_calls_ > 0; });
    }

    int         ClosedCalls() { std::unique_lock<std::mutex> lk(m_); return closed_calls_; }
    std::string Logs()        { std::unique_lock<std::mutex> lk(m_); return logs_; }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    echo_         = false;
    int                     closed_calls_ = 0;
    std::string             logs_;
    std::string             exceptions_;
};









class FakeWSServer
{
public:
    enum Mode {
        
        MODE_ECHO        = 0,
        
        MODE_REJECT      = 1,
        
        MODE_NO_ACCEPT   = 2,
        
        MODE_BAD_ACCEPT  = 3,
        
        MODE_NO_REPLY    = 4,
        
        MODE_FRAGMENTS   = 5,
        
        MODE_OVERSIZE    = 6,
        
        MODE_CLOSE_FIRST = 7,
        
        MODE_PIGGYBACK   = 8,
        
        MODE_PING        = 9,
        
        MODE_CLOSE_RACE  = 10,
        
        
        
        MODE_CLOSE_BAD_LEN = 11,
        
        MODE_CLOSE_EMPTY   = 12,
    };

    ~FakeWSServer() { Stop(); }

    bool Start(Mode mode, bool tls = false, int conns = 1)
    {
        mode_      = mode;
        tls_       = tls;
        max_conns_ = conns;

        if (tls_)
        {
            ssl_ctx_ = SSL_CTX_new(TLS_server_method());
            if (!ssl_ctx_) return false;
            if (SSL_CTX_use_certificate_file(ssl_ctx_, "certs/localhost.crt",
                                             SSL_FILETYPE_PEM) != 1)
            {
                printf("  （提示：certs/localhost.crt 读不到，请在仓库根目录跑）\n");
                return false;
            }
            if (SSL_CTX_use_PrivateKey_file(ssl_ctx_, "certs/localhost.key",
                                            SSL_FILETYPE_PEM) != 1)
            {
                return false;
            }
        }

        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) return false;
        listen_fd_.store(lfd);

        int on = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;      
        if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) return false;
        if (listen(lfd, 8) != 0) return false;

        socklen_t len = sizeof(addr);
        if (getsockname(lfd, (struct sockaddr*)&addr, &len) != 0) return false;
        port_ = ntohs(addr.sin_port);

        thread_ = std::thread([this] { _Run(); });
        return true;
    }

    void Stop()
    {
        stop_ = true;
        int fd = listen_fd_.exchange(-1);
        if (fd >= 0)
        {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if (thread_.joinable()) thread_.join();
        if (ssl_ctx_)
        {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = NULL;
        }
    }

    uint16_t Port() const { return port_; }

    std::string Request()
    {
        std::unique_lock<std::mutex> lk(m_);
        return request_;
    }
    int  ClientFrames()      { return client_frames_.load(); }
    int  UnmaskedFrames()    { return unmasked_frames_.load(); }
    int  PongsFromClient()   { return pongs_.load(); }
    int  ClosesFromClient()  { return closes_.load(); }
    
    int  ClientCloseCode()   { return close_code_.load(); }
    int  Accepted()          { return accepted_.load(); }
    std::vector<std::string> ClientPayloads()
    {
        std::unique_lock<std::mutex> lk(m_);
        return client_payloads_;
    }

private:
    
    
    
    struct IO
    {
        int  fd  = -1;
        SSL* ssl = NULL;

        ssize_t Recv(void* buf, size_t n)
        {
            if (ssl)
            {
                int r = SSL_read(ssl, buf, (int)n);
                return r > 0 ? r : (r == 0 ? 0 : -1);
            }
            return recv(fd, buf, n, 0);
        }
        bool SendAll(const char* data, size_t size)
        {
            size_t off = 0;
            while (off < size)
            {
                ssize_t n;
                if (ssl)
                {
                    n = SSL_write(ssl, data + off, (int)(size - off));
                }
                else
                {
                    n = send(fd, data + off, size - off, 0);
                }
                if (n <= 0) return false;
                off += (size_t)n;
            }
            return true;
        }
        bool SendAll(const std::string& s) { return SendAll(s.data(), s.size()); }
    };

    void _Run()
    {
        for (int i = 0; i < max_conns_ && !stop_; i++)
        {
            int cfd = accept(listen_fd_.load(), NULL, NULL);
            if (cfd < 0) break;
            accepted_++;
            _ServeOne(cfd);
        }
    }

    void _ServeOne(int cfd)
    {
        IO io;
        io.fd = cfd;
        if (tls_)
        {
            io.ssl = SSL_new(ssl_ctx_);
            if (!io.ssl)
            {
                close(cfd);
                return;
            }
            SSL_set_fd(io.ssl, cfd);
            if (SSL_accept(io.ssl) != 1)
            {
                SSL_free(io.ssl);
                close(cfd);
                return;
            }
        }

        
        struct timeval tv = { 3, 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        
        std::string req;
        char        buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos && !stop_)
        {
            ssize_t n = io.Recv(buf, sizeof(buf));
            if (n <= 0) break;
            req.append(buf, (size_t)n);
        }
        {
            std::unique_lock<std::mutex> lk(m_);
            request_ = req;
        }

        const std::string key = _ExtractKey(req);

        switch (mode_)
        {
        case MODE_REJECT:
            io.SendAll("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
                       "Connection: close\r\n\r\n");
            _Finish(io);
            return;
        case MODE_NO_REPLY:
            while (!stop_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            _Finish(io);
            return;
        case MODE_NO_ACCEPT:
            io.SendAll("HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n");
            _Loop(io);
            return;
        case MODE_BAD_ACCEPT:
            io.SendAll("HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n");
            _Loop(io);
            return;
        default:
            break;
        }

        
        
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: keep-alive, Upgrade\r\n"
                           "Sec-WebSocket-Accept: "
                         + WSParser::WSAcceptKey(key) + "\r\n"
                           "X-Server-Note: fake\r\n\r\n";

        if (mode_ == MODE_PIGGYBACK)
        {
            
            resp += MakeFrame(true, 0x1, "piggybacked");
            io.SendAll(resp);
            _Loop(io);
            return;
        }
        io.SendAll(resp);

        if (mode_ == MODE_FRAGMENTS)
        {
            io.SendAll(MakeFrame(false, 0x1, "Hello, "));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            io.SendAll(MakeFrame(true, 0x9, "hb"));          
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            io.SendAll(MakeFrame(true, 0x0, "fragmented world"));
        }
        else if (mode_ == MODE_OVERSIZE)
        {
            
            
            std::string f;
            f.push_back((char)0x82);            
            f.push_back((char)127);
            uint64_t len = 4ull * 1024 * 1024;
            for (int i = 7; i >= 0; i--)
            {
                f.push_back((char)((len >> (i * 8)) & 0xFF));
            }
            io.SendAll(f);
        }
        else if (mode_ == MODE_CLOSE_FIRST)
        {
            io.SendAll(MakeCloseFrame(1001));
        }
        else if (mode_ == MODE_CLOSE_BAD_LEN)
        {
            
            io.SendAll(MakeFrame(true, 0x8, std::string(1, (char)0x03)));
        }
        else if (mode_ == MODE_CLOSE_EMPTY)
        {
            
            io.SendAll(MakeFrame(true, 0x8, std::string()));
        }
        else if (mode_ == MODE_PING)
        {
            io.SendAll(MakeFrame(true, 0x9, "srv-ping"));
        }
        else if (mode_ == MODE_CLOSE_RACE)
        {
            io.SendAll(MakeCloseFrame(1000));
            
            _Finish(io);
            return;
        }

        _Loop(io);
    }

    
    void _Loop(IO& io)
    {
        std::string acc;
        char        buf[4096];
        while (!stop_)
        {
            ssize_t n = io.Recv(buf, sizeof(buf));
            if (n <= 0) break;
            acc.append(buf, (size_t)n);

            
            for (;;)
            {
                bool        fin = false;
                uint8_t     opcode = 0;
                bool        masked = false;
                std::string payload;
                size_t      used = 0;
                if (!_TryParse(acc, fin, opcode, masked, payload, used))
                {
                    break;
                }
                acc.erase(0, used);
                client_frames_++;
                if (!masked)
                {
                    
                    unmasked_frames_++;
                }
                {
                    std::unique_lock<std::mutex> lk(m_);
                    client_payloads_.push_back(payload);
                }
                if (opcode == 0xA)
                {
                    pongs_++;
                    continue;
                }
                if (opcode == 0x8)
                {
                    if (payload.size() >= 2)
                    {
                        close_code_.store(((uint8_t)payload[0] << 8)
                                          | (uint8_t)payload[1]);
                    }
                    closes_++;   
                                 
                    if (mode_ == MODE_ECHO || mode_ == MODE_PIGGYBACK
                        || mode_ == MODE_PING || mode_ == MODE_FRAGMENTS)
                    {
                        io.SendAll(MakeCloseFrame(1000));
                    }
                    _Finish(io);
                    return;
                }
                if (opcode == 0x9)
                {
                    io.SendAll(MakeFrame(true, 0xA, payload));
                    continue;
                }
                if (opcode == 0x1 || opcode == 0x2)
                {
                    
                    io.SendAll(MakeFrame(true, opcode, payload));
                }
            }
        }
        _Finish(io);
    }

    void _Finish(IO& io)
    {
        if (io.ssl)
        {
            SSL_shutdown(io.ssl);
            SSL_free(io.ssl);
            io.ssl = NULL;
        }
        if (io.fd >= 0)
        {
            close(io.fd);
            io.fd = -1;
        }
    }

    
    static bool _TryParse(const std::string& in, bool& fin, uint8_t& opcode,
                          bool& masked, std::string& payload, size_t& used)
    {
        if (in.size() < 2) return false;
        fin    = (in[0] & 0x80) != 0;
        opcode = (uint8_t)(in[0] & 0x0F);
        masked = (in[1] & 0x80) != 0;
        uint64_t len = (uint8_t)(in[1] & 0x7F);
        size_t   off = 2;
        if (len == 126)
        {
            if (in.size() < off + 2) return false;
            len = ((uint8_t)in[off] << 8) | (uint8_t)in[off + 1];
            off += 2;
        }
        else if (len == 127)
        {
            if (in.size() < off + 8) return false;
            len = 0;
            for (int i = 0; i < 8; i++)
            {
                len = (len << 8) | (uint8_t)in[off + i];
            }
            off += 8;
        }
        uint8_t mask[4] = { 0, 0, 0, 0 };
        if (masked)
        {
            if (in.size() < off + 4) return false;
            memcpy(mask, in.data() + off, 4);
            off += 4;
        }
        if (in.size() < off + (size_t)len) return false;
        payload.assign(in.data() + off, (size_t)len);
        if (masked)
        {
            for (size_t i = 0; i < payload.size(); i++)
            {
                payload[i] = (char)(payload[i] ^ mask[i % 4]);
            }
        }
        used = off + (size_t)len;
        return true;
    }

    static std::string _ExtractKey(const std::string& req)
    {
        
        std::string lower(req);
        for (size_t i = 0; i < lower.size(); i++)
        {
            if (lower[i] >= 'A' && lower[i] <= 'Z')
            {
                lower[i] = (char)(lower[i] - 'A' + 'a');
            }
        }
        const char* name = "sec-websocket-key:";
        size_t      pos  = lower.find(name);
        if (pos == std::string::npos) return std::string();
        pos += strlen(name);
        size_t end = lower.find("\r\n", pos);
        if (end == std::string::npos) return std::string();
        std::string v = req.substr(pos, end - pos);
        size_t      b = v.find_first_not_of(" \t");
        size_t      e = v.find_last_not_of(" \t");
        if (b == std::string::npos) return std::string();
        return v.substr(b, e - b + 1);
    }

    Mode              mode_ = MODE_ECHO;
    bool              tls_  = false;
    int               max_conns_ = 1;
    SSL_CTX*          ssl_ctx_ = NULL;
    
    
    std::atomic<int>  listen_fd_{-1};
    uint16_t          port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int>  client_frames_{0};
    std::atomic<int>  unmasked_frames_{0};
    std::atomic<int>  pongs_{0};
    std::atomic<int>  closes_{0};
    std::atomic<int>  close_code_{-1};
    std::atomic<int>  accepted_{0};
    std::thread       thread_;
    std::mutex        m_;
    std::string       request_;
    std::vector<std::string> client_payloads_;
};

























template <typename Pred>
static bool WaitFor(Pred pred, int timeoutMs = 2000)
{
    const int kStepMs = 20;
    for (int waited = 0; waited < timeoutMs; waited += kStepMs)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
    }
    return pred();
}

static std::string WsUrl(uint16_t port, const char* path = "/")
{
    char buf[128];
    snprintf(buf, sizeof(buf), "ws://127.0.0.1:%u%s", (unsigned)port, path);
    return buf;
}

static uint16_t FindFreePort()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr*)&addr, &len);
    uint16_t p = ntohs(addr.sin_port);
    close(fd);
    return p;
}





static void case_a_echo()
{
    printf("[case A] ws:// 握手 + 文本/二进制回声 + Close()\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_ECHO))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }

    CHECK(conn->Connect(WsUrl(srv.Port(), "/chat?x=1"), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectCalls(), 1);
    CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);
    CHECK(conn->IsUpgraded());
    
    CHECK_EQ_STR(w.Headers()["x-server-note"], "fake");
    CHECK(!w.Headers()["sec-websocket-accept"].empty());
    CHECK_EQ_STR(conn->PeerIp(), "127.0.0.1");

    
    std::string req = srv.Request();
    CHECK(Contains(req, "GET /chat?x=1 HTTP/1.1\r\n"));
    CHECK(Contains(req, "Host: 127.0.0.1:"));
    CHECK(Contains(req, "Upgrade: websocket\r\n"));
    CHECK(Contains(req, "Sec-WebSocket-Version: 13\r\n"));
    CHECK(Contains(req, "Sec-WebSocket-Key: "));
    CHECK(Contains(req, "User-Agent: ttsignal/1.0\r\n"));

    CHECK(conn->SendText("hello ws") == BC_R_SUCCESS);
    CHECK(w.WaitTexts(1, 5000));
    CHECK_EQ_STR(w.Texts()[0], "hello ws");

    const char bin[] = { 0x00, 0x01, 0x02, (char)0xFF };
    CHECK(conn->SendData(bin, sizeof(bin)) == BC_R_SUCCESS);
    CHECK(w.WaitDatas(1, 5000));
    CHECK_EQ_INT(w.Datas()[0].size(), sizeof(bin));
    CHECK(memcmp(w.Datas()[0].data(), bin, sizeof(bin)) == 0);

    
    CHECK(conn->SendText("") == BC_R_SUCCESS);
    CHECK(w.WaitTexts(2, 5000));
    CHECK_EQ_STR(w.Texts()[1], "");

    
    {
        SMPacketPtr pkt(new SMPacket);
        BufferPtr   data(new BCBuffer);
        data->Write("smp-pkt", 7);
        pkt->Create(data);
        CHECK(conn->SendPacket(pkt) == BC_R_SUCCESS);
        CHECK(w.WaitDatas(2, 5000));
        CHECK_EQ_STR(w.Datas()[1], "smp-pkt");
    }

    conn->Close();
    CHECK(w.WaitClosed(8000));
    CHECK_EQ_INT(w.ClosedCalls(), 1);
    CHECK_EQ_INT(w.ConnectCalls(), 1);
    printf("  close reason: %s\n", w.CloseReason().c_str());

    
    
    
    
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK(srv.ClientFrames() >= 5);
    
    
    
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);

    c.Close();
    CHECK(ch.WaitClosed(8000));
    CHECK_EQ_INT(ch.ClosedCalls(), 1);
    srv.Stop();
}







static void case_b_handshake_failures()
{
    printf("[case B] 握手失败：403 / 缺 Sec-WebSocket-Accept / accept 值错\n");

    struct { FakeWSServer::Mode mode; const char* name; } cases[] = {
        { FakeWSServer::MODE_REJECT,     "403" },
        { FakeWSServer::MODE_NO_ACCEPT,  "缺 accept" },
        { FakeWSServer::MODE_BAD_ACCEPT, "accept 值错" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        FakeWSServer srv;
        if (!srv.Start(cases[i].mode))
        {
            printf("  FAIL 起假服务端失败\n"); g_failures++; return;
        }
        CtorHandler ch;
        WSConnector c;
        CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

        Waiter    w;
        WSConnPtr conn = c.CreateConnection(NULL, &w);
        CHECK(conn != NULL);
        if (!conn) { srv.Stop(); return; }
        CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
        CHECK(w.WaitConnect(8000));
        CHECK_EQ_INT(w.ConnectCalls(), 1);
        CHECK_EQ_INT(w.ConnectResult(), BC_R_WS_HANDSHAKE_FAILED);
        CHECK(!conn->IsUpgraded());
        
        CHECK(conn->SendText("x") == BC_R_NOTCONNECTED);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_EQ_INT(w.ClosedCalls(), 0);
        printf("  %s：result=%d，未发 OnClosed\n", cases[i].name,
               (int)w.ConnectResult());
        srv.Stop();
    }
}








static void case_c_handshake_timeout()
{
    printf("[case C] 服务端不回 101 -> 握手超时\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_NO_REPLY))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }

    const uint64_t t0 = (uint64_t)time(NULL);
    CHECK(conn->Connect(WsUrl(srv.Port()), 1200) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectCalls(), 1);
    CHECK_EQ_INT(w.ConnectResult(), BC_R_CONNECT_TIMEOUT);
    CHECK_EQ_INT(w.ClosedCalls(), 0);
    CHECK((uint64_t)time(NULL) - t0 < 6);
    srv.Stop();
}





static void case_d_fragments_and_ping()
{
    printf("[case D] 分片消息重组 + 中间插 ping（自动回 pong）\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_FRAGMENTS))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }
    CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);

    
    CHECK(w.WaitTexts(1, 5000));
    CHECK_EQ_STR(w.Texts()[0], "Hello, fragmented world");
    CHECK_EQ_INT(w.Texts().size(), 1);

    
    CHECK(WaitFor([&] { return srv.PongsFromClient() >= 1; }));
    
    
    std::vector<std::string> payloads = srv.ClientPayloads();
    bool foundHb = false;
    for (size_t i = 0; i < payloads.size(); i++)
    {
        if (payloads[i] == "hb") foundHb = true;
    }
    CHECK(foundHb);
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);

    conn->Close();
    CHECK(w.WaitClosed(8000));
    
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}







static void case_e_oversize_frame()
{
    printf("[case E] 超大帧 -> OnException + 关连接\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_OVERSIZE))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    BCFObject cfg;
    cfg.PutInt("maxFrameBytes", 64 * 1024);

    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }
    CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);

    CHECK(w.WaitException(5000));
    CHECK(w.ExceptionCalls() >= 1);
    CHECK(w.WaitClosed(8000));
    CHECK_EQ_INT(w.ClosedCalls(), 1);
    printf("  exception: %s\n  close: %s\n", w.ExceptionMsg().c_str(),
           w.CloseReason().c_str());
    srv.Stop();
}





static void case_f_peer_close()
{
    printf("[case F] 对端发 close(1001) -> 回 close 帧并收尾\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_CLOSE_FIRST))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }
    CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);

    CHECK(w.WaitClosed(8000));
    CHECK_EQ_INT(w.ClosedCalls(), 1);
    CHECK(Contains(w.CloseReason(), "1001"));
    
    
    
    
    
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}








static void case_g_piggyback()
{
    printf("[case G] 101 与第一帧粘包\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_PIGGYBACK))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }
    CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);
    CHECK(w.WaitTexts(1, 5000));
    CHECK_EQ_STR(w.Texts()[0], "piggybacked");

    conn->Close();
    CHECK(w.WaitClosed(8000));
    srv.Stop();
}





static void case_h_server_ping()
{
    printf("[case H] 服务端 ping -> 客户端自动 pong（payload 原样回显）\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_PING))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }
    CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));

    CHECK(WaitFor([&] { return srv.PongsFromClient() >= 1; }, 3000));
    
    std::vector<std::string> payloads = srv.ClientPayloads();
    bool echoed = false;
    for (size_t i = 0; i < payloads.size(); i++)
    {
        if (payloads[i] == "srv-ping") echoed = true;
    }
    CHECK(echoed);

    
    
    const int framesBeforePing = srv.ClientFrames();
    CHECK(conn->SendPing() == BC_R_SUCCESS);
    CHECK(WaitFor([&] { return srv.ClientFrames() > framesBeforePing; }));

    conn->Close();
    CHECK(w.WaitClosed(8000));
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}









static void case_i_close_race()
{
    printf("[case I] 关闭握手与 Close() 并发（20 轮）\n");

    for (int round = 0; round < 20; round++)
    {
        FakeWSServer srv;
        if (!srv.Start(FakeWSServer::MODE_CLOSE_RACE))
        {
            printf("  FAIL 起假服务端失败\n"); g_failures++; return;
        }
        CtorHandler ch;
        WSConnector c;
        CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

        Waiter    w;
        WSConnPtr conn = c.CreateConnection(NULL, &w);
        if (!conn) { g_failures++; srv.Stop(); return; }
        CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);

        
        
        
        const int delayMs = round % 5;
        std::thread killer([conn, delayMs] {
            if (delayMs > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
            conn->Close();
        });
        killer.join();

        
        CHECK(w.WaitConnect(8000));
        CHECK_EQ_INT(w.ConnectCalls(), 1);
        if (w.ConnectResult() == BC_R_SUCCESS)
        {
            CHECK(w.WaitClosed(8000));
            CHECK_EQ_INT(w.ClosedCalls(), 1);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK_EQ_INT(w.ClosedCalls(), 0);
        }
        srv.Stop();
    }
    printf("  20 轮全部收尾正常\n");
}





static void case_j_connect_refused()
{
    printf("[case J] 端口没人监听 -> OnConnectResult 报错，无 OnClosed\n");

    uint16_t port = FindFreePort();
    CHECK(port != 0);

    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) return;
    CHECK(conn->Connect(WsUrl(port), 3000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectCalls(), 1);
    CHECK(w.ConnectResult() != BC_R_SUCCESS);
    CHECK_EQ_INT(w.ClosedCalls(), 0);
    printf("  result=%d\n", (int)w.ConnectResult());
}





static void case_k_argument_validation()
{
    printf("[case K] 参数校验\n");

    
    {
        WSConnector c;
        Waiter      w;
        CHECK(c.CreateConnection(NULL, &w) == NULL);
    }

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_ECHO))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    {
        Waiter    w;
        WSConnPtr conn = c.CreateConnection(NULL, &w);
        CHECK(conn != NULL);
        if (conn)
        {
            
            CHECK(conn->Connect("http://example.com/", 3000) == BC_R_INVALIDARG);
            CHECK(conn->Connect("ws://", 3000) == BC_R_INVALIDARG);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK_EQ_INT(w.ConnectCalls(), 0);
            CHECK_EQ_INT(w.ClosedCalls(), 0);
        }
    }
    {
        Waiter    w;
        WSConnPtr conn = c.CreateConnection(NULL, &w);
        CHECK(conn != NULL);
        if (conn)
        {
            
            CHECK(conn->SetRequestHeader("X-A", "1") == BC_R_SUCCESS);
            CHECK(conn->SetRequestHeader("X B", "1") == BC_R_INVALIDARG);
            CHECK(conn->SetRequestHeader("X-A", "a\r\nb") == BC_R_INVALIDARG);
            CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
            CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_ALREADYRUNNING);
            CHECK(conn->SetRequestHeader("X-B", "2") == BC_R_ALREADYRUNNING);
            CHECK(w.WaitConnect(8000));
            CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);
            CHECK(Contains(srv.Request(), "X-A: 1\r\n"));
            conn->Close();
            CHECK(w.WaitClosed(8000));
        }
    }
    srv.Stop();
}





static void case_l_idle_timeout()
{
    printf("[case L] idleTimeoutMs -> 主动关闭\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_ECHO))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    BCFObject cfg;
    cfg.PutInt("idleTimeoutMs", 600);

    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }
    CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);

    CHECK(w.WaitClosed(8000));
    CHECK_EQ_INT(w.ClosedCalls(), 1);
    CHECK(Contains(w.CloseReason(), "空闲超时"));

    ConnStatsMap stats;
    CHECK(c.GetStats(stats) == BC_R_SUCCESS);
    CHECK_EQ_INT(stats["idle_timeout_size"], 1);
    CHECK_EQ_INT(stats["allocated_conn_size"], 1);
    srv.Stop();
}











static void case_m_wss_with_domain()
{
    printf("[case M] wss:// + 域名（localhost）+ 自签证书校验\n");

    std::string pem;
    if (!ReadFileAll("certs/localhost.crt", pem))
    {
        printf("  SKIP：读不到 certs/localhost.crt（请在仓库根目录运行）\n");
        g_skipped++;
        return;
    }

    
    {
        FakeWSServer srv;
        if (!srv.Start(FakeWSServer::MODE_ECHO, true))
        {
            printf("  SKIP：起 TLS 假服务端失败\n"); g_skipped++; return;
        }
        char url[160];
        snprintf(url, sizeof(url), "wss://localhost:%u/", (unsigned)srv.Port());

        CtorHandler ch;
        WSConnector c;
        CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);
        CHECK(c.TlsSettings().caCertsPem.empty());

        Waiter    w;
        WSConnPtr conn = c.CreateConnection(NULL, &w);
        CHECK(conn != NULL);
        if (conn)
        {
            CHECK(conn->Connect(url, 8000) == BC_R_SUCCESS);
            CHECK(w.WaitConnect(12000));
            CHECK_EQ_INT(w.ConnectCalls(), 1);
            CHECK(w.ConnectResult() != BC_R_SUCCESS);   
            printf("  无 CA：result=%d（拒绝自签证书）\n", (int)w.ConnectResult());
        }
        srv.Stop();
    }

    
    {
        FakeWSServer srv;
        if (!srv.Start(FakeWSServer::MODE_ECHO, true))
        {
            printf("  SKIP：起 TLS 假服务端失败\n"); g_skipped++; return;
        }
        char url[160];
        snprintf(url, sizeof(url), "wss://localhost:%u/secure", (unsigned)srv.Port());

        BCFObject cfg;
        cfg.PutString("caCerts", pem.c_str());

        CtorHandler ch;
        WSConnector c;
        CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

        Waiter    w;
        WSConnPtr conn = c.CreateConnection(NULL, &w);
        CHECK(conn != NULL);
        if (conn)
        {
            CHECK(conn->Connect(url, 8000) == BC_R_SUCCESS);
            CHECK(w.WaitConnect(12000));
            CHECK_EQ_INT(w.ConnectCalls(), 1);
            CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);
            
            CHECK(Contains(srv.Request(), "Host: localhost:"));
            CHECK(Contains(srv.Request(), "GET /secure HTTP/1.1"));

            CHECK(conn->SendText("over tls") == BC_R_SUCCESS);
            CHECK(w.WaitTexts(1, 8000));
            CHECK_EQ_STR(w.Texts()[0], "over tls");
            
            CHECK_EQ_STR(conn->PeerIp(), "127.0.0.1");
            printf("  有 CA：握手成功，回声正常，peer=%s\n",
                   conn->PeerIp().c_str());

            conn->Close();
            CHECK(w.WaitClosed(8000));
            CHECK_EQ_INT(w.ClosedCalls(), 1);
            
            CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
            CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
        }
        srv.Stop();
    }
}








static void case_n_close_and_drain()
{
    printf("[case N] Close() 与析构排空（含从没 Connect 的连接）\n");

    
    {
        FakeWSServer srv;
        if (!srv.Start(FakeWSServer::MODE_ECHO, false, 3))
        {
            printf("  FAIL 起假服务端失败\n"); g_failures++; return;
        }
        CtorHandler ch;
        WSConnector c;
        CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

        Waiter    w1, w2;
        WSConnPtr c1 = c.CreateConnection(NULL, &w1);
        WSConnPtr c2 = c.CreateConnection(NULL, &w2);
        CHECK(c1 != NULL && c2 != NULL);
        if (c1) CHECK(c1->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
        if (c2) CHECK(c2->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
        CHECK(w1.WaitConnect(8000));

        c.Close();
        CHECK(ch.WaitClosed(10000));
        CHECK_EQ_INT(ch.ClosedCalls(), 1);
        
        c.Close();
        CHECK_EQ_INT(ch.ClosedCalls(), 1);
        srv.Stop();
    }

    
    {
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 30000);
        CtorHandler ch;
        const uint64_t t0 = (uint64_t)time(NULL);
        {
            WSConnector c;
            CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);
            Waiter    w;
            WSConnPtr conn = c.CreateConnection(NULL, &w);
            CHECK(conn != NULL);
            
        }
        const uint64_t elapsed = (uint64_t)time(NULL) - t0;
        CHECK(elapsed < 5);
        CHECK_EQ_INT(ch.ClosedCalls(), 1);
        printf("  从没 Connect 的连接：析构耗时 %llus\n",
               (unsigned long long)elapsed);
    }

    
    {
        FakeWSServer srv;
        if (!srv.Start(FakeWSServer::MODE_ECHO))
        {
            printf("  FAIL 起假服务端失败\n"); g_failures++; return;
        }
        CtorHandler ch;
        Waiter      w;
        {
            WSConnector c;
            CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);
            WSConnPtr conn = c.CreateConnection(NULL, &w);
            CHECK(conn != NULL);
            if (conn) CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
        }
        
        const int connectCalls = w.ConnectCalls();
        const int closedCalls  = w.ClosedCalls();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK_EQ_INT(w.ConnectCalls(), connectCalls);
        CHECK_EQ_INT(w.ClosedCalls(), closedCalls);
        srv.Stop();
    }
}







class ReentrantHandler : public IWSConnectionHandler
{
public:
    WSConnPtr conn;

    void OnConnectResult(BCRESULT result, const HttpHeaderMap&) override
    {
        std::unique_lock<std::mutex> lk(m_);
        connect_result_ = result;
        if (result == BC_R_SUCCESS && conn)
        {
            
            conn->SendText("from-callback");
        }
        cv_.notify_all();
    }
    void OnRecvText(LPCSTR text) override
    {
        std::unique_lock<std::mutex> lk(m_);
        texts_.push_back(text ? text : "");
        if (texts_.size() == 1 && conn)
        {
            
            conn->Close();
        }
        cv_.notify_all();
    }
    void OnRecvData(LPCVOID, size_t) override {}
    void OnClosed(LPCSTR) override
    {
        std::unique_lock<std::mutex> lk(m_);
        closed_++;
        
        conn.reset();
        cv_.notify_all();
    }
    void OnException(BCException&) override {}

    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return closed_ > 0; });
    }
    int Closed() { std::unique_lock<std::mutex> lk(m_); return closed_; }
    std::vector<std::string> Texts()
    {
        std::unique_lock<std::mutex> lk(m_);
        return texts_;
    }
    BCRESULT ConnectResult()
    {
        std::unique_lock<std::mutex> lk(m_);
        return connect_result_;
    }

private:
    std::mutex               m_;
    std::condition_variable  cv_;
    int                      closed_ = 0;
    BCRESULT                 connect_result_ = BC_R_FAILURE;
    std::vector<std::string> texts_;
};

static void case_o_reentrant_callbacks()
{
    printf("[case O] 回调里 SendText / Close / 释放 WSConnPtr\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_ECHO))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler      ch;
    ReentrantHandler rh;
    {
        WSConnector c;
        CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);
        WSConnPtr conn = c.CreateConnection(NULL, &rh);
        CHECK(conn != NULL);
        if (!conn) { srv.Stop(); return; }
        rh.conn = conn;
        conn.reset();        
        CHECK(rh.conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
        CHECK(rh.WaitClosed(10000));
        CHECK_EQ_INT(rh.Closed(), 1);
        CHECK_EQ_INT(rh.ConnectResult(), BC_R_SUCCESS);
        CHECK(rh.Texts().size() >= 1);
        if (rh.Texts().size() >= 1)
        {
            CHECK_EQ_STR(rh.Texts()[0], "from-callback");
        }
    }
    srv.Stop();
}











static void case_p_malformed_close_frame()
{
    printf("[case P] close 帧 body 长度为 1 -> 协议违规，回 1002\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_CLOSE_BAD_LEN))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }
    CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);

    
    CHECK(w.WaitException(5000));
    CHECK(w.ExceptionCalls() >= 1);
    CHECK(Contains(w.ExceptionMsg(), "close frame body"));

    CHECK(w.WaitClosed(8000));
    CHECK_EQ_INT(w.ClosedCalls(), 1);
    
    CHECK(!Contains(w.CloseReason(), "1000"));
    printf("  exception: %s\n  close: %s\n", w.ExceptionMsg().c_str(),
           w.CloseReason().c_str());

    
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.ClientCloseCode(), 1002);
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}








static void case_q_close_frame_legal_lengths()
{
    printf("[case Q] 合法 close 帧：body 为空也要正常收尾\n");

    FakeWSServer srv;
    if (!srv.Start(FakeWSServer::MODE_CLOSE_EMPTY))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    CtorHandler ch;
    WSConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter    w;
    WSConnPtr conn = c.CreateConnection(NULL, &w);
    CHECK(conn != NULL);
    if (!conn) { srv.Stop(); return; }
    CHECK(conn->Connect(WsUrl(srv.Port()), 5000) == BC_R_SUCCESS);
    CHECK(w.WaitConnect(8000));
    CHECK_EQ_INT(w.ConnectResult(), BC_R_SUCCESS);

    CHECK(w.WaitClosed(8000));
    CHECK_EQ_INT(w.ClosedCalls(), 1);
    
    CHECK_EQ_INT(w.ExceptionCalls(), 0);
    CHECK(Contains(w.CloseReason(), "1000"));
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.ClientCloseCode(), 1000);
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}





typedef void (*CaseFn)();

int main(int argc, char** argv)
{
    
    setvbuf(stdout, NULL, _IONBF, 0);
    (void)argc;
    (void)argv;

    
    signal(SIGPIPE, SIG_IGN);

    SSL_library_init();

    const char* only = getenv("TT_CASE");

#define RUN(fn)                                                                \
    do {                                                                       \
        if (!only || !*only || strstr(#fn, only)) { fn(); }                     \
    } while (0)

    RUN(case_a_echo);
    RUN(case_b_handshake_failures);
    RUN(case_c_handshake_timeout);
    RUN(case_d_fragments_and_ping);
    RUN(case_e_oversize_frame);
    RUN(case_f_peer_close);
    RUN(case_g_piggyback);
    RUN(case_h_server_ping);
    RUN(case_i_close_race);
    RUN(case_j_connect_refused);
    RUN(case_k_argument_validation);
    RUN(case_l_idle_timeout);
    RUN(case_m_wss_with_domain);
    RUN(case_n_close_and_drain);
    RUN(case_o_reentrant_callbacks);
    RUN(case_p_malformed_close_frame);
    RUN(case_q_close_frame_legal_lengths);

    Runtime::Destroy();

    if (g_failures == 0 && g_skipped == 0)
    {
        printf("WSConnector_integration_test: ALL PASSED\n");
        return 0;
    }
    if (g_failures == 0)
    {
        printf("WSConnector_integration_test: PASSED（%d 个用例被跳过）\n",
               g_skipped);
        return 2;
    }
    printf("WSConnector_integration_test: %d FAILURE(S)\n", g_failures);
    return 1;
}




