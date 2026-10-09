///////////////////////////////////////////////////////////////////////////////
// file : TcpChannelReuse_integration_test.cpp
//
// TcpChannel 复用（Park / Adopt）的集成测试：真的起事件循环、真的建 TCP 连接。
// 不进任何构建目标，手动编译运行。POSIX only。全程明文（tls=false）+
// resolvedIp，不需要证书，也不碰 DNS。
//
// ---------------------------------------------------------------------------
// 这两个接口是干什么的
// ---------------------------------------------------------------------------
// HttpConnector 原本一次请求一条连接（请求头里硬写 Connection: close），每次
// 都要重付 TCP 握手 + 完整 TLS 握手。实测同一个 https 端点，Node 的默认 agent
// 复用连接后每次 95ms，ttsignal 每次 280ms。
//
// 要复用，通道就必须能在两条 HttpConnection 之间转手：
//   Park()  —— 上一条请求收完响应，把通道从自己身上摘下来，连接本身留着
//   Adopt() —— 下一条请求接手这条现成的连接，跳过握手直接发报文
//
// 转手期间对端随时可能 FIN，所以 Adopt 的失败路径和 Park 期间的"意外来数据"
// 都必须钉死 —— 这正是复用最容易出错的地方：把一条已经半死的连接交给新请求，
// 症状是偶发的、无从复现的请求失败。
//
// ---------------------------------------------------------------------------
// 怎么跑
// ---------------------------------------------------------------------------
//   c++ -std=c++17 -g -fsanitize=address -fno-omit-frame-pointer \
//       -I src/cpp -I deps/env/src -I deps/jquic/include \
//       -I deps/boringssl/src/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       src/cpp/TlsContext.cpp src/cpp/SSLayer.cpp src/cpp/Runtime.cpp \
//       src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//       src/cpp/TcpChannel.cpp src/cpp/Utils.cpp \
//       src/cpp/tests/TcpChannelReuse_integration_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//       -o /tmp/tcpreuse_it && /tmp/tcpreuse_it
//
// 编译单与 TcpChannel_integration_test.cpp 一致，原因见那个文件的头注释。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// Recorder —— 记录回调，供主线程等待
///////////////////////////////////////////////////////////////////////////////

class Recorder : public ITcpChannelHandler
{
public:
    // 非空时，一收到数据就在**回调线程上**把通道 Park 掉 —— 这正是
    // HttpConnection 收完响应后归还连接的真实时机。
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
            park_on_data = NULL;   // 只 Park 一次
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
    BCRESULT                close_result_ = BC_R_SUCCESS;
    std::string             close_reason_;
    std::string             data_;
};

// 见 TcpChannel_integration_test.cpp 里同名函数的注释：收到 OnChannelClosed
// 之后销毁就是安全的，这里多等一会儿只是为了让队列线程退干净。
static void SettleBeforeDestroy()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

///////////////////////////////////////////////////////////////////////////////
// 假服务端：一条连接上反复收一行、回一行，直到被要求收摊
///////////////////////////////////////////////////////////////////////////////

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

    // 主动把已建立的那条连接关掉，模拟服务端的 keep-alive 空闲超时。
    void DropPeer()
    {
        int fd = peer_fd_.exchange(-1);
        if (fd >= 0) close(fd);
    }

    // 不等客户端开口，直接往连接上塞一段字节。
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
            if (cur < 0) break;           // DropPeer 把它关了

            struct timeval tv = { 0, 100 * 1000 };
            setsockopt(cur, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = recv(cur, buf, sizeof(buf), 0);
            if (n == 0) break;            // 对端关了
            if (n < 0) continue;          // 收包超时，回头看一眼 stop_

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

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

static TcpChannelConfig MakeCfg(uint16_t port)
{
    TcpChannelConfig cfg;
    cfg.host              = "localhost";
    cfg.port              = port;
    cfg.tls               = false;
    cfg.resolvedIp        = "127.0.0.1";   // 跳过 DNS，用例只关心连接复用
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

///////////////////////////////////////////////////////////////////////////////
// 用例
///////////////////////////////////////////////////////////////////////////////

// 复用的主干：一条连接先后伺候两个 handler，第二个不需要重新握手。
static void test_parked_channel_serves_a_second_handler()
{
    printf("Case A: Park 过的通道能交给下一个 handler\n");

    EchoServer server;
    CHECK(server.Start());

    TcpChannel ch;
    Recorder   first;
    first.park_on_data = &ch;   // 收到响应就地归还，模拟 HttpConnection 的收尾

    CHECK(ch.Open(MakeCfg(server.Port()), &first) == BC_R_SUCCESS);
    CHECK(first.WaitReady(5000));

    Send(ch, "PING");
    CHECK(first.WaitData(4, 5000));
    CHECK(first.Data() == "PONG");

    // 连接没断：Park 只是把 handler 摘掉
    CHECK(!first.Closed());
    CHECK(ch.GetState() == TcpChannel::TCPCH_READY);

    Recorder second;
    CHECK(ch.Adopt(&second) == BC_R_SUCCESS);
    // 接手就绪的信号走 OnChannelReady，新 handler 因此不必区分"新连接"和
    // "捡来的连接"——HttpConnection 那套发请求的代码一行都不用改。
    CHECK(second.WaitReady(5000));

    Send(ch, "PING");
    CHECK(second.WaitData(4, 5000));
    CHECK(second.Data() == "PONG");

    // 转手之后，旧 handler 一个字节都不该再收到
    CHECK(first.Data() == "PONG");

    ch.Close();
    CHECK(second.WaitClosed(5000));
    SettleBeforeDestroy();
    server.Stop();
}

// 转手期间对端 FIN 是 keep-alive 的日常（服务端有自己的空闲超时）。把一条
// 已经死掉的连接交给新请求，换来的是偶发且无从复现的请求失败，所以 Adopt
// 必须当场认出来。
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

    // 通道此刻是 Park 状态，没有 handler。服务端关掉连接。
    server.DropPeer();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    Recorder second;
    BCRESULT r = ch.Adopt(&second);
    CHECK(r != BC_R_SUCCESS);
    // 拒绝必须是干净的：不产生任何回调，调用方直接改走新建连接那条路。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(second.ReadyCount() == 0);
    CHECK(!second.Closed());

    SettleBeforeDestroy();
    server.Stop();
}

// Park 期间本不该有任何字节到来（HTTP/1.1 上一条报文已经完整收完了）。真收到
// 了就说明这条连接上的报文边界已经对不齐——服务端多发了东西，或者我们把边界
// 算错了。继续复用会把这段字节当成下一条响应的开头，于是下一个请求解析出一堆
// 莫名其妙的结果。宁可丢掉这条连接。
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

    // 那段字节也绝不该漏给旧 handler
    CHECK(first.Data() == "PONG");

    ch.Close();
    SettleBeforeDestroy();
    server.Stop();
}

// Park 只对活着的连接有意义。没连上的通道上调它是调用方写错了，不能让它悄悄
// 变成一条"看起来可复用"的通道。
static void test_park_is_rejected_before_ready()
{
    printf("Case D: 未就绪的通道 Park 不生效\n");

    TcpChannel ch;
    ch.Park();   // IDLE 状态，什么都不该发生

    CHECK(ch.GetState() == TcpChannel::TCPCH_IDLE);

    Recorder r;
    CHECK(ch.Adopt(&r) != BC_R_SUCCESS);
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

    test_parked_channel_serves_a_second_handler();
    test_adopt_rejects_a_channel_the_peer_closed();
    test_adopt_rejects_a_channel_that_got_unexpected_bytes();
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

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
