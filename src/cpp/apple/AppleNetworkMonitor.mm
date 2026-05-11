///////////////////////////////////////////////////////////////////////////////
// file : AppleNetworkMonitor.mm
// author : anto
//
// NWPathMonitor wrapper. Shared between iOS xcframework and macOS NAPI builds.
// Reports active interface changes (ifIndex) so SMPConnector can migrate
// UDPSender to the new interface.
///////////////////////////////////////////////////////////////////////////////

#include "AppleNetworkMonitor.h"

#if !defined(__APPLE__)
#  error "AppleNetworkMonitor is Apple-platform only (iOS + macOS)."
#endif

#import <Foundation/Foundation.h>
#import <Network/Network.h>
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

// Returned to the caller as an opaque TTNetworkMonitorRef.
struct Monitor {
    nw_path_monitor_t       monitor      = nullptr;
    dispatch_queue_t        queue        = nullptr;
    TTPathChangeCallback    callback     = nullptr;
    void*                   userdata     = nullptr;
    // Fingerprint of (ifIndex, sorted IP addrs on that ifName). Going by
    // ifIndex alone is wrong on macOS: when the user roams between two SSIDs
    // the active interface stays "en0" so ifIndex doesn't change, but the
    // local IPv4/IPv6 (and the gateway) absolutely do, which means the
    // QUIC 5-tuple is stale and we *must* restart the UDP socket. Mixing
    // the per-interface IP list into the dedup key catches that case while
    // still ignoring DNS/viability churn on a stable network.
    std::mutex              sig_mutex;
    std::string             last_signature;            // guarded by sig_mutex
    std::atomic<bool>       stopped      { false };
};

// Pull the first interface out of the path that's actually usable, then
// turn its name into a numeric ifIndex via if_nametoindex. Network.framework
// does not give us nw_interface_get_index directly on iOS 12, but
// if_nametoindex on the interface name is equivalent and is what the
// system itself does internally.
static int64_t ResolveActiveIfIndex(nw_path_t path,
                                    std::string& descOut,
                                    std::string& ifNameOut)
{
    __block int64_t      ifIndex = 0;
    __block std::string  desc;
    __block std::string  ifName;

    nw_path_enumerate_interfaces(path, ^bool(nw_interface_t iface) {
        const char* name = nw_interface_get_name(iface);
        if (!name || !name[0]) return true; // continue
        unsigned int idx = if_nametoindex(name);
        if (idx == 0) return true;
        ifIndex = (int64_t)idx;
        ifName  = name;

        nw_interface_type_t t = nw_interface_get_type(iface);
        const char* typeStr = "other";
        switch (t) {
            case nw_interface_type_wifi:     typeStr = "wifi";     break;
            case nw_interface_type_cellular: typeStr = "cellular"; break;
            case nw_interface_type_wired:    typeStr = "wired";    break;
            case nw_interface_type_loopback: typeStr = "loopback"; break;
            default: break;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%s (%s)", typeStr, name);
        desc = buf;
        return false; // first interface wins
    });

    descOut   = std::move(desc);
    ifNameOut = std::move(ifName);
    return ifIndex;
}

// Walk getifaddrs() and collect every non-loopback IPv4/IPv6 address bound
// to `ifName`. Returns "" if the interface has no addresses (yet) — that's
// fine, the next NWPathMonitor update will retry. The result is sorted so
// the order of the kernel's linked list doesn't perturb the fingerprint.
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
            // IPv6 link-local (fe80::/10) is generated from the MAC address
            // (EUI-64), which doesn't change across SSIDs, so it would mask
            // a real network change. Skip it; routable addresses (DHCPv6 /
            // SLAAC global, ULA) will still drive the fingerprint.
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

} // namespace

extern "C" {

TTNetworkMonitorRef tt_netmon_start(TTPathChangeCallback cb, void* userdata)
{
    if (!cb) return nullptr;

    Monitor* self = new Monitor();
    self->callback = cb;
    self->userdata = userdata;
    self->queue = dispatch_queue_create("ttsignal.netmon", DISPATCH_QUEUE_SERIAL);
    self->monitor = nw_path_monitor_create();

    nw_path_monitor_set_queue(self->monitor, self->queue);
    nw_path_monitor_set_update_handler(self->monitor, ^(nw_path_t path) {
        if (self->stopped.load(std::memory_order_acquire)) return;

        nw_path_status_t status = nw_path_get_status(path);
        if (status != nw_path_status_satisfied &&
            status != nw_path_status_satisfiable) {
            // Path is unsatisfied — wait for next update. Don't restart on
            // an unusable interface, that would just churn UDP sockets.
            return;
        }

        std::string desc;
        std::string ifName;
        int64_t ifIndex = ResolveActiveIfIndex(path, desc, ifName);
        if (ifIndex <= 0) return;

        std::string ips = CollectInterfaceIPs(ifName);
        std::string sig = MakeSignature(ifIndex, ips);

        {
            std::lock_guard<std::mutex> lock(self->sig_mutex);
            if (self->last_signature == sig) {
                // Same interface AND same local IP set as before (e.g. only
                // viability or DNS changed). Don't drop QUIC sockets — RFC
                // 9000 migration is only useful when the local 5-tuple
                // actually changed.
                return;
            }
            self->last_signature = sig;
        }
        // The very first update (last_signature was empty before this) also
        // fires through here, which is intentional: it lets the caller learn
        // the initial ifIndex. SMPConnector::OnPathChange swallows that first
        // hit so it never causes a spurious mid-handshake restart.
        self->callback(self->userdata, ifIndex, desc.c_str());
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
    self->queue   = nullptr; // ARC-managed by Network.framework
    delete self;
}

int64_t tt_netmon_query_default_ifindex(void)
{
    // Spin a one-shot path monitor synchronously. NWPathMonitor's first
    // update fires almost immediately on dispatch, but it's still async, so
    // we block on a semaphore with a small timeout. Returning 0 (unknown)
    // is a fine fallback — the long-lived monitor started later via
    // tt_netmon_start will fix things up on its first callback.
    __block int64_t result = 0;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    nw_path_monitor_t mon   = nw_path_monitor_create();
    dispatch_queue_t  q     = dispatch_queue_create(
        "ttsignal.netmon.probe", DISPATCH_QUEUE_SERIAL);
    nw_path_monitor_set_queue(mon, q);
    nw_path_monitor_set_update_handler(mon, ^(nw_path_t path) {
        if (result != 0) return; // already captured
        nw_path_status_t st = nw_path_get_status(path);
        if (st != nw_path_status_satisfied &&
            st != nw_path_status_satisfiable) {
            // Signal anyway so we don't hang on no-network.
            dispatch_semaphore_signal(sem);
            return;
        }
        std::string desc;
        std::string ifName;
        result = ResolveActiveIfIndex(path, desc, ifName);
        dispatch_semaphore_signal(sem);
    });
    nw_path_monitor_start(mon);

    // 200ms cap — getting an answer should be near-instant.
    dispatch_semaphore_wait(sem,
        dispatch_time(DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC));
    nw_path_monitor_cancel(mon);
    return result;
}

} // extern "C"
