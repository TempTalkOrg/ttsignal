///////////////////////////////////////////////////////////////////////////////
// file   : AppleRouteLookup.cpp
// author : anto
//
// PF_ROUTE / RTM_GET implementation of tt_route_lookup_ifindex().
//
// The BSD routing socket (route(4)) lets userspace ask the kernel about
// the current routing table without modifying it. We send an RTM_GET
// message whose payload is a single destination sockaddr; the kernel
// replies with the same header plus an array of sockaddrs — one entry
// for each bit set in rtm_addrs. The RTA_IFP entry of the reply is a
// sockaddr_dl whose sdl_index is the interface the kernel would route
// a real packet through right now.
//
// This is what `route get <ip>` (the userland tool) does under the hood;
// see Apple/Darwin source `network_cmds/route.tproj/route.c`.
///////////////////////////////////////////////////////////////////////////////

#include "../NetworkRouteLookup.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// PF_ROUTE / RTM_GET is only fully implemented (and SDK-exposed) on macOS.
// The iOS / iPadOS / tvOS / watchOS public SDKs deliberately omit
// <net/route.h> and <net/if_dl.h> because sandboxed apps are not allowed
// to query the kernel routing table directly. We therefore only compile
// the real implementation under TARGET_OS_OSX; on every other Apple
// target the function is a stub that returns 0, and UDPSender falls back
// to its fake-IP heuristic exactly as it did before this file existed.
#if defined(__APPLE__) && defined(TARGET_OS_OSX) && TARGET_OS_OSX

#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{

// BSD route-socket sockaddrs are padded to a 4-byte boundary; a zero-
// length sa is encoded as 4 bytes. This rounding is part of the wire
// protocol (see <net/route.h> rt_xaddrs() macro family).
inline size_t sa_roundup(const struct sockaddr* sa)
{
    size_t len = sa->sa_len;
    if (len == 0)
    {
        len = sizeof(uint32_t);
    }
    return (len + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
}

// Walk the trailing sockaddr array following an rt_msghdr and return
// the sockaddr corresponding to RTAX_IFP (interface descriptor), or
// nullptr if RTA_IFP was not set in @p addrs_mask. @p p points at the
// first sockaddr (i.e. right after the rt_msghdr); @p end is one past
// the last valid byte of the message.
const struct sockaddr* find_ifp_sa(const char* p,
                                   const char* end,
                                   int addrs_mask)
{
    for (int i = 0; i < RTAX_MAX; ++i)
    {
        if (!(addrs_mask & (1 << i)))
        {
            continue;
        }
        if (p + sizeof(struct sockaddr) > end)
        {
            return nullptr;
        }
        const struct sockaddr* sa =
            reinterpret_cast<const struct sockaddr*>(p);
        size_t step = sa_roundup(sa);
        if (p + step > end)
        {
            return nullptr;
        }
        if (i == RTAX_IFP)
        {
            return sa;
        }
        p += step;
    }
    return nullptr;
}

}  // namespace

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* dst,
                                            socklen_t dst_len)
{
    if (dst == nullptr || dst_len == 0)
    {
        return 0;
    }
    if (dst->sa_family != AF_INET && dst->sa_family != AF_INET6)
    {
        return 0;
    }

    int sock = socket(PF_ROUTE, SOCK_RAW, AF_UNSPEC);
    if (sock < 0)
    {
        return 0;
    }

    // Hard upper bound on how long we wait for the kernel reply. RTM_GET
    // is normally well under 1 ms but we MUST guarantee progress under
    // pathological conditions (route socket backlog, kernel under load).
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 500000;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Build "rt_msghdr | dst sockaddr (4-byte aligned)".
    char buf[512];
    memset(buf, 0, sizeof(buf));

    struct rt_msghdr* hdr = reinterpret_cast<struct rt_msghdr*>(buf);
    char* sa_area = buf + sizeof(*hdr);

    size_t dst_align = (static_cast<size_t>(dst_len) + sizeof(uint32_t) - 1) &
                       ~(sizeof(uint32_t) - 1);
    if (sizeof(*hdr) + dst_align > sizeof(buf))
    {
        close(sock);
        return 0;
    }
    memcpy(sa_area, dst, dst_len);

    // Use our pid as a discriminator so we can ignore unrelated route
    // events (other processes' RTM_ADD / RTM_DELETE) that may land on
    // the same socket. seq=1 is sufficient because we only issue one
    // request per socket and tear the socket down right after.
    const pid_t my_pid = getpid();
    hdr->rtm_msglen = static_cast<u_short>(sizeof(*hdr) + dst_align);
    hdr->rtm_version = RTM_VERSION;
    hdr->rtm_type = RTM_GET;
    hdr->rtm_addrs = RTA_DST | RTA_IFP;
    hdr->rtm_pid = my_pid;
    hdr->rtm_seq = 1;
    hdr->rtm_flags = RTF_UP;

    if (write(sock, buf, hdr->rtm_msglen) < 0)
    {
        close(sock);
        return 0;
    }

    // The route socket is shared by all userspace consumers, so the
    // first datagram we read might belong to someone else. Drain a few
    // (bounded) messages until we see one matching our (pid, seq).
    char rbuf[1024];
    uint32_t result = 0;
    for (int tries = 0; tries < 16; ++tries)
    {
        ssize_t n = read(sock, rbuf, sizeof(rbuf));
        if (n <= 0)
        {
            break;
        }
        if (static_cast<size_t>(n) < sizeof(struct rt_msghdr))
        {
            continue;
        }
        struct rt_msghdr* rhdr = reinterpret_cast<struct rt_msghdr*>(rbuf);
        if (rhdr->rtm_type != RTM_GET)
        {
            continue;
        }
        if (rhdr->rtm_seq != 1)
        {
            continue;
        }
        if (rhdr->rtm_pid != my_pid)
        {
            continue;
        }
        if (rhdr->rtm_errno != 0)
        {
            break;
        }
        const char* sa_begin = rbuf + sizeof(*rhdr);
        const char* sa_end = rbuf + n;
        const struct sockaddr* ifp =
            find_ifp_sa(sa_begin, sa_end, rhdr->rtm_addrs);
        if (ifp != nullptr && ifp->sa_family == AF_LINK)
        {
            const struct sockaddr_dl* sdl =
                reinterpret_cast<const struct sockaddr_dl*>(ifp);
            result = sdl->sdl_index;
        }
        break;
    }

    close(sock);
    return result;
}

#else  // !(macOS Apple platform)

// Stub for iOS / iPadOS / tvOS / watchOS / non-Apple builds. Callers must
// treat 0 as "lookup unavailable" and fall back to a heuristic.
extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* /*dst*/,
                                            socklen_t /*dst_len*/)
{
    return 0;
}

#endif  // macOS Apple platform

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
