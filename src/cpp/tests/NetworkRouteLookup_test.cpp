


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








static const char* kPublicDst = "1.1.1.1";

static void MakeV4(struct sockaddr_in& sin, const char* ip)
{
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_len    = sizeof(sin);
    sin.sin_port   = htons(443);
    inet_pton(AF_INET, ip, &sin.sin_addr);
}



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
        return 0;  
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
            continue;  
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



static bool PlatformHonoursScope()
{
#if defined(__APPLE__) && defined(TARGET_OS_OSX) && TARGET_OS_OSX
    return true;
#else
    return false;
#endif
}





static void test_argument_validation()
{
    struct sockaddr_in dst;
    MakeV4(dst, kPublicDst);
    const struct sockaddr* sa =
        reinterpret_cast<const struct sockaddr*>(&dst);

    
    
    CHECK(tt_route_lookup_ifindex(NULL, sizeof(dst), 0) ==
          TT_ROUTE_IFINDEX_UNKNOWN);
    CHECK(tt_route_lookup_ifindex(NULL, sizeof(dst), 14) ==
          TT_ROUTE_IFINDEX_UNKNOWN);
    CHECK(tt_route_lookup_ifindex(sa, 0, 0) == TT_ROUTE_IFINDEX_UNKNOWN);
    CHECK(tt_route_lookup_ifindex(sa, 0, 14) == TT_ROUTE_IFINDEX_UNKNOWN);

    
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





static void test_unscoped_matches_unbound_socket()
{
    if (!PlatformHasRealImplementation())
    {
        SKIP("本平台是返回 UNKNOWN 的桩实现");
        return;
    }

    struct sockaddr_in dst;
    MakeV4(dst, kPublicDst);

    uint32_t kernel = KernelEgressIfIndex(dst, 0);
    if (kernel == 0)
    {
        SKIP("本机当前到 %s 没有可用路由（离线？），跳过", kPublicDst);
        return;
    }

    uint32_t looked = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst),
        0);

    printf("  [scope=0] 查询=%u，未绑定 socket 的内核实际出口=%u\n",
           looked, kernel);

    
    CHECK(looked == kernel);
    
    CHECK(looked != TT_ROUTE_IFINDEX_NO_ROUTE);
}





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

    uint32_t kernel = KernelEgressIfIndex(dst, phys);
    if (kernel == 0)
    {
        SKIP("硬绑到 %s(ifIndex=%u) 后内核认为 %s 不可达，跳过",
             ifname, phys, kPublicDst);
        return;
    }

    uint32_t scoped = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst),
        phys);
    uint32_t global = tt_route_lookup_ifindex(
        reinterpret_cast<const struct sockaddr*>(&dst), sizeof(dst),
        0);

    printf("  [scope=%s(%u)] scoped 查询=%u，全局查询=%u，"
           "IP_BOUND_IF 后内核实际出口=%u\n",
           ifname, phys, scoped, global, kernel);

    
    CHECK(kernel == phys);
    
    
    CHECK(scoped == phys);
    
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
        lo);

    printf("  [scope=loopback(%u)] 查询=0x%X\n", lo, scoped);

    
    
    
    CHECK(scoped == TT_ROUTE_IFINDEX_NO_ROUTE);
    
    CHECK(KernelEgressIfIndex(dst, lo) == 0);
}


















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
        if (strcmp(p->ifa_name, ifname) == 0) continue;          
        if (strncmp(p->ifa_name, "lo", 2) == 0) continue;        
        const struct sockaddr_in* sin = (const struct sockaddr_in*)p->ifa_addr;
        uint32_t host = ntohl(sin->sin_addr.s_addr);
        uint32_t peer = (host & 0xFFFFFF00u) | 0x01u;            
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

    
    
    CHECK(global != phys);
    CHECK(scoped == phys);
    printf("  >> 已确认：scoped 查询看不见 %s 上的专用路由，恒答\"绑定的网卡\"。\n"
           "     所以黑洞判定不能只看 scoped —— 它对这类地址没有区分度。\n",
           viaName);
}




















static void test_zero_salen_matches_correct_salen()
{
    char     ifname[IF_NAMESIZE] = "";
    uint32_t phys = PickPhysicalIfIndex(ifname, sizeof(ifname));

    
    
    
    const char* dsts[] = { "127.0.0.1", kPublicDst, "255.255.255.255" };

    for (size_t i = 0; i < sizeof(dsts) / sizeof(dsts[0]); ++i)
    {
        struct sockaddr_in withLen;
        struct sockaddr_in zeroLen;
        MakeV4(withLen, dsts[i]);          
        MakeV4(zeroLen, dsts[i]);
        zeroLen.sin_len = 0;               

        const uint32_t scopes[] = { 0u, phys };
        for (size_t k = 0; k < sizeof(scopes) / sizeof(scopes[0]); ++k)
        {
            if (k == 1 && phys == 0)
            {
                continue;                  
            }
            uint32_t a = tt_route_lookup_ifindex(
                reinterpret_cast<const struct sockaddr*>(&withLen),
                sizeof(withLen), scopes[k]);
            uint32_t b = tt_route_lookup_ifindex(
                reinterpret_cast<const struct sockaddr*>(&zeroLen),
                sizeof(zeroLen), scopes[k]);
            printf("  %-16s scope=%-3u  sa_len=16 -> %-10u  sa_len=0 -> %u\n",
                   dsts[i], scopes[k], a, b);
            
            CHECK(a == b);
        }
    }

    
    
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
        
        
        CHECK(sc == lo);
        CHECK(sc != phys);
    }
}





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




