///////////////////////////////////////////////////////////////////////////////
// file : HttpConnector_integration_test.cpp
//
// HttpConnector 的集成测试：真的起事件循环、真的建 TCP 连接、真的收发 HTTP
// 报文。不进任何构建目标，手动编译运行。POSIX only（用 socket/thread 起本地
// 假服务端），Windows 上不适用。
//
// ---------------------------------------------------------------------------
// 为什么必须有这个文件
// ---------------------------------------------------------------------------
// HttpConnector_test.cpp 那些纯函数用例一行网络代码都碰不到，覆盖不了真正会
// 出事的地方：llhttp 的增量喂料、EOF 收尾、maxResponseBytes 截断时从回调里
// 返回 -1、超时定时器与关闭流程的交错、以及"OnChannelClosed 之后什么时候
// delete 才安全"。Task 5 的教训就是纯函数单测全绿、ASan 一跑就是 UAF，所以
// 这里的每个用例都必须在 ASan 下干净通过。
//
// ---------------------------------------------------------------------------
// 怎么跑（macOS arm64；Linux 见末尾说明）
// ---------------------------------------------------------------------------
//
//   cd /Users/antonio/chative/ttsignal
//
//   # llhttp 是纯 C，必须单独按 C 编（跟着 -std=c++17 走会因为 int->enum 的
//   # 隐式转换编不过）
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
//       src/cpp/tests/HttpConnector_integration_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//       -o /tmp/httpconnector_it
//
//   /tmp/httpconnector_it                       # 只跑自建假服务端的用例
//   /tmp/httpconnector_it 18445 certs/localhost.crt   # 额外跑 TLS 用例
//
// TLS 用例需要一个本地 HTTPS 服务端（要求会发 Content-Length，openssl s_server
// -www 不满足，它靠关连接断句且不发 close_notify）。最省事的起法：
//
//   python3 - <<'EOF'
//   import http.server, ssl
//   d = http.server.HTTPServer(('127.0.0.1', 18445),
//                              http.server.SimpleHTTPRequestHandler)
//   c = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
//   c.load_cert_chain('certs/localhost.crt', 'certs/localhost.key')
//   d.socket = c.wrap_socket(d.socket, server_side=True)
//   d.serve_forever()
//   EOF
//
//   注意第三个参数：case N 请求的是 https://localhost:<port><argv[3]>，默认
//   /localhost.crt。上面那个服务端的工作目录是仓库根，所以证书的实际路径是
//   /certs/localhost.crt —— 用默认值会拿到 404 并让 case N 以
//   "status != 200" 失败。完整调用：
//
//     /tmp/httpconnector_it 18445 certs/localhost.crt /certs/localhost.crt
//
// ⚠️ 想覆盖 vpnPolicy 的网卡绑定路径，必须额外带上 path monitor：
//
//     -DTT_HAS_PATH_MONITOR src/cpp/apple/AppleNetworkMonitor.mm \
//     -framework Foundation -framework Network -fobjc-arc
//
//   不带它时 TcpChannel::_PickPhysicalIfIndex() 整段被条件编译掉、恒返回 0，
//   于是 bound_ifindex_ 永远是 0、**一次绑定都不会发生**。本测试的对端全是
//   127.0.0.1，而默认档 prefer-physical 会把 socket 绑到物理网卡——环回从物理
//   网卡出不去。也就是说"不带 path monitor"这个构建配置，恰好把这条最容易踩的
//   缺陷完整地藏了起来：带上它，修复前本文件有 90 条失败；不带，全绿。
//   这就是它值得作为一个独立构建变体常跑的原因。
//
// 退出码：0 = 全过，1 = 有失败，2 = TLS 用例被跳过。
//
// （Linux 把 apple/AppleRouteLookup.cpp 换成 linux/LinuxRouteLookup.cpp，
//   库路径换成 deps/*/lib/Linux/x86_64/Debug/*.a。）
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Waiter —— 记录一次请求的结果
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

///////////////////////////////////////////////////////////////////////////////
// ConnHandler —— 收 OnLog / OnClosed
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// FakeHttpServer —— 自建假服务端，只服务一条连接
//
// 沿用 TcpChannel_integration_test.cpp 里的 FakeServer 模式：内核选端口、
// 单独一条线程 accept、Stop() 收摊，不依赖任何外部进程。
///////////////////////////////////////////////////////////////////////////////

