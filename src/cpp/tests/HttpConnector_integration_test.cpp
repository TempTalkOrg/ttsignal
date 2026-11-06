



#include "HttpConnector.h"
#include "Runtime.h"
#include "TTErrors.h"

#include <BC/BCFCodec.h>

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

static bool Contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}





class Waiter : public IHttpRequestHandler
{
public:
    void OnHttpResponse(const HttpResponse& resp) override
    {
        std::unique_lock<std::mutex> lk(m_);
        got_response_ = true;
        resp_         = resp;
        calls_++;
        cv_.notify_all();
    }

    void OnHttpError(BCRESULT result, const std::string& message) override
    {
        std::unique_lock<std::mutex> lk(m_);
        got_error_ = true;
        result_    = result;
        message_   = message;
        calls_++;
        cv_.notify_all();
    }

    bool Wait(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return calls_ > 0; });
    }

    bool         GotResponse() { std::unique_lock<std::mutex> lk(m_); return got_response_; }
    bool         GotError()    { std::unique_lock<std::mutex> lk(m_); return got_error_; }
    int          Calls()       { std::unique_lock<std::mutex> lk(m_); return calls_; }
    BCRESULT     Result()      { std::unique_lock<std::mutex> lk(m_); return result_; }
    std::string  Message()     { std::unique_lock<std::mutex> lk(m_); return message_; }
    HttpResponse Resp()        { std::unique_lock<std::mutex> lk(m_); return resp_; }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    int                     calls_        = 0;
    bool                    got_response_ = false;
    bool                    got_error_    = false;
    BCRESULT                result_       = BC_R_SUCCESS;
    std::string             message_;
    HttpResponse            resp_;
};





class ConnHandler : public IHttpConnectorHandler
{
public:
    explicit ConnHandler(bool echoLogs = false) : echo_(echoLogs) {}

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
};








class FakeHttpServer
{
public:
    enum Mode {
        
        MODE_CANNED  = 0,
        
        MODE_SPLIT   = 1,
        
        MODE_STALL   = 2,
        
        MODE_NOREPLY = 3,
        
        
        MODE_INSTANT_CLOSE = 9,
        
        
        
        
        MODE_EOF_BODY_STALL = 4,
        
        
        
        
        MODE_TWO_STAGE = 5,
        
        
        MODE_HEADER_FLOOD = 6,
        
        
        MODE_REASON_FLOOD = 7,
        
        
        
        
        MODE_CANNED_KEEPALIVE = 8,
    };

    bool Start(Mode mode, const std::string& response,
               const std::string& response2 = std::string())
    {
        mode_      = mode;
        response_  = response;
        response2_ = response2;

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
        if (listen(lfd, 4) != 0) return false;

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
    }

    uint16_t Port() const { return port_; }

    std::string Request()
    {
        std::unique_lock<std::mutex> lk(m_);
        return request_;
    }

private:
    void _Run()
    {
        if (mode_ == MODE_INSTANT_CLOSE)
        {
            
            while (!stop_)
            {
                int c = accept(listen_fd_.load(), NULL, NULL);
                if (c < 0) break;
                close(c);
            }
            return;
        }

        int fd = accept(listen_fd_.load(), NULL, NULL);
        if (fd < 0) return;

        if (mode_ == MODE_INSTANT_CLOSE)
        {
            close(fd);
            return;
        }

        
        std::string req;
        char        buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos)
        {
            struct timeval tv = { 3, 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, (size_t)n);
        }
        {
            std::unique_lock<std::mutex> lk(m_);
            request_ = req;
        }

