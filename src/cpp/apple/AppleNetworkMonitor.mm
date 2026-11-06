







#include "AppleNetworkMonitor.h"

#if !defined(__APPLE__)
#  error "AppleNetworkMonitor is Apple-platform only (iOS + macOS)."
#endif

#import <Foundation/Foundation.h>
#import <Network/Network.h>
#import <TargetConditionals.h>
#import <arpa/inet.h>
#import <ifaddrs.h>
#import <net/if.h>
#import <netinet/in.h>
#import <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace {


struct Monitor {
    nw_path_monitor_t       monitor      = nullptr;
    dispatch_queue_t        queue        = nullptr;
    TTPathChangeCallback    callback     = nullptr;
    void*                   userdata     = nullptr;
    
    
    
    
    
    TTVpnPolicy             policy       = TT_VPN_POLICY_PREFER_PHYSICAL;
    
    
    
    TTPathLogCallback       raw_log_fn   = nullptr;
    void*                   raw_log_ctx  = nullptr;
    
    
    
    
    
    
    
    std::mutex              sig_mutex;
    std::string             last_signature;            
    
    
    
    
    
    
    
    
    
    std::string             pending_signature;         
    int64_t                 pending_ifIndex   = 0;     
    std::string             pending_desc;              
    dispatch_source_t       debounce_timer    = nullptr; 
    std::atomic<bool>       stopped      { false };
};










static constexpr int64_t kDebounceMs = 1500;





































