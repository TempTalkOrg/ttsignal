///////////////////////////////////////////////////////////////////////////////
// file : UDPSenderRouteRecheck_integration_test.cpp
//
// UDPSender 路由复核（vpnPolicy）的**真实路径**集成测试：真的起 Runtime、真的
// 建 UDP socket、真的 setsockopt(IP_BOUND_IF)、真的查内核路由表、真的调
// UDPSender::Connect()，然后用 getsockname() 拿到内核选定的源地址来判断"绑定
// 到底留住了没有"。
//
// ---------------------------------------------------------------------------
// 为什么必须有这个文件
// ---------------------------------------------------------------------------
// VpnPolicy_test.cpp 能穷举 tt_vpn_pin_recheck() 的决策表，但它喂的是**假**的
// 路由查询结果。真正出过事故的两个环节都不在那张表里：
//
//   1) UDPSender::Connect() 里那两次 tt_route_lookup_ifindex 的 scope 参数到底
//      传了什么 —— 传错了单测一个都不会红，因为决策函数收到的仍然是"某个
//      uint32_t"。上一版就是把 scope=0 的答案当成绑定有效性判据，于是企业 VPN
//      抢占默认路由时 prefer-physical 必然解绑回落 VPN。
//   2) "解绑/保持绑定"这个决定在**内核**里的实际后果 —— 只有 getsockname() 的
//      源地址能证明。
//
// 这个测试把两个环节都覆盖住：它跑在真实路由表上，断言的是"源地址属于哪块
// 网卡"。
//
// ---------------------------------------------------------------------------
// 前置条件与自动跳过
// ---------------------------------------------------------------------------
// 核心用例需要一台"VPN 抢占了全局默认路由，但物理网卡自己仍有默认路由"的机器
// （本仓的复现环境：macOS + 企业 GlobalProtect 全隧道，en0=14 / utun4=22）。
// 不满足时相关用例自动 SKIP 而不是 FAIL —— 否则这个文件在 CI 和没挂 VPN 的
// 开发机上永远是红的。是否满足由测试自己探测，不写死 ifIndex。
//
// 本测试**会**向公网目标发少量 UDP 报文（QUIC 那条路径上 UDP connect 本身不
// 发包，但 Connect() 之后我们不发任何数据，所以实际上一个字节都不发）。
//
// ---------------------------------------------------------------------------
// 怎么编（可直接粘贴执行，在仓库根目录）
// ---------------------------------------------------------------------------
//   c++ -std=c++17 -g -fsanitize=address -fno-omit-frame-pointer \
//       -DTT_HAS_PATH_MONITOR \
//       -I src/cpp -I deps/env/src -I deps/jquic/include \
//       -I deps/boringssl/src/include \
//       src/cpp/UDPSender.cpp src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp \
//       src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//       src/cpp/apple/AppleNetworkMonitor.mm \
//       src/cpp/Runtime.cpp src/cpp/Utils.cpp \
//       src/cpp/tests/UDPSenderRouteRecheck_integration_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       -framework Foundation -framework Network -fobjc-arc \
//       -o /tmp/udpsender_recheck_it && /tmp/udpsender_recheck_it
//
// （Linux 把 apple/AppleRouteLookup.cpp 换成 linux/LinuxRouteLookup.cpp、
//   AppleNetworkMonitor.mm 换成 linux 下的监视器实现，去掉两个 -framework 与
//   -fobjc-arc，库路径换成 deps/env/lib/Linux/x86_64/Debug/libenv.a。Linux 的
//   tt_route_lookup_ifindex 刻意忽略 scope，所以那边只有 os 档用例有意义。）
//
// 退出码：0 = 全过，1 = 有失败，2 = 关键用例被跳过（环境不满足）。
///////////////////////////////////////////////////////////////////////////////

