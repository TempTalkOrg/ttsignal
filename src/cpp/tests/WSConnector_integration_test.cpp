///////////////////////////////////////////////////////////////////////////////
// file : WSConnector_integration_test.cpp
//
// WSConnector 的集成测试：真的起事件循环、真的建 TCP/TLS 连接、真的收发
// WebSocket 帧。不进任何构建目标，手动编译运行。POSIX only（用 socket/thread
// 起本地假服务端），Windows 上不适用。
//
// ---------------------------------------------------------------------------
// 为什么必须有这个文件
// ---------------------------------------------------------------------------
// WSConnector_test.cpp 那些纯函数用例一行网络代码都碰不到，覆盖不了真正会出事的
// 地方：llhttp 的增量喂料与 HPE_PAUSED_UPGRADE 之后的残留字节、握手超时定时器与
// 关闭流程的交错、"关闭握手与 Close() 并发"、以及"OnChannelClosed 之后什么时候
// delete TcpChannel 才安全"。前几轮的教训就是纯函数单测全绿、ASan 一跑就是
// UAF，所以这里的每个用例都必须在 ASan 下干净通过。
//
// ---------------------------------------------------------------------------
// ⚠️ 写新用例时的一条硬规则
// ---------------------------------------------------------------------------
// **任何读取假服务端计数器（ClientFrames / UnmaskedFrames / ClosesFromClient /
// PongsFromClient）的断言，都必须用 WaitFor 有界轮询，不能在客户端回调返回后
// 立即断言。** 客户端回调只说明客户端这一侧完成了，与"服务端线程已经 recv 到
// 并计过数"之间没有任何 happens-before。这条规则是被实测打出来的：case F 里
// 一行立即断言，30 轮挂 4 轮。细节见 WaitFor 上方的注释。
//
// 例外（可以直接读，因为有真正的 happens-before）：
//   * srv.Request() —— 服务端在**发 101 之前**于互斥量下写入，客户端要拿到
//     OnConnectResult(0) 必须先收到那个 101，因果链成立；
//   * WaitFor(PongsFromClient()>=1) 之后读 ClientPayloads() —— 服务端线程是先
//     push_back 再自增计数的，同线程程序序 + 互斥量保证可见。
//
// ⚠️ **不要用固定 sleep 代替有界轮询**：那只压低概率，不消除竞态。
//
// ⚠️ 本文件自带一个 **TLS 假服务端**（BoringSSL 的 TLS_server_method + 手写
// WebSocket 握手），所以 wss:// 用例不需要任何外部进程，默认就会跑。它用
// certs/localhost.crt（SAN 里有 DNS:localhost 与 IP:127.0.0.1，有效期到 2036），
// 客户端把这张证书当 CA 传进去，并用 **wss://localhost:<port>** 连 —— 于是
// 域名解析（DnsResolver）+ TLS 校验（TlsContext）+ WS 握手三件事一起被覆盖。
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
//       src/cpp/SMPacket.cpp src/cpp/SMPParser.cpp \
//       src/cpp/LLHTTPParser.cpp src/cpp/HttpConnector.cpp \
//       src/cpp/WSParser.cpp src/cpp/WSConnector.cpp \
//       /tmp/llhttp_api.o /tmp/llhttp_http.o /tmp/llhttp_llhttp.o \
//       src/cpp/tests/WSConnector_integration_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//       -o /tmp/wsconnector_it
//
//   /tmp/wsconnector_it                 # 全部用例（含 wss）
//   TT_CASE=case_m /tmp/wsconnector_it  # 只跑名字里含 case_m 的用例
//
// TSan 版：把两处 -fsanitize=address 换成 -fsanitize=thread，产物换个名字。
//
// 退出码：0 = 全过，1 = 有失败，2 = 有用例被跳过。
//
// （Linux 把 apple/AppleRouteLookup.cpp 换成 linux/LinuxRouteLookup.cpp，
//   库路径换成 deps/*/lib/Linux/x86_64/Debug/*.a。）
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// 帧工具（与 WSConnector_test.cpp 里的同名函数一致；两个文件都是 standalone 的，
// 不共享任何东西）
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Waiter —— 一条连接的回调记录
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// CtorHandler —— 连接器级回调
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// FakeWSServer —— 自建假 WebSocket 服务端
//
// 沿用 TcpChannel_integration_test.cpp / HttpConnector_integration_test.cpp 里的
// FakeServer 模式：内核选端口、单独一条线程 accept、Stop() 收摊，不依赖任何外部
// 进程。tls=true 时用 BoringSSL 的 TLS_server_method + certs/localhost.crt。
///////////////////////////////////////////////////////////////////////////////

