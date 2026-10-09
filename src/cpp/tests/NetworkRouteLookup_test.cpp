///////////////////////////////////////////////////////////////////////////////
// file : NetworkRouteLookup_test.cpp
//
// Standalone unit test for tt_route_lookup_ifindex() 的 scoped / 非 scoped
// 行为。Not part of any build target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp \
//       src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//       src/cpp/tests/NetworkRouteLookup_test.cpp \
//       -o /tmp/routelookup_test && /tmp/routelookup_test
//
// 依赖极轻：NetworkRouteLookup.cpp 在 Apple 上编译成空 TU，真身在
// apple/AppleRouteLookup.cpp；Linux 换成 src/cpp/linux/LinuxRouteLookup.cpp，
// Windows 换成 src/cpp/win32/WinRouteLookup.cpp。这条查询不碰 LogQ / Runtime，
// 所以不需要 libenv.a、不需要 boringssl。
//
// 本测试**不发任何网络包**：验证"内核实际会怎么走"用的是 UDP socket 的
// connect() + getsockname()——UDP connect 只在内核里做一次路由查询并选定源
// 地址，不产生任何报文。因此测试离线可跑（无路由时相关用例自动 SKIP）。
//
// 覆盖：
//  1. 参数校验（NULL / 长度 0 / 非 IP 地址族），scoped 与非 scoped 一致；
//  2. scope=0 的行为与"未绑定 socket 的内核实际出口"一致 —— 这就是
//     "scope=0 时行为与改动前一致"的可执行断言；
//  3. scope=物理网卡时，查询结果 == 该物理网卡，且与"IP_BOUND_IF 绑到该网卡
//     后内核实际选的出口"一致 —— 这是本次修复的核心断言，修复前必然失败：
//     VPN 抢占默认路由时旧实现（只有 RTF_UP、不带 RTF_IFSCOPE）会答 utunN；
//  4. scope=没有到公网路由的网卡（lo0）时返回 TT_ROUTE_IFINDEX_NO_ROUTE，
//     而不是被折叠进含义模糊的 0 —— 保证"真实不可达"仍然拦得住；
//  5. scoped 查询**忽略**挂在别的网卡上的非 scoped 专用路由 —— 这是"黑洞判定
//     不能只看 scoped 结果"的全部依据，钉在这一层而不是留在上层注释里；
//  6. sa_len 为 0 的 sockaddr（本仓 BCSockAddrS 的实际形态）必须与 sa_len 正确
//     填写时得到相同答案 —— 否则 RTM_GET 静默退化成"查默认路由"。
///////////////////////////////////////////////////////////////////////////////
#include "NetworkRouteLookup.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

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

// 测试用目的地址。刻意用一个公网 IP 字面量而不是域名：DNS 本身可能走 VPN，
// 会把"路由查询"和"名字解析"两件事混在一起。1.1.1.1 只是一个"有默认路由才
// 到得了"的目标，测试不会向它发任何包。
static const char* kPublicDst = "1.1.1.1";

static void MakeV4(struct sockaddr_in& sin, const char* ip)
{
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_len    = sizeof(sin);
    sin.sin_port   = htons(443);
    inet_pton(AF_INET, ip, &sin.sin_addr);
}

// 把一个本地 IPv4 地址映射回 ifIndex。getsockname() 只给地址不给网卡号，
// 需要靠 getifaddrs() 反查。
static uint32_t IfIndexOfLocalV4(const struct in_addr& addr)
{
    struct ifaddrs* head = NULL;
    if (getifaddrs(&head) != 0)
    {
        return 0;
    }

    uint32_t found = 0;
    for (struct ifaddrs* it = head; it != NULL; it = it->ifa_next)
    {
        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        const struct sockaddr_in* sin =
            reinterpret_cast<const struct sockaddr_in*>(it->ifa_addr);
        if (sin->sin_addr.s_addr == addr.s_addr)
        {
            found = if_nametoindex(it->ifa_name);
            break;
        }
    }
    freeifaddrs(head);
    return found;
}