        if (mode_ == MODE_CANNED)
        {
            _SendAll(fd, response_.data(), response_.size());
        }
        else if (mode_ == MODE_SPLIT)
        {
            const size_t kChunk = 7;    
            for (size_t off = 0; off < response_.size() && !stop_; off += kChunk)
            {
                size_t n = response_.size() - off;
                if (n > kChunk) n = kChunk;
                if (!_SendAll(fd, response_.data() + off, n)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        else if (mode_ == MODE_STALL)
        {
            const char kHalf[] = "HTTP/1.1 200 OK\r\nContent-Type: text/pl";
            _SendAll(fd, kHalf, sizeof(kHalf) - 1);
            while (!stop_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        else if (mode_ == MODE_CANNED_KEEPALIVE)
        {
            _SendAll(fd, response_.data(), response_.size());
            while (!stop_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        else if (mode_ == MODE_TWO_STAGE)
        {
            _SendAll(fd, response_.data(), response_.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            _SendAll(fd, response2_.data(), response2_.size());
        }
        else if (mode_ == MODE_HEADER_FLOOD)
        {
            const char kHead[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
            _SendAll(fd, kHead, sizeof(kHead) - 1);
            
            
            std::string pad(4000, 'A');
            for (uint64_t i = 0; !stop_; i++)
            {
                char name[64];
                snprintf(name, sizeof(name), "X-Pad-%llu: ",
                         (unsigned long long)i);
                if (!_SendAll(fd, name, strlen(name))) break;
                if (!_SendAll(fd, pad.data(), pad.size())) break;
                if (!_SendAll(fd, "\r\n", 2)) break;
            }
        }
        else if (mode_ == MODE_REASON_FLOOD)
        {
            const char kHead[] = "HTTP/1.1 200 ";
            _SendAll(fd, kHead, sizeof(kHead) - 1);
            std::string pad(60000, 'R');
            while (!stop_)
            {
                if (!_SendAll(fd, pad.data(), pad.size())) break;
            }
        }
        else if (mode_ == MODE_EOF_BODY_STALL)
        {
            
            
            const char kResp[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "\r\n"
                "partial-body-then-silence";
            _SendAll(fd, kResp, sizeof(kResp) - 1);
            while (!stop_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        

        close(fd);
    }

    bool _SendAll(int fd, const char* data, size_t size)
    {
        size_t off = 0;
        while (off < size)
        {
            ssize_t n = send(fd, data + off, size - off, 0);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }

    Mode              mode_      = MODE_CANNED;
    std::string       response_;
    std::string       response2_;
    
    
    std::atomic<int>  listen_fd_{-1};
    uint16_t          port_      = 0;
    std::atomic<bool> stop_{false};
    std::thread       thread_;
    std::mutex        m_;
    std::string       request_;
};





static uint64_t NowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

static std::string UrlFor(uint16_t port, const char* path)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "http://127.0.0.1:%u%s", (unsigned)port, path);
    return buf;
}

static bool PortIsOpen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool ok = (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    close(fd);
    return ok;
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





static void case_a_plain_ok()
{
    printf("[case A] 明文 GET，Content-Length 响应\n");

    const char* kResp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 11\r\n"
        "X-Multi: a\r\n"
        "X-Multi: b\r\n"
        "\r\n"
        "hello world";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url                  = UrlFor(srv.Port(), "/api/x?a=1");
    req.timeoutMs            = 5000;
    req.headers["X-Trace-Id"] = "abc123";

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);              
    CHECK(w.GotResponse());
    CHECK(!w.GotError());

    HttpResponse r = w.Resp();
    CHECK(r.status == 200);
    
    CHECK_EQ_STR(r.reason, "OK");
    CHECK_EQ_STR(r.body, "hello world");
    CHECK_EQ_STR(r.headers["content-type"], "text/plain");
    CHECK_EQ_STR(r.headers["content-length"], "11");
    
    CHECK_EQ_STR(r.headers["x-multi"], "a, b");
    
    CHECK_EQ_STR(r.peerIp, "127.0.0.1");
    CHECK(r.pinMethod.empty());

    
    std::string got = srv.Request();
    CHECK(Contains(got, "GET /api/x?a=1 HTTP/1.1\r\n"));
    CHECK(Contains(got, "Host: 127.0.0.1:"));
    CHECK(Contains(got, "User-Agent: ttsignal/1.0\r\n"));
    
    
    
    CHECK(!Contains(got, "Connection:"));
    CHECK(Contains(got, "X-Trace-Id: abc123\r\n"));
    CHECK(!Contains(got, "Content-Length"));

    printf("  status=%d body=%zu peer=%s\n", r.status, r.body.size(),
           r.peerIp.c_str());
    srv.Stop();
}





static void case_b_chunked()
{
    printf("[case B] chunked 响应\n");

    const char* kResp =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "1\r\n \r\n"
        "5\r\nworld\r\n"
        "0\r\n\r\n";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.GotResponse());
    CHECK(w.Resp().status == 200);
    
    CHECK_EQ_STR(w.Resp().body, "hello world");
    srv.Stop();
}








static void case_c_split_delivery()
{
    printf("[case C] 响应被切成 7 字节一片（增量喂料）\n");

    const char* kResp =
        "HTTP/1.1 201 Created\r\n"
        "Content-Type: application/json\r\n"
        "X-Long-Header-Name: some-fairly-long-header-value-here\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "b\r\n{\"ok\":true}\r\n"
        "0\r\n\r\n";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_SPLIT, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/");
    req.timeoutMs = 8000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(12000));
    CHECK(w.GotResponse());
    CHECK(w.Resp().status == 201);
    CHECK_EQ_STR(w.Resp().body, "{\"ok\":true}");
    CHECK_EQ_STR(w.Resp().headers["content-type"], "application/json");
    CHECK_EQ_STR(w.Resp().headers["x-long-header-name"],
                 "some-fairly-long-header-value-here");
    srv.Stop();
}









static void case_d_max_response_bytes()
{
    printf("[case D] 响应体超过 maxResponseBytes\n");

    std::string body(64 * 1024, 'x');
    char        header[256];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n",
             body.size());
    std::string resp = std::string(header) + body;

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, resp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    BCFObject cfg;
    cfg.PutInt("maxResponseBytes", 1024);

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);
    CHECK(c.MaxResponseBytes() == 1024);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/big");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(!w.GotResponse());        
    CHECK(w.Result() == BC_R_RESPONSE_TOO_LARGE);
    CHECK(Contains(w.Message(), "maxResponseBytes"));
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}





static void case_e_no_redirect_follow()
{
    printf("[case E] 302 原样返回，不跟随\n");

    const char* kResp =
        "HTTP/1.1 302 Found\r\n"
        "Location: https://elsewhere.example.com/next\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/old");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.GotResponse());
    CHECK(w.Resp().status == 302);
    
    CHECK_EQ_STR(w.Resp().headers["location"],
                 "https://elsewhere.example.com/next");
    CHECK(w.Resp().body.empty());
    
    CHECK(Contains(srv.Request(), "GET /old HTTP/1.1"));
    srv.Stop();
}








static void case_f_truncated_body()
{
    printf("[case F] 响应体中途 FIN，不得当成功\n");

    const char* kResp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "abcd";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotError());            
    CHECK(!w.GotResponse());
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}









static void case_g_response_timeout()
{
    printf("[case G] 服务端回半截头就装死 → 超时\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_STALL, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/");
    req.timeoutMs = 1200;       

    Waiter   w;
    uint64_t t0 = NowMs();
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    uint64_t t1 = NowMs();

    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(w.Result() == BC_R_TIMEDOUT);
    
    CHECK(t1 - t0 >= 1000);
    CHECK(t1 - t0 < 6000);
    printf("  result=%d 耗时=%llums msg=%s\n", (int)w.Result(),
           (unsigned long long)(t1 - t0), w.Message().c_str());
    srv.Stop();
}





static void case_h_no_reply()
{
    printf("[case H] 服务端读完请求直接关连接\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_NOREPLY, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(w.Result() == BC_R_UNEXPECTEDEND);
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}





static void case_i_connect_refused()
{
    printf("[case I] 端口没人听\n");

    
    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_NOREPLY, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }
    uint16_t deadPort = srv.Port();
    srv.Stop();

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(deadPort, "/");
    req.timeoutMs = 3000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(w.Result() != BC_R_SUCCESS);
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
}











static void case_j_close_with_inflight()
{
    printf("[case J] Close() 与在途请求并发（10 轮）\n");

    const int kIters = 10;
    for (int i = 0; i < kIters; i++)
    {
        FakeHttpServer srv;
        if (!srv.Start(FakeHttpServer::MODE_STALL, ""))
        {
            printf("  FAIL 起假服务端失败\n"); g_failures++; return;
        }

        ConnHandler   ch;
        {
            HttpConnector c;
            CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

            HttpRequest req;
            req.url       = UrlFor(srv.Port(), "/");
            req.timeoutMs = 30000;      

            Waiter w;
            CHECK(c.Request(req, &w) == BC_R_SUCCESS);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(i * 3));
            c.Close();

            CHECK(w.Wait(8000));
            CHECK(w.Calls() == 1);
            CHECK(w.GotError());
            CHECK(ch.WaitClosed(8000));
            
            CHECK(c.Request(req, &w) == BC_R_SHUTTINGDOWN);
            
            c.Close();
            
        }
        CHECK(ch.ClosedCalls() == 1);
        srv.Stop();

        if (g_failures > 0)
        {
            printf("  第 %d 轮出现失败，停止\n", i);
            break;
        }
    }
    printf("  done\n");
}





static void case_k_parallel_requests()
{
    printf("[case K] 同一个 connector 上并发 4 条请求\n");

    const char* kResp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "ok";

    const int      kN = 4;
    FakeHttpServer srv[kN];
    for (int i = 0; i < kN; i++)
    {
        if (!srv[i].Start(FakeHttpServer::MODE_CANNED, kResp))
        {
            printf("  FAIL 起假服务端失败\n"); g_failures++; return;
        }
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    Waiter w[kN];
    for (int i = 0; i < kN; i++)
    {
        HttpRequest req;
        req.url       = UrlFor(srv[i].Port(), "/");
        req.timeoutMs = 8000;
        CHECK(c.Request(req, &w[i]) == BC_R_SUCCESS);
    }
    for (int i = 0; i < kN; i++)
    {
        CHECK(w[i].Wait(12000));
        CHECK(w[i].GotResponse());
        CHECK(w[i].Resp().status == 200);
        CHECK_EQ_STR(w[i].Resp().body, "ok");
    }
    for (int i = 0; i < kN; i++)
    {
        srv[i].Stop();
    }
}





static void case_l_post_body()
{
    printf("[case L] POST 带 body\n");

    const char* kResp =
        "HTTP/1.1 204 No Content\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.method                     = "POST";
    req.url                        = UrlFor(srv.Port(), "/submit");
    req.body                       = "{\"k\":1}";
    req.headers["Content-Type"]    = "application/json";
    req.timeoutMs                  = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.GotResponse());
    CHECK(w.Resp().status == 204);
    CHECK(w.Resp().body.empty());

    std::string got = srv.Request();
    CHECK(Contains(got, "POST /submit HTTP/1.1\r\n"));
    CHECK(Contains(got, "Content-Length: 7\r\n"));
    CHECK(Contains(got, "Content-Type: application/json\r\n"));
    CHECK(Contains(got, "{\"k\":1}"));
    srv.Stop();
}





static void case_m_malformed_response()
{
    printf("[case M] 畸形响应\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, "NOT-HTTP AT ALL\r\n\r\n"))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(!w.GotResponse());
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}






















static void case_o_destroy_parser_in_eof_state()
{
    printf("[case O] EOF 断句状态下销毁 parser（析构崩溃回归）\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_EOF_BODY_STALL, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    {
        ConnHandler   ch;
        HttpConnector c;
        CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

        HttpRequest req;
        req.url       = UrlFor(srv.Port(), "/eof-body");
        req.timeoutMs = 1000;

        Waiter w;
        CHECK(c.Request(req, &w) == BC_R_SUCCESS);
        CHECK(w.Wait(8000));
        CHECK(w.Calls() == 1);
        CHECK(w.GotError());
        CHECK(!w.GotResponse());        
        CHECK(w.Result() == BC_R_TIMEDOUT);
        printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());

        
        
    }

    printf("  parser 已销毁，进程存活 —— 回归通过\n");
    srv.Stop();
}













static void case_p_eof_body_clean_close()
{
    printf("[case P] EOF 断句 + 干净关闭 → 必须成功（case O 的对照组）\n");

    
    const char* kResp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "body-terminated-by-close";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/eof-clean");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotResponse());
    CHECK(!w.GotError());
    CHECK(w.Resp().status == 200);
    CHECK_EQ_STR(w.Resp().body, "body-terminated-by-close");
    CHECK_EQ_STR(w.Resp().headers["content-type"], "text/plain");
    printf("  status=%d body=%zu\n", w.Resp().status, w.Resp().body.size());
    srv.Stop();
}












static void case_q_header_flood()
{
    printf("[case Q] 无限响应头流 → 必须被上限截断（C-1 回归）\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_HEADER_FLOOD, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/flood");
    req.timeoutMs = 8000;

    Waiter   w;
    uint64_t t0 = NowMs();
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(15000));
    uint64_t t1 = NowMs();

    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(!w.GotResponse());
    CHECK(w.Result() == BC_R_RESPONSE_TOO_LARGE);
    
    CHECK(w.Result() != BC_R_TIMEDOUT);
    CHECK(t1 - t0 < 5000);
    CHECK(Contains(w.Message(), "响应头总大小"));
    printf("  result=%d 耗时=%llums msg=%s\n", (int)w.Result(),
           (unsigned long long)(t1 - t0), w.Message().c_str());
    srv.Stop();

    
    
    
    std::string many = "HTTP/1.1 200 OK\r\n";
    for (int i = 0; i < 500; i++)
    {
        char line[64];
        snprintf(line, sizeof(line), "X-N-%d: v\r\n", i);
        many += line;
    }
    many += "Content-Length: 2\r\n\r\nok";

    FakeHttpServer srv2;
    if (!srv2.Start(FakeHttpServer::MODE_CANNED, many))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch2;
    HttpConnector c2;
    CHECK(c2.Create(NULL, &ch2) == BC_R_SUCCESS);

    HttpRequest req2;
    req2.url       = UrlFor(srv2.Port(), "/manyhdr");
    req2.timeoutMs = 5000;

    Waiter w2;
    CHECK(c2.Request(req2, &w2) == BC_R_SUCCESS);
    CHECK(w2.Wait(8000));
    CHECK(w2.GotError());
    CHECK(!w2.GotResponse());
    CHECK(w2.Result() == BC_R_RESPONSE_TOO_LARGE);
    CHECK(Contains(w2.Message(), "响应头条数"));
    printf("  500 条小头：result=%d msg=%s\n", (int)w2.Result(),
           w2.Message().c_str());
    srv2.Stop();
}










static void case_r_reason_flood()
{
    printf("[case R] 无限 status reason → 必须被上限截断（C-1 回归）\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_REASON_FLOOD, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/reason");
    req.timeoutMs = 8000;

    Waiter   w;
    uint64_t t0 = NowMs();
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(15000));
    uint64_t t1 = NowMs();

    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(w.Result() == BC_R_RESPONSE_TOO_LARGE);
    CHECK(t1 - t0 < 5000);
    CHECK(Contains(w.Message(), "reason-phrase"));
    printf("  result=%d 耗时=%llums msg=%s\n", (int)w.Result(),
           (unsigned long long)(t1 - t0), w.Message().c_str());
    srv.Stop();
}








static void case_s_single_huge_header()
{
    printf("[case S] 单条 8 MB 响应头 → 必须被上限截断（C-1 回归）\n");

    std::string resp = "HTTP/1.1 200 OK\r\nX-Big: ";
    resp.append(8 * 1024 * 1024, 'A');
    resp += "\r\nContent-Length: 2\r\n\r\nok";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, resp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    BCFObject cfg;
    cfg.PutInt("maxResponseBytes", 1024);

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/bighdr");
    req.timeoutMs = 8000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(15000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(!w.GotResponse());        
    CHECK(w.Result() == BC_R_RESPONSE_TOO_LARGE);
    CHECK(Contains(w.Message(), "单条响应头"));
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}











static void case_t_interim_1xx()
{
    printf("[case T] 103 Early Hints 之后才是真响应（I-1 回归）\n");

    const char* kEarly =
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </s.css>; rel=preload\r\n"
        "\r\n";
    const char* kFinal =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 9\r\n"
        "\r\n"
        "REAL-BODY";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_TWO_STAGE, kEarly, kFinal))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/early");
    req.timeoutMs = 8000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(12000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotResponse());
    CHECK(!w.GotError());
    