#include "UDPSender.h"
#include "Runtime.h"
#include "NetworkRouteLookup.h"
#include "VpnPolicy.h"
#include "INetworkPathMonitor.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
static int g_skipped  = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define SKIP(fmt, ...)                                                         \
    do {                                                                       \
        printf("SKIP %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);    \
        g_skipped++;                                                           \
    } while (0)

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

// 只需要一个"有默认路由才到得了"的公网目标。刻意用 IP 字面量而不是域名：DNS
// 本身可能走 VPN，会把"路由"和"名字解析"两件事混在一起。34.117.59.81 是
// ipinfo.io —— 端到端人工验证也用它，两边对得上。测试不向它发任何数据。
static const char* kPublicDst = "34.117.59.81";

// UDPSender 需要一个 handler，但本测试只关心 Connect() 之后的 socket 状态，
// 所有回调都可以是空实现。
class NullHandler : public IUDPSenderHandler
{
public:
    void OnSendData(uint32_t, UDPSender*) override {}
    void OnRecvData(BCBuffer*, BCSockAddrS&) override {}
    void OnCheckAvailable() override {}
    void OnRestart(BCRESULT) override {}
    void OnUdpClosed() override {}
};

static void MakeV4(BCSockAddrS& addr, const char* ip, uint16_t port)
{
    memset(&addr, 0, sizeof(addr));
    addr.type.sin.sin_family = AF_INET;
    addr.type.sin.sin_len    = sizeof(struct sockaddr_in);
    addr.type.sin.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &addr.type.sin.sin_addr);
    addr.length = sizeof(struct sockaddr_in);
}

// 某个 ifIndex 上是否配了这个 IPv4 地址。用来把 getsockname() 拿到的源地址
// 归属到具体网卡 —— 这是"绑定留住了没有"的唯一可靠判据。
static bool AddrBelongsToIface(const struct in_addr& want, uint32_t ifIndex)
{
    char name[IF_NAMESIZE] = {0};
    if (if_indextoname(ifIndex, name) == NULL)
    {
        return false;
    }
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) != 0)
    {
        return false;
    }
    bool found = false;
    for (struct ifaddrs* p = ifa; p != NULL; p = p->ifa_next)
    {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, name) != 0) continue;
        const struct sockaddr_in* sin = (const struct sockaddr_in*)p->ifa_addr;
        if (sin->sin_addr.s_addr == want.s_addr) { found = true; break; }
    }
    freeifaddrs(ifa);
    return found;
}

static std::string IfNameOf(uint32_t ifIndex)
{
    char name[IF_NAMESIZE] = {0};
    if (if_indextoname(ifIndex, name) == NULL) return "?";
    return name;
}


// UDPSender 没有暴露 fd，而 GetSockName() 返回的是 bind 时缓存的 m_sSelfAddr
// （0.0.0.0 + 临时端口），拿不到 connect 之后内核选定的源地址。所以这里按
// "本进程内本地端口唯一"把 fd 找回来——不为测试往生产类上加访问器。
static int FindUdpFdByLocalPort(uint16_t port)
{
    for (int fd = 0; fd < 1024; ++fd)
    {
        int type = 0;
        socklen_t tlen = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) != 0) continue;
        if (type != SOCK_DGRAM) continue;
        struct sockaddr_in sin;
        socklen_t slen = sizeof(sin);
        memset(&sin, 0, sizeof(sin));
        if (getsockname(fd, (struct sockaddr*)&sin, &slen) != 0) continue;
        if (sin.sin_family != AF_INET) continue;
        if (ntohs(sin.sin_port) == port) return fd;
    }
    return -1;
}

// 读回内核里 IP_BOUND_IF 的当前值。这是"钉子还在不在"的**直接**证据：
// UDPSender::Connect() 里的路由复核是同步执行的，所以它返回时这个值已经定了。
// 0 表示没有绑定（含被解绑）。
static int ReadBoundIf(int fd)
{
#if defined(IP_BOUND_IF)
    int v = -1;
    socklen_t l = sizeof(v);
    if (getsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &v, &l) != 0) return -1;
    return v;
#else
    (void)fd;
    return -1;
#endif
}

// connect() 是异步完成的，源地址要等内核选完才看得到。轮询到非 0 为止。
static bool PollSourceAddr(int fd, struct in_addr& out, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 20)
    {
        struct sockaddr_in sin;
        socklen_t slen = sizeof(sin);
        memset(&sin, 0, sizeof(sin));
        if (getsockname(fd, (struct sockaddr*)&sin, &slen) == 0 &&
            sin.sin_family == AF_INET && sin.sin_addr.s_addr != 0)
        {
            out = sin.sin_addr;
            return true;
        }
        usleep(20 * 1000);
    }
    return false;
}