class FakeWSServer
{
public:
    enum Mode {
        // 握手成功后回声：TEXT/BINARY 原样回发，PING 回 PONG，CLOSE 回 CLOSE
        MODE_ECHO        = 0,
        // 握手直接回 403
        MODE_REJECT      = 1,
        // 回 101 但不带 Sec-WebSocket-Accept
        MODE_NO_ACCEPT   = 2,
        // 回 101 但 Sec-WebSocket-Accept 是错的
        MODE_BAD_ACCEPT  = 3,
        // 读完握手请求就装死，一个字节都不回（压握手超时）
        MODE_NO_REPLY    = 4,
        // 握手后发一条分片消息，中间插一个 ping
        MODE_FRAGMENTS   = 5,
        // 握手后发一个超过 maxFrameBytes 的帧
        MODE_OVERSIZE    = 6,
        // 握手后立刻发 close(1001)，随后照常收发（对端发起关闭握手）
        MODE_CLOSE_FIRST = 7,
        // 101 与第一帧**粘在同一次 send 里**（压 HPE_PAUSED_UPGRADE 之后的残留字节）
        MODE_PIGGYBACK   = 8,
        // 握手后立刻发 ping，等客户端回 pong
        MODE_PING        = 9,
        // 握手后立刻 close(1000) 并断开，用来撞 "Close() 与对端关闭并发"
        MODE_CLOSE_RACE  = 10,
        // 握手后发一个 **payload 长度为 1** 的 close 帧。RFC 6455 5.5.1 规定
        // close 帧的 body 必须是 0 或 >= 2 字节，长度 1 是畸形帧。
        // 客户端必须判协议违规（回 1002），而不是当成"没带关闭码"的正常关闭。
        MODE_CLOSE_BAD_LEN = 11,
        // 握手后发一个 **body 为空** 的 close 帧（合法形态之一，RFC 6455 5.5.1）
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
        addr.sin_port        = 0;      // 让内核选端口，免得撞车
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
    // 客户端回的 close 帧里的关闭码；-1 表示还没收到（或 body 不足 2 字节）
    int  ClientCloseCode()   { return close_code_.load(); }
    int  Accepted()          { return accepted_.load(); }
    std::vector<std::string> ClientPayloads()
    {
        std::unique_lock<std::mutex> lk(m_);
        return client_payloads_;
    }

private:
    ///////////////////////////////////////////////////////////////////////
    // 明文 / TLS 两用的 IO
    ///////////////////////////////////////////////////////////////////////
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

        // 3 秒收超时，免得测试卡死
        struct timeval tv = { 3, 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // 读到握手请求头结束
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

        // 正常 101。刻意写成 "keep-alive, Upgrade" 的 token 列表形式 ——
        // jmp 原版对 Connection 头做全等比较，这么回它就永远握不上手。
        std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: keep-alive, Upgrade\r\n"
                           "Sec-WebSocket-Accept: "
                         + WSParser::WSAcceptKey(key) + "\r\n"
                           "X-Server-Note: fake\r\n\r\n";