    CHECK(w.Resp().status == 200);
    CHECK_EQ_STR(w.Resp().body, "REAL-BODY");
    
    
    
    HttpResponse got = w.Resp();
    CHECK_EQ_STR(got.headers["content-type"], "text/plain");
    
    CHECK(got.headers.find("link") == got.headers.end());
    CHECK(got.headers.size() == 2);     
    printf("  status=%d body=%s\n", w.Resp().status, w.Resp().body.c_str());
    srv.Stop();
}














static void case_u_head_request()
{
    printf("[case U] HEAD + Content-Length 且无 body、服务端 keep-alive（I-2 回归）\n");

    
    const char* kResp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 42\r\n"
        "\r\n";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED_KEEPALIVE, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.method    = "HEAD";
    req.url       = UrlFor(srv.Port(), "/head");
    req.timeoutMs = 3000;

    Waiter   w;
    uint64_t t0 = NowMs();
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(12000));
    uint64_t t1 = NowMs();

    CHECK(w.Calls() == 1);
    CHECK(w.GotResponse());         
    CHECK(!w.GotError());
    CHECK(w.Resp().status == 200);
    CHECK(w.Resp().body.empty());   
    CHECK_EQ_STR(w.Resp().headers["content-length"], "42");
    
    CHECK(t1 - t0 < 2500);
    printf("  status=%d 耗时=%llums content-length=%s\n", w.Resp().status,
           (unsigned long long)(t1 - t0),
           w.Resp().headers["content-length"].c_str());
    srv.Stop();
}