class FakeHttpServer
{
public:
    enum Mode {
        // 读完请求头，把 response_ 一次性写出去再关闭
        MODE_CANNED  = 0,
        // 同上，但把 response_ 切成小片、每片之间睡一会儿，逼 llhttp 走增量喂料
        MODE_SPLIT   = 1,
        // 只回半截响应头就再也不动，用来触发响应超时
        MODE_STALL   = 2,
        // 读完请求就直接关连接，一个字节都不回
        MODE_NOREPLY = 3,
        // 连请求都不读，accept 之后立刻 close。用于压测：让连接尽快收尾，
        // 把"析构 与 delete 任务"的交错窗口撑到必现。
        MODE_INSTANT_CLOSE = 9,
        // 发一个"报文体以连接关闭为终止"的响应（响应头完整，既没有
        // Content-Length 也不是 chunked），发完 body 的一部分就装死不关连接。
        // 这会把 llhttp 稳稳地停在 HTTP_FINISH_SAFE_WITH_CB 状态上 ——
        // case O 的析构回归就靠它，别改这里的报文形状。
        MODE_EOF_BODY_STALL = 4,
        // 先发 response_，睡 200ms，再发 response2_，然后关闭。
        // 用来构造"1xx 中间响应 + 真正的最终响应"这种两段式报文 —— 中间那段
        // 延迟是关键：客户端如果把 1xx 当最终响应就会当场拆连接，第二段根本
        // 送不进来。
        MODE_TWO_STAGE = 5,
        // 发完响应头之后无限重复 "X-Pad-N: AAA...\r\n"，直到对端断开或 Stop()。
        // 压响应头总大小的上限。
        MODE_HEADER_FLOOD = 6,
        // 发 "HTTP/1.1 200 " 之后无限发 'R'，连 CR 都不发 —— 全部进
        // reason-phrase。压 status line 的上限。
        MODE_REASON_FLOOD = 7,
        // 把 response_ 发出去之后**保持连接不关**，一直挂到 Stop()。
        // 模拟 keep-alive 服务端。case U/V 必须用它：HEAD / 204 / 304 在
        // "服务端不关连接"下的症状是**一路挂到 timeoutMs**，比干净关连接
        // （立即报"响应不完整"）隐蔽得多，也更接近真实 CDN 的行为。
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
        addr.sin_port        = 0;      // 让内核选端口，免得撞车
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
            // 压测模式：不停地 accept 然后立刻 close，服务多条连接
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

        // 读到请求头结束为止（本测试从不发带 body 的请求给它）
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
            const size_t kChunk = 7;    // 故意切得很碎
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
            // 单条 4000 字节：低于单条上限（16 KiB），于是压的是**总大小**上限
            // （64 KiB，约 16 条之后触发）。单条上限另由 case S 覆盖。
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
            // 注意：**没有** Content-Length，**不是** chunked。llhttp 因此进入
            // "body 读到 EOF 为止"的状态并把 finish 置为 HTTP_FINISH_SAFE_WITH_CB。
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
        // MODE_NOREPLY 什么都不发，直接走到下面的 close

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
    // ⚠️ 必须是 atomic：Stop()（主线程）与 _Run()（服务端线程）之间没有别的
    // 同步手段，TSan 会在这个字段上报竞态。stop_ 本来就是 atomic，这个漏了。
    std::atomic<int>  listen_fd_{-1};
    uint16_t          port_      = 0;
    std::atomic<bool> stop_{false};
    std::thread       thread_;
    std::mutex        m_;
    std::string       request_;
};

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case A —— 明文 happy path：请求组装 + Content-Length 响应
///////////////////////////////////////////////////////////////////////////////

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
    CHECK(w.Calls() == 1);              // 恰好一次回调
    CHECK(w.GotResponse());
    CHECK(!w.GotError());

    HttpResponse r = w.Resp();
    CHECK(r.status == 200);
    // reason-phrase 真的暴露出来了（它有独立上限，见 case R）
    CHECK_EQ_STR(r.reason, "OK");
    CHECK_EQ_STR(r.body, "hello world");
    CHECK_EQ_STR(r.headers["content-type"], "text/plain");
    CHECK_EQ_STR(r.headers["content-length"], "11");
    // 同名头合并
    CHECK_EQ_STR(r.headers["x-multi"], "a, b");
    // 诊断三件套：明文 + policy=os，peerIp 必须有，绑定信息为空
    CHECK_EQ_STR(r.peerIp, "127.0.0.1");
    CHECK(r.pinMethod.empty());

    // 服务端实际收到的请求报文
    std::string got = srv.Request();
    CHECK(Contains(got, "GET /api/x?a=1 HTTP/1.1\r\n"));
    CHECK(Contains(got, "Host: 127.0.0.1:"));
    CHECK(Contains(got, "User-Agent: ttsignal/1.0\r\n"));
    // 连接池默认开着，请求头因此**不**发 Connection —— HTTP/1.1 默认就是
    // keep-alive，发 close 等于放弃复用。禁用池化（maxIdleConnections=0）时
    // 才会显式发 close，那条由 HttpConnectorReuse_integration_test 守着。
    CHECK(!Contains(got, "Connection:"));
    CHECK(Contains(got, "X-Trace-Id: abc123\r\n"));
    CHECK(!Contains(got, "Content-Length"));

