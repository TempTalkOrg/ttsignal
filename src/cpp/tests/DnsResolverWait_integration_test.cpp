///////////////////////////////////////////////////////////////////////////////
// file : DnsResolverWait_integration_test.cpp
//
// DnsResolver 等待策略的集成测试：起一个本地假 DNS server，真的收发 UDP。
// 不进任何构建目标，手动编译运行。POSIX only。
//
// ---------------------------------------------------------------------------
// 为什么必须有这个文件
// ---------------------------------------------------------------------------
// DnsResolver_test.cpp 只测报文编解码，碰不到 _ResolveViaUdp 的等待循环，于是
// 漏掉了一个每次冷启动都在付钱的问题：A 和 AAAA 一起发出去，循环却要求**两路
// 应答都收到**才肯退出。A 早就带着地址回来了，只要 AAAA 那一路丢一个包，就得
// 干等到 timeoutMs（当时默认 2000ms）。实测 force-physical 首次请求
// test.ablivekit.org 因此花掉 1991ms，而同一时刻 os 档只要 275ms。
//
// 这里用假 server 把"AAAA 永不应答"钉成一个必现场景。
//
// ---------------------------------------------------------------------------
// 怎么跑
// ---------------------------------------------------------------------------
//   c++ -std=c++17 -g -I src/cpp -I deps/env/src -I deps/jquic/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       src/cpp/Utils.cpp \
//       src/cpp/tests/DnsResolverWait_integration_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       -o /tmp/dnswait_it && /tmp/dnswait_it
//
// 编译依赖与 DnsResolver_test.cpp 完全一致，原因见那个文件的头注释。
///////////////////////////////////////////////////////////////////////////////
#include "DnsResolver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

///////////////////////////////////////////////////////////////////////////////
// 假 DNS server
///////////////////////////////////////////////////////////////////////////////

// 对每种 qtype 单独规定怎么应答，好把"AAAA 那一路丢了"这种偏科场景摆出来。
enum FakeReply
{
    kReplyAddr,    // 正常回一条地址记录
    kReplyEmpty,   // 回 NOERROR 但 answer 为空（域名没有这种记录时的真实行为）
    kReplySilent,  // 根本不回（丢包 / 中间设备吞掉 AAAA）
};

class FakeDnsServer
{
public:
    FakeDnsServer(FakeReply onA, FakeReply onAAAA)
        : on_a_(onA), on_aaaa_(onAAAA), fd_(-1), port_(0), stop_(false)
    {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return;

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;   // 让内核挑一个空闲端口，免得撞车
        if (bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) return;

        socklen_t len = sizeof(addr);
        if (getsockname(fd_, (struct sockaddr*)&addr, &len) != 0) return;
        port_ = ntohs(addr.sin_port);

        // 收包超时是析构能收尾的唯一办法：macOS 上 shutdown() 对这个没
        // connect 过的 UDP socket 返回 ENOTCONN，压根唤不醒阻塞中的
        // recvfrom，join 会永久挂住。
        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 100 * 1000;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        thread_ = std::thread([this] { this->_Loop(); });
    }

    ~FakeDnsServer()
    {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) close(fd_);
    }

    uint16_t Port() const { return port_; }
    int      QueriesSeen() const { return queries_seen_.load(); }

