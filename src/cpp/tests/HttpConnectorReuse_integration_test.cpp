



#include "HttpConnector.h"
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
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)





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
    void OnLog(int, LPCSTR) override {}
    void OnClosed() override
    {
        std::unique_lock<std::mutex> lk(m_);
        closed_ = true;
        cv_.notify_all();
    }
    bool WaitClosed(int ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [this] { return closed_; });
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    closed_ = false;
};





class KeepAliveServer
{
public:
    
    
    int  replies_before_drop = 0;
    
    
    int  drop_delay_us       = 0;
    
    bool announce_close      = false;

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
        if (listen(listen_fd_, 8) != 0) return false;

        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd_, (struct sockaddr*)&addr, &len) != 0) return false;
        port_ = ntohs(addr.sin_port);

        accept_thread_ = std::thread([this] { _AcceptLoop(); });
        return true;
    }

    void Stop()
    {
        stop_ = true;
        if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : conn_threads_) if (t.joinable()) t.join();
    }

    uint16_t Port()        const { return port_; }
    
    int      Accepts()     const { return accepts_.load(); }
    int      RequestsSeen() const { return requests_.load(); }
    
    bool     SawConnectionClose() const { return saw_close_.load(); }
    
    int      ClosedByPeer() const { return closed_by_peer_.load(); }

private:
    void _AcceptLoop()
    {
        while (!stop_)
        {
            int lfd = listen_fd_;
            if (lfd < 0) break;
            int fd = accept(lfd, NULL, NULL);
            if (fd < 0) break;
            accepts_++;
            conn_threads_.emplace_back([this, fd] { _Serve(fd); });
        }
    }

    void _Serve(int fd)
    {
        std::string pending;
        int         served = 0;
        char        buf[1024];

        while (!stop_)
        {
            struct timeval tv = { 0, 100 * 1000 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n == 0) { closed_by_peer_++; break; }   
            if (n < 0) continue;        
            pending.append(buf, (size_t)n);

            
            size_t end;
            while ((end = pending.find("\r\n\r\n")) != std::string::npos)
            {
                std::string head = pending.substr(0, end);
                if (head.find("Connection: close") != std::string::npos
                    || head.find("connection: close") != std::string::npos)
                {
                    saw_close_ = true;
                }
                pending.erase(0, end + 4);
                requests_++;
                served++;

                std::string body = "hi";
                std::string resp = "HTTP/1.1 200 OK\r\n";
                resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                if (announce_close) resp += "Connection: close\r\n";
                resp += "\r\n";
                resp += body;
                (void)!send(fd, resp.data(), resp.size(), 0);

                if (announce_close ||
                    (replies_before_drop > 0 && served >= replies_before_drop))
                {
                    if (drop_delay_us > 0) usleep((useconds_t)drop_delay_us);
                    close(fd);
                    return;
                }
            }
        }
        close(fd);
    }

    int                      listen_fd_ = -1;
    uint16_t                 port_      = 0;
    std::atomic<int>         accepts_{0};
    std::atomic<int>         requests_{0};
    std::atomic<bool>        saw_close_{false};
    std::atomic<int>         closed_by_peer_{0};
    std::atomic<bool>        stop_{false};
    std::thread              accept_thread_;
    std::vector<std::thread> conn_threads_;
};






static bool CreateConnector(HttpConnector& c, ConnHandler& h,
                            int maxIdle = -1, int idleTimeoutMs = -1)
{
    BCFObject cfg;
    cfg.PutString("vpnPolicy", "os");
    cfg.PutInt("logLevel", 1);
    if (maxIdle >= 0)       cfg.PutInt("maxIdleConnections", maxIdle);
    if (idleTimeoutMs >= 0) cfg.PutInt("idleTimeoutMs", idleTimeoutMs);
    return c.Create(&cfg, &h) == BC_R_SUCCESS;
}

static HttpRequest MakeReq(uint16_t port)
{
    HttpRequest req;
    req.url       = "http://127.0.0.1:" + std::to_string(port) + "/";
    req.method    = "GET";
    req.timeoutMs = 5000;
    return req;
}


static bool DoRequest(HttpConnector& c, uint16_t port)
{
    Waiter      w;
    std::string err;
    if (c.Request(MakeReq(port), &w, &err) != BC_R_SUCCESS)
    {
        printf("    Request 同步失败：%s\n", err.c_str());
        return false;
    }
    if (!w.Wait(8000))
    {
        printf("    请求没有收尾\n");
        return false;
    }
    if (!w.GotResponse())
    {
        printf("    请求失败 result=%d %s\n", (int)w.Result(), w.Message().c_str());
        return false;
    }
    return w.Resp().status == 200;
}






static void case_a_second_request_reuses_the_connection()
{
    printf("Case A: 第二条请求复用同一条 TCP 连接\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h));

    CHECK(DoRequest(c, srv.Port()));
    CHECK(DoRequest(c, srv.Port()));

    CHECK(srv.RequestsSeen() == 2);
    
    CHECK(srv.Accepts() == 1);

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}



static void case_b_connection_close_is_honoured()
{
    printf("Case B: 服务端声明 Connection: close -> 不复用\n");

    KeepAliveServer srv;
    srv.announce_close = true;
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h));

    CHECK(DoRequest(c, srv.Port()));
    CHECK(DoRequest(c, srv.Port()));

    CHECK(srv.RequestsSeen() == 2);
    CHECK(srv.Accepts() == 2);

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}