    printf("  status=%d body=%zu peer=%s\n", r.status, r.body.size(),
           r.peerIp.c_str());
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case B —— chunked 响应
///////////////////////////////////////////////////////////////////////////////

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
    // 交给业务的必须是解码后的报文体，不能带 chunk 长度行
    CHECK_EQ_STR(w.Resp().body, "hello world");
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case C —— 响应被切成 7 字节一片：llhttp 增量喂料
//
// 头名、头值、chunk 长度行都会被切断在任意位置，专门压 http_on_header_field /
// http_on_header_value 的分片累积逻辑。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case D —— maxResponseBytes 截断
//
// 压的是"从 llhttp 回调里返回 -1 中断解析"这条路：返回值一路传回
// llhttp_execute，再由 OnChannelData 决定收尾。搞错的话要么把超限的 body 全
// 收下来（等于没有上限），要么把已经记好的失败原因覆盖成解析错误。
///////////////////////////////////////////////////////////////////////////////

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
    CHECK(!w.GotResponse());        // 绝不能把超限的 body 交上去
    CHECK(w.Result() == BC_R_RESPONSE_TOO_LARGE);
    CHECK(Contains(w.Message(), "maxResponseBytes"));
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case E —— 3xx 原样返回，不自动跟随
///////////////////////////////////////////////////////////////////////////////

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
    // Location 原样交给业务，由它自己决定要不要发第二次请求
    CHECK_EQ_STR(w.Resp().headers["location"],
                 "https://elsewhere.example.com/next");
    CHECK(w.Resp().body.empty());
    // 服务端只应该收到一次请求（跟随了的话这里会是第二条 URL）
    CHECK(Contains(srv.Request(), "GET /old HTTP/1.1"));
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case F —— 对端在响应体中途 FIN
//
// Content-Length 说还有 100 字节，实际只发了 4 个就关连接。必须报错，不能把
// 半截 body 当成成功交上去。
///////////////////////////////////////////////////////////////////////////////

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
    CHECK(w.GotError());            // 核心断言
    CHECK(!w.GotResponse());
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case G —— 响应超时
//
// TcpChannel 的 connectTimeoutMs 进 READY 就取消了，之后全靠 HttpConnection
// 自己挂在通道事件队列上的那个定时器。这个用例专门压它 —— 少了它，一个只回
// 半截响应头就装死的服务端能把调用方永远吊住。
///////////////////////////////////////////////////////////////////////////////

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
    req.timeoutMs = 1200;       // 短一点，别让测试等太久

    Waiter   w;
    uint64_t t0 = NowMs();
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);
    CHECK(w.Wait(8000));
    uint64_t t1 = NowMs();

    CHECK(w.Calls() == 1);
    CHECK(w.GotError());
    CHECK(w.Result() == BC_R_TIMEDOUT);
    // 超时必须大体按 timeoutMs 生效，不能提前也不能拖到天荒地老
    CHECK(t1 - t0 >= 1000);
    CHECK(t1 - t0 < 6000);
    printf("  result=%d 耗时=%llums msg=%s\n", (int)w.Result(),
           (unsigned long long)(t1 - t0), w.Message().c_str());
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case H —— 服务端一个字节都不回就 FIN
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case I —— 连不上（端口没人听）
///////////////////////////////////////////////////////////////////////////////

static void case_i_connect_refused()
{
    printf("[case I] 端口没人听\n");

    // 先起一个再立刻停掉，拿一个几乎肯定没人占的端口号
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

///////////////////////////////////////////////////////////////////////////////
// Case J —— Close() 与在途请求并发
//
// 压两件事：
//   1. Close() 会把在途请求收掉，业务能拿到 OnHttpError 而不是干等；
//   2. OnClosed 在所有 HttpConnection **真的析构完**之后才发，且只发一次。
// 顺带压 HttpConnector::Close() 里"持锁遍历 conns_ 调 Cancel()"那段 —— 早前
// 的写法是锁外遍历快照，快照里的指针可能已经被销毁，ASan 下就是 UAF。
///////////////////////////////////////////////////////////////////////////////

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
            req.timeoutMs = 30000;      // 故意设得很长：收尾必须靠 Close()

            Waiter w;
            CHECK(c.Request(req, &w) == BC_R_SUCCESS);
            // 让连接推进到不同阶段再关，把交错窗口撑开
            std::this_thread::sleep_for(std::chrono::milliseconds(i * 3));
            c.Close();

            CHECK(w.Wait(8000));
            CHECK(w.Calls() == 1);
            CHECK(w.GotError());
            CHECK(ch.WaitClosed(8000));
            // 关掉之后不再受理新请求
            CHECK(c.Request(req, &w) == BC_R_SHUTTINGDOWN);
            // Close() 幂等
            c.Close();
            // ~HttpConnector 在这里同步等所有 HttpConnection 析构完
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

///////////////////////////////////////////////////////////////////////////////
// Case K —— 多条请求并发跑在同一个 connector 上
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case L —— POST 带 body
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case M —— 畸形响应
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case O —— 【析构崩溃回归】parser 停在 EOF 断句状态时被销毁，不得中止进程
//
// 触发链：
//   1. 服务端发一个"报文体以连接关闭为终止"的响应（响应头完整，没有
//      Content-Length，也不是 chunked），发一半就装死；
//   2. llhttp 进入"body 读到 EOF 为止"的状态，把 llhttp_t::finish 置为
//      HTTP_FINISH_SAFE_WITH_CB；
//   3. 请求超时 → 走失败路径收尾。失败路径**刻意不**调 llhttp_finish()
//      （响应本来就不完整，不该补派一次 message_complete 把半截 body 当成功），
//      于是 parser 一直停在 SAFE_WITH_CB 且 error == 0；
//   4. HttpConnection 被销毁 → ~LLHTTPParser 跑。
//
// 修复前 ~LLHTTPParser 在这里无条件调 llhttp_finish(this)，llhttp 看到
// SAFE_WITH_CB 就补派发一次 on_message_complete；而基类析构跑在所有派生类
// 析构**之后**，虚函数只会落到 LLHTTPParser 自己的纯虚声明上，
// 直接 __cxa_pure_virtual → 进程中止（整个测试进程当场没）。
//
// 这个坑对 WSConnection 一样成立，所以修在基类而不是在派生类里各自绕开。
///////////////////////////////////////////////////////////////////////////////

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
        CHECK(!w.GotResponse());        // 半截 body 绝不能当成功交上去
        CHECK(w.Result() == BC_R_TIMEDOUT);
        printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());

        // ~HttpConnector 在这里同步等到 HttpConnection 真正析构完 ——
        // 也就是 ~LLHTTPParser 真的跑过一遍。能走到下面那行就说明没中止。
    }

    printf("  parser 已销毁，进程存活 —— 回归通过\n");
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case P —— EOF 断句 + 干净关闭：必须成功
//
// case O 的对照组，专门守住"把 llhttp_finish() 从 ~LLHTTPParser 里拿掉"这个
// 修复没有误伤正常路径：EOF 断句的响应本来就得靠显式 llhttp_finish() 才会派发
// message_complete，只是那一下必须发生在对象还活着的时候
// （HttpConnection::_DeliverOnce），而不是析构里。
//
// 少了 _DeliverOnce 里那次显式收尾，这个用例会退化成"连接正常关闭但响应不完整"
// 的失败 —— 也就是把一类完全正常的 HTTP/1.0 风格响应全判死。
///////////////////////////////////////////////////////////////////////////////