// 建一个 UDPSender、绑到 boundIfIndex、设好策略、Connect 到公网目标，
// 回填内核选定的源地址。返回 Connect 的结果码。
// 一次复核的可观测结果。
struct ConnectOutcome
{
    BCRESULT       result;
    int            boundIf;      // Connect() 返回后内核里的 IP_BOUND_IF，-1=读不到
    bool           haveSrc;      // 是否拿到了 connect 之后的源地址
    struct in_addr src;
};

// 建一个 UDPSender、绑到 boundIfIndex、设好策略、Connect 到公网目标，
// 回填"钉子还在不在"与内核选定的源地址。
static ConnectOutcome RunConnect(TTVpnPolicy policy, int64_t boundIfIndex)
{
    ConnectOutcome out;
    memset(&out, 0, sizeof(out));
    out.boundIf = -1;

    NullHandler handler;
    BCFObject   cfg;

    // UDPSender 没有静态工厂，SMPConnection 也是直接 new（见 SMPConnector.cpp
    // 里的 new UDPSenderGroup()）。Destroy(&p) 会 delete this。
    UDPSender* sender = new UDPSender();
    out.result = sender->Create(NULL, Runtime::TaskMgr(), Runtime::TimerMgr(),
                       Runtime::SocketMgr(), &cfg, &handler,
                       /*bindIP=*/false, /*bindPort=*/false,
                       /*initialNetworkHandle=*/boundIfIndex);
    if (out.result != BC_R_SUCCESS)
    {
        sender->Destroy(&sender);
        return out;
    }
    sender->SetVpnPolicy(policy);

    // bind 时缓存下来的本地端口，用它把 fd 找回来。
    BCSockAddrS bound;
    memset(&bound, 0, sizeof(bound));
    sender->GetSockName(bound);
    const uint16_t localPort = ntohs(bound.type.sin.sin_port);
    const int fd = FindUdpFdByLocalPort(localPort);

    BCSockAddrS peer;
    MakeV4(peer, kPublicDst, 443);
    out.result = sender->Connect(peer);

    if (fd >= 0)
    {
        // 复核是同步的，Connect() 返回时 IP_BOUND_IF 已定。
        out.boundIf = ReadBoundIf(fd);
        out.haveSrc = PollSourceAddr(fd, out.src, 1500);
    }

    sender->Close();
    sender->Destroy(&sender);
    return out;
}

///////////////////////////////////////////////////////////////////////////////
// 环境探测
///////////////////////////////////////////////////////////////////////////////

struct Topology
{
    uint32_t physIf;      // 物理网卡 ifIndex
    uint32_t osBestIf;    // 系统默认出口 ifIndex（挂了全隧道 VPN 时就是 utunN）
    uint32_t scopedPhys;  // scoped 到物理网卡时，内核对 kPublicDst 的答案
    uint32_t globalIf;    // 全局路由表对 kPublicDst 的答案
    bool     vpnPreempts; // VPN 抢占了默认路由，且物理网卡自己仍有路由
};

static Topology Probe()
{
    Topology t;
    memset(&t, 0, sizeof(t));

    int64_t phys = tt_netmon_query_default_ifindex_ex(
        (int)TT_VPN_POLICY_PREFER_PHYSICAL);
    int64_t best = tt_netmon_query_default_ifindex_ex((int)TT_VPN_POLICY_OS);
    t.physIf   = (phys > 0) ? (uint32_t)phys : 0;
    t.osBestIf = (best > 0) ? (uint32_t)best : 0;

    BCSockAddrS peer;
    MakeV4(peer, kPublicDst, 443);
    t.globalIf = tt_route_lookup_ifindex(&peer.type.sa,
        (socklen_t)sizeof(struct sockaddr_in), 0);
    if (t.physIf != 0)
    {
        t.scopedPhys = tt_route_lookup_ifindex(&peer.type.sa,
            (socklen_t)sizeof(struct sockaddr_in), t.physIf);
    }

    // "VPN 抢占默认路由"的判据：全局表把公网目标判给了物理网卡之外的网卡，
    // 而物理网卡的 scoped 表里到这个目标是有路的。
    t.vpnPreempts = (t.physIf != 0 &&
                     t.globalIf != TT_ROUTE_IFINDEX_UNKNOWN &&
                     t.globalIf != t.physIf &&
                     t.scopedPhys == t.physIf);
    return t;
}

