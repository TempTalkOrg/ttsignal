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

// Returned to the caller as an opaque TTNetworkMonitorRef.
struct Monitor {
    nw_path_monitor_t       monitor      = nullptr;
    dispatch_queue_t        queue        = nullptr;
    TTPathChangeCallback    callback     = nullptr;
    void*                   userdata     = nullptr;
    // Snapshot of TTNetworkMonitorOptions taken at start time; the caller
    // is free to reuse / destroy the struct it passed to us. immutable
    // after tt_netmon_start returns, no locking needed.
    // 生效策略。由 tt_netmon_start 从 options->vpnPolicy 与 options->bypassVpn
    // 合并得出，start 返回后不再变更，无需加锁。
    TTVpnPolicy             policy       = TT_VPN_POLICY_PREFER_PHYSICAL;
    // Optional raw-event log sink (see TTNetworkMonitorOptions::rawLogFn).
    // Captured at start time so the update_handler block doesn't need to
    // re-read options across threads.
    TTPathLogCallback       raw_log_fn   = nullptr;
    void*                   raw_log_ctx  = nullptr;
    // Fingerprint of (ifIndex, sorted IP addrs on that ifName). Going by
    // ifIndex alone is wrong on macOS: when the user roams between two SSIDs
    // the active interface stays "en0" so ifIndex doesn't change, but the
    // local IPv4/IPv6 (and the gateway) absolutely do, which means the
    // QUIC 5-tuple is stale and we *must* restart the UDP socket. Mixing
    // the per-interface IP list into the dedup key catches that case while
    // still ignoring DNS/viability churn on a stable network.
    std::mutex              sig_mutex;
    std::string             last_signature;            // guarded by sig_mutex; last value actually delivered to the callback
    // Debounce staging. NWPathMonitor fires multiple updates back-to-back
    // during a real Wi-Fi switch — disassociate -> associate -> DHCPv4 ->
    // SLAAC global -> temporary addr — and every step changes the local
    // IP list (and therefore the fingerprint). Without coalescing, each
    // step bounces UDPSender to an interface that isn't yet routable
    // (ENETUNREACH). Linux/Windows monitors already do 150ms debounce on
    // their netlink / NotifyIpInterfaceChange equivalents; on Apple we
    // use a longer window because SLAAC + temporary-address completion
    // routinely takes 300-800ms.
    std::string             pending_signature;         // guarded by sig_mutex
    int64_t                 pending_ifIndex   = 0;     // guarded by sig_mutex
    std::string             pending_desc;              // guarded by sig_mutex
    dispatch_source_t       debounce_timer    = nullptr; // only touched on `queue`
    std::atomic<bool>       stopped      { false };
};

// 1500 ms picked from empirical macOS Wi-Fi handover traces: with 800ms we
// saw the burst split into two callbacks ~1.06s apart — the first one fires
// before kernel has installed the new IPv4 (UDPSender::Connect then returns
// ENETUNREACH and we burn a PATH_CHALLENGE on a dead socket), the second
// one one second later actually succeeds. 1500ms safely brackets the whole
// associate -> DHCPv4 -> SLAAC global -> temporary addr sequence so we
// migrate once, on the final fingerprint. Linux/Windows still use 150ms
// because their netlink / IpInterfaceChange notifications fire only once
// the kernel has the route installed.
static constexpr int64_t kDebounceMs = 1500;