        if (mode_ == MODE_PIGGYBACK)
        {
            // 101 与第一帧粘在同一次 send 里
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
            io.SendAll(MakeFrame(true, 0x9, "hb"));          // 插一个 ping
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            io.SendAll(MakeFrame(true, 0x0, "fragmented world"));
        }
        else if (mode_ == MODE_OVERSIZE)
        {
            // 只发帧头：长度字段声明 4 MiB。客户端的上限检查必须发生在分配之前，
            // 所以它应该在这里就判协议错误，而不是等 payload 真的来。
            std::string f;
            f.push_back((char)0x82);            // FIN + BINARY
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
            // body 只有 1 字节 —— 非法
            io.SendAll(MakeFrame(true, 0x8, std::string(1, (char)0x03)));
        }
        else if (mode_ == MODE_CLOSE_EMPTY)
        {
            // body 为空 —— 合法，表示"没有关闭码"
            io.SendAll(MakeFrame(true, 0x8, std::string()));
        }
        else if (mode_ == MODE_PING)
        {
            io.SendAll(MakeFrame(true, 0x9, "srv-ping"));
        }
        else if (mode_ == MODE_CLOSE_RACE)
        {
            io.SendAll(MakeCloseFrame(1000));
            // 立刻断开：客户端可能正好在另一条线程上调 Close()
            _Finish(io);
            return;
        }

        _Loop(io);
    }

    // 收客户端的帧并按 mode 反应
    void _Loop(IO& io)
    {
        std::string acc;
        char        buf[4096];
        while (!stop_)
        {
            ssize_t n = io.Recv(buf, sizeof(buf));
            if (n <= 0) break;
            acc.append(buf, (size_t)n);

            // 尽可能多地解出完整帧
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
                    // RFC 6455 5.1：客户端发出的每一帧都必须掩码
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
                    closes_++;   // ⚠️ 必须排在 close_code_ 之后：测试是先等
                                 // closes_ 再读 close_code_ 的
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
                    // 回声：不掩码（服务端必须不掩码）
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

    // 从缓冲里取一帧。返回 false 表示还不够。
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
        // 头名大小写无关
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
    // ⚠️ 必须是 atomic：Stop()（主线程）与 _Run()（服务端线程）之间没有别的
    // 同步手段，TSan 会在这个字段上报竞态。
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

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// WaitFor —— 有界轮询等待一个条件成立
//
// ⚠️ 为什么服务端侧的断言必须用它，不能写完就断言：
//
// 客户端的回调（OnClosed / OnRecvText / ...）只说明**客户端这一侧**走完了，
// 它与"服务端线程已经 recv 到那些字节并计过数"之间**没有任何 happens-before**。
// 尤其是关闭路径：客户端发出 close 帧之后紧接着就关通道，OnClosed 随即派发，
// 而服务端线程可能还阻塞在 recv 里没被调度到。
//
// 实测（修复前，仓库根目录连跑 case F 30 轮）：4 轮在
//     CHECK(srv.ClosesFromClient() >= 1)
// 上失败，约 1/7。带探针复现确认 close 帧确实发出去了（channel_->Send 返回 0、
// 8 字节、TcpChannel 的 lambda 看到 state==READY），纯粹是测试自己抢跑。
//
// ⚠️ **不要用固定 sleep 代替**：那只是把失败概率压低，竞态还在，机器一忙就又
// 冒出来 —— 而且下次再挂时人们会以为是被测代码的问题。有界轮询才是真的消除：
// 条件成立就立刻返回（快路径几乎不耗时），不成立就在上限内一直等。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// case A —— ws:// happy path：握手 + 文本/二进制回声 + 主动关闭
///////////////////////////////////////////////////////////////////////////////

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
    // 握手响应头交给了业务，键统一小写
    CHECK_EQ_STR(w.Headers()["x-server-note"], "fake");
    CHECK(!w.Headers()["sec-websocket-accept"].empty());
    CHECK_EQ_STR(conn->PeerIp(), "127.0.0.1");

    // 服务端实际收到的握手请求
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

    // 空文本帧也要能收发（零长度 payload 那条 UB 的真实路径版）
    CHECK(conn->SendText("") == BC_R_SUCCESS);
    CHECK(w.WaitTexts(2, 5000));
    CHECK_EQ_STR(w.Texts()[1], "");

    // SendPacket：SMPacket 走 binary 帧
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

    // ⚠️ 服务端侧的计数要等它真的读到（见 WaitFor 顶部的说明）。close 帧是
    // 客户端发出的最后一帧，所以一旦它被计到，前面 4 帧必然也已计入 ——
    // 服务端是单线程按序解帧的，且 client_frames_ / unmasked_frames_ 都在
    // closes_ 之前自增。
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK(srv.ClientFrames() >= 5);
    // ⚠️ 客户端发出的每一帧都必须掩码，否则合规服务端会以 1002 关连接。
    // 这是"缺失性"断言，必须排在上面那个正向等待之后 —— 提前读只会读到一个
    // 还没看过任何帧的 0，看着像通过，其实什么都没验证。
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);

    c.Close();
    CHECK(ch.WaitClosed(8000));
    CHECK_EQ_INT(ch.ClosedCalls(), 1);
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// case B —— 握手失败的三种形态：非 101 / 缺 accept / accept 值错
//
// 契约：失败时只发 OnConnectResult，**不发 OnClosed**。
///////////////////////////////////////////////////////////////////////////////

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
        // 发送必须被拒（还没 upgrade）
        CHECK(conn->SendText("x") == BC_R_NOTCONNECTED);
        // 等一会儿确认真的不会补一个 OnClosed
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK_EQ_INT(w.ClosedCalls(), 0);
        printf("  %s：result=%d，未发 OnClosed\n", cases[i].name,
               (int)w.ConnectResult());
        srv.Stop();
    }
}

///////////////////////////////////////////////////////////////////////////////
// case C —— 服务端不回握手响应 -> 握手超时
//
// TcpChannel 的 connectTimeoutMs 进 READY 就取消了，这里压的是 WSConnection
// 自己补的那个 hs_timer_。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// case D —— 分片消息 + 中间插的 ping/pong
///////////////////////////////////////////////////////////////////////////////

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