///////////////////////////////////////////////////////////////////////////////
// 用例
///////////////////////////////////////////////////////////////////////////////

// 一次复核结果的统一打印 + 断言。expectBoundIf：期望 Connect() 之后内核里
// IP_BOUND_IF 的值（0 = 期望已被解绑）。
static void ReportAndAssert(const char* tag,
                            const ConnectOutcome& o,
                            const Topology& t,
                            int expectBoundIf,
                            bool expectSrcOnPhys)
{
    char ipbuf[INET_ADDRSTRLEN] = "(未取到)";
    bool onPhys = false;
    bool onVpn  = false;
    if (o.haveSrc)
    {
        inet_ntop(AF_INET, &o.src, ipbuf, sizeof(ipbuf));
        onPhys = AddrBelongsToIface(o.src, t.physIf);
        onVpn  = (t.osBestIf != 0) && AddrBelongsToIface(o.src, t.osBestIf);
    }
    printf("[%s] IP_BOUND_IF=%d（期望 %d），connect 后源地址 %s"
           "（物理网卡=%s，VPN 网卡=%s）\n",
           tag, o.boundIf, expectBoundIf, ipbuf,
           onPhys ? "是" : "否", onVpn ? "是" : "否");

    CHECK(o.result == BC_R_SUCCESS);
    // 主断言：钉子的去留。这是被修改的那段代码的直接输出。
    CHECK(o.boundIf == expectBoundIf);

    // 次断言：内核层面的实际后果。源地址取不到时不判失败（connect 可能因为
    // 网络原因没落地），但取到了就必须一致。
    if (o.haveSrc)
    {
        CHECK(onPhys == expectSrcOnPhys);
        if (t.osBestIf != 0 && t.osBestIf != t.physIf)
        {
            CHECK(onVpn == !expectSrcOnPhys);
        }
    }
}

// 本次修复的核心断言。VPN 抢占默认路由时，prefer-physical 必须**保持绑定**，
// 于是内核选出的源地址属于物理网卡 —— 服务端看到的就是用户真实 IP。
//
// 改动前这里必然失败：Connect() 用全局路由表的答案判 mismatch，全局表指向
// utunN，于是 setsockopt(IP_BOUND_IF, 0) 把钉子拔掉，源地址落到 VPN 上。
static void case_prefer_physical_keeps_pin_under_vpn(const Topology& t)
{
    if (!t.vpnPreempts)
    {
        SKIP("[prefer-physical] 当前机器没有\"VPN 抢占默认路由 + 物理网卡仍有路由\""
             "的拓扑（physIf=%u global=%u scopedPhys=%u），核心用例无法验证",
             t.physIf, t.globalIf, t.scopedPhys);
        return;
    }
    ConnectOutcome o = RunConnect(TT_VPN_POLICY_PREFER_PHYSICAL,
                                  (int64_t)t.physIf);
    ReportAndAssert("prefer-physical", o, t,
                    /*expectBoundIf=*/(int)t.physIf,
                    /*expectSrcOnPhys=*/true);
}

// force-physical 的行为一个字都不能变：也保持绑定、也拿到物理网卡的源地址。
static void case_force_physical_keeps_pin(const Topology& t)
{
    if (!t.vpnPreempts)
    {
        SKIP("[force-physical] 拓扑不满足，跳过");
        return;
    }
    ConnectOutcome o = RunConnect(TT_VPN_POLICY_FORCE_PHYSICAL,
                                  (int64_t)t.physIf);
    ReportAndAssert("force-physical", o, t,
                    /*expectBoundIf=*/(int)t.physIf,
                    /*expectSrcOnPhys=*/true);
}