static void case_v_304_with_content_length()
{
    printf("[case V] 304 + Content-Length 且无 body、服务端 keep-alive（I-2 回归）\n");

    const char* kResp =
        "HTTP/1.1 304 Not Modified\r\n"
        "ETag: \"abc\"\r\n"
        "Content-Length: 42\r\n"
        "\r\n";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED_KEEPALIVE, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/cached");
    req.timeoutMs = 3000;

    Waiter   w;
    uint64_t t0 = NowMs();
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(12000));
    uint64_t t1 = NowMs();

    CHECK(w.Calls() == 1);
    CHECK(w.GotResponse());
    CHECK(w.Resp().status == 304);
    CHECK(w.Resp().body.empty());
    CHECK_EQ_STR(w.Resp().headers["etag"], "\"abc\"");
    CHECK(t1 - t0 < 2500);
    printf("  status=%d 耗时=%llums\n", w.Resp().status,
           (unsigned long long)(t1 - t0));
    srv.Stop();
}










static void case_w_reject_request_as_response()
{
    printf("[case W] 服务端回的是 HTTP 请求 → 必须拒绝（I-3 回归）\n");

    const char* kEvil =
        "GET /evil HTTP/1.1\r\n"
        "Host: x\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kEvil))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotError());            
    CHECK(!w.GotResponse());
    CHECK(w.Resp().status == 0);
    CHECK(w.Resp().body.empty());
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}
