static void case_p_eof_body_clean_close()
{
    printf("[case P] EOF 断句 + 干净关闭 → 必须成功（case O 的对照组）\n");

    // 没有 Content-Length，不是 chunked：报文体读到连接关闭为止
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

///////////////////////////////////////////////////////////////////////////////
// Case Q —— 【C-1 回归】无限响应头流必须被上限截断，不能靠超时才停
//
// maxResponseBytes 只盖报文体，头部是另一条完全没设防的路：服务端不停地发
// "X-Pad-N: " + 60000 个 'A' + CRLF，修复前 RSS 会从 29 MB 一路涨到 1.4 GB，
// 只有 8 秒的请求超时才能停下来。
//
// 断言里特意压了"耗时远小于 timeoutMs"：如果哪天上限被改回去，症状就是这个
// 用例靠超时才结束，时间断言会先炸。
///////////////////////////////////////////////////////////////////////////////

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
    // 必须是上限截断的，不是超时熬出来的
    CHECK(w.Result() != BC_R_TIMEDOUT);
    CHECK(t1 - t0 < 5000);
    CHECK(Contains(w.Message(), "响应头总大小"));
    printf("  result=%d 耗时=%llums msg=%s\n", (int)w.Result(),
           (unsigned long long)(t1 - t0), w.Message().c_str());
    srv.Stop();

    // ---- 同一条上限的另一面：头**条数** ----
    // 500 条小头，字节总数远没到 64 KiB，但条数上限（200）必须先拦住。
    // 每条 header 在 std::map 里都是一次分配，光靠字节数管不住。
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

///////////////////////////////////////////////////////////////////////////////
// Case R —— 【C-1 回归】无限 status reason 必须被上限截断
//
// "HTTP/1.1 200 " 之后一直发 'R'，连 CR 都不发。修复前这些字节全进
// reason_phrase_，而那个成员当时既没有上限、也从来没人读 —— 一个纯粹的
// 可被远端无限增长的死变量（实测又吃掉 1 GB）。
// 现在它有上限，而且真的出现在 HttpResponse::reason 里（case A 会验）。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case S —— 【C-1 回归】单条超大响应头
//
// 一条 8 MB 的 "X-Big: AAAA..."。修复前即使 maxResponseBytes 设成 1024 也照收
// 不误（那个上限只管 body）。
///////////////////////////////////////////////////////////////////////////////

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
    CHECK(!w.GotResponse());        // 绝不能把 8 MB 的头交上去
    CHECK(w.Result() == BC_R_RESPONSE_TOO_LARGE);
    CHECK(Contains(w.Message(), "单条响应头"));
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case T —— 【I-1 回归】1xx 中间响应不得被当成最终响应
//
// 服务端先发 103 Early Hints，200ms 后才发真正的 200。修复前客户端把 103 当
// 最终响应交付（status=103 + 空 body + 判成功），连接当场拆掉，真响应被丢弃。
// 103 Early Hints 现在 Cloudflare / Fastly 都在发，100 Continue 更是标准行为。
//
// 中间那 200ms 延迟是关键：客户端要是拆了连接，第二段根本送不进来。
///////////////////////////////////////////////////////////////////////////////

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
    // 核心断言：交上来的必须是 200，不是 103
    CHECK(w.Resp().status == 200);
    CHECK_EQ_STR(w.Resp().body, "REAL-BODY");
    // ⚠️ Resp() 返回的是副本，必须先取一份再比 —— 直接写
    // w.Resp().headers.find(x) == w.Resp().headers.end() 是拿两个不同容器的
    // 迭代器互比，属于未定义行为，实测永远不相等。
    HttpResponse got = w.Resp();
    CHECK_EQ_STR(got.headers["content-type"], "text/plain");
    // 中间响应的头必须被丢干净，不能混进最终响应
    CHECK(got.headers.find("link") == got.headers.end());
    CHECK(got.headers.size() == 2);     // 只有 content-type + content-length
    printf("  status=%d body=%s\n", w.Resp().status, w.Resp().body.c_str());
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case U —— 【I-2 回归】HEAD 响应带 Content-Length 但没有 body
//
// llhttp 不知道请求方法，会老老实实等 Content-Length 声明的 42 字节。
// 修复前：服务端干净 FIN -> "响应不完整"报错；服务端 keep-alive 不关 ->
// 一路挂到 timeoutMs。而 --method HEAD 是 httpget 用法说明里承诺的能力。
//
// 用 MODE_CANNED_KEEPALIVE：服务端发完响应头就**保持连接不关**。这是 I-2
// 更隐蔽也更真实的那一面 —— 修复前不是立刻报错，而是一路挂到 timeoutMs
// （干净关连接的话会立刻报"响应不完整"，反而好发现）。
// 时间断言 t1-t0 < 2500（timeoutMs = 3000）就是用来钉死"不是熬到超时"的。
///////////////////////////////////////////////////////////////////////////////

static void case_u_head_request()
{
    printf("[case U] HEAD + Content-Length 且无 body、服务端 keep-alive（I-2 回归）\n");

    // 发完响应头就保持连接：修复前必然挂到超时
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
    CHECK(w.GotResponse());         // 核心断言
    CHECK(!w.GotError());
    CHECK(w.Resp().status == 200);
    CHECK(w.Resp().body.empty());   // HEAD 响应没有 body
    CHECK_EQ_STR(w.Resp().headers["content-length"], "42");
    // 必须是解析出来的，不是熬到超时的
    CHECK(t1 - t0 < 2500);
    printf("  status=%d 耗时=%llums content-length=%s\n", w.Resp().status,
           (unsigned long long)(t1 - t0),
           w.Resp().headers["content-length"].c_str());
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case V —— 【I-2 回归】304 Not Modified 回显 Content-Length
//
// RFC 7232 明确允许 304 带上 Content-Length（Apache 和多家 CDN 就这么做），
// 但 304 本身绝不带 body。和 HEAD 同源。204 同理。
// 同样用 keep-alive 服务端，理由见 case U。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case W —— 【I-3 回归】服务端发来 HTTP **请求**，客户端必须拒绝
//
// 修复前 LLHTTPParser 用 HTTP_BOTH 初始化（那是从 WS 侧原样搬来的，WS 握手
// 确实两种都要），于是服务端回一段 "GET /evil HTTP/1.1\r\n..." 会被当成一条
// 合法报文收下：OnHttpResponse(status=0, body="hello")，不报任何错。
// HTTP 客户端只应接受响应。
///////////////////////////////////////////////////////////////////////////////

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
    CHECK(w.GotError());            // 核心断言
    CHECK(!w.GotResponse());
    CHECK(w.Resp().status == 0);
    CHECK(w.Resp().body.empty());
    printf("  result=%d msg=%s\n", (int)w.Result(), w.Message().c_str());
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case X —— 【N-1 回归】析构放弃等待之后，迟到的回调不得碰已析构的连接器
//
// ~HttpConnector 等不到在途请求排空时会放弃等待。放弃之前必须把残留连接
// 回指连接器的指针掐断，否则它们稍后走到 OnChannelClosed 时会去调
// owner_->_OnConnectionFinished()，碰的是**已经析构**的 lock_ / conns_ ——
// ASan 下就是 heap-use-after-free（把死锁换成了 UAF，更糟）。
//
// 用 drainTimeoutMs = 0 来构造：不等，直接走放弃路径。真跑 30 秒默认值只是
// 把同一条代码路径拖慢 30 秒，没有额外覆盖，所以这里刻意把它做成可配置的。
// 服务端装死不回话，保证连接器析构时请求一定还在途。
//
// 断言只能是"不崩"——这个用例的价值全在 ASan 上，务必带 -fsanitize=address 跑。
///////////////////////////////////////////////////////////////////////////////

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
        cfg.PutInt("drainTimeoutMs", 0);    // 不等，直接走放弃路径

        ConnHandler   ch;
        HttpConnector c;
        CHECK(c.Create(&cfg, &ch) == BC_R_SUCCESS);

        HttpRequest req;
        req.url       = UrlFor(srv.Port(), "/stall");
        req.timeoutMs = 30000;              // 长到不可能自己先结束

        CHECK(c.Request(req, &w) == BC_R_SUCCESS);
        // 等连接真的建起来（进 READY 并把请求发出去），让残留状态更接近真实
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 这里析构：在途请求还没收尾，drainTimeoutMs=0 → 立刻放弃等待。
        // 修复前：残留连接稍后调 owner_->_OnConnectionFinished()，
        //         碰的是刚被销毁的 std::mutex / std::set → ASan UAF。
        // 修复后：Disown() 已经把 owner_ 掐断，那些调用全部变 no-op。
        //
        // ⚠️ 连接器析构之后 w / ch 也随时可能不再收到任何回调，这是放弃等待
        // 的既定语义（OnClosed 明确不再发），不做断言。
    }

    // 给残留连接足够时间走完 OnChannelClosed + 自销毁。ASan 会在这段时间里
    // 抓到任何越界访问。
    srv.Stop();                 // 让服务端撒手，残留连接立刻收到 FIN
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    printf("  连接器已析构、残留连接已收尾，进程存活 —— 回归通过\n");
}