    // 分片必须重组成一条完整消息（原版会把前半截丢掉）
    CHECK(w.WaitTexts(1, 5000));
    CHECK_EQ_STR(w.Texts()[0], "Hello, fragmented world");
    CHECK_EQ_INT(w.Texts().size(), 1);

    // 中间那个 ping 必须被自动回 pong（payload 原样回显）
    CHECK(WaitFor([&] { return srv.PongsFromClient() >= 1; }));
    // ⚠️ 观察到 pongs_ 就一定能看到对应的 payload：服务端线程是先在互斥量下
    // push_back 再自增 pongs_ 的，两者在同一条线程上有程序序。
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
    // 同 case A：等服务端真的读到 close 帧，再断言"一帧都没漏掩码"
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// case E —— 超大帧：长度字段声明 4 MiB，maxFrameBytes 设 64 KiB
//
// 上限检查必须发生在分配之前，否则这一条就是 OOM 而不是"协议错误"。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// case F —— 对端发起关闭握手（close 帧）
///////////////////////////////////////////////////////////////////////////////

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
    // 客户端必须回一个 close 帧（关闭握手），而且要掩码。
    //
    // ⚠️ 这里**必须**有界轮询：w.WaitClosed() 只表示客户端自己收尾完了，
    // 服务端线程可能还没 recv 到那个 close 帧。修复前这一行是立即断言，
    // 30 轮里挂 4 轮（见 WaitFor 顶部的记录）。
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// case G —— 101 与第一帧粘在同一次 send 里
//
// llhttp 在 101 之后以 HPE_PAUSED_UPGRADE 停下，停的位置之后全是 WS 帧字节，
// 必须接着喂给 ParseWSFrame。漏了这一段的症状是"第一条消息神秘丢失"。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// case H —— 服务端主动 ping，客户端必须回 pong
///////////////////////////////////////////////////////////////////////////////

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
    // 可见性依据同 case D
    std::vector<std::string> payloads = srv.ClientPayloads();
    bool echoed = false;
    for (size_t i = 0; i < payloads.size(); i++)
    {
        if (payloads[i] == "srv-ping") echoed = true;
    }
    CHECK(echoed);

    // 客户端主动 ping：服务端收到后会回 pong。等服务端真的收到那一帧再往下走，
    // 否则 Close() 可能抢在它前面，这条断言就退化成"只要不崩就算过"。
    const int framesBeforePing = srv.ClientFrames();
    CHECK(conn->SendPing() == BC_R_SUCCESS);
    CHECK(WaitFor([&] { return srv.ClientFrames() > framesBeforePing; }));