// 内核 ground truth：建一个 UDP socket，可选地用 IP_BOUND_IF 硬绑到
// @p boundIf，connect() 到 @p dst，然后看内核给它选了哪块网卡的源地址。
// UDP connect 不发包，只做一次路由查询 + 源地址选择——正是我们要对照的东西。
// 返回 0 表示内核认为不可达（connect 失败）或反查不到网卡。
static uint32_t KernelEgressIfIndex(const struct sockaddr_in& dst,
                                    uint32_t boundIf)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return 0;
    }

#if defined(IP_BOUND_IF)
    if (boundIf != 0)
    {
        unsigned int idx = boundIf;
        if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof(idx)) != 0)
        {
            close(fd);
            return 0;
        }
    }
#else
    if (boundIf != 0)
    {
        close(fd);
        return 0;  // 平台没有硬绑原语，调用方会 SKIP
    }
#endif

    if (connect(fd, reinterpret_cast<const struct sockaddr*>(&dst),
                sizeof(dst)) != 0)
    {
        close(fd);
        return 0;
    }

    struct sockaddr_in local;
    socklen_t          len = sizeof(local);
    memset(&local, 0, sizeof(local));
    if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&local), &len) != 0)
    {
        close(fd);
        return 0;
    }
    close(fd);

    return IfIndexOfLocalV4(local.sin_addr);
}

// 挑一块"物理网卡"：UP + RUNNING、非 loopback、非点对点（排掉 utunN）、
// 有一个非 link-local 的 IPv4 地址。本机上就是 en0。
static uint32_t PickPhysicalIfIndex(char* nameOut, size_t nameLen)
{
    struct ifaddrs* head = NULL;
    if (getifaddrs(&head) != 0)
    {
        return 0;
    }

    uint32_t found = 0;
    for (struct ifaddrs* it = head; it != NULL; it = it->ifa_next)
    {
        if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET)
        {
            continue;
        }
        if ((it->ifa_flags & IFF_UP) == 0 ||
            (it->ifa_flags & IFF_RUNNING) == 0)
        {
            continue;
        }
        if ((it->ifa_flags & IFF_LOOPBACK) != 0 ||
            (it->ifa_flags & IFF_POINTOPOINT) != 0)
        {
            continue;
        }
        const struct sockaddr_in* sin =
            reinterpret_cast<const struct sockaddr_in*>(it->ifa_addr);
        uint32_t host = ntohl(sin->sin_addr.s_addr);
        if ((host & 0xFFFF0000u) == 0xA9FE0000u)
        {
            continue;  // 169.254/16 link-local，没有默认路由
        }
        found = if_nametoindex(it->ifa_name);
        if (found != 0)
        {
            snprintf(nameOut, nameLen, "%s", it->ifa_name);
            break;
        }
    }
    freeifaddrs(head);
    return found;
}

static bool PlatformHasRealImplementation()
{
#if defined(__APPLE__) && defined(TARGET_OS_OSX) && TARGET_OS_OSX
    return true;
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(OS_ANDROID)
    return true;
#elif defined(_WIN32)
    return true;
#else
    return false;
#endif
}