static void case_x_drain_giveup_no_uaf()
{
    printf("[case X] 析构放弃等待后迟到回调不得 UAF（N-1 回归，需 ASan）\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_STALL, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    Waiter w;
    {
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 0);    

        ConnHandler   ch;
        HttpConnector c;
        CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

        HttpRequest req;
        req.url       = UrlFor(srv.Port(), "/stall");
        req.timeoutMs = 30000;              

        CHECK(c.Request(req, &w) == BC_R_SUCCESS);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        
        
        
        
        
        
        
    }

    
    
    srv.Stop();                 
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    printf("  连接器已析构、残留连接已收尾，进程存活 —— 回归通过\n");
}















static void case_y_control_chars_in_reason()
{
    printf("[case Y] reason-phrase 里的控制字符（N-2 回归）\n");

    
    const char* kResp =
        "HTTP/1.1 200 \x1b[31mPWNED\x1b[0m\x07\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "ok";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/evil");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.GotResponse());

    HttpResponse r = w.Resp();
    CHECK(r.status == 200);
    CHECK_EQ_STR(r.body, "ok");
    
    CHECK_EQ_STR(r.reason, "\x1b[31mPWNED\x1b[0m\x07");
    CHECK(r.reason.size() == 15);
    CHECK(r.reason.find('\x1b') != std::string::npos);
    CHECK(r.reason.find('\x07') != std::string::npos);
    printf("  reason 长度=%zu，含 ESC=%d 含 BEL=%d（展示层负责过滤）\n",
           r.reason.size(),
           r.reason.find('\x1b') != std::string::npos ? 1 : 0,
           r.reason.find('\x07') != std::string::npos ? 1 : 0);
    srv.Stop();
}













