///////////////////////////////////////////////////////////////////////////////
// file   : NetworkRouteLookup.h
// author : anto
//
// Cross-platform kernel-route-table query. Given a destination sockaddr,
// ask the OS which network interface a packet sent right now would
// naturally egress from. Used by UDPSender to validate (and on macOS,
// proactively correct) the IP_BOUND_IF / IP_UNICAST_IF pin selected by
// the path monitor.
//
// Platform implementations:
//   * macOS                -> apple/AppleRouteLookup.cpp (PF_ROUTE/RTM_GET)
//   * Linux (non-Android)  -> linux/LinuxRouteLookup.cpp (NETLINK_ROUTE/RTM_GETROUTE)
//   * Windows              -> win32/WinRouteLookup.cpp   (GetBestInterfaceEx)
//   * iOS / Android / etc  -> stub returning 0 (no SDK or sandbox support)
//
// The kernel route table is ground truth: it knows whether a peer is
// reachable via the currently active physical interface, a utunN VPN
// tunnel, or something else. Pure IP-range heuristics (e.g. detecting
// well-known TUN fake-IP ranges) only catch a few well-known cases.
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_NETWORK_ROUTE_LOOKUP_H
#define TT_NETWORK_ROUTE_LOOKUP_H

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resolve the kernel-chosen egress interface for a destination.
 *
 * The exact mechanism varies by platform:
 *   - macOS:   PF_ROUTE socket + RTM_GET (~tens of microseconds).
 *   - Linux:   NETLINK_ROUTE socket + RTM_GETROUTE + RTA_DST.
 *   - Windows: GetBestInterfaceEx from iphlpapi.
 *
 * Each call uses its own short-lived OS handle and is safe to invoke
 * concurrently from multiple threads.
 *
 * @param dst      Destination sockaddr (AF_INET or AF_INET6). The port
 *                 is ignored by the route-lookup APIs on every platform.
 * @param dst_len  Length of *dst (typically sizeof(sockaddr_in) or
 *                 sizeof(sockaddr_in6)).
 *
 * @return         ifIndex (>= 1) of the egress interface that the
 *                 current routing table would pick for @p dst, or 0 on
 *                 any failure (unsupported family, no route, sandbox /
 *                 permission denied, malformed reply, timeout, etc.).
 *                 Callers MUST treat 0 as "don't know" and either fall
 *                 back to a heuristic or leave the existing binding
 *                 untouched.
 *
 * A 1-2 second timeout is enforced internally so the call always returns
 * even under degenerate OS conditions.
 */
uint32_t tt_route_lookup_ifindex(const struct sockaddr* dst,
                                 socklen_t dst_len);

#ifdef __cplusplus
}
#endif

#endif // TT_NETWORK_ROUTE_LOOKUP_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
