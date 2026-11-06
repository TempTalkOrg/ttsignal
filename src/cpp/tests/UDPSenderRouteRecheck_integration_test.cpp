



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








static const char* kPublicDst = "34.117.59.81";



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




struct ConnectOutcome
{
    BCRESULT       result;
    int            boundIf;      
    bool           haveSrc;      
    struct in_addr src;
};



static ConnectOutcome RunConnect(TTVpnPolicy policy, int64_t boundIfIndex)
{
    ConnectOutcome out;
    memset(&out, 0, sizeof(out));
    out.boundIf = -1;

    NullHandler handler;
    BCFObject   cfg;

    
    
    UDPSender* sender = new UDPSender();
    out.result = sender->Create(NULL, Runtime::TaskMgr(), Runtime::TimerMgr(),
                       Runtime::SocketMgr(), &cfg, &handler,
                       false, false,
                       boundIfIndex);
    if (out.result != BC_R_SUCCESS)
    {
        sender->Destroy(&sender);
        return out;
    }
    sender->SetVpnPolicy(policy);

    
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
        
        out.boundIf = ReadBoundIf(fd);
        out.haveSrc = PollSourceAddr(fd, out.src, 1500);
    }

    sender->Close();
    sender->Destroy(&sender);
    return out;
}





struct Topology
{
    uint32_t physIf;      
    uint32_t osBestIf;    
    uint32_t scopedPhys;  
    uint32_t globalIf;    
    bool     vpnPreempts; 
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

    
    
    t.vpnPreempts = (t.physIf != 0 &&
                     t.globalIf != TT_ROUTE_IFINDEX_UNKNOWN &&
                     t.globalIf != t.physIf &&
                     t.scopedPhys == t.physIf);
    return t;
}







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
    
    CHECK(o.boundIf == expectBoundIf);

    
    
    if (o.haveSrc)
    {
        CHECK(onPhys == expectSrcOnPhys);
        if (t.osBestIf != 0 && t.osBestIf != t.physIf)
        {
            CHECK(onVpn == !expectSrcOnPhys);
        }
    }
}






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
                    (int)t.physIf,
                    true);
}


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
                    (int)t.physIf,
                    true);
}







static void case_os_follows_kernel_route(const Topology& t)
{
    if (!t.vpnPreempts || t.osBestIf == 0 || t.osBestIf == t.physIf)
    {
        SKIP("[os] 拓扑不满足（osBestIf=%u physIf=%u），跳过",
             t.osBestIf, t.physIf);
        return;
    }
    ConnectOutcome o = RunConnect(TT_VPN_POLICY_OS, (int64_t)t.physIf);
    
    ReportAndAssert("os", o, t,
                    0,
                    false);
}



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







static void case_private_peer_unpins(const Topology& t)
{
    if (!t.vpnPreempts)
    {
        SKIP("[private-peer] 拓扑不满足，跳过");
        return;
    }

    
    
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
            continue;  
        }
        peerV4 = (host & 0xFFFFFF00u) | 0x01u;  
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

    
    
    CHECK(sc == t.physIf);   
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




