
























#include "WinIpChangeMonitor.h"

#if !defined(_WIN32)
#  error "WinIpChangeMonitor is Windows-only."
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Monitor {
    TTPathChangeCallback     callback   = nullptr;
    void*                    userdata   = nullptr;
    HANDLE                   notify_h   = nullptr;
    std::atomic<bool>        stop       { false };

    
    
    
    
    
    
    
    
    
    std::mutex               sig_mtx;
    std::string              last_sig;          

    
    
    std::thread              worker;
    std::mutex               mtx;
    std::condition_variable  cv;
    bool                     pending    { false };

    
    
    TTVpnPolicy              policy     = TT_VPN_POLICY_PREFER_PHYSICAL;
};








static bool IsPhysicalAdapter(NET_IFINDEX ifIndex)
{
    MIB_IF_ROW2 row;
    memset(&row, 0, sizeof(row));
    row.InterfaceIndex = ifIndex;
    if (GetIfEntry2(&row) != NO_ERROR) {
        return false;
    }
    if (row.InterfaceAndOperStatusFlags.HardwareInterface == 0) {
        return false;
    }
    switch (row.Type) {
        case IF_TYPE_TUNNEL:
        case IF_TYPE_PPP:
        case IF_TYPE_SOFTWARE_LOOPBACK:
            return false;
        default:
            break;
    }
    return true;
}


static int64_t QueryBestPhysicalIfIndex()
{
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return 0;
    }
    int64_t  best       = 0;
    ULONG    bestMetric = 0xFFFFFFFFu;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.OperStatus != IfOperStatusUp) continue;
        if (!IsPhysicalAdapter(row.InterfaceIndex)) continue;

        
        
        MIB_IPINTERFACE_ROW ipRow;
        memset(&ipRow, 0, sizeof(ipRow));
        ipRow.Family         = AF_INET;
        ipRow.InterfaceIndex = row.InterfaceIndex;
        ULONG metric = 0xFFFFFFFEu;
        if (GetIpInterfaceEntry(&ipRow) == NO_ERROR) {
            metric = ipRow.Metric;
        }
        if (best == 0 || metric < bestMetric) {
            best       = (int64_t)row.InterfaceIndex;
            bestMetric = metric;
        }
    }
    FreeMibTable(table);
    return best;
}





static int64_t QueryBestIfIndexOsOrder()
{
    struct sockaddr_in dst4 = {};
    dst4.sin_family = AF_INET;
    dst4.sin_addr.s_addr = INADDR_ANY;
    DWORD ifx = 0;
    if (GetBestInterfaceEx((struct sockaddr*)&dst4, &ifx) == NO_ERROR
        && ifx != 0) {
        return (int64_t)ifx;
    }

    struct sockaddr_in6 dst6 = {};
    dst6.sin6_family = AF_INET6;
    if (GetBestInterfaceEx((struct sockaddr*)&dst6, &ifx) == NO_ERROR
        && ifx != 0) {
        return (int64_t)ifx;
    }
    return 0;
}







static int64_t QueryBestIfIndex(TTVpnPolicy policy)
{
    const int64_t osBest = QueryBestIfIndexOsOrder();
    if (policy == TT_VPN_POLICY_OS) {
        return osBest;
    }
    if (osBest > 0 && IsPhysicalAdapter((NET_IFINDEX)osBest)) {
        return osBest;
    }
    const int64_t phys = QueryBestPhysicalIfIndex();
    if (phys > 0) {
        return phys;
    }
    return (policy == TT_VPN_POLICY_FORCE_PHYSICAL) ? 0 : osBest;
}





static std::string CollectInterfaceIPs(int64_t ifIndex)
{
    if (ifIndex <= 0) return std::string();

    PMIB_UNICASTIPADDRESS_TABLE table = nullptr;
    if (GetUnicastIpAddressTable(AF_UNSPEC, &table) != NO_ERROR || !table) {
        return std::string();
    }

    std::vector<std::string> addrs;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_UNICASTIPADDRESS_ROW& row = table->Table[i];
        if ((int64_t)row.InterfaceIndex != ifIndex) continue;

        char buf[INET6_ADDRSTRLEN] = {0};
        const SOCKADDR_INET& a = row.Address;
        if (a.si_family == AF_INET) {
            if (!inet_ntop(AF_INET, (void*)&a.Ipv4.sin_addr,
                           buf, sizeof(buf))) continue;
        } else if (a.si_family == AF_INET6) {
            if (!inet_ntop(AF_INET6, (void*)&a.Ipv6.sin6_addr,
                           buf, sizeof(buf))) continue;
            
            
            
            const UCHAR* b = a.Ipv6.sin6_addr.s6_addr;
            if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) continue;
        } else {
            continue;
        }
        addrs.emplace_back(buf);
    }
    FreeMibTable(table);

    std::sort(addrs.begin(), addrs.end());
    std::string out;
    out.reserve(addrs.size() * 16);
    for (const auto& s : addrs) {
        if (!out.empty()) out.push_back(',');
        out.append(s);
    }
    return out;
}