// scope 参数只在 macOS 上被真正兑现；Linux / Windows 刻意忽略它
// （见 NetworkRouteLookup.h 的平台表）。
static bool PlatformHonoursScope()
{
#if defined(__APPLE__) && defined(TARGET_OS_OSX) && TARGET_OS_OSX
    return true;
#else
    return false;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// 1. 参数校验
///////////////////////////////////////////////////////////////////////////////

static void test_argument_validation()
{
    struct sockaddr_in dst;
    MakeV4(dst, kPublicDst);
    const struct sockaddr* sa =
        reinterpret_cast<const struct sockaddr*>(&dst);

    // NULL / 长度 0 —— scoped 与非 scoped 都必须是 UNKNOWN，绝不能返回
    // NO_ROUTE（那会让 force-physical 把"调用方传错参"当成"网络不可达"）。
    CHECK(tt_route_lookup_ifindex(NULL, sizeof(dst), 0) ==
          TT_ROUTE_IFINDEX_UNKNOWN);
    CHECK(tt_route_lookup_ifindex(NULL, sizeof(dst), 14) ==
          TT_ROUTE_IFINDEX_UNKNOWN);
    CHECK(tt_route_lookup_ifindex(sa, 0, 0) == TT_ROUTE_IFINDEX_UNKNOWN);
    CHECK(tt_route_lookup_ifindex(sa, 0, 14) == TT_ROUTE_IFINDEX_UNKNOWN);

    // 非 IP 地址族
    struct sockaddr_in bogus;
    MakeV4(bogus, kPublicDst);
    bogus.sin_family = AF_UNIX;
    const struct sockaddr* bsa =
        reinterpret_cast<const struct sockaddr*>(&bogus);
    CHECK(tt_route_lookup_ifindex(bsa, sizeof(bogus), 0) ==
          TT_ROUTE_IFINDEX_UNKNOWN);
    CHECK(tt_route_lookup_ifindex(bsa, sizeof(bogus), 14) ==
          TT_ROUTE_IFINDEX_UNKNOWN);
}

///////////////////////////////////////////////////////////////////////////////
// 2. scope=0：与改动前同义——问的是全局路由表
///////////////////////////////////////////////////////////////////////////////

static void test_unscoped_matches_unbound_socket()
{
    if (!PlatformHasRealImplementation())
    {
        SKIP("本平台是返回 UNKNOWN 的桩实现");
        return;
    }

    struct sockaddr_in dst;
    MakeV4(dst, kPublicDst);

    uint32_t kernel = KernelEgressIfIndex(dst, /*boundIf=*/0);
    if (kernel == 0)
    {
        SKIP("本机当前到 %s 没有可用路由（离线？），跳过", kPublicDst);
        return;
    }

    uint32_t looked = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst),
        /*scope_ifindex=*/0);

    printf("  [scope=0] 查询=%u，未绑定 socket 的内核实际出口=%u\n",
           looked, kernel);

    // scope=0 必须仍然回答全局路由表的那块网卡——这正是改动前的语义。
    CHECK(looked == kernel);
    // scope=0 永远不返回 NO_ROUTE 哨兵（契约见 NetworkRouteLookup.h）。
    CHECK(looked != TT_ROUTE_IFINDEX_NO_ROUTE);
}

///////////////////////////////////////////////////////////////////////////////
// 3. scope=物理网卡：本次修复的核心断言
///////////////////////////////////////////////////////////////////////////////

static void test_scoped_to_physical_resolves_physical()
{
    if (!PlatformHonoursScope())
    {
        SKIP("本平台刻意忽略 scope 参数（见 NetworkRouteLookup.h 平台表）");
        return;
    }

    char     ifname[IF_NAMESIZE] = "";
    uint32_t phys = PickPhysicalIfIndex(ifname, sizeof(ifname));
    if (phys == 0)
    {
        SKIP("找不到带 IPv4 地址的物理网卡，跳过");
        return;
    }

    struct sockaddr_in dst;
    MakeV4(dst, kPublicDst);

    uint32_t kernel = KernelEgressIfIndex(dst, /*boundIf=*/phys);
    if (kernel == 0)
    {
        SKIP("硬绑到 %s(ifIndex=%u) 后内核认为 %s 不可达，跳过",
             ifname, phys, kPublicDst);
        return;
    }

    uint32_t scoped = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst),
        /*scope_ifindex=*/phys);
    uint32_t global = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst),
        /*scope_ifindex=*/0);

    printf("  [scope=%s(%u)] scoped 查询=%u，全局查询=%u，"
           "IP_BOUND_IF 后内核实际出口=%u\n",
           ifname, phys, scoped, global, kernel);

    // (a) IP_BOUND_IF 是硬绑：内核实际就走这块网卡。
    CHECK(kernel == phys);
    // (b) scoped 查询必须给出同一个答案。修复前这里拿到的是全局答案，
    //     VPN 抢占默认路由时就是 utunN，断言必然失败。
    CHECK(scoped == phys);
    // (c) 更强的表述：scoped 查询与内核对同一条 socket 的判断完全一致。
    CHECK(scoped == kernel);

    if (global != phys && global != TT_ROUTE_IFINDEX_UNKNOWN)
    {
        printf("  >> 本机正处在缺陷复现环境：全局路由表被 ifIndex=%u 抢占，"
               "非 scoped 查询会误判 force-physical 失败\n", global);
    }
    else
    {
        printf("  >> 提示：本机全局默认路由就在物理网卡上，"
               "scoped 与非 scoped 结果相同，(b) 这条断言此时区分度较弱\n");
    }
}

