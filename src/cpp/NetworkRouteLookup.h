///////////////////////////////////////////////////////////////////////////////
// file   : NetworkRouteLookup.h
// author : anto
//
// Cross-platform kernel-route-table query. Given a destination sockaddr,
// ask the OS which network interface a packet sent right now would
// egress from — either from the global routing table (scope_ifindex=0)
// or from a specific interface's SCOPED routing table (scope_ifindex>0,
// i.e. "what would a socket already bound to that interface do?").
// Used by UDPSender and TcpChannel to validate the IP_BOUND_IF /
// IP_UNICAST_IF pin selected by the path monitor.
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
 * @brief Well-known return values of tt_route_lookup_ifindex().
 *
 * TT_ROUTE_IFINDEX_UNKNOWN (0) — "don't know". The query could not be
 *   answered: unsupported family, sandboxed / EPERM, malformed reply,
 *   timeout, or a platform without a real implementation. Callers MUST
 *   treat it as "no information" and never as a failure signal.
 *
 * TT_ROUTE_IFINDEX_NO_ROUTE — the query DID get a definitive answer and
 *   that answer is "there is no route to this destination within the
 *   requested scope". Only ever returned when @p scope_ifindex is
 *   non-zero AND the platform implements scoped lookups (macOS today);
 *   a non-scoped query keeps reporting 0 for this case, exactly as it
 *   did before the scope parameter existed.
 */
#define TT_ROUTE_IFINDEX_UNKNOWN   ((uint32_t)0)
#define TT_ROUTE_IFINDEX_NO_ROUTE  ((uint32_t)0xFFFFFFFFu)

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
 * @param scope_ifindex
 *                 0  — ask the global (non-scoped) routing table: "where
 *                      would an unbound socket send this?". This is the
 *                      historical behaviour and is bit-for-bit unchanged.
 *                 >0 — ask the SCOPED routing table of that interface:
 *                      "where would a socket already hard-bound to this
 *                      interface send this?". Pass the ifIndex the socket
 *                      was pinned to.
 *
 * Why the scope parameter exists — the false-negative it removes:
 *   macOS IP_BOUND_IF (and Linux SO_BINDTODEVICE) is a HARD bind: the
 *   kernel redoes the FIB lookup in that interface's scoped table and
 *   ignores whatever owns the global default route. So when a VPN grabs
 *   the default route, a NON-scoped query answers "utunN" while the
 *   pinned socket demonstrably still egresses the physical NIC. Using
 *   that answer to validate the pin rejects perfectly good connections.
 *   A scoped query asks the same question the kernel will actually ask
 *   for that socket, so the answer matches reality.
 *
 * Which platforms honour it (deliberately NOT all of them):
 *   - macOS   — honoured: RTM_GET with RTF_IFSCOPE + rtm_index. This is
 *               what `route -n get -ifscope <if> <ip>` does.
 *   - Linux   — IGNORED on purpose. See linux/LinuxRouteLookup.cpp for
 *               the full argument: force-physical there may end up
 *               pinned with the SOFT IP_UNICAST_IF hint (SO_BINDTODEVICE
 *               needs CAP_NET_RAW), and only a non-scoped query can
 *               still catch "the packet is about to enter the tunnel".
 *   - Windows — IGNORED: GetBestInterfaceEx has no scope concept.
 *   - iOS / Android — stub, always TT_ROUTE_IFINDEX_UNKNOWN.
 *
 * @return         ifIndex (>= 1) of the egress interface the routing
 *                 table would pick for @p dst under @p scope_ifindex, or
 *                 TT_ROUTE_IFINDEX_UNKNOWN / TT_ROUTE_IFINDEX_NO_ROUTE
 *                 as documented above.
 *
 * A 1-2 second timeout is enforced internally so the call always returns
 * even under degenerate OS conditions.
 */
uint32_t tt_route_lookup_ifindex(const struct sockaddr* dst,
                                 socklen_t dst_len,
                                 uint32_t scope_ifindex);

#ifdef __cplusplus
}
#endif

#endif // TT_NETWORK_ROUTE_LOOKUP_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
