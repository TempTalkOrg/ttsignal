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

// Allocate + start a monitor. Returns NULL on failure (out of memory,
// missing OS support, etc.). The callback may fire synchronously once
// before this function returns (with the current initial path), so callers
// must be ready to receive callbacks immediately.
//
// Implementations MUST de-duplicate on ifIndex (don't fire when the active
// interface didn't actually change — Linux netlink is especially noisy
// during DHCP renew / systemd-networkd restarts).
TTNetworkMonitorRef tt_netmon_start(TTPathChangeCallback cb, void* userdata);

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