// 挑选 QUIC socket 要绑定的网卡。Network.framework 按系统偏好顺序（默认路由
// 赢家在前）把网卡交给我们。NWPathMonitor 在 iOS 12 上不直接给
// nw_interface_get_index，用 if_nametoindex 取名字换索引是等价的，系统内部
// 也是这么做的。
//
// macOS 与 iOS 共用这一份实现。它们的差别只体现在默认 policy 上
// （见 tt_vpn_policy_platform_default）：iOS 默认 os，桌面默认
// prefer-physical。行为本身由参数决定，不再按 #if TARGET_OS_OSX 分叉。
//
// policy 决定"物理优先"和"能否回落到隧道"两件事：
//
//   TT_VPN_POLICY_OS
//       完全跟随系统顺序，第一块非 loopback 网卡胜出（含 utun / ipsec /
//       ppp）。iOS 的历史行为，也是 iOS 的默认值——装了 per-app VPN 的
//       用户通常就是希望流量走 VPN。
//
//   TT_VPN_POLICY_PREFER_PHYSICAL
//       遍历整条 path，优先挑第一块物理网卡（wifi / wired / cellular）。
//       找不到物理网卡时：启动查询允许回落到隧道（VPN-only 机器还得能
//       bootstrap），运行中的 update_handler 不回落而是返回 0。
//
//       为什么运行中不回落：Wi-Fi 切换过程中 path 常常有几百毫秒只剩
//       utun4，这时把 socket 弹到隧道上只会打死连接（底下的物理链路同时
//       也断了）。返回 0 让 update_handler 保持当前 socket，下一次带着
//       真实物理网卡的更新再提交迁移。
//
//   TT_VPN_POLICY_FORCE_PHYSICAL
//       只接受物理网卡，启动查询也不回落。业务显式声明"必须拿到真实 IP"。
//
// 公司 VPN 客户端（Cisco、GlobalProtect）、Tailscale、WireGuard 和 iCloud
// 私密代理都会装一块 nw_interface_type_other 的 utunN / ipsecN / ppp* 并
// 抢走默认路由，这正是 prefer / force 两档要绕开的东西。
//
// isStartupQuery 只在 prefer-physical 下有意义：true 表示这是
// tt_netmon_query_default_ifindex_ex 的一次性查询，允许回落隧道；false
// 表示这是长期 monitor 的 live 更新，不允许。
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
        if (!name || !name[0]) return true; // continue
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
            // 系统顺序：第一块网卡直接胜出。loopback 永远跳过——绑到 lo0
            // 上等于把连接打死，而且系统也不会把它排在默认路由赢家前面。
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
            // 第一块物理网卡胜出，提前结束枚举。
            return false;
        }
        if (t != nw_interface_type_loopback && fallbackIdx == 0) {
            // VPN / 隧道。先记下来当备选，继续找物理网卡。
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

    // 没有物理网卡。只有 prefer-physical 的启动查询允许回落到隧道。
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

// Serialize an NWPath into a single human-readable line for the raw-event
// log sink. Captures *everything* the upper layers will subsequently
// filter on: status (satisfied / unsatisfied / satisfiable / invalid),
// constrained / expensive flags, and the full ordered list of interfaces
// with their type and ifIndex. Side effects: none — getifaddrs is NOT
// called here, the line is meant to be cheap enough to print on every
// raw event including the ones we'd otherwise drop.
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

    // ObjC++ blocks default-capture non-POD locals by const copy, so any
    // string we want the enumerator block to append to must be __block.
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

} // namespace