private:
    void _Loop()
    {
        uint8_t buf[512];
        while (!stop_)
        {
            struct sockaddr_in from;
            socklen_t          fromLen = sizeof(from);
            ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&from, &fromLen);
            if (n < 12) continue;   // 收包超时到点，回头看一眼 stop_
            queries_seen_++;

            // 查询报文末尾就是 QTYPE(2) + QCLASS(2)，取 QTYPE 判断这是哪一路。
            uint16_t qtype = (uint16_t)((buf[n - 4] << 8) | buf[n - 3]);
            FakeReply how = (qtype == 28) ? on_aaaa_ : on_a_;
            if (how == kReplySilent) continue;

            // 应答 = 原样回显 header + question，再按需要追加一条 answer。
            std::vector<uint8_t> resp(buf, buf + n);
            resp[2] = 0x81;   // QR=1, RD=1
            resp[3] = 0x80;   // RA=1, RCODE=0
            if (how == kReplyAddr)
            {
                resp[6] = 0x00; resp[7] = 0x01;   // ANCOUNT=1

                resp.push_back(0xC0); resp.push_back(0x0C);   // NAME -> question
                if (qtype == 28)
                {
                    resp.push_back(0x00); resp.push_back(0x1C);   // TYPE=AAAA
                }
                else
                {
                    resp.push_back(0x00); resp.push_back(0x01);   // TYPE=A
                }
                resp.push_back(0x00); resp.push_back(0x01);       // CLASS=IN
                resp.push_back(0x00); resp.push_back(0x00);
                resp.push_back(0x01); resp.push_back(0x2C);       // TTL=300

                if (qtype == 28)
                {
                    resp.push_back(0x00); resp.push_back(0x10);   // RDLENGTH=16
                    // 2001:db8::1
                    const uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 0x01};
                    resp.insert(resp.end(), v6, v6 + 16);
                }
                else
                {
                    resp.push_back(0x00); resp.push_back(0x04);   // RDLENGTH=4
                    resp.push_back(203); resp.push_back(0);
                    resp.push_back(113); resp.push_back(7);       // 203.0.113.7
                }
            }
            sendto(fd_, resp.data(), resp.size(), 0,
                   (struct sockaddr*)&from, fromLen);
        }
    }

    FakeReply        on_a_;
    FakeReply        on_aaaa_;
    int              fd_;
    uint16_t         port_;
    std::atomic<bool> stop_;
    std::atomic<int>  queries_seen_{0};
    std::thread      thread_;
};

///////////////////////////////////////////////////////////////////////////////
// 辅助
///////////////////////////////////////////////////////////////////////////////

// policy 取 prefer-physical：ifIndex 为 0 时 tt_socket_pin 返回 NOT_NEEDED，
// 这一档允许放行（不绑定直接查），正好让用例能打到 127.0.0.1 上的假 server。
// force-physical 在这里不可用——它要求必须真绑上物理网卡。
static DnsConfig MakeCfg(const FakeDnsServer& server)
{
    DnsConfig cfg;
    cfg.policy     = TT_VPN_POLICY_PREFER_PHYSICAL;
    cfg.servers    = {"127.0.0.1"};
    cfg.serverPort = server.Port();
    cfg.timeoutMs  = 1000;
    return cfg;
}

static bool HasV4(const std::vector<BCSockAddrS>& addrs, const char* want)
{
    for (size_t i = 0; i < addrs.size(); i++)
    {
        if (addrs[i].type.sa.sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addrs[i].type.sin.sin_addr, buf, sizeof(buf));
        if (strcmp(buf, want) == 0) return true;
    }
    return false;
}

static bool HasV6(const std::vector<BCSockAddrS>& addrs, const char* want)
{
    for (size_t i = 0; i < addrs.size(); i++)
    {
        if (addrs[i].type.sa.sa_family != AF_INET6) continue;
        char buf[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &addrs[i].type.sin6.sin6_addr, buf, sizeof(buf));
        if (strcmp(buf, want) == 0) return true;
    }
    return false;
}

static long ElapsedMs(std::chrono::steady_clock::time_point t0)
{
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
}

///////////////////////////////////////////////////////////////////////////////
// 用例
///////////////////////////////////////////////////////////////////////////////

// 脚手架自检，同时钉住 serverPort：没有它，查询会打到 127.0.0.1:53（本机没人
// 监听），整个用例根本连不到假 server。
static void test_both_answers_are_collected()
{
    FakeDnsServer server(kReplyAddr, kReplyAddr);
    CHECK(server.Port() != 0);

    DnsResolver::ClearCache();
    DnsResult r = DnsResolver::Resolve("both.test.invalid", MakeCfg(server));

    CHECK(r.result == BC_R_SUCCESS);
    CHECK(HasV4(r.addrs, "203.0.113.7"));
    CHECK(HasV6(r.addrs, "2001:db8::1"));
}