///////////////////////////////////////////////////////////////////////////////
// Case Y —— 【N-2 回归】reason-phrase 里的控制字符
//
// llhttp 对 **reason-phrase** 不做字符过滤（对 header value 则会拒绝，见下面
// 第二段），所以远端可以往这里塞 ESC —— 也就是 ANSI 转义序列，能改颜色、
// 清屏、移光标，把终端里的输出伪造成完全不同的样子。
//
// 库这一层的正确行为是**如实交付原始字节**（不能悄悄改数据，那会让上层拿到
// 与实际收到的不一致的东西），净化是展示层的责任。所以：
//   * 这里钉死"库如实交付"；
//   * tools/httpget.cpp 的 PrintSanitized() 负责另一半（把 ESC 显示成 <1B>），
//     实测见 fix 报告里的 cat -v 对比。
///////////////////////////////////////////////////////////////////////////////

static void case_y_control_chars_in_reason()
{
    printf("[case Y] reason-phrase 里的控制字符（N-2 回归）\n");

    // ESC [ 3 1 m P W N E D ESC [ 0 m BEL = 15 字节
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
    // 核心断言：原始字节一个不少地交上来（ESC 与 BEL 都在）
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

///////////////////////////////////////////////////////////////////////////////
// Case Z —— header value 里的控制字符由 llhttp 直接拒绝
//
// 这条是 case Y 的边界确认：reason-phrase 能塞控制字符，**头值不能**。
// llhttp 的头值字符集是 >= 0x20 加 TAB，遇到 ESC / NUL 直接
// HPE_INVALID_HEADER_TOKEN。也就是说 ANSI 注入这条路只在 reason 上存在，
// 头值那边天然是关的。
//
// 钉住它是为了让"头值相对安全"这句话有依据 —— 万一哪天换了更宽松的解析器，
// 这个用例会先炸出来。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case AA —— 【放弃路径压测】drainTimeoutMs=0 下的 delete 任务竞态
//
// case X 只用一条连接、一轮，命中窗口全靠运气。这个用例按 review 给的方式压：
// 堆分配 connector + 每轮 8 条并发请求 + drainTimeoutMs=0 + 服务端 accept 即
// close + 析构延时 0..800us 扫描 + 多轮。
//
// 修复前必崩：OnChannelClosed 里那个负责 delete 的 PostTask lambda **按值捕获
// 了裸的 HttpConnector\***，绕过了所有"掐断"机制。drainTimeoutMs=0 时每次带在途
// 请求的析构都走放弃路径，delete 任务只要落后几微秒就命中已析构的连接器。
// 症状是 heap-use-after-free 或 "mutex lock failed: Invalid argument"。
//
// 修复后：连接器、每条连接、每个 delete 任务各持一份 HttpConnectorState 的
// shared_ptr，最后一个撒手时状态才析构，谁也碰不到已析构的东西。
//
// ⚠️ 必须带 -fsanitize=address 跑，这个用例的价值全在 ASan 上。
///////////////////////////////////////////////////////////////////////////////

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
        // 堆分配：让 connector 的存储真的被释放，ASan 才能标记出 UAF
        HttpConnector* c  = new HttpConnector();
        ConnHandler*   ch = new ConnHandler();

        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 0);    // 不等，每轮都走放弃路径
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
            c->Request(req, w);     // 失败无所谓，压的是收尾竞态
        }

        // 0..800us 扫描：把"析构"与"delete 任务"的交错窗口逐格推过去
        std::this_thread::sleep_for(
            std::chrono::microseconds((round * 4) % 800));

        delete c;       // 放弃路径
        // 连接器析构返回之后，调用方就可以释放自己的 handler 了 —— 这正是
        // "放弃之后不再回调任何 handler"这条契约的意义。立刻释放，把违约
        // 暴露给 ASan。
        delete ch;
        for (size_t i = 0; i < waiters.size(); i++)
        {
            delete waiters[i];
        }
    }

    // 给残留连接跑完自销毁的时间，ASan 在这段时间里抓越界
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    srv.Stop();
    printf("  %d 轮 × %d 并发跑完，进程存活 —— 回归通过\n", kRounds, kPerRound);
}

