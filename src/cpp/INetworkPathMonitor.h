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

// TTVpnPolicy 与其解析辅助函数。放在独立头文件里是因为 UDPSender 与
// SMPConnector 也要用这个枚举，但它们并不需要下面的 monitor C API。
// 从本头文件 include 它，保证"包含 INetworkPathMonitor.h 就能拿到
// TTVpnPolicy"这个既有契约不变。
#include "VpnPolicy.h"

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
    // 三态 VPN / 虚拟网卡策略，取值见 TTVpnPolicy。取代上面的 bypassVpn，
    // 后者保留但已废弃。语义（三平台一致）：
    //
    //   TT_VPN_POLICY_OS (0)
    //       完全跟随系统路由，允许把 QUIC 弹到 VPN 隧道上。
    //   TT_VPN_POLICY_PREFER_PHYSICAL (1)
    //       优先物理网卡。启动查询找不到物理网卡时可回落隧道（VPN-only 机器
    //       还得能 bootstrap），运行中则拒绝回落、保持当前 socket。
    //   TT_VPN_POLICY_FORCE_PHYSICAL (2)
    //       只接受物理网卡，任何阶段都不回落。
    //
    // ⚠️ TT_VPN_POLICY_UNSET 是 -1 而不是 0。C-struct 零初始化会把本字段填成
    // 0，也就是 TT_VPN_POLICY_OS——那跟"未设置"不是一回事。所有调用方在填充
    // options 时必须显式赋值；实现侧只有在读到 -1 时才回落到 bypassVpn 的
    // 兼容映射与平台默认值。SMPConnector 已经显式赋值（见 SMPConnector.cpp
    // 里的 tt_netmon_start 调用）。
    int vpnPolicy;
} TTNetworkMonitorOptions;

// Allocate + start a monitor. Returns NULL on failure (out of memory,
// missing OS support, etc.). The callback may fire synchronously once
// before this function returns (with the current initial path), so callers
// must be ready to receive callbacks immediately.
//
// `options` may be NULL — implementations then fall back to
// tt_vpn_policy_platform_default() (prefer-physical on macOS / Windows /
// Linux, os on iOS). Passing a non-NULL pointer lets callers override
// per-instance; the struct is copied internally so the caller may free it as
// soon as this call returns.
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

// 同 tt_netmon_query_default_ifindex，但显式指定策略（取值见 TTVpnPolicy）。
//
//   TT_VPN_POLICY_OS              — 返回系统默认路由的出口网卡，含 VPN 隧道
//   TT_VPN_POLICY_PREFER_PHYSICAL — 优先物理网卡，没有物理网卡时回落隧道
//   TT_VPN_POLICY_FORCE_PHYSICAL  — 只返回物理网卡，没有则返回 0
//
// 无法识别的取值按 tt_vpn_policy_platform_default() 处理。
//
// 为什么是个新函数而不是给原函数加参数：tt_netmon_query_default_ifindex 是
// 已发布的导出符号（见 ios/SYMBOL_COLLISION_FIX_REPORT.md 的符号表），改签名
// 会破坏 ABI。原函数保留为"传平台默认策略"的薄包装。
int64_t tt_netmon_query_default_ifindex_ex(int vpnPolicy);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TTSIGNAL_INETWORK_PATH_MONITOR_H_INCLUDED__