///////////////////////////////////////////////////////////////////////////////
// 4. scope=无路由网卡：NO_ROUTE 必须与 UNKNOWN 区分开
///////////////////////////////////////////////////////////////////////////////

static void test_scoped_to_routeless_iface_reports_no_route()
{
    if (!PlatformHonoursScope())
    {
        SKIP("本平台刻意忽略 scope 参数");
        return;
    }

    uint32_t lo = if_nametoindex("lo0");
    if (lo == 0)
    {
        lo = if_nametoindex("lo");
    }
    if (lo == 0)
    {
        SKIP("找不到 loopback 网卡，跳过");
        return;
    }

    struct sockaddr_in dst;
    MakeV4(dst, kPublicDst);

    uint32_t scoped = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst),
        /*scope_ifindex=*/lo);

    printf("  [scope=loopback(%u)] 查询=0x%X\n", lo, scoped);

    // loopback 的 scoped 路由表里没有到公网的路由。这是**确定性**的否定
    // 答案，必须报 NO_ROUTE：折叠成 0（UNKNOWN）会让 _RouteRecheck 把真正
    // 的不可达当成"查不了"放行。
    CHECK(scoped == TT_ROUTE_IFINDEX_NO_ROUTE);
    // 顺带确认内核也认为不可达，两边结论一致。
    CHECK(KernelEgressIfIndex(dst, lo) == 0);
}

///////////////////////////////////////////////////////////////////////////////
// 5. scoped 查询会忽略"挂在别的网卡上的非 scoped 专用路由"
//
// 这条是 UDPSender/TcpChannel 的黑洞保护为什么**不能**只看 scoped 结果的
// 全部依据，所以必须在这一层钉死，而不是留在上层注释里。
//
// 场景：VPN / TUN 代理会把一段地址（企业内网的 RFC1918 段、Clash 的 fake-IP
// 段）的路由装在自己的隧道网卡上。这些地址在公网上不可达。
//   * 全局查询答"隧道网卡" —— 正确，只有它送得到；
//   * scoped 到物理网卡的查询答"物理网卡" —— 因为它只看物理网卡自己的 scoped
//     表，那里有一条默认路由就够了，别人网卡上的专用路由它根本不看。
//
// 于是"钉在物理网卡上会不会打进黑洞"这个问题，scoped 查询结构上答不了：它
// 恒答"一致"。上层必须另配一条判据（地址段 + 全局表），见
// VpnPolicy.h 里 tt_vpn_pin_recheck() 的注释。
///////////////////////////////////////////////////////////////////////////////