///////////////////////////////////////////////////////////////////////////////
// Case AB —— 【问题二回归】放弃之后不得再回调**请求级** handler
//
// 放弃路径把连接器级 handler 置空之后，调用方再也拿不到"全部收尾"的信号，
// 也就无从判断何时可以释放自己的 IHttpRequestHandler。契约因此必须是：
// **放弃之后任何 handler 都不再回调**，调用方在析构返回后即可释放。
//
// 修复前只掐了 owner_，残留连接稍后照样调 handler_->OnHttpError() ——
// 而那时 request handler 已经被调用方 delete 掉了（heap-use-after-free）。
//
// 这个用例是确定性的：服务端一直装死，连接必然还在途；drainTimeoutMs=0 保证
// 一定走放弃路径；handler 堆分配并在析构后立刻释放。
///////////////////////////////////////////////////////////////////////////////

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

        // 等连接真的建起来并把请求发出去
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(w->Calls() == 0);     // 此刻还什么都没回调
        // 析构 -> drainTimeoutMs=0 -> 放弃路径
    }

    // 契约：析构返回之后可以立刻释放 request handler
    delete w;

    // 让服务端撒手，残留连接立刻收到 FIN 并走 OnChannelClosed。
    // 修复前这里会去调已经 delete 掉的 w（ASan: heap-use-after-free
    // HttpConnector.cpp in HttpConnection::_DeliverOnce）。
    srv.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    printf("  handler 已释放、残留连接已收尾，进程存活 —— 回归通过\n");
}