static void case_z_control_chars_in_header_value_rejected()
{
    printf("[case Z] header value 里的控制字符必须被拒（N-2 边界）\n");

    const char* kResp =
        "HTTP/1.1 200 OK\r\n"
        "X-Evil: \x1b[2J-cleared\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "ok";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/evilhdr");
    req.timeoutMs = 5000;

    Waiter w;
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(!w.GotResponse());
    CHECK(w.Result() == BC_R_UNEXPECTEDTOKEN);
    CHECK(Contains(w.Message(), "HPE_INVALID_HEADER_TOKEN"));
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}



















static void case_aa_drain_giveup_stress()
{
    printf("[case AA] drainTimeoutMs=0 放弃路径压测（200 轮 × 8 并发，需 ASan）\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_INSTANT_CLOSE, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    const int kRounds   = 200;
    const int kPerRound = 8;

    for (int round = 0; round < kRounds; round++)
    {
        
        HttpConnector* c  = new HttpConnector();
        ConnHandler*   ch = new ConnHandler();

        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 0);    
        if (c->Create(&cfg, ch) != BC_R_SUCCESS)
        {
            printf("  FAIL Create 失败\n"); g_failures++;
            delete c; delete ch; srv.Stop(); return;
        }

        std::vector<Waiter*> waiters;
        for (int i = 0; i < kPerRound; i++)
        {
            Waiter* w = new Waiter();
            waiters.push_back(w);

            HttpRequest req;
            req.url       = UrlFor(srv.Port(), "/stress");
            req.timeoutMs = 30000;
            c->Request(req, w);     
        }

        
        std::this_thread::sleep_for(
            std::chrono::microseconds((round * 4) % 800));

        delete c;       
        
        
        
        delete ch;
        for (size_t i = 0; i < waiters.size(); i++)
        {
            delete waiters[i];
        }
    }

    
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    srv.Stop();
    printf("  %d 轮 × %d 并发跑完，进程存活 —— 回归通过\n", kRounds, kPerRound);
}















static void case_ab_no_request_callback_after_giveup()
{
    printf("[case AB] 放弃之后不得再回调请求级 handler（问题二回归，需 ASan）\n");

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_STALL, ""))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    Waiter* w = new Waiter();
    {
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 0);

        ConnHandler   ch;
        HttpConnector c;
        CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

        HttpRequest req;
        req.url       = UrlFor(srv.Port(), "/stall");
        req.timeoutMs = 30000;
        CHECK(c.Request(req, w) == BC_R_SUCCESS);

        
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(w->Calls() == 0);     
        
    }

    
    delete w;

    
    
    
    srv.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    printf("  handler 已释放、残留连接已收尾，进程存活 —— 回归通过\n");
}



















class SuicidalHandler : public IHttpRequestHandler
{
public:
    HttpConnector*    conn = NULL;
    std::atomic<long> ms{-1};

    void OnHttpResponse(const HttpResponse&) override            { _Kill(); }
    void OnHttpError(BCRESULT, const std::string&) override      { _Kill(); }

private:
    void _Kill()
    {
        uint64_t t0 = NowMs();
        delete conn;            
        conn = NULL;
        ms.store((long)(NowMs() - t0));
    }
};

