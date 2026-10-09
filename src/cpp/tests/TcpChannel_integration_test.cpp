///////////////////////////////////////////////////////////////////////////////
// file : TcpChannel_integration_test.cpp
//
// TcpChannel 的集成测试：真的起事件循环、真的建 TCP 连接、真的跑 TLS 握手。
// 不进任何构建目标，手动编译运行。POSIX only（用了 socket/pthread 起本地
// 假服务端），Windows 上不适用。
//
// ---------------------------------------------------------------------------
// 为什么必须有这个文件
// ---------------------------------------------------------------------------
// TcpChannel_test.cpp 那 14 个纯函数用例一个都碰不到真实网络路径，于是漏掉了
// 一个 heap-use-after-free：SSLayer 的回调是从 SSLayer 自己的栈帧里发出来的，
// 而回调里的 _Fail() 会一路走到 _Cleanup() 去 ssl_layer_.reset() +
// SSL_free(ssl_)，销毁的正是脚下那个对象；返回后 SSLayer::ReadFromSSL 继续读
// ssl_state_、继续 SSL_read，就踩在已释放内存上。
//
// 这个洞在 TLS 1.3 下**不会**暴露：SSL_connect 返回前会写出 client Finished，
// pending_send_ == 1，_MaybeFinishClose() 因此被推迟，等回到事件循环才清理。
// 只有 TLS 1.2 完整握手的最后一次 SSL_connect 不写任何字节，pending_send_ == 0
// 且 pending_recv_ 已归零，_Cleanup() 当场执行。**所以下面的 case A 必须钉死
// -tls1_2，换成 1.3 就测不出东西。**
//
// ---------------------------------------------------------------------------
// 怎么跑
// ---------------------------------------------------------------------------
// 1) 另开一个终端起服务端（证书用仓库里现成的 certs/）：
//
//      openssl s_server -accept 18444 \
//          -cert certs/localhost.crt -key certs/localhost.key \
//          -www -tls1_2 -no_ticket -no_cache
//
// 2) 编译（务必带 ASan，这个测试的价值有一半在 ASan 上）：
//
//      c++ -std=c++17 -g -fsanitize=address -fno-omit-frame-pointer \
//          -I src/cpp -I deps/env/src -I deps/jquic/include \
//          -I deps/boringssl/src/include \
//          src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//          src/cpp/TlsContext.cpp src/cpp/SSLayer.cpp src/cpp/Runtime.cpp \
//          src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//          src/cpp/TcpChannel.cpp src/cpp/Utils.cpp \
//          src/cpp/tests/TcpChannel_integration_test.cpp \
//          deps/env/lib/Darwin/arm64/Debug/libenv.a \
//          deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//          deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//          -o /tmp/tcpchannel_it
//
//    （Linux 把 apple/AppleRouteLookup.cpp 换成 linux/LinuxRouteLookup.cpp，
//      库路径换成 deps/*/lib/Linux/x86_64/Debug/*.a。）
//
// 3) 跑：
//
//      /tmp/tcpchannel_it                                   # 用默认参数
//      /tmp/tcpchannel_it 127.0.0.1 18444 certs/localhost.crt
//
//    服务端没起来时，需要 TLS 的两个用例会跳过并打印提示，自建假服务端的四个
//    用例照常跑；退出码 0 = 全过，1 = 有失败，2 = TLS 用例被跳过。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Recorder —— 记录回调，供主线程等待
///////////////////////////////////////////////////////////////////////////////

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

// 契约上收到 OnChannelClosed 之后销毁就是安全的（那时事件队列已经 Detach，
// 不会再有任何回调派发过来）。这里仍然等一小会儿再 delete，是因为主线程是被
// 条件变量唤醒的，唤醒时通道那条队列线程还在 OnEventProcShutdown() 这个成员
// 函数的栈帧里往外退——真实调用方（Task 6/7）会把销毁排进自己的事件循环，
// 天然就有这一步；测试里用等待达到同样效果。
static void SettleBeforeDestroy()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

