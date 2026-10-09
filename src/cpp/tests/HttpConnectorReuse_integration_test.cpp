///////////////////////////////////////////////////////////////////////////////
// file : HttpConnectorReuse_integration_test.cpp
//
// HttpConnector 连接复用（keep-alive 连接池）的集成测试。不进任何构建目标，
// 手动编译运行。POSIX only，全程明文 http://，不需要证书。
// 这里的每个用例都必须在 ASan 下干净通过。
//
// ---------------------------------------------------------------------------
// 为什么要有连接池
// ---------------------------------------------------------------------------
// HttpConnector 原本在每条请求头里硬写 Connection: close，一次请求一条连接，
// 于是每次都要重付 TCP 握手 + 完整 TLS 握手。实测同一个 https 端点：Node 的
// 默认 agent（keepAlive）第二个请求起 95ms，ttsignal 每次都是 280ms。
//
// ---------------------------------------------------------------------------
// 这几个用例守的是什么
// ---------------------------------------------------------------------------
// 复用出错的方式都很隐蔽——不是当场崩，而是把一条已经半死或者报文边界对不齐
// 的连接交给下一个请求，换来偶发、无从复现的失败。所以除了"真的复用了"，
// 还必须钉死三种不该复用的情形：服务端说了 Connection: close、服务端在空闲期
// 单方面关掉连接、以及连接器自己关停时池子要清干净。
//
// ---------------------------------------------------------------------------
// 怎么跑（macOS arm64）
// ---------------------------------------------------------------------------
//   cd /Users/antonio/chative/ttsignal
//
//   # llhttp 是纯 C，必须单独按 C 编，理由见 HttpConnector_integration_test.cpp
//   for f in api http llhttp; do \
//       cc -c -g -fsanitize=address -I deps/llhttp/include \
//          deps/llhttp/src/$f.c -o /tmp/llhttp_$f.o || break; done
//
//   c++ -std=c++17 -g -Wall -fsanitize=address -fno-omit-frame-pointer \
//       -I src/cpp -I deps/env/src -I deps/jquic/include \
//       -I deps/boringssl/src/include -I deps/llhttp/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       src/cpp/TlsContext.cpp src/cpp/SSLayer.cpp src/cpp/Runtime.cpp \
//       src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//       src/cpp/TcpChannel.cpp src/cpp/Utils.cpp \
//       src/cpp/LLHTTPParser.cpp src/cpp/HttpConnector.cpp \
//       /tmp/llhttp_api.o /tmp/llhttp_http.o /tmp/llhttp_llhttp.o \
//       src/cpp/tests/HttpConnectorReuse_integration_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//       -o /tmp/httpreuse_it && /tmp/httpreuse_it
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Waiter / ConnHandler —— 与 HttpConnector_integration_test.cpp 同形
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// 假服务端：真正的 HTTP/1.1 keep-alive，一条连接上按请求逐条应答
///////////////////////////////////////////////////////////////////////////////

class KeepAliveServer
{
public:
    // 每条连接回完 replies_before_drop_ 条响应就把连接关掉，模拟服务端自己的
    // 空闲超时。0 表示不主动关。
    int  replies_before_drop = 0;
    // 每条响应都带上 Connection: close，明确告诉客户端别复用。
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
    // 建立过多少条 TCP 连接 —— 复用与否全看它
    int      Accepts()     const { return accepts_.load(); }
    int      RequestsSeen() const { return requests_.load(); }
    // 请求头里出现过 Connection: close
    bool     SawConnectionClose() const { return saw_close_.load(); }
    // 客户端主动关掉（我们读到 FIN）的连接数 —— 空闲超时生效与否全看它
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
            if (n == 0) { closed_by_peer_++; break; }   // 对端关了
            if (n < 0) continue;        // 收包超时，回头看一眼 stop_
            pending.append(buf, (size_t)n);

            // 用例里的请求都没有 body，请求头收全就算一条。
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

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

// 取值 < 0 表示不写这个配置项，走默认值。
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

// 发一条请求并等它收尾。返回是否拿到了 200。
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

///////////////////////////////////////////////////////////////////////////////
// 用例
///////////////////////////////////////////////////////////////////////////////

// 主干：同一个连接器连发两条请求，第二条必须捡上一条留下的连接。
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
    // 整个用例的意义就在这一行
    CHECK(srv.Accepts() == 1);

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}

// 服务端说了 Connection: close 就必须听话。继续往一条已经宣告要关的连接上发
// 下一条请求，等来的是 RST 或静默丢弃。
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

// keep-alive 的经典竞态：服务端有自己的空闲超时，我们手上那条池化连接随时
// 可能已经是死的。第二条请求必须自己重新建连，而不是把这个失败甩给调用方。
static void case_c_dead_pooled_connection_is_replaced()
{
    printf("Case C: 池里的连接被对端关掉 -> 第二条请求照样成功\n");

    KeepAliveServer srv;
    srv.replies_before_drop = 1;   // 回完第一条就关，但不声明 Connection: close
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h));

    CHECK(DoRequest(c, srv.Port()));
    // 给 FIN 一点时间走到我们这边，模拟"闲置了一会儿才发下一条"
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(DoRequest(c, srv.Port()));

    CHECK(srv.RequestsSeen() == 2);
    CHECK(srv.Accepts() == 2);

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}