static void test_scoped_ignores_offiface_host_routes()
{
    if (!PlatformHonoursScope())
    {
        SKIP("本平台刻意忽略 scope 参数");
        return;
    }

    char     ifname[IF_NAMESIZE] = "";
    uint32_t phys = PickPhysicalIfIndex(ifname, sizeof(ifname));
    if (phys == 0)
    {
        SKIP("找不到带 IPv4 地址的物理网卡，跳过");
        return;
    }

    // 找一个"全局表判给非物理网卡"的目的地址。优先用隧道网卡自己的网段
    // （企业 VPN 环境下必然存在），这样不依赖任何写死的 IP。
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) != 0)
    {
        SKIP("getifaddrs 失败");
        return;
    }
    uint32_t candidate = 0;
    char     viaName[IF_NAMESIZE] = "";
    for (struct ifaddrs* p = ifa; p != NULL; p = p->ifa_next)
    {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(p->ifa_name, ifname) == 0) continue;          // 跳过物理网卡
        if (strncmp(p->ifa_name, "lo", 2) == 0) continue;        // 跳过 loopback
        const struct sockaddr_in* sin = (const struct sockaddr_in*)p->ifa_addr;
        uint32_t host = ntohl(sin->sin_addr.s_addr);
        uint32_t peer = (host & 0xFFFFFF00u) | 0x01u;            // 同 /24 的 .1
        if (peer == host) peer = (host & 0xFFFFFF00u) | 0x02u;
        candidate = peer;
        snprintf(viaName, sizeof(viaName), "%s", p->ifa_name);
        break;
    }
    freeifaddrs(ifa);

    if (candidate == 0)
    {
        SKIP("本机除物理网卡与 loopback 外没有别的带 IPv4 的网卡"
             "（没挂 VPN / TUN 代理），构造不出该场景");
        return;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_len    = sizeof(dst);
    dst.sin_port   = htons(443);
    dst.sin_addr.s_addr = htonl(candidate);
    char dstStr[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &dst.sin_addr, dstStr, sizeof(dstStr));

    uint32_t global = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst), 0);
    uint32_t scoped = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst), phys);

    printf("  目的 %s（取自 %s 的网段）：全局查询=%u，scoped(%s=%u)=%u\n",
           dstStr, viaName, global, ifname, phys, scoped);

    if (global == TT_ROUTE_IFINDEX_UNKNOWN || global == phys)
    {
        SKIP("全局表把 %s 判给了物理网卡本身，构造不出该场景", dstStr);
        return;
    }

    // 核心断言：两次查询给出**不同**答案，且 scoped 的答案是"物理网卡自己"。
    // 也就是说 scoped 查询看不见 %s 上那条专用路由。
    CHECK(global != phys);
    CHECK(scoped == phys);
    printf("  >> 已确认：scoped 查询看不见 %s 上的专用路由，恒答\"绑定的网卡\"。\n"
           "     所以黑洞判定不能只看 scoped —— 它对这类地址没有区分度。\n",
           viaName);
}

///////////////////////////////////////////////////////////////////////////////
// 6. sa_len 为 0 的 sockaddr 必须得到与 sa_len 正确填写时**相同**的答案
//
// 这条是回归测试，对应一个隐蔽到极点的缺陷：PF_ROUTE 的报文体是一串变长
// sockaddr，内核**完全依赖每个条目自己的 sa_len** 断句。sa_len 为 0 时内核读
// 不到地址字节，RTM_GET 退化成"查默认路由"——查询不报错，而是静默地回答了另一
// 个问题。
//
// 为什么这不是假想情况：本仓 BCSockAddrS 的所有构造函数（deps/env 的
// bc_sockaddr_fromin / _fromin6 等）都把 sin_len 的赋值放在
// #ifdef BC_PLATFORM_HAVESALEN 里，而这个宏**全仓从未定义**。也就是说
// TcpChannel::peer_addr_ / UDPSender 传下来的 sockaddr 里 sa_len 恒为 0，
// 而这两处正是本模块唯一的两个生产调用方。
//
// 修复前本用例必然失败，实测（en0=14 / utun4=22，GlobalProtect 全隧道）：
//   sa_len=16 : 127.0.0.1 -> global=1(lo0)  scoped(en0)=1(lo0)
//   sa_len=0  : 127.0.0.1 -> global=22      scoped(en0)=14      ← 默认路由
///////////////////////////////////////////////////////////////////////////////