// os 档的行为一个字都不能变：它的语义是"跟随系统路由"，所以全局表说走 VPN 就
// 该解绑跟着走，源地址落在系统默认出口那块网卡上。
//
// 注意 os 档在真实产品路径里根本不会带着 ifIndex 进来（SMPConnection::Create
// 对 os 档不查询网卡，initial_ifindex 恒为 0）；这里刻意手工喂一个 ifIndex，
// 就是为了把"万一经 Restart 带进来"那条分支也覆盖到。
static void case_os_follows_kernel_route(const Topology& t)
{
    if (!t.vpnPreempts || t.osBestIf == 0 || t.osBestIf == t.physIf)
    {
        SKIP("[os] 拓扑不满足（osBestIf=%u physIf=%u），跳过",
             t.osBestIf, t.physIf);
        return;
    }
    ConnectOutcome o = RunConnect(TT_VPN_POLICY_OS, (int64_t)t.physIf);
    // 期望被解绑：IP_BOUND_IF 归 0，源地址回到系统默认出口那块网卡。
    ReportAndAssert("os", o, t,
                    /*expectBoundIf=*/0,
                    /*expectSrcOnPhys=*/false);
}

// 没有绑定目标（ifIndex=0）时整块复核被跳过，行为与历史一致：跟随内核默认
// 路由。这条在任何机器上都能跑，用来保证测试文件本身不是"全 SKIP"。
static void case_unpinned_follows_default_route(const Topology& t)
{
    ConnectOutcome o = RunConnect(TT_VPN_POLICY_PREFER_PHYSICAL, 0);
    CHECK(o.result == BC_R_SUCCESS);
    CHECK(o.boundIf == 0);
    char ipbuf[INET_ADDRSTRLEN] = "(未取到)";
    if (o.haveSrc) inet_ntop(AF_INET, &o.src, ipbuf, sizeof(ipbuf));
    printf("[unpinned] ifIndex=0，IP_BOUND_IF=%d，源地址 %s"
           "（跟随内核默认路由）\n", o.boundIf, ipbuf);
    if (o.haveSrc && t.osBestIf != 0)
    {
        CHECK(AddrBelongsToIface(o.src, t.osBestIf));
    }
}