static void case_ac_destroy_in_callback(uint32_t drainMs)
{
    printf("[case AC] 回调栈内析构 connector，drainTimeoutMs=%u\n", drainMs);

    const char* kResp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "ok";

    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler     ch;
    SuicidalHandler h;
    h.conn = new HttpConnector();

    BCFObject cfg;
    cfg.PutInt("drainTimeoutMs", (uint64_t)drainMs);
    CHECK(h.conn->Create(&cfg, &ch) == BC_R_SUCCESS);

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/suicide");
    req.timeoutMs = 5000;
    if (h.conn->Request(req, &h) != BC_R_SUCCESS)
    {
        printf("  FAIL Request 失败\n"); g_failures++;
        delete h.conn; srv.Stop(); return;
    }

    
    for (int i = 0; i < 120 && h.ms.load() < 0; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    long ms = h.ms.load();
    CHECK(ms >= 0);                 
    
    CHECK(ms >= 0 && ms < 1000);
    
    CHECK(Contains(ch.Logs(), "违约用法"));
    printf("  析构耗时=%ld ms，日志含违约告警=%d\n", ms,
           Contains(ch.Logs(), "违约用法") ? 1 : 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    srv.Stop();
}

static void case_ac_destroy_in_callback_all()
{
    case_ac_destroy_in_callback(0);
    case_ac_destroy_in_callback(2000);
    case_ac_destroy_in_callback(30000);
}
















class RequestFromLogHandler : public IHttpConnectorHandler
                           , public IHttpRequestHandler
{
public:
    
    void OnHttpResponse(const HttpResponse&) override       {}
    void OnHttpError(BCRESULT, const std::string&) override {}

    HttpConnector*    conn = NULL;
    std::string       url;
    std::atomic<bool> armed{false};
    std::atomic<bool> tried{false};
    std::atomic<int>  result{-1};

    void OnLog(int, LPCSTR) override
    {
        if (!armed.load())
        {
            return;
        }
        bool expected = false;
        if (!tried.compare_exchange_strong(expected, true))
        {
            return;
        }
        HttpRequest req;
        req.url       = url;
        req.timeoutMs = 3000;
        
        result.store((int)conn->Request(req, this));
    }
    void OnClosed() override {}
};

static void case_ad_request_from_onlog()
{
    printf("[case AD] 在 OnLog 里调 Request()（probe4，看门狗 15s）\n");

    const char* kResp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    RequestFromLogHandler h;
    HttpConnector         c;
    BCFObject cfg;
    cfg.PutInt("logLevel", 4);      
    CHECK(c.Create(&cfg, &h) == BC_R_SUCCESS);
    h.conn = &c;
    h.url  = UrlFor(srv.Port(), "/from-log");

    Waiter w;
    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/first");
    req.timeoutMs = 3000;
    h.armed.store(true);            
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);

    uint64_t t0 = NowMs();
    while (!h.tried.load() && NowMs() - t0 < 15000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK(h.tried.load());          
    
    CHECK(h.result.load() == (int)BC_R_NOTIMPLEMENTED);
    printf("  OnLog 内 Request() 返回=%d（期望 %d=BC_R_NOTIMPLEMENTED）耗时=%llums\n",
           h.result.load(), (int)BC_R_NOTIMPLEMENTED,
           (unsigned long long)(NowMs() - t0));

    w.Wait(5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    srv.Stop();
}























class NestedRequestHandler : public IHttpRequestHandler
{
public:
    HttpConnector*    conn = NULL;
    std::string       url;
    std::atomic<int>  depth{0};
    std::atomic<bool> done{false};

    void OnHttpResponse(const HttpResponse&) override        { _Nest(); }
    void OnHttpError(BCRESULT, const std::string&) override  { _Nest(); }

private:
    void _Nest()
    {
        if (depth.fetch_add(1) == 0)
        {
            
            HttpRequest req;
            req.url       = url;
            req.timeoutMs = 4000;
            conn->Request(req, this);
            return;
        }
        done.store(true);
    }
};

static void case_ae_nested_request_no_log_under_lock()
{
    printf("[case AE] 回调里嵌套 Request()：持锁不得打日志（probe5 的不变量版）\n");

    const char* kResp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    FakeHttpServer srv;
    FakeHttpServer srv2;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp)
        || !srv2.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    ConnHandler   ch;
    HttpConnector c;
    BCFObject cfg;
    
    
    cfg.PutInt("logLevel", 4);
    CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

    NestedRequestHandler h;
    h.conn = &c;
    h.url  = UrlFor(srv2.Port(), "/second");

    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/first");
    req.timeoutMs = 4000;
    CHECK(c.Request(req, &h) == BC_R_SUCCESS);

    uint64_t t0 = NowMs();
    while (!h.done.load() && NowMs() - t0 < 15000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(h.done.load());               
    CHECK(NowMs() - t0 < 10000);
    printf("  嵌套请求完成=%d 耗时=%llums（HTTP_LOGQ 断言未触发）\n",
           h.done.load() ? 1 : 0, (unsigned long long)(NowMs() - t0));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    srv.Stop();
    srv2.Stop();
}














class CloseSuicideHandler : public IHttpConnectorHandler
{
public:
    HttpConnector*    conn = NULL;
    std::atomic<bool> done{false};

    void OnLog(int, LPCSTR) override {}
    void OnClosed() override
    {
        delete conn;            
        conn = NULL;
        done.store(true);
    }
};

static void case_af_destroy_in_onclosed()
{
    printf("[case AF] 在 OnClosed 里析构 connector\n");

    const char* kResp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    FakeHttpServer srv;
    if (!srv.Start(FakeHttpServer::MODE_CANNED, kResp))
    {
        printf("  FAIL 起假服务端失败\n"); g_failures++; return;
    }

    CloseSuicideHandler h;
    h.conn = new HttpConnector();
    CHECK(h.conn->Create(NULL, &h) == BC_R_SUCCESS);

    Waiter w;
    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/x");
    req.timeoutMs = 5000;
    CHECK(h.conn->Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));

    
    if (h.conn)
    {
        h.conn->Close();
    }

    uint64_t t0 = NowMs();
    while (!h.done.load() && NowMs() - t0 < 10000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(h.done.load());
    printf("  OnClosed 内析构完成=%d，进程存活\n", h.done.load() ? 1 : 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    srv.Stop();
}







static void case_n_tls_verify(uint16_t port, const char* certPath,
                              const char* path)
{
    printf("[case N] TLS 证书校验（caCerts 为空 → 拒绝；给 CA → 放行）\n");

    char url[192];
    snprintf(url, sizeof(url), "https://localhost:%u%s", (unsigned)port, path);

    
    {
        ConnHandler   ch;
        HttpConnector c;
        CHECK(c.Create(NULL, &ch) == BC_R_SUCCESS);
        CHECK(c.TlsSettings().caCertsPem.empty());

        HttpRequest req;
        req.url       = url;
        req.timeoutMs = 8000;

        Waiter w;
        CHECK(c.Request(req, &w) == BC_R_SUCCESS);
        CHECK(w.Wait(12000));
        CHECK(w.Calls() == 1);
        CHECK(w.GotError());            
        CHECK(!w.GotResponse());
        printf("  无 CA：result=%d msg=%s\n", (int)w.Result(),
               w.Message().c_str());
    }

    
    {
        std::string pem;
        if (!ReadFileAll(certPath, pem))
        {
            printf("  FAIL 读不到证书 %s\n", certPath);
            g_failures++;
            return;
        }
        BCFObject cfg;
        cfg.PutString("caCerts", pem.c_str());

        ConnHandler   ch;
        HttpConnector c;
        CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

        HttpRequest req;
        req.url       = url;
        req.timeoutMs = 8000;

        Waiter w;
        CHECK(c.Request(req, &w) == BC_R_SUCCESS);
        CHECK(w.Wait(12000));
        CHECK(w.Calls() == 1);
        CHECK(w.GotResponse());
        CHECK(!w.GotError());
        CHECK(w.Resp().status == 200);
        CHECK(!w.Resp().body.empty());
        printf("  有 CA：status=%d body=%zu peer=%s\n", w.Resp().status,
               w.Resp().body.size(), w.Resp().peerIp.c_str());
    }
}





int main(int argc, char** argv)
{
    
    setvbuf(stdout, NULL, _IONBF, 0);

    
    
    
    signal(SIGPIPE, SIG_IGN);

    uint16_t    tlsPort  = (argc > 1) ? (uint16_t)atoi(argv[1]) : 0;
    const char* certPath = (argc > 2) ? argv[2] : "certs/localhost.crt";
    const char* tlsPath  = (argc > 3) ? argv[3] : "/localhost.crt";

    BCFObject runtimeCfg;
    runtimeCfg.PutInt("workerThreads", 1);
    runtimeCfg.PutInt("taskThreads", 4);
    runtimeCfg.PutInt("timerThreads", 2);
    if (Runtime::Initialize(&runtimeCfg) != BC_R_SUCCESS)
    {
        printf("Runtime::Initialize 失败\n");
        return 1;
    }

    
    
    const char* only = getenv("TT_CASE");
#define RUN(fn)                                                                \
    do {                                                                       \
        if (!only || !*only || strstr(#fn, only)) { fn(); }                    \
    } while (0)

    RUN(case_a_plain_ok);
    RUN(case_b_chunked);
    RUN(case_c_split_delivery);
    RUN(case_d_max_response_bytes);
    RUN(case_e_no_redirect_follow);
    RUN(case_f_truncated_body);
    RUN(case_g_response_timeout);
    RUN(case_h_no_reply);
    RUN(case_i_connect_refused);
    RUN(case_j_close_with_inflight);
    RUN(case_k_parallel_requests);
    RUN(case_l_post_body);
    RUN(case_m_malformed_response);
    RUN(case_o_destroy_parser_in_eof_state);
    RUN(case_p_eof_body_clean_close);
    RUN(case_q_header_flood);
    RUN(case_r_reason_flood);
    RUN(case_s_single_huge_header);
    RUN(case_t_interim_1xx);
    RUN(case_u_head_request);
    RUN(case_v_304_with_content_length);
    RUN(case_w_reject_request_as_response);
    RUN(case_x_drain_giveup_no_uaf);
    RUN(case_y_control_chars_in_reason);
    RUN(case_z_control_chars_in_header_value_rejected);
    RUN(case_aa_drain_giveup_stress);
    RUN(case_ab_no_request_callback_after_giveup);
    RUN(case_ac_destroy_in_callback_all);
    RUN(case_ad_request_from_onlog);
    RUN(case_ae_nested_request_no_log_under_lock);
    RUN(case_af_destroy_in_onclosed);

    if (only && *only && !strstr("case_n_tls_verify", only))
    {
        
    }
    else if (tlsPort != 0 && PortIsOpen(tlsPort))
    {
        case_n_tls_verify(tlsPort, certPath, tlsPath);
    }
    else
    {
        printf("[case N] SKIP：没给可用的本地 HTTPS 端口（起法见文件头注释）\n");
        g_skipped++;
    }

    Runtime::Destroy();

    if (g_failures == 0 && g_skipped == 0)
    {
        printf("HttpConnector_integration_test: ALL PASSED\n");
        return 0;
    }
    if (g_failures == 0)
    {
        printf("HttpConnector_integration_test: PASSED（%d 个用例被跳过）\n",
               g_skipped);
        return 2;
    }
    printf("HttpConnector_integration_test: %d FAILURE(S)\n", g_failures);
    return 1;
}