///////////////////////////////////////////////////////////////////////////////
// 自建假服务端：accept 之后按 mode 处理，只服务一条连接
///////////////////////////////////////////////////////////////////////////////

class FakeServer
{
public:
    enum Mode {
        // accept 之后立刻关闭 —— 模拟"对端在 TLS 握手中途 FIN"
        MODE_ACCEPT_THEN_CLOSE = 0,
        // 读一次、回一句、再关闭 —— 明文 happy path
        MODE_ECHO_ONCE         = 1,
        // accept 之后一个字节都不回，挂到测试主动收摊 —— 模拟握手黑洞，
        // 用来触发超时路径
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
        addr.sin_port        = 0;      // 让内核选端口，免得撞车
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
            // 收下 ClientHello 但绝不回应，把连接晾在那里，直到 Stop() 收摊
            char buf[512];
            while (!stop_)
            {
                struct timeval tv = { 0, 200 * 1000 };
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n == 0) break;      // 对端关了
            }
        }
        // MODE_ACCEPT_THEN_CLOSE 什么都不做，直接进下面的 close
        close(fd);
    }

    Mode              mode_      = MODE_ACCEPT_THEN_CLOSE;
    int               listen_fd_ = -1;
    uint16_t          port_      = 0;
    std::atomic<bool> stop_{false};
    std::thread       thread_;
};

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

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

// 从 PEM 证书文件算出它的 SPKI pin，用的就是被测代码自己那份实现。
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
    cfg.resolvedIp       = ip;           // 跳过 DNS，测试不依赖解析器
    cfg.port             = port;
    cfg.policy           = TT_VPN_POLICY_OS;   // 环回地址，不做网卡绑定
    cfg.connectTimeoutMs = 8000;
    return cfg;
}

///////////////////////////////////////////////////////////////////////////////
// Case A —— 【Critical 回归】TLS1.2 + 错误 spkiPin
//
// 期望：OnChannelClosed(result = BC_R_TLS_VERIFY_FAILED)，恰好一次，且
//       ASan 下无 heap-use-after-free。
//
// 修复前这里必崩：pin 校验失败 → _Fail → _MaybeFinishClose → _Cleanup 在
// SSLayer::ReadFromSSL 的栈帧里把 SSLayer 和 SSL 都释放掉，返回后 SSLayer
// 继续读 ssl_state_。
///////////////////////////////////////////////////////////////////////////////

static void case_a_tls12_bad_pin(const char* ip, uint16_t port)
{
    printf("[case A] TLS1.2 + 错误 spkiPin（Critical 回归，需 ASan）\n");

    Recorder rec;
    TcpChannel* ch = new TcpChannel();

    TcpChannelConfig cfg = BaseConfig(ip, port);
    cfg.tls = true;
    // 合法 base64、长度也对，但不是这张证书的指纹 —— 走到"握手成功、pin 不符"
    cfg.tls_cfg.spkiPin = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";

    BCRESULT r = ch->Open(cfg, &rec);
    CHECK(r == BC_R_SUCCESS);
    if (r != BC_R_SUCCESS) { delete ch; return; }

    CHECK(rec.WaitClosed(10000));
    CHECK(rec.CloseCount() == 1);
    CHECK(rec.Result() == BC_R_TLS_VERIFY_FAILED);
    CHECK(!rec.Ready());          // pin 没过就绝不能报 ready
    CHECK(rec.Data().empty());    // 更不能漏任何应用数据上来
    printf("  result=%d reason=%s\n", (int)rec.Result(), rec.Reason().c_str());

    SettleBeforeDestroy();
    delete ch;
}

