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
//
// Scoped lookups (scope_ifindex != 0) set RTF_IFSCOPE and rtm_index,
// which is exactly what `route -n get -ifscope <if> <ip>` does. Darwin
// then resolves the destination inside that interface's scoped routing
// table instead of the global one. That distinction is the whole point:
// IP_BOUND_IF makes the kernel do a scoped FIB lookup for the socket, so
// only a scoped query predicts where a pinned socket's packets really go.
//
// Two behaviours of the scoped table, both verified on macOS 15 with
// GlobalProtect owning the default route, are worth writing down because
// callers depend on them:
//
//   1. A scoped lookup IGNORES more-specific non-scoped routes that live
//      on other interfaces. With `10.10.0.1 -> utun4` present in the
//      global table, `-ifscope en0 10.10.0.1` still answers with en0's
//      own default route. So a scoped answer is essentially always the
//      scope interface itself whenever that interface has any matching
//      route — it can confirm "the pin is viable", it can NOT be used to
//      detect "this peer is only reachable through the tunnel".
//   2. When the scoped interface has no matching route at all, write()
//      on the route socket fails with ESRCH ("not in table") rather than
//      returning a reply. That is a definitive negative answer, so we
//      surface it as TT_ROUTE_IFINDEX_NO_ROUTE instead of folding it
//      into the ambiguous 0.
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

#include <errno.h>
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
                                            socklen_t dst_len,
                                            uint32_t scope_ifindex)
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

    // ⚠️ 必须把 sa_len 补正，不能信任调用方传进来的值。
    //
    // PF_ROUTE 的报文体是一串变长 sockaddr，内核**完全依赖每个条目自己的
    // sa_len** 来断句。sa_len 为 0 时内核读不到地址字节，RTM_GET 退化成
    // "查默认路由"，于是任何目的地址都会得到同一个答案 —— 查询静默地在回答
    // 另一个问题，而不是报错。
    //
    // 这不是假想情况：本仓 BCSockAddrS 的所有构造函数（deps/env 的
    // bc_sockaddr_fromin / _fromin6 等）都把 sin_len 的赋值放在
    // #ifdef BC_PLATFORM_HAVESALEN 里，而这个宏**全仓从未定义**，所以
    // TcpChannel::peer_addr_ / UDPSender 传下来的 sockaddr 里 sa_len 恒为 0。
    // 实测（en0=14 / utun4=22，GlobalProtect 全隧道）：
    //
    //   sa_len=16 : 127.0.0.1 -> global=1(lo0)  scoped(en0)=1(lo0)   ← 正确
    //   sa_len=0  : 127.0.0.1 -> global=22      scoped(en0)=14       ← 默认路由
    //
    // 后果是 scoped 复核对每个对端都答"绑定的网卡自己"，force-physical 的
    // 不可达检测与 prefer-physical 的绑定有效性判定一起失效。补正放在这里
    // 而不是各调用点，是因为这是唯一的收口位置，对所有调用方都生效。
    {
        struct sockaddr* sa_dst = reinterpret_cast<struct sockaddr*>(sa_area);
        if (sa_dst->sa_family == AF_INET)
        {
            sa_dst->sa_len = static_cast<uint8_t>(sizeof(struct sockaddr_in));
        }
        else if (sa_dst->sa_family == AF_INET6)
        {
            sa_dst->sa_len = static_cast<uint8_t>(sizeof(struct sockaddr_in6));
        }
        else
        {
            // 上面的地址族校验已经挡掉了其它取值，走不到这里；保守起见按
            // 调用方给的长度填，至少不会是 0。
            sa_dst->sa_len = static_cast<uint8_t>(dst_len);
        }
    }

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

    // scope_ifindex != 0 —— 改问"已经硬绑在这块网卡上的 socket 会怎么走"。
    // rtm_index 必须一起填，只置 RTF_IFSCOPE 而不给 index 的话内核会按
    // scope=0 处理，等于没 scoped。
    if (scope_ifindex != 0)
    {
        hdr->rtm_flags |= RTF_IFSCOPE;
        hdr->rtm_index = static_cast<u_short>(scope_ifindex);
    }

    if (write(sock, buf, hdr->rtm_msglen) < 0)
    {
        // scoped 查询下，"该网卡的 scoped 路由表里没有到 dst 的路由"是通过
        // write() 的 ESRCH / ENETUNREACH / EHOSTUNREACH 报出来的，而不是回一条
        // 带 rtm_errno 的应答（`route -n get -ifscope lo0 <公网 IP>` 打印的
        // "not in table" 就是这条路径）。这是确定性的否定答案，必须和"查不了"
        // 区分开，否则 force-physical 会把真正的不可达当成"不知道"放行。
        const int werr = errno;
        close(sock);
        if (scope_ifindex != 0 &&
            (werr == ESRCH || werr == ENETUNREACH || werr == EHOSTUNREACH))
        {
            return TT_ROUTE_IFINDEX_NO_ROUTE;
        }
        return TT_ROUTE_IFINDEX_UNKNOWN;
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
            // 少数内核版本会用应答里的 rtm_errno 而不是 write() 失败来报
            // "无路由"，两条路径给同样的结论。
            if (scope_ifindex != 0 &&
                (rhdr->rtm_errno == ESRCH ||
                 rhdr->rtm_errno == ENETUNREACH ||
                 rhdr->rtm_errno == EHOSTUNREACH))
            {
                result = TT_ROUTE_IFINDEX_NO_ROUTE;
            }
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
                                            socklen_t /*dst_len*/,
                                            uint32_t /*scope_ifindex*/)
{
    return TT_ROUTE_IFINDEX_UNKNOWN;
}

#endif  // macOS Apple platform

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