// 本文件存在的理由：A 带着地址回来了，AAAA 那一路永远不来。拿到的地址已经够
// 建连接了，不该为了一个注定不到的应答把整次解析拖满 timeoutMs。
static void test_a_answer_does_not_wait_for_missing_aaaa()
{
    FakeDnsServer server(kReplyAddr, kReplySilent);
    CHECK(server.Port() != 0);

    DnsConfig cfg = MakeCfg(server);   // timeoutMs = 1000

    DnsResolver::ClearCache();
    auto      t0 = std::chrono::steady_clock::now();
    DnsResult r  = DnsResolver::Resolve("aonly.test.invalid", cfg);
    long      ms = ElapsedMs(t0);

    CHECK(r.result == BC_R_SUCCESS);
    CHECK(HasV4(r.addrs, "203.0.113.7"));
    // 宽限期是 50ms 量级，300ms 留足了假 server 与调度的余量；旧实现在这里
    // 会交出 ~1000ms。
    if (ms >= 300) printf("      实测耗时 %ld ms\n", ms);
    CHECK(ms < 300);
}

// 反过来也要成立：AAAA 先带地址回来、A 那一路丢了，同样不该等满。
static void test_aaaa_answer_does_not_wait_for_missing_a()
{
    FakeDnsServer server(kReplySilent, kReplyAddr);
    CHECK(server.Port() != 0);

    DnsResolver::ClearCache();
    auto      t0 = std::chrono::steady_clock::now();
    DnsResult r  = DnsResolver::Resolve("v6only.test.invalid", MakeCfg(server));
    long      ms = ElapsedMs(t0);

    CHECK(r.result == BC_R_SUCCESS);
    CHECK(HasV6(r.addrs, "2001:db8::1"));
    if (ms >= 300) printf("      实测耗时 %ld ms\n", ms);
    CHECK(ms < 300);
}

// 宽限期只在"已经拿到地址"之后才开始算。两路都空手而归时没什么可抢救的，该
// 老老实实等满 timeoutMs 再判这台 server 不行——否则 server 只是慢一点就会
// 被误判。
static void test_empty_answers_still_wait_full_timeout()
{
    FakeDnsServer server(kReplyEmpty, kReplySilent);
    CHECK(server.Port() != 0);

    DnsConfig cfg = MakeCfg(server);   // timeoutMs = 1000

    DnsResolver::ClearCache();
    auto      t0 = std::chrono::steady_clock::now();
    DnsResult r  = DnsResolver::Resolve("empty.test.invalid", cfg);
    long      ms = ElapsedMs(t0);

    // 自建查询拿不到地址 -> prefer-physical 回落 getaddrinfo，那个域名不存在，
    // 所以最终还是失败。这里只关心"等够了没有"。
    CHECK(r.result != BC_R_SUCCESS);
    if (ms < 900) printf("      实测耗时 %ld ms\n", ms);
    CHECK(ms >= 900);
}

// 默认值是跨绑定层的契约（JS 的 dnsTimeoutMs、Swift/Java 的同名配置都只是把它
// 透传下来），改动必须是有意为之，所以在这里钉死。
//
// timeoutMs 是"这台 server 判死"的门槛，跟宽限期是两回事：宽限期只在已经拿到
// 地址时才起作用，两路都空手时仍然要等满 timeoutMs。默认列表有两台 server，
// 全都哑掉的最坏情况就是 2 × timeoutMs，2000ms 一台意味着最坏 4 秒，对一次
// HTTP 请求来说太久了。
static void test_config_defaults()
{
    DnsConfig cfg;

    CHECK(cfg.timeoutMs == 1000);
    CHECK(cfg.secondAnswerGraceMs == 50);
    CHECK(cfg.serverPort == 53);
}

///////////////////////////////////////////////////////////////////////////////

int main()
{
    test_config_defaults();
    test_both_answers_are_collected();
    test_a_answer_does_not_wait_for_missing_aaaa();
    test_aaaa_answer_does_not_wait_for_missing_a();
    test_empty_answers_still_wait_full_timeout();

    if (g_failures == 0)
    {
        printf("DnsResolverWait_integration_test: all passed\n");
        return 0;
    }
    printf("DnsResolverWait_integration_test: %d failure(s)\n", g_failures);
    return 1;
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