static void case_c_dead_pooled_connection_is_replaced()
{
    printf("Case C: 池里的连接被对端关掉 -> 第二条请求照样成功\n");

    KeepAliveServer srv;
    srv.replies_before_drop = 1;   
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h));

    CHECK(DoRequest(c, srv.Port()));
    
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(DoRequest(c, srv.Port()));

    CHECK(srv.RequestsSeen() == 2);
    CHECK(srv.Accepts() == 2);

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}



static void case_d_close_drains_the_pool()
{
    printf("Case D: Close() 把池里的空闲连接一并收走\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h));

    CHECK(DoRequest(c, srv.Port()));
    CHECK(srv.Accepts() == 1);

    c.Close();
    
    CHECK(h.WaitClosed(5000));

    srv.Stop();
}




static void case_e_pool_can_be_turned_off()
{
    printf("Case E: maxIdleConnections=0 -> 不复用，且发 Connection: close\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h, 0));

    CHECK(DoRequest(c, srv.Port()));
    CHECK(DoRequest(c, srv.Port()));

    CHECK(srv.RequestsSeen() == 2);
    CHECK(srv.Accepts() == 2);
    CHECK(srv.SawConnectionClose());

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}




static void case_f_idle_connection_times_out()
{
    printf("Case F: 空闲连接超过 idleTimeoutMs 之后自行关闭\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h, -1, 300));   

    CHECK(DoRequest(c, srv.Port()));
    CHECK(srv.Accepts() == 1);
    
    CHECK(srv.ClosedByPeer() == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    CHECK(srv.ClosedByPeer() == 1);

    
    CHECK(DoRequest(c, srv.Port()));
    CHECK(srv.Accepts() == 2);

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}



static void case_g_idle_timer_restarts_on_each_return()
{
    printf("Case G: 每次归还都重新计时，热连接不会被掐\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h, -1, 400));

    
    for (int i = 0; i < 4; i++)
    {
        CHECK(DoRequest(c, srv.Port()));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    CHECK(srv.RequestsSeen() == 4);
    CHECK(srv.Accepts() == 1);
    CHECK(srv.ClosedByPeer() == 0);

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}





static void case_h_default_idle_timeout_is_60_minutes()
{
    printf("Case H: idleTimeoutMs 默认 60 分钟\n");

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h));

    CHECK(c.IdleTimeoutMs() == 60u * 60u * 1000u);

    c.Close();
    CHECK(h.WaitClosed(5000));
}









static void case_i_destroy_while_pool_is_not_empty()
{
    printf("Case I: 池里还有连接时析构连接器（压测，须在 ASan 下跑）\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    
    
    
    
    
    const int kRounds  = 40;
    const int kPreFill = 4;   
    const int kInFlight = 4;  
    for (int i = 0; i < kRounds; i++)
    {
        
        
        
        ConnHandler         h;
        std::vector<Waiter> waiters((size_t)kInFlight);
        HttpConnector       c;
        if (!CreateConnector(c, h)) { CHECK(false); break; }

        for (int k = 0; k < kPreFill; k++)
        {
            if (!DoRequest(c, srv.Port())) { CHECK(false); break; }
        }

        for (int k = 0; k < kInFlight; k++)
        {
            std::string err;
            c.Request(MakeReq(srv.Port()), &waiters[(size_t)k], &err);
        }
        
        
        
    }

    printf("      %d 轮析构跑完，进程存活\n", kRounds);
    srv.Stop();
}












static void case_j_pooled_connection_dies_while_being_adopted()
{
    printf("Case J: 连接在被接手的那一瞬被对端关掉（压测，须在 ASan 下跑）\n");

    KeepAliveServer srv;
    srv.replies_before_drop = 1;
    CHECK(srv.Start());

    ConnHandler   h;
    HttpConnector c;
    CHECK(CreateConnector(c, h));

    const int kRounds = 400;
    int ok = 0, failed = 0;
    for (int i = 0; i < kRounds; i++)
    {
        
        srv.drop_delay_us = (i * 37) % 400;

        if (!DoRequest(c, srv.Port())) { CHECK(false); break; }

        Waiter      w;
        std::string err;
        if (c.Request(MakeReq(srv.Port()), &w, &err) != BC_R_SUCCESS)
        {
            failed++;
            continue;
        }
        if (!w.Wait(8000))
        {
            printf("    第 %d 轮请求没有收尾（回调丢失）\n", i);
            CHECK(false);
            break;
        }
        CHECK(w.Calls() == 1);
        if (w.GotResponse()) ok++; else failed++;

        
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    printf("      %d 轮：成功 %d，干净失败 %d，进程存活\n", kRounds, ok, failed);
    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
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

    case_a_second_request_reuses_the_connection();
    case_b_connection_close_is_honoured();
    case_c_dead_pooled_connection_is_replaced();
    case_d_close_drains_the_pool();
    case_e_pool_can_be_turned_off();
    case_f_idle_connection_times_out();
    case_g_idle_timer_restarts_on_each_return();
    case_h_default_idle_timeout_is_60_minutes();
    case_i_destroy_while_pool_is_not_empty();
    case_j_pooled_connection_dies_while_being_adopted();

    if (g_failures == 0)
    {
        printf("HttpConnectorReuse_integration_test: all passed\n");
        return 0;
    }
    printf("HttpConnectorReuse_integration_test: %d failure(s)\n", g_failures);
    return 1;
}