static void test_zero_salen_matches_correct_salen()
{
    char     ifname[IF_NAMESIZE] = "";
    uint32_t phys = PickPhysicalIfIndex(ifname, sizeof(ifname));

    // 刻意挑一批"答案各不相同"的目的地址：全用公网地址的话，sa_len=0 退化成
    // 的"默认路由"答案恰好和正确答案相同，用例就没有区分度了。环回是关键的
    // 一个 —— 它的正确答案是 lo0，与任何默认路由都不同。
    const char* dsts[] = { "127.0.0.1", kPublicDst, "255.255.255.255" };

    for (size_t i = 0; i < sizeof(dsts) / sizeof(dsts[0]); ++i)
    {
        struct sockaddr_in withLen;
        struct sockaddr_in zeroLen;
        MakeV4(withLen, dsts[i]);          // MakeV4 会填 sin_len
        MakeV4(zeroLen, dsts[i]);
        zeroLen.sin_len = 0;               // 模拟 BCSockAddrS 的实际形态

        const uint32_t scopes[] = { 0u, phys };
        for (size_t k = 0; k < sizeof(scopes) / sizeof(scopes[0]); ++k)
        {
            if (k == 1 && phys == 0)
            {
                continue;                  // 没物理网卡就只测 scope=0
            }
            uint32_t a = tt_route_lookup_ifindex(
                reinterpret_cast<const struct sockaddr*>(&withLen),
                sizeof(withLen), scopes[k]);
            uint32_t b = tt_route_lookup_ifindex(
                reinterpret_cast<const struct sockaddr*>(&zeroLen),
                sizeof(zeroLen), scopes[k]);
            printf("  %-16s scope=%-3u  sa_len=16 -> %-10u  sa_len=0 -> %u\n",
                   dsts[i], scopes[k], a, b);
            // 核心断言：调用方填不填 sa_len，答案必须一样。
            CHECK(a == b);
        }
    }

    // 环回是最能暴露问题的一格，单独再钉一次：它的正确答案是 lo0，
    // 既不是物理网卡也不是任何隧道网卡。
    uint32_t lo = if_nametoindex("lo0");
    if (lo == 0) lo = if_nametoindex("lo");
    if (lo != 0 && phys != 0 && PlatformHonoursScope())
    {
        struct sockaddr_in loop;
        MakeV4(loop, "127.0.0.1");
        loop.sin_len = 0;
        uint32_t g = tt_route_lookup_ifindex(
            reinterpret_cast<const struct sockaddr*>(&loop), sizeof(loop), 0);
        uint32_t sc = tt_route_lookup_ifindex(
            reinterpret_cast<const struct sockaddr*>(&loop), sizeof(loop), phys);
        printf("  127.0.0.1（sa_len=0）：global=%u scoped(%s=%u)=%u，"
               "loopback ifIndex=%u\n", g, ifname, phys, sc, lo);
        CHECK(g == lo);
        // scoped 到物理网卡时，内核照样把环回判给 lo0 —— 这正是
        // prefer-physical 必须放弃绑定、force-physical 必须明确失败的依据。
        CHECK(sc == lo);
        CHECK(sc != phys);
    }
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

int main()
{
    printf("== NetworkRouteLookup_test ==\n");

    printf("[1] 参数校验\n");
    test_argument_validation();

    printf("[2] scope=0 与未绑定 socket 的内核出口一致\n");
    test_unscoped_matches_unbound_socket();

    printf("[3] scope=物理网卡 → 查出物理路由\n");
    test_scoped_to_physical_resolves_physical();

    printf("[4] scope=无路由网卡 → NO_ROUTE\n");
    test_scoped_to_routeless_iface_reports_no_route();

    printf("[5] scoped 查询忽略别的网卡上的专用路由（黑洞判据的依据）\n");
    test_scoped_ignores_offiface_host_routes();

    printf("[6] sa_len=0 与 sa_len 正确填写必须同答案（回归）\n");
    test_zero_salen_matches_correct_salen();

    if (g_failures == 0)
    {
        printf("ALL PASS（skipped=%d）\n", g_skipped);
        return 0;
    }
    printf("%d FAILURE(S)（skipped=%d）\n", g_failures, g_skipped);
    return 1;
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
