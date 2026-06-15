///////////////////////////////////////////////////////////////////////////////
// file : INetworkPathMonitor.h
// author : anto
//
// Platform-agnostic C interface for network path / default-route change
// monitoring. SMPConnector uses this to automatically migrate UDPSender
// onto the new active network interface whenever the OS reports a switch
// (Wi-Fi <-> cellular on iOS, Wi-Fi <-> ethernet on macOS, primary route
// change on Linux/Windows).
//
// Implementations:
//   src/cpp/apple/AppleNetworkMonitor.mm  (iOS + macOS, NWPathMonitor)
//   src/cpp/linux/LinuxNetlinkMonitor.cpp (Linux,  NETLINK_ROUTE)
//   src/cpp/win32/WinIpChangeMonitor.cpp  (Windows, NotifyIpInterfaceChange)
//
// Android intentionally does NOT link any of these — its NetworkCallback
// path stays in the Java layer and feeds Connection::Restart(networkHandle)
// directly.
///////////////////////////////////////////////////////////////////////////////
#ifndef TTSIGNAL_INETWORK_PATH_MONITOR_H_INCLUDED__
#define TTSIGNAL_INETWORK_PATH_MONITOR_H_INCLUDED__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle returned by tt_netmon_start. Not thread-safe; create/destroy
// from the same thread (typically the SMPConnector owner).
typedef struct TTNetworkMonitor* TTNetworkMonitorRef;

// Invoked on an implementation-defined thread (iOS/macOS: serial dispatch
// queue; Linux: dedicated reader thread; Windows: OS worker thread). The
// callback MUST be cheap and non-blocking — schedule the actual restart
// onto the SMP runtime via PostEvent rather than doing work inline.
//
//   newIfIndex : numeric interface index suitable for setsockopt(IP_BOUND_IF
//                / IP_UNICAST_IF / IPV6_BOUND_IF / IPV6_UNICAST_IF). Always
//                > 0 when invoked; 0 is reserved for "unknown".
//   pathDesc   : short human-readable description for logging
//                ("wifi (en0)" / "cellular (pdp_ip0)" / "ifIndex=12"). May
//                be "" but never NULL.
typedef void (*TTPathChangeCallback)(void* userdata,
                                     int64_t newIfIndex,
                                     const char* pathDesc);

// Optional diagnostic-log sink for raw, pre-filter monitor events. The
// monitor itself never calls into the SDK's logging macros (it lives in a
// header-only-friendly layer), so callers pass a function pointer that
// forwards to whatever logger (LogQ, NSLog, fprintf, ...) they want.
// All arguments after `userdata` form a single already-formatted line.
typedef void (*TTPathLogCallback)(void* userdata, const char* line);

// Per-instance options. Designed to grow without breaking ABI: add new
// fields at the end, never reorder. Pass NULL to tt_netmon_start to accept
// every platform's defaults (which match the behaviour we ship by default
// — apps that don't care about VPN-vs-physical preferences don't need to
// touch this struct at all).
typedef struct TTNetworkMonitorOptions {
    // macOS-only knob. When non-zero (the default for apps that DO pass an
    // options struct, see SMPConnector::Config::bypassVpn) the live update
    // handler refuses to commit a path change whose only available
    // interface is a virtual one (utun / ipsec / ppp). The rationale is
    // covered in AppleNetworkMonitor.mm: corporate VPN clients (Cisco,
    // GlobalProtect), Tailscale, WireGuard and iCloud Private Relay all
    // install a tunnel of nw_interface_type_other and grab the default
    // route, which causes NWPathMonitor to flip-flop the "best" interface
    // across a Wi-Fi handover and bounces UDPSender onto a tunnel whose
    // underlying physical link just went down. Setting this to 0 restores
    // the OS preference order, which is what server-side / VPN-only
    // deployments want.
    //
    // iOS, Linux and Windows monitors honour the struct shape but ignore
    // this field — iOS users explicitly opt in to per-app VPN, and
    // Linux/Windows route monitors don't see the same flap pattern.
    int bypassVpn;
    // Optional sink for *raw, unfiltered* path events — i.e. every time
    // the platform monitor wakes us up, before any dedup / debounce /
    // VPN-bypass / status filtering. Intended purely for diagnosing why a
    // given `path change` callback did or did not fire; production builds
    // can leave both NULL. When set, every line is already formatted
    // (single trailing newline NOT included) so the caller can dispatch
    // to LogQ / NSLog / fprintf as-is.
    TTPathLogCallback rawLogFn;
    void* rawLogCtx;
} TTNetworkMonitorOptions;

// Allocate + start a monitor. Returns NULL on failure (out of memory,
// missing OS support, etc.). The callback may fire synchronously once
// before this function returns (with the current initial path), so callers
// must be ready to receive callbacks immediately.
//
// `options` may be NULL — implementations then fall back to their hardcoded
// defaults (currently: bypassVpn = 1 on macOS, no effect elsewhere). Passing
// a non-NULL pointer lets callers override per-instance; the struct is
// copied internally so the caller may free it as soon as this call returns.
//
// Implementations MUST de-duplicate on ifIndex (don't fire when the active
// interface didn't actually change — Linux netlink is especially noisy
// during DHCP renew / systemd-networkd restarts).
TTNetworkMonitorRef tt_netmon_start(const TTNetworkMonitorOptions* options,
                                    TTPathChangeCallback cb,
                                    void* userdata);

// Cancel the underlying monitor and free the handle. Safe to call with
// NULL. After this returns the callback is guaranteed not to fire again.
void tt_netmon_stop(TTNetworkMonitorRef ref);

// Synchronous best-effort lookup of the current default route's outgoing
// interface index. Returns 0 if unknown / no default route. Used by
// SMPConnector at connect() time so the very first UDPSender already binds
// to the right interface, without waiting for the first path-change
// callback. It is OK if this returns 0 — UDPSender just won't bind, and
// the first callback will fix things up.
int64_t tt_netmon_query_default_ifindex(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TTSIGNAL_INETWORK_PATH_MONITOR_H_INCLUDED__