// 非全局可路由的对端（这里用企业 VPN 的内网网关地址）在 prefer-physical 下
// 必须**解绑** —— 这是黑洞保护那条独立判据的真实路径覆盖。硬绑在物理网卡上
// 发往一个只存在于隧道另一侧的 RFC1918 地址，包会被静默丢弃。
//
// 需要一个"全局表指向非物理网卡"的私网地址；测试从系统默认出口那块网卡自己的
// 网段里取网关地址来构造，取不到就 SKIP。
static void case_private_peer_unpins(const Topology& t)
{
    if (!t.vpnPreempts)
    {
        SKIP("[private-peer] 拓扑不满足，跳过");
        return;
    }

    // 找一个挂在 VPN 网卡上的 IPv4 地址，用它同网段的另一个地址当对端：
    // 这样全局表一定把它判给 VPN 网卡，而物理网卡的 scoped 表只有默认路由。
    char vpnName[IF_NAMESIZE] = {0};
    if (if_indextoname(t.osBestIf, vpnName) == NULL)
    {
        SKIP("[private-peer] 取不到 VPN 网卡名，跳过");
        return;
    }
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) != 0)
    {
        SKIP("[private-peer] getifaddrs 失败，跳过");
        return;
    }
    uint32_t peerV4 = 0;
    for (struct ifaddrs* p = ifa; p != NULL; p = p->ifa_next)
    {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, vpnName) != 0) continue;
        const struct sockaddr_in* sin = (const struct sockaddr_in*)p->ifa_addr;
        uint32_t host = ntohl(sin->sin_addr.s_addr);
        if ((tt_peer_address_class(p->ifa_addr, sizeof(*sin))
             & TT_PEER_CLASS_PRIVATE) == 0)
        {
            continue;  // VPN 给的不是私网地址，这个用例就不适用
        }
        peerV4 = (host & 0xFFFFFF00u) | 0x01u;  // 同 /24 的 .1
        if (peerV4 == host) peerV4 = (host & 0xFFFFFF00u) | 0x02u;
        break;
    }
    freeifaddrs(ifa);
    if (peerV4 == 0)
    {
        SKIP("[private-peer] %s 上没有私网 IPv4 地址，跳过", vpnName);
        return;
    }

    struct sockaddr_in probe;
    memset(&probe, 0, sizeof(probe));
    probe.sin_family = AF_INET;
    probe.sin_len    = sizeof(probe);
    probe.sin_addr.s_addr = htonl(peerV4);
    char peerStr[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &probe.sin_addr, peerStr, sizeof(peerStr));

    const uint32_t g = tt_route_lookup_ifindex(
        (const struct sockaddr*)&probe, sizeof(probe), 0);
    const uint32_t sc = tt_route_lookup_ifindex(
        (const struct sockaddr*)&probe, sizeof(probe), t.physIf);
    printf("[private-peer] 对端 %s：global=%u scoped(%u)=%u peerClass=0x%x\n",
           peerStr, g, t.physIf, sc,
           (unsigned)tt_peer_address_class((const struct sockaddr*)&probe,
                                           sizeof(probe)));

    if (g == TT_ROUTE_IFINDEX_UNKNOWN || g == t.physIf)
    {
        SKIP("[private-peer] 全局表把 %s 判给了物理网卡本身，构造不出该场景",
             peerStr);
        return;
    }

    // 这就是"只把 mismatch 改成 scoped 就会出的 bug"：scoped 说一致，
    // 但对端在公网上不可达。决策必须是 UNPIN。
    CHECK(sc == t.physIf);   // 先钉死 macOS scoped 查询的这个反直觉行为
    TTPinRecheckReason why = TT_PIN_REASON_OK;
    CHECK(tt_vpn_pin_recheck(
              TT_VPN_POLICY_PREFER_PHYSICAL, t.physIf, sc, g,
              tt_peer_address_class((const struct sockaddr*)&probe,
                                    sizeof(probe)),
              &why) == TT_PIN_VERDICT_UNPIN);
    CHECK(why == TT_PIN_REASON_BLACKHOLE_RISK);
    printf("[private-peer] 判定 UNPIN，reason=%s（黑洞保护生效）\n",
           tt_vpn_pin_reason_to_string(why));
}

///////////////////////////////////////////////////////////////////////////////

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);

    BCFObject runtimeCfg;
    runtimeCfg.PutInt("workerThreads", 1);
    runtimeCfg.PutInt("taskThreads", 2);
    runtimeCfg.PutInt("timerThreads", 1);
    if (Runtime::Initialize(&runtimeCfg) != BC_R_SUCCESS)
    {
        printf("Runtime::Initialize 失败\n");
        return 1;
    }

    Topology t = Probe();
    printf("=== 拓扑探测 ===\n"
           "  物理网卡 ifIndex = %u (%s)\n"
           "  系统默认出口     = %u (%s)\n"
           "  %s 全局表        = %u\n"
           "  %s scoped(物理)  = %u\n"
           "  VPN 抢占默认路由 = %s\n",
           t.physIf, IfNameOf(t.physIf).c_str(),
           t.osBestIf, IfNameOf(t.osBestIf).c_str(),
           kPublicDst, t.globalIf,
           kPublicDst, t.scopedPhys,
           t.vpnPreempts ? "是" : "否");

    case_unpinned_follows_default_route(t);
    case_prefer_physical_keeps_pin_under_vpn(t);
    case_force_physical_keeps_pin(t);
    case_os_follows_kernel_route(t);
    case_private_peer_unpins(t);

    Runtime::Destroy();

    if (g_failures != 0)
    {
        printf("UDPSenderRouteRecheck_integration_test: %d FAILURE(S), "
               "%d SKIPPED\n", g_failures, g_skipped);
        return 1;
    }
    if (g_skipped != 0)
    {
        printf("UDPSenderRouteRecheck_integration_test: ALL PASS, "
               "%d SKIPPED\n", g_skipped);
        return 2;
    }
    printf("UDPSenderRouteRecheck_integration_test: ALL PASS\n");
    return 0;
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