///////////////////////////////////////////////////////////////////////////////
// Case AC —— 【回调栈内析构】必须快速返回，不得冻结整个连接器
//
// 契约 7(3)：**不要**在回调栈里析构连接器。本次回调所属的那条连接要等回调返回
// 之后才会收尾，所以析构根本等不到它 —— 白等满 drainTimeoutMs 之后照样走放弃
// 流程。于是本模块自检"本线程是否正在派发本连接器的回调"，命中就直接跳过等待，
// 并打一条含"违约用法"的 _ERROR_ 把现场记下来。
//
// （当初这条更严重：回调是持内部递归锁发出的，而 condition_variable_any 只
//  unlock 一次，整把锁根本放不开 —— 那是全连接器冻结。回调改到锁外派发之后
//  只剩"等不到"这一层，但白等满 drainTimeoutMs 仍然毫无意义。
//  修复前实测：drainTimeoutMs=0 -> 0ms；=2000 -> 2005ms；=30000 -> 10 秒
//  看门狗都等不到返回。ASan 干净，是纯活性问题。）
//
// 这里对三个有代表性的 drainTimeoutMs 都验一遍"必须快速返回 + 留下违约告警"。
///////////////////////////////////////////////////////////////////////////////

// 在回调栈里析构 connector 的违约用法
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
        delete conn;            // ← 违约：在回调栈里析构
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

    // 等回调跑完（最多 12 秒；修复前 drainTimeoutMs=30000 会在这里超时）
    for (int i = 0; i < 120 && h.ms.load() < 0; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    long ms = h.ms.load();
    CHECK(ms >= 0);                 // 必须返回了
    // 核心断言：不管 drainTimeoutMs 配多大，都必须**快速**返回而不是卡满超时
    CHECK(ms >= 0 && ms < 1000);
    // 违约现场必须留下痕迹，否则调用方无从知道自己写错了
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

///////////////////////////////////////////////////////////////////////////////
// Case AD —— 【probe4】在 OnLog 里调 Request()：必须立刻返回错误，不得挂死
//
// OnLog 是被 BCLogger 持着它的**全局非递归自旋锁**调进来的。发起请求的路上
// TcpChannel::Open() 一定会打日志，再进那把锁就是 100% CPU 空转、无任何诊断
// 的挂死（修复前实测如此）。
//
// 这条路本模块修不好 —— TcpChannel 的日志不在可改范围内，而 BCLogger 的锁是
// 非递归的。所以正确的产品行为不是"让它能跑"，而是**当场拒绝**：
// Request() 检测到自己在 OnLog 栈帧里就直接返回 BC_R_NOTIMPLEMENTED。
// 把无法诊断的挂死换成调用方看得见的错误码。
//
// 看门狗 15 秒兜底，卡住就报失败而不是把测试套件挂死。
///////////////////////////////////////////////////////////////////////////////

class RequestFromLogHandler : public IHttpConnectorHandler
                           , public IHttpRequestHandler
{
public:
    // 只为了给 Request() 一个合法的 handler，不会真的收到回调
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
        // ⚠️ 契约明令禁止的用法。这里刻意这么写，验证的是"被拒绝"而不是"能用"。
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
    cfg.PutInt("logLevel", 4);      // debug：确保日志真的打出来
    CHECK(c.Create(&cfg, &h) == BC_R_SUCCESS);
    h.conn = &c;
    h.url  = UrlFor(srv.Port(), "/from-log");

    Waiter w;
    HttpRequest req;
    req.url       = UrlFor(srv.Port(), "/first");
    req.timeoutMs = 3000;
    h.armed.store(true);            // 从现在起 OnLog 开始搅局
    CHECK(c.Request(req, &w) == BC_R_SUCCESS);

    uint64_t t0 = NowMs();
    while (!h.tried.load() && NowMs() - t0 < 15000)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK(h.tried.load());          // 核心断言：OnLog 里那次调用返回了，没挂死
    // 必须是"明确拒绝"，不是悄悄成功
    CHECK(h.result.load() == (int)BC_R_NOTIMPLEMENTED);
    printf("  OnLog 内 Request() 返回=%d（期望 %d=BC_R_NOTIMPLEMENTED）耗时=%llums\n",
           h.result.load(), (int)BC_R_NOTIMPLEMENTED,
           (unsigned long long)(NowMs() - t0));

    w.Wait(5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case AE —— 【probe5 的不变量版】回调里嵌套 Request()：持锁时不得打日志
//
// probe5 描述的两线程死锁是：
//   线程 A：OnHttpResponse（**持 state->lock**）-> Request() -> LogQ
//           -> 阻塞在 BCLogger 的全局自旋锁
//   线程 B：OnLog（**持 BCLogger 全局自旋锁**）  -> Close()
//           -> 阻塞在 state->lock
//
// 我没能把这个两线程交错做成稳定复现的用例（两半必须精确重叠，而一旦 OnLog
// 侧调了 Close()，A 侧的嵌套 Request 就会在打日志之前提前返回 SHUTTINGDOWN，
// 窗口自己就没了）。见 fix 报告里的说明。
//
// 所以这里改测**让那个死锁不可能成立的不变量**：线程 A 那一半的前提是
// "持 state->lock 时打日志"，而本模块的 HTTP_LOGQ 宏在持锁时会直接 assert
// 失败。回调里嵌套 Request() 正是最容易踩中它的路径（Request 与
// TcpChannel::Open 一路都在打日志），修复前回调持锁发出，这里必然触发断言；
// 修复后回调在锁外发出，Request 不在锁内，断言不响。
//
// 这比复现那个交错更强：它不依赖时序，且覆盖所有会打日志的重入路径。
///////////////////////////////////////////////////////////////////////////////

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
            // 契约 7(1) 明确祝福的用法：回调里直接再发一次请求
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
    // ⚠️ logLevel=debug 是关键：必须让 Request()/TcpChannel 的日志真的打出来，
    // 否则"持锁打日志"这条路根本走不到，断言也就无从触发。
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
    CHECK(h.done.load());               // 嵌套请求走完了，没卡死
    CHECK(NowMs() - t0 < 10000);
    printf("  嵌套请求完成=%d 耗时=%llums（HTTP_LOGQ 断言未触发）\n",
           h.done.load() ? 1 : 0, (unsigned long long)(NowMs() - t0));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    srv.Stop();
    srv2.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case AF —— 在 OnClosed 里析构 connector：不得在已释放的互斥量上 unlock
//
// _NotifyClosedIfDrained 早前收的是 const shared_ptr&，而调用方传的正是成员
// state_（自己不持强引用）。业务在 OnClosed 里 delete connector 时，
// ~HttpConnector 释放最后一份引用 -> HttpConnectorState 当场析构 ->
// 返回外层后 lock_guard 在**已释放的 recursive_mutex** 上 unlock()。
//
// ⚠️ ASan / TSan 都看不见这个：pthread_mutex_unlock 在未插桩的 libsystem 里。
// 所以本用例的价值不在"sanitizer 干净"，而在于它把这条路径真的跑一遍；
// 参数改成按值传之后，函数体内始终持有一份强引用。
///////////////////////////////////////////////////////////////////////////////

class CloseSuicideHandler : public IHttpConnectorHandler
{
public:
    HttpConnector*    conn = NULL;
    std::atomic<bool> done{false};

    void OnLog(int, LPCSTR) override {}
    void OnClosed() override
    {
        delete conn;            // ← 在 OnClosed 栈帧里析构
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

    // 请求收尾之后再 Close()，OnClosed 会在排空时发出
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

///////////////////////////////////////////////////////////////////////////////
// Case N —— TLS：caCerts 为空必须拒绝自签证书，给了 CA 才放行
//
// 这是"绝不静默放行"的核心回归。需要外部 HTTPS 服务端，见文件头的起法。
///////////////////////////////////////////////////////////////////////////////

static void case_n_tls_verify(uint16_t port, const char* certPath,
                              const char* path)
{
    printf("[case N] TLS 证书校验（caCerts 为空 → 拒绝；给 CA → 放行）\n");

    char url[192];
    snprintf(url, sizeof(url), "https://localhost:%u%s", (unsigned)port, path);

    // N-1：不给 CA。系统信任库不认这张自签证书，必须失败。
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
        CHECK(w.GotError());            // 核心断言：不能静默放行
        CHECK(!w.GotResponse());
        printf("  无 CA：result=%d msg=%s\n", (int)w.Result(),
               w.Message().c_str());
    }

    // N-2：给 CA，必须成功
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

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
    // 不缓冲：ASan 报错走 stderr（无缓冲），stdout 一缓冲就看不出崩在哪个用例上
    setvbuf(stdout, NULL, _IONBF, 0);

    // ⚠️ 必须忽略 SIGPIPE。case Q/R 的假服务端会往 socket 里持续灌数据，而被测
    // 代码撞到响应头上限之后会立刻关连接 —— 那一刻服务端线程的 send() 就会
    // 触发 SIGPIPE 把整个测试进程打死（实测 RC=141），看起来像是被测代码崩了。
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

    // 可选：环境变量 TT_CASE 只跑名字里含该子串的用例，便于单独复现某一条。
    //   TT_CASE=aa /tmp/httpconnector_it
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
        // 被 TT_CASE 过滤掉，不计入跳过
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

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