static int64_t ResolveActiveIfIndex(nw_path_t path,
                                    std::string& descOut,
                                    std::string& ifNameOut,
                                    TTVpnPolicy policy,
                                    bool isStartupQuery)
{
    __block int64_t      physicalIdx  = 0;
    __block std::string  physicalDesc;
    __block std::string  physicalName;

    __block int64_t      fallbackIdx  = 0;
    __block std::string  fallbackDesc;
    __block std::string  fallbackName;

    const bool osOrder = (policy == TT_VPN_POLICY_OS);

    nw_path_enumerate_interfaces(path, ^bool(nw_interface_t iface) {
        const char* name = nw_interface_get_name(iface);
        if (!name || !name[0]) return true; 
        unsigned int idx = if_nametoindex(name);
        if (idx == 0) return true;

        nw_interface_type_t t = nw_interface_get_type(iface);
        const char* typeStr = "other";
        bool isPhysical = false;
        switch (t) {
            case nw_interface_type_wifi:     typeStr = "wifi";     isPhysical = true; break;
            case nw_interface_type_cellular: typeStr = "cellular"; isPhysical = true; break;
            case nw_interface_type_wired:    typeStr = "wired";    isPhysical = true; break;
            case nw_interface_type_loopback: typeStr = "loopback";                    break;
            default: break;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (%s)", typeStr, name);

        if (osOrder) {
            
            
            if (t == nw_interface_type_loopback) return true;
            fallbackIdx  = (int64_t)idx;
            fallbackName = name;
            fallbackDesc = buf;
            return false;
        }

        if (isPhysical) {
            if (physicalIdx == 0) {
                physicalIdx  = (int64_t)idx;
                physicalName = name;
                physicalDesc = buf;
            }
            
            return false;
        }
        if (t != nw_interface_type_loopback && fallbackIdx == 0) {
            
            fallbackIdx  = (int64_t)idx;
            fallbackName = name;
            fallbackDesc = buf;
        }
        return true;
    });

    if (osOrder) {
        descOut   = std::move(fallbackDesc);
        ifNameOut = std::move(fallbackName);
        return fallbackIdx;
    }

    if (physicalIdx > 0) {
        descOut   = std::move(physicalDesc);
        ifNameOut = std::move(physicalName);
        return physicalIdx;
    }

    
    const bool allowTunnelFallback =
        (policy == TT_VPN_POLICY_PREFER_PHYSICAL && isStartupQuery);
    if (!allowTunnelFallback) {
        descOut.clear();
        ifNameOut.clear();
        return 0;
    }
    descOut   = std::move(fallbackDesc);
    ifNameOut = std::move(fallbackName);
    return fallbackIdx;
}





static std::string CollectInterfaceIPs(const std::string& ifName)
{
    if (ifName.empty()) return std::string();

    struct ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0 || !head) return std::string();

    std::vector<std::string> addrs;
    for (struct ifaddrs* it = head; it != nullptr; it = it->ifa_next) {
        if (!it->ifa_name || ifName != it->ifa_name) continue;
        if (!it->ifa_addr) continue;
        if ((it->ifa_flags & IFF_LOOPBACK) != 0) continue;

        char buf[INET6_ADDRSTRLEN] = {0};
        int family = it->ifa_addr->sa_family;
        if (family == AF_INET) {
            auto* sin = (const struct sockaddr_in*)it->ifa_addr;
            if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
        } else if (family == AF_INET6) {
            auto* sin6 = (const struct sockaddr_in6*)it->ifa_addr;
            if (!inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf))) continue;
            
            
            
            
            if (sin6->sin6_addr.s6_addr[0] == 0xfe &&
                (sin6->sin6_addr.s6_addr[1] & 0xc0) == 0x80) {
                continue;
            }
        } else {
            continue;
        }
        addrs.emplace_back(buf);
    }
    freeifaddrs(head);

    std::sort(addrs.begin(), addrs.end());
    std::string out;
    out.reserve(addrs.size() * 16);
    for (const auto& a : addrs) {
        if (!out.empty()) out.push_back(',');
        out.append(a);
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








static std::string DescribePath(nw_path_t path)
{
    if (!path) return std::string("(null path)");

    const char* statusStr = "?";
    switch (nw_path_get_status(path)) {
        case nw_path_status_invalid:      statusStr = "invalid";     break;
        case nw_path_status_satisfied:    statusStr = "satisfied";   break;
        case nw_path_status_unsatisfied:  statusStr = "unsatisfied"; break;
        case nw_path_status_satisfiable:  statusStr = "satisfiable"; break;
    }

    
    
    __block std::string line = "status=";
    line += statusStr;
    if (nw_path_is_expensive(path))   line += " expensive";
    if (nw_path_is_constrained(path)) line += " constrained";
    line += " ifaces=[";

    __block bool first = true;
    nw_path_enumerate_interfaces(path, ^bool(nw_interface_t iface) {
        const char* name = nw_interface_get_name(iface);
        if (!name) name = "?";
        unsigned int idx = name[0] ? if_nametoindex(name) : 0;
        const char* tstr = "other";
        switch (nw_interface_get_type(iface)) {
            case nw_interface_type_wifi:     tstr = "wifi";     break;
            case nw_interface_type_cellular: tstr = "cellular"; break;
            case nw_interface_type_wired:    tstr = "wired";    break;
            case nw_interface_type_loopback: tstr = "loopback"; break;
            default: break;
        }
        char buf[80];
        snprintf(buf, sizeof(buf), "%s%s(%s,idx=%u)",
                 first ? "" : ",", tstr, name, idx);
        line.append(buf);
        first = false;
        return true;
    });
    line += "]";
    return line;
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
    if (options) {
        self->raw_log_fn  = options->rawLogFn;
        self->raw_log_ctx = options->rawLogCtx;
    }
    self->queue = dispatch_queue_create("ttsignal.netmon", DISPATCH_QUEUE_SERIAL);
    self->monitor = nw_path_monitor_create();

    nw_path_monitor_set_queue(self->monitor, self->queue);
    nw_path_monitor_set_update_handler(self->monitor, ^(nw_path_t path) {
        if (self->stopped.load(std::memory_order_acquire)) return;

        
        
        
        
        
        if (self->raw_log_fn) {
            std::string raw = DescribePath(path);
            self->raw_log_fn(self->raw_log_ctx, raw.c_str());
        }

        nw_path_status_t status = nw_path_get_status(path);
        if (status != nw_path_status_satisfied &&
            status != nw_path_status_satisfiable) {
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            
            if (self->debounce_timer) {
                dispatch_source_cancel(self->debounce_timer);
                self->debounce_timer = nullptr;
            }
            {
                std::lock_guard<std::mutex> lock(self->sig_mutex);
                self->pending_signature.clear();
                self->pending_ifIndex = 0;
                self->pending_desc.clear();
            }
            return;
        }

        std::string desc;
        std::string ifName;
        
        
        
        
        
        int64_t ifIndex = ResolveActiveIfIndex(path, desc, ifName,
                                               self->policy,
                                               false);
        if (ifIndex <= 0) return;

        std::string ips = CollectInterfaceIPs(ifName);
        std::string sig = MakeSignature(ifIndex, ips);

        {
            std::lock_guard<std::mutex> lock(self->sig_mutex);
            if (self->pending_signature == sig) {
                
                
                
                
                
                
                
                
                return;
            }
            self->pending_signature = sig;
            self->pending_ifIndex   = ifIndex;
            self->pending_desc      = desc;
        }

        
        
        
        
        
        
        
        if (self->debounce_timer) {
            dispatch_source_cancel(self->debounce_timer);
            self->debounce_timer = nullptr;
        }
        dispatch_source_t timer = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self->queue);
        dispatch_source_set_timer(timer,
            dispatch_time(DISPATCH_TIME_NOW, kDebounceMs * NSEC_PER_MSEC),
            DISPATCH_TIME_FOREVER,
            50 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{
            
            
            dispatch_source_cancel(timer);
            if (self->debounce_timer == timer) {
                self->debounce_timer = nullptr;
            }
            if (self->stopped.load(std::memory_order_acquire)) return;

            int64_t     fire_idx = 0;
            std::string fire_desc;
            bool        deliver = false;
            {
                std::lock_guard<std::mutex> lock(self->sig_mutex);
                
                
                
                
                if (self->pending_signature != self->last_signature) {
                    self->last_signature = self->pending_signature;
                    fire_idx  = self->pending_ifIndex;
                    fire_desc = self->pending_desc;
                    deliver   = true;
                }
            }
            if (deliver) {
                
                
                
                
                
                self->callback(self->userdata, fire_idx, fire_desc.c_str());
            }
        });
        self->debounce_timer = timer;
        dispatch_resume(timer);
    });
    nw_path_monitor_start(self->monitor);
    return (TTNetworkMonitorRef)self;
}