// 池子里躺着的是真实的 socket。连接器关停时必须把它们一并关掉，否则 fd 泄漏，
// 而且 OnClosed 会因为"还有连接没收尾"迟迟发不出来。
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
    // 池里那条连接如果没被收走，这里等不到 OnClosed
    CHECK(h.WaitClosed(5000));

    srv.Stop();
}

// maxIdleConnections=0 是一键退回旧行为的开关。它得管两头：既不复用连接，也
// 要把 Connection: close 发出去 —— 只做前者的话，服务端那一侧还傻等着我们复用，
// 连接要挂到它自己的空闲超时才释放。
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

// 池里的连接不能无限期占着。服务端多半有自己的 keep-alive 空闲超时会替我们
// 收摊，但"永不主动关连接"的服务端是存在的 —— 对着那种服务端只发一次请求，
// 没有本地超时的话那条 socket 会一直挂到连接器关停。
static void case_f_idle_connection_times_out()
{
    printf("Case F: 空闲连接超过 idleTimeoutMs 之后自行关闭\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h, -1, 300));   // 300ms 空闲就收

    CHECK(DoRequest(c, srv.Port()));
    CHECK(srv.Accepts() == 1);
    // 刚归还，还在超时窗口里，连接必须还活着
    CHECK(srv.ClosedByPeer() == 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    CHECK(srv.ClosedByPeer() == 1);

    // 超时收走之后，下一条请求照常走新连接
    CHECK(DoRequest(c, srv.Port()));
    CHECK(srv.Accepts() == 2);

    c.Close();
    CHECK(h.WaitClosed(5000));
    srv.Stop();
}

// 超时计的是"空闲"时长，不是连接建立以来的总时长。每次归还都该重新开始算，
// 否则一条被持续复用的热连接会在用得正欢的时候被掐掉。
static void case_g_idle_timer_restarts_on_each_return()
{
    printf("Case G: 每次归还都重新计时，热连接不会被掐\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    HttpConnector c;
    ConnHandler   h;
    CHECK(CreateConnector(c, h, -1, 400));

    // 每隔 200ms 发一条：单次空闲都没到 400ms，连接应当一路活着
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

// 默认值是跨绑定层的契约（JS/Swift/Java 的同名配置都只是把它透传下来），
// 改动必须是有意为之。60 分钟的取法：它不是"多久算空闲"的最优解，而是一道
// 兜底 —— 正常情况下服务端的 keep-alive 超时（通常几十秒到几分钟）会先把
// 连接收走，这里只负责不让"永不主动关连接"的服务端把 fd 永久占住。
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

// 池化连接的销毁是异步的（PostTask 到 Runtime），而析构必须等它们全部回收完
// 才能返回 —— 否则调用方一释放 handler，那些还在路上的收尾动作就打在野指针上。
//
// ⚠️ 诚实说明这个用例的效力：它覆盖的是"析构时池里有连接、同时还有在途请求"
// 这条混合路径，在 ASan 下反复跑能暴露该路径上的悬垂与重复回调。但它**不是**
// idle_destroying 那个销账窗口的护栏 —— 实测把那段登记删掉，本用例照样跑过
// 5/5。真正能复现那个窗口的是 HttpConnector_integration_test（删掉登记后
// 8 次里崩 3 次，补回去 8 次全过）。别指望本用例替它把关。
static void case_i_destroy_while_pool_is_not_empty()
{
    printf("Case I: 池里还有连接时析构连接器（压测，须在 ASan 下跑）\n");

    KeepAliveServer srv;
    CHECK(srv.Start());

    // ⚠️ 要撑开那个窗口，析构时必须**同时**有在途请求和池化连接，而且让它们
    // 的收尾交错：在途请求销毁时会 notify 析构线程去重新检查排空谓词，如果
    // 恰好撞上"池化连接刚从 idle 摘掉、销毁任务还没跑"的那一瞬，谓词就会假性
    // 满足。光有池化连接不行（没人 notify，析构会一直等到销毁任务自己跑完再
    // 收场），光有在途请求更不行 —— 实测这两种写法撤掉修复也照样跑过。
    const int kRounds  = 40;
    const int kPreFill = 4;   // = max_idle_per_key 默认值，先把池囤满
    const int kInFlight = 4;  // 再压一批在途的，然后立刻析构
    for (int i = 0; i < kRounds; i++)
    {
        // ⚠️ 声明顺序就是契约：handler 们必须**先**声明，连接器**后**声明，
        // 这样析构时连接器先走、它们还活着。反过来写的话崩的是测试自己
        // （waiters 先析构，在途回调打在它们身上），与被测代码无关。
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
        // 刻意**不等**这些请求，也**不调** Close()：让 ~HttpConnector 自己去
        // 排空在途请求和池里那几条连接。析构只要提前返回一次，后续回调就踩在
        // 下一轮已经覆盖掉的栈对象上。
    }

    printf("      %d 轮析构跑完，进程存活\n", kRounds);
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////

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

    if (g_failures == 0)
    {
        printf("HttpConnectorReuse_integration_test: all passed\n");
        return 0;
    }
    printf("HttpConnectorReuse_integration_test: %d failure(s)\n", g_failures);
    return 1;
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