    conn->Close();
    CHECK(w.WaitClosed(8000));
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// case I —— 关闭握手与 Close() 并发
//
// TcpChannel 那条"Close 与对端 FIN 同时发生"的常规场景。服务端在握手后立刻
// close(1000) 并断开，测试同时从另一条线程调 Close()。要求：OnClosed 恰好一次，
// 不崩、不 UAF。跑 20 轮把窗口撑开。
///////////////////////////////////////////////////////////////////////////////

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

        // 不等 OnConnectResult：让 Close() 和握手/对端关闭在任意点交错。
        // 延迟随轮次变化，把整个窗口（连接中 / 握手中 / 刚 upgrade / 已收到
        // 对端 close 帧）都扫一遍。
        const int delayMs = round % 5;
        std::thread killer([conn, delayMs] {
            if (delayMs > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
            conn->Close();
        });
        killer.join();

        // 无论怎么交错，回调总数都必须是"恰好一次 connect 结果"
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

///////////////////////////////////////////////////////////////////////////////
// case J —— 连不上（端口没人听）
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// case K —— 参数校验：非法 URL / 重复 Connect / 未 Create
///////////////////////////////////////////////////////////////////////////////

static void case_k_argument_validation()
{
    printf("[case K] 参数校验\n");

    // 没 Create 就 CreateConnection
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
            // 非 ws/wss scheme、缺 host：返回错误码且**不产生任何回调**
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
            // 握手前可以加头，Connect 之后不行
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

///////////////////////////////////////////////////////////////////////////////
// case L —— 空闲超时
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// case M —— wss:// + **域名**（wss://localhost:<port>）
//
// 一次覆盖三件事：DnsResolver 解析域名、TlsContext 校验证书链 + hostname、
// WS 握手。证书是 certs/localhost.crt（SAN 含 DNS:localhost），当 CA 传进去。
//
// M-1 不给 CA：系统信任库不认这张自签证书，必须失败（不能静默放行）。
// M-2 给 CA：必须成功并能回声。
///////////////////////////////////////////////////////////////////////////////

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

    // M-1：不给 CA，必须失败
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
            CHECK(w.ConnectResult() != BC_R_SUCCESS);   // 核心断言
            printf("  无 CA：result=%d（拒绝自签证书）\n", (int)w.ConnectResult());
        }
        srv.Stop();
    }

    // M-2：给 CA，必须成功
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
            // Host 头必须是域名而不是解析出来的 IP（否则虚拟主机会返回错内容）
            CHECK(Contains(srv.Request(), "Host: localhost:"));
            CHECK(Contains(srv.Request(), "GET /secure HTTP/1.1"));

            CHECK(conn->SendText("over tls") == BC_R_SUCCESS);
            CHECK(w.WaitTexts(1, 8000));
            CHECK_EQ_STR(w.Texts()[0], "over tls");
            // 域名解析出来的一定是 127.0.0.1
            CHECK_EQ_STR(conn->PeerIp(), "127.0.0.1");
            printf("  有 CA：握手成功，回声正常，peer=%s\n",
                   conn->PeerIp().c_str());

            conn->Close();
            CHECK(w.WaitClosed(8000));
            CHECK_EQ_INT(w.ClosedCalls(), 1);
            // 同 case A：先等服务端读到 close 帧，缺失性断言才有意义
            CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
            CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
        }
        srv.Stop();
    }
}

///////////////////////////////////////////////////////////////////////////////
// case N —— 连接器 Close() / 析构：在途连接、从没 Connect 的连接
//
// "从没 Connect 的连接"是个专门的坑：它的 TcpChannel 没建过事件队列，永远不会
// 有 OnChannelClosed 来把它摘掉。若不特殊处理，析构会白等满 drainTimeoutMs。
///////////////////////////////////////////////////////////////////////////////