///////////////////////////////////////////////////////////////////////////////
// Case B —— TLS1.2 + 正确 spkiPin：握手应当成功
//
// 反证 case A 不是"pin 路径总是失败"。顺带覆盖主动 Close() 的收尾。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Case C —— 【Important 2 回归】握手中途对端 FIN 不得报成 BC_R_SUCCESS
//
// 假服务端 accept 之后立刻关闭。客户端还停在 TLS_HANDSHAKE，证书一个字节都
// 没验过。若这里报 BC_R_SUCCESS，中间人只要在握手中途 FIN 就能让上层以为
// "对端正常关闭"。
///////////////////////////////////////////////////////////////////////////////

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
    cfg.tls = true;     // 没有 pin，走 BoringSSL 内建校验

    BCRESULT r = ch->Open(cfg, &rec);
    CHECK(r == BC_R_SUCCESS);
    if (r != BC_R_SUCCESS) { delete ch; server.Stop(); return; }

    CHECK(rec.WaitClosed(10000));
    CHECK(rec.CloseCount() == 1);
    CHECK(rec.Result() != BC_R_SUCCESS);          // 核心断言
    CHECK(rec.Result() == BC_R_UNEXPECTEDEND);
    CHECK(!rec.Ready());
    printf("  result=%d reason=%s\n", (int)rec.Result(), rec.Reason().c_str());

    SettleBeforeDestroy();
    delete ch;
    server.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case D —— 明文 happy path：ready → 发 → 收 → 对端正常关闭
//
// 覆盖非 TLS 分支的完整来回，以及"READY 之后的 EOF 才算 BC_R_SUCCESS"。
///////////////////////////////////////////////////////////////////////////////

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
    // 明文连接在 READY 之后收到 FIN —— 这才是真正的"对端正常关闭"
    CHECK(rec.Result() == BC_R_SUCCESS);
    printf("  data=%s result=%d reason=%s\n", rec.Data().c_str(),
           (int)rec.Result(), rec.Reason().c_str());

    SettleBeforeDestroy();
    delete ch;
    server.Stop();
}

///////////////////////////////////////////////////////////////////////////////
// Case E —— 握手黑洞触发超时
//
// 假服务端收下 ClientHello 后一个字节都不回。除了验证超时码本身，这个用例
// 还专门压一条危险路径：超时回调是 BCDelayTask 派发过来的，而它触发的
// _Fail() 会一路 UnscheduleTask + Detach，把这个 BCDelayTask 在它自己的回调
// 里删掉。ASan 下跑干净才说明没问题。
///////////////////////////////////////////////////////////////////////////////

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
    cfg.connectTimeoutMs = 1200;    // 短一点，别让测试等太久

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

///////////////////////////////////////////////////////////////////////////////
// Case F —— 【Close 丢回调回归】Close() 与关闭流程并发，OnChannelClosed 不能丢
//
// 曾经的漏法：Close() 先向 guard_lock_ 取号再投递 lambda。如果最后一个在途
// IO 回调恰好在"号还没还"的那一瞬间跑完，_MaybeFinishClose() 会被
// pending_tasks_ > 0 挡下；等 Close 的 lambda 还完号再调 _Fail()，而 _Fail()
// 的幂等分支直接 return，没有任何人会再推一下 —— 通道永久卡在 TCPCH_CLOSING，
// 调用方永远等不到 OnChannelClosed，对象泄漏。
//
// 紧密自旋只是把窗口撑大，真实触发条件是"调用方 Close() 与对端 FIN / IO 取消
// 完成同时发生"，正是 WebSocket 关闭握手的常规场景。
//
// 跑 20 轮，每轮 ready 之后另起一条线程紧密自旋 Close() 300ms。
///////////////////////////////////////////////////////////////////////////////

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
        cfg.tls              = false;    // 明文，accept 上来就是 READY
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

        // 紧密自旋 Close()：把"取号 / 还号"与 IO 取消完成的交错窗口撑到必现
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
            break;      // 已经复现，不用再跑剩下的轮次
        }
    }

    CHECK(hangs == 0);
    printf("  done: %d iters, %d hangs\n", kIters, hangs);
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
    // 不缓冲：这个测试经常在 ASan 报错（stderr，无缓冲）里被打断，
    // stdout 一缓冲就看不出崩在哪个用例上了。
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

    // 自建假服务端的四个用例不依赖外部进程，永远跑
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

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