static std::string MakeSignature(int64_t ifIndex, const std::string& ips)
{
    char head[32];
    snprintf(head, sizeof(head), "%lld|", (long long)ifIndex);
    std::string sig = head;
    sig.append(ips);
    return sig;
}

static void DescribeIfIndex(int64_t ifx, char* out, size_t cap)
{
    
    
    MIB_IF_ROW2 row = {};
    row.InterfaceIndex = (NET_IFINDEX)ifx;
    if (GetIfEntry2(&row) == NO_ERROR) {
        
        char alias[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, row.Alias, -1, alias,
                            sizeof(alias), nullptr, nullptr);
        snprintf(out, cap, "best (%s, ifx=%lld)",
                 alias[0] ? alias : "?", (long long)ifx);
        return;
    }
    snprintf(out, cap, "best (ifx=%lld)", (long long)ifx);
}


static void WorkerLoop(Monitor* self)
{
    for (;;) {
        std::unique_lock<std::mutex> lk(self->mtx);
        self->cv.wait(lk, [self] {
            return self->stop.load() || self->pending;
        });
        if (self->stop.load()) return;
        self->pending = false;
        lk.unlock();

        
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        
        {
            std::lock_guard<std::mutex> g(self->mtx);
            self->pending = false;
        }
        if (self->stop.load()) return;

        int64_t ifx = QueryBestIfIndex(self->policy);
        if (ifx <= 0) continue;

        std::string sig = MakeSignature(ifx, CollectInterfaceIPs(ifx));
        {
            std::lock_guard<std::mutex> lock(self->sig_mtx);
            if (self->last_sig == sig) continue;
            self->last_sig = sig;
        }

        char desc[160];
        DescribeIfIndex(ifx, desc, sizeof(desc));
        self->callback(self->userdata, ifx, desc);
    }
}



static VOID WINAPI OnIpIfChange(PVOID context,
                                PMIB_IPINTERFACE_ROW ,
                                MIB_NOTIFICATION_TYPE )
{
    Monitor* self = (Monitor*)context;
    if (!self || self->stop.load()) return;

    {
        std::lock_guard<std::mutex> g(self->mtx);
        self->pending = true;
    }
    self->cv.notify_one();
}

} 

extern "C" {

TTNetworkMonitorRef tt_netmon_start(const TTNetworkMonitorOptions* options,
                                    TTPathChangeCallback cb,
                                    void* userdata)
{
    if (!cb) return nullptr;
    Monitor* self = new Monitor();
    self->callback = cb;
    self->userdata = userdata;
    
    
    
    
    
    
    
    
    if (options == nullptr) {
        self->policy = tt_vpn_policy_platform_default();
    } else {
        self->policy = tt_vpn_policy_resolve(
            (TTVpnPolicy)options->vpnPolicy,
            1, options->bypassVpn,
            tt_vpn_policy_platform_default(),
            nullptr);
    }
    self->worker   = std::thread(WorkerLoop, self);

    
    
    int64_t init = QueryBestIfIndex(self->policy);
    if (init > 0) {
        {
            std::lock_guard<std::mutex> lock(self->sig_mtx);
            self->last_sig = MakeSignature(init, CollectInterfaceIPs(init));
        }
        char desc[160];
        DescribeIfIndex(init, desc, sizeof(desc));
        cb(userdata, init, desc);
    }

    DWORD r = NotifyIpInterfaceChange(AF_UNSPEC,
                                      (PIPINTERFACE_CHANGE_CALLBACK)
                                          OnIpIfChange,
                                      self,
                                      FALSE,           
                                      &self->notify_h);
    if (r != NO_ERROR) {
        
        
        self->notify_h = nullptr;
    }
    return (TTNetworkMonitorRef)self;
}

void tt_netmon_stop(TTNetworkMonitorRef ref)
{
    Monitor* self = (Monitor*)ref;
    if (!self) return;
    self->stop.store(true, std::memory_order_release);
    if (self->notify_h) {
        CancelMibChangeNotify2(self->notify_h);
        self->notify_h = nullptr;
    }
    self->cv.notify_all();
    if (self->worker.joinable()) self->worker.join();
    delete self;
}

int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    TTVpnPolicy policy = (TTVpnPolicy)vpnPolicy;
    if (policy != TT_VPN_POLICY_OS &&
        policy != TT_VPN_POLICY_PREFER_PHYSICAL &&
        policy != TT_VPN_POLICY_FORCE_PHYSICAL) {
        policy = tt_vpn_policy_platform_default();
    }
    return QueryBestIfIndex(policy);
}

int64_t tt_netmon_query_default_ifindex(void)
{
    return tt_netmon_query_default_ifindex_ex(
        (int)tt_vpn_policy_platform_default());
}

} 