extern "C" {

TTNetworkMonitorRef tt_netmon_start(const TTNetworkMonitorOptions* options,
                                    TTPathChangeCallback cb,
                                    void* userdata)
{
    if (!cb) return nullptr;

    Monitor* self = new Monitor();
    self->callback = cb;
    self->userdata = userdata;
    // options 为 NULL 时用平台默认策略（macOS 是 prefer-physical，等价于
    // 历史上的 bypassVpn=1；iOS 是 os，等价于历史上"第一块网卡胜出"）。
    // 给了 options 就走 vpnPolicy / bypassVpn 的合并规则。
    if (options == nullptr) {
        self->policy = tt_vpn_policy_platform_default();
    } else {
        self->policy = tt_vpn_policy_resolve(
            (TTVpnPolicy)options->vpnPolicy,
            /*hasBypassVpn=*/1, options->bypassVpn,
            tt_vpn_policy_platform_default(),
            /*outBothGiven=*/nullptr);
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

        // Emit the raw event *first*, before any filter / dedup / debounce.
        // This is how upper layers (and humans staring at logs) audit
        // "OS reported N path changes, SDK reacted to M of them" — every
        // status flip, every interface re-ordering, every probe wakeup
        // is logged here exactly once per NWPathMonitor callback.
        if (self->raw_log_fn) {
            std::string raw = DescribePath(path);
            self->raw_log_fn(self->raw_log_ctx, raw.c_str());
        }

        nw_path_status_t status = nw_path_get_status(path);
        if (status != nw_path_status_satisfied &&
            status != nw_path_status_satisfiable) {
            // Path is unsatisfied. A naive "just return" is what we used to
            // do, but on a real Wi-Fi handover the OS reports the burst as
            // satisfied(old) → unsatisfied → satisfied(new). If we already
            // armed a debounce timer for the first satisfied (the *old*
            // path's fingerprint refreshing) and now leave it running,
            // it will fire ~mid-handover and trigger a useless restart
            // before the new network has even come up — then 1.5s after
            // the *real* recovery a second debounce fires another restart
            // that ends up killing the QUIC path the server just finished
            // validating. Empirically this is the dominant failure mode
            // under Clash / Surge TUN mode (see UDPSender::
            // _TryClearInterfaceBinding comment). Cancel any pending
            // timer and clear pending state so the only debounce we
            // arm is the one driven by the *next* satisfied update,
            // when the kernel has the new interface IPs.
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
        // Live 更新：prefer / force 两档都拒绝 utun/ipsec/ppp 回落。Wi-Fi
        // 切换过程中 path 常常有几百毫秒只剩 utun4，把 QUIC 弹到那条隧道
        // 上只会打死连接（底下的物理链路同时也断了）。此时
        // ResolveActiveIfIndex 返回 0，我们保持当前 socket，等下一次带着
        // 真实物理网卡的更新再提交迁移。os 档则拿到系统偏好顺序的原样结果。
        int64_t ifIndex = ResolveActiveIfIndex(path, desc, ifName,
                                               self->policy,
                                               /*isStartupQuery=*/false);
        if (ifIndex <= 0) return;

        std::string ips = CollectInterfaceIPs(ifName);
        std::string sig = MakeSignature(ifIndex, ips);

        {
            std::lock_guard<std::mutex> lock(self->sig_mutex);
            if (self->pending_signature == sig) {
                // Identical to the most recent fingerprint we've staged.
                // Either the debounce timer is already armed for exactly
                // this value, or it has already fired and `last_signature`
                // matches — either way there is nothing new to dispatch
                // and we deliberately do NOT reset the clock (otherwise a
                // periodic NWPathMonitor refresh on a stable network
                // could indefinitely postpone delivery of the very first
                // value).
                return;
            }
            self->pending_signature = sig;
            self->pending_ifIndex   = ifIndex;
            self->pending_desc      = desc;
        }

        // (Re)arm the debounce timer. Cancelling the previous one resets
        // the clock so a Wi-Fi switch burst (associate -> DHCPv4 -> SLAAC
        // global -> temporary addr, all within a few hundred ms) only
        // produces one callback once the kernel has finished writing the
        // final IP set. The update handler runs serialised on
        // `self->queue`, so reading/writing `debounce_timer` here doesn't
        // need locking.
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
            // Single-shot timer: cancel ourselves before doing any work
            // so the source can be released by ARC after this handler.
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
                // During the debounce window the fingerprint can bounce
                // back to the previously-delivered value (e.g. brief
                // flap-and-recover on the same Wi-Fi). When that happens
                // there is nothing new for the upper layer.
                if (self->pending_signature != self->last_signature) {
                    self->last_signature = self->pending_signature;
                    fire_idx  = self->pending_ifIndex;
                    fire_desc = self->pending_desc;
                    deliver   = true;
                }
            }
            if (deliver) {
                // The very first delivery (last_signature was empty before
                // this) also flows through here, which is intentional: it
                // lets the caller learn the initial ifIndex.
                // SMPConnector::OnPathChange swallows that first hit so it
                // never causes a spurious mid-handshake restart.
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
    // Drain the serial queue. Anything that was already in flight (a
    // running update_handler) or scheduled (a pending debounce-timer
    // fire) captured `self`, so we must let it finish — and cancel any
    // still-pending timer — before destroying the struct. Without this
    // step a debounce_timer queued behind dispatch_resume could fire
    // after `delete self` and reach freed memory.
    if (self->queue) {
        dispatch_sync(self->queue, ^{
            if (self->debounce_timer) {
                dispatch_source_cancel(self->debounce_timer);
                self->debounce_timer = nullptr;
            }
        });
    }
    self->queue   = nullptr; // ARC-managed by Network.framework
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
        // 启动查询：prefer-physical 下允许 utun/ipsec 回落，这样纯 VPN
        // 机器（完全没有 wifi/wired/cellular）也能拿到一个 ifIndex 把连接
        // 建起来；live update_handler 之后会收紧成"必须物理网卡"。
        // force-physical 下则不回落，返回 0 让上层明确失败。
        result = ResolveActiveIfIndex(path, desc, ifName,
                                      policy,
                                      /*isStartupQuery=*/true);
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