void tt_netmon_stop(TTNetworkMonitorRef ref)
{
    Monitor* self = (Monitor*)ref;
    if (!self) return;
    self->stopped.store(true, std::memory_order_release);
    if (self->monitor) {
        nw_path_monitor_cancel(self->monitor);
        self->monitor = nullptr;
    }
    
    
    
    
    
    
    if (self->queue) {
        dispatch_sync(self->queue, ^{
            if (self->debounce_timer) {
                dispatch_source_cancel(self->debounce_timer);
                self->debounce_timer = nullptr;
            }
        });
    }
    self->queue   = nullptr; 
    delete self;
}

int64_t tt_netmon_query_default_ifindex(void)
{
    return tt_netmon_query_default_ifindex_ex(
        (int)tt_vpn_policy_platform_default());
}

int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy)
{
    TTVpnPolicy policy = (TTVpnPolicy)vpnPolicy;
    if (policy != TT_VPN_POLICY_OS &&
        policy != TT_VPN_POLICY_PREFER_PHYSICAL &&
        policy != TT_VPN_POLICY_FORCE_PHYSICAL) {
        policy = tt_vpn_policy_platform_default();
    }

    
    
    
    
    
    __block int64_t result = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    nw_path_monitor_t mon   = nw_path_monitor_create();
    dispatch_queue_t  q     = dispatch_queue_create(
        "ttsignal.netmon.probe", DISPATCH_QUEUE_SERIAL);
    nw_path_monitor_set_queue(mon, q);
    nw_path_monitor_set_update_handler(mon, ^(nw_path_t path) {
        if (result != 0) return; 
        nw_path_status_t st = nw_path_get_status(path);
        if (st != nw_path_status_satisfied &&
            st != nw_path_status_satisfiable) {
            
            dispatch_semaphore_signal(sem);
            return;
        }
        std::string desc;
        std::string ifName;
        
        
        
        
        result = ResolveActiveIfIndex(path, desc, ifName,
                                      policy,
                                      true);
        dispatch_semaphore_signal(sem);
    });
    nw_path_monitor_start(mon);

    
    dispatch_semaphore_wait(sem,
        dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC));
    nw_path_monitor_cancel(mon);
    return result;
}

} 