static void case_n_close_and_drain()
{
    printf("[case N] Close() 与析构排空（含从没 Connect 的连接）\n");

    // N-1：有在途连接时 Close()
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
        // 幂等
        c.Close();
        CHECK_EQ_INT(ch.ClosedCalls(), 1);
        srv.Stop();
    }

    // N-2：从没 Connect 的连接不能把析构拖满 drainTimeoutMs
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
            // 故意不 Connect，直接让连接器离开作用域
        }
        const uint64_t elapsed = (uint64_t)time(NULL) - t0;
        CHECK(elapsed < 5);
        CHECK_EQ_INT(ch.ClosedCalls(), 1);
        printf("  从没 Connect 的连接：析构耗时 %llus\n",
               (unsigned long long)elapsed);
    }

    // N-3：Connect 之后立刻析构连接器（不等任何回调）
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
        // 析构返回之后不允许再有任何回调，这里睡一会儿让残留任务跑完
        const int connectCalls = w.ConnectCalls();
        const int closedCalls  = w.ClosedCalls();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK_EQ_INT(w.ConnectCalls(), connectCalls);
        CHECK_EQ_INT(w.ClosedCalls(), closedCalls);
        srv.Stop();
    }
}

///////////////////////////////////////////////////////////////////////////////
// case O —— 业务在回调里发送 / 关闭（回调必须在锁外派发）
//
// 回调若在内部锁里发出，这一组会立刻死锁或自锁。
///////////////////////////////////////////////////////////////////////////////

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
            // 在回调里直接发送 —— 契约允许
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
            // 在回调里关闭 —— 契约允许
            conn->Close();
        }
        cv_.notify_all();
    }
    void OnRecvData(LPCVOID, size_t) override {}
    void OnClosed(LPCSTR) override
    {
        std::unique_lock<std::mutex> lk(m_);
        closed_++;
        // 在回调里丢掉自己那份 WSConnPtr —— 契约明确允许
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
        conn.reset();        // 只留 handler 里那一份
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

///////////////////////////////////////////////////////////////////////////////
// case P —— 畸形 close 帧（body 长度为 1）必须判协议违规并回 1002
//
// RFC 6455 5.5.1：close 帧的 body 要么为空，要么 >= 2 字节。长度 1 是畸形帧。
// review 抓出来的漏检：修复前它会被当成"没带关闭码"，一路交付上去、按 1000
// 正常关闭处理 —— 一个畸形帧被报成了干净收尾。
//
// 本用例断言的是**对端能观察到的行为**：客户端回的必须是 1002 Protocol Error。
///////////////////////////////////////////////////////////////////////////////

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

    // 必须报协议违规（OnException），而不是安安静静地当成正常关闭
    CHECK(w.WaitException(5000));
    CHECK(w.ExceptionCalls() >= 1);
    CHECK(Contains(w.ExceptionMsg(), "close frame body"));

    CHECK(w.WaitClosed(8000));
    CHECK_EQ_INT(w.ClosedCalls(), 1);
    // 关闭原因不能是"对端发起关闭握手（code=1000）"那一套
    CHECK(!Contains(w.CloseReason(), "1000"));
    printf("  exception: %s\n  close: %s\n", w.ExceptionMsg().c_str(),
           w.CloseReason().c_str());

    // ⚠️ 核心断言：对端观察到的关闭码必须是 1002 Protocol Error
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.ClientCloseCode(), 1002);
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// case Q —— 合法 close 帧的两个边界：body 为空 / body >= 2
//
// 与 case P 配套：确认修 case P 的那道校验没有把合法形态一起判掉。
// （body >= 2 的路径 case F 已经覆盖过 code=1001，这里补 body 为空。）
///////////////////////////////////////////////////////////////////////////////

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
    // 空 body 没有关闭码，按 1000 正常关闭处理，且**不该**报协议违规
    CHECK_EQ_INT(w.ExceptionCalls(), 0);
    CHECK(Contains(w.CloseReason(), "1000"));
    CHECK(WaitFor([&] { return srv.ClosesFromClient() >= 1; }));
    CHECK_EQ_INT(srv.ClientCloseCode(), 1000);
    CHECK_EQ_INT(srv.UnmaskedFrames(), 0);
    srv.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

typedef void (*CaseFn)();

int main(int argc, char** argv)
{
    // 不缓冲：ASan 报错走 stderr（无缓冲），stdout 一缓冲就看不出崩在哪个用例上
    setvbuf(stdout, NULL, _IONBF, 0);
    (void)argc;
    (void)argv;

    // 对端在我们写之前关连接是常态，别让 SIGPIPE 杀掉进程
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

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
