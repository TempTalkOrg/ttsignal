///////////////////////////////////////////////////////////////////////////////
// file   : LinuxRouteLookup.cpp
// author : anto
//
// Linux implementation of tt_route_lookup_ifindex(). Uses a transient
// NETLINK_ROUTE socket and an RTM_GETROUTE request with an RTA_DST
// attribute pointing at the caller's destination, so the kernel can
// answer "if I were to send a packet to <dst> right now, which interface
// would it go out?".
//
// Compare with the broader-coverage RTM_GETROUTE in LinuxNetlinkMonitor:
// that one asks for the default route (dst_len=0) to track which adapter
// "is the internet"; this one asks about a specific host so we can sanity
// check whether the IP_UNICAST_IF hint we're about to apply actually
// agrees with the real outgoing interface for the peer.
//
// ⚠️ scope_ifindex 在 Linux 上被**刻意忽略**，查询始终针对全局路由表。
//
// netlink 本来提供了等价物：给 RTM_GETROUTE 请求带一个 RTA_OIF 属性，内核
// 就会按"从这块网卡发"去做 FIB 查询，效果与 macOS 的 RTF_IFSCOPE 相同。
// 之所以不用，是因为 Linux 上的"绑网卡"未必是硬绑：
//
//   * SocketPinner 在 force-physical 下优先 SO_BINDTODEVICE（硬绑，内核会
//     按 oif 重做 FIB 查询），但该选项需要 CAP_NET_RAW / root；
//   * 拿不到权限时它会回落到 IP_UNICAST_IF 并**照样返回 TT_PIN_OK**。
//     IP_UNICAST_IF 只是软提示，内核完全可以把包交给默认路由——也就是
//     VPN 隧道。
//
// 调用方（TcpChannel::_RouteRecheck）拿到的只是"绑上了"，它无法保证拿到的
// 是硬绑还是软提示。如果这里按 RTA_OIF 做 scoped 查询，软提示那一路会得到
// "一致"的答复，force-physical 就会放行一条实际走 VPN 的连接——正是这套
// 复核存在的意义所在。非 scoped 查询在这里是"失败关闭"的那一侧：最坏情况
// 是多报一次不一致，而不是漏掉一次真的钻进隧道。
//
// 等哪天 SocketPinner 把"实际用了哪种手段"回传到复核点，这里可以改成
// "SO_BINDTODEVICE ⇒ 走 RTA_OIF scoped 查询，IP_UNICAST_IF ⇒ 维持全局查询"。
///////////////////////////////////////////////////////////////////////////////

#include "../NetworkRouteLookup.h"

#if defined(__linux__) && !defined(__ANDROID__) && !defined(OS_ANDROID)

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{

// netlink attributes are 4-byte aligned, like rtnetlink everywhere else.
// We don't trust the toolchain's NLMSG_LENGTH / RTA_LENGTH macros to be
// constexpr-friendly across libc versions, so the constants below are
// computed at runtime.
inline size_t nlmsg_align(size_t n)
{
    return (n + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
}

}  // namespace

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* dst,
                                            socklen_t dst_len,
                                            uint32_t scope_ifindex)
{
    // 见文件头：Linux 刻意不做 scoped 查询。
    (void)scope_ifindex;

    if (dst == nullptr || dst_len == 0)
    {
        return 0;
    }
    if (dst->sa_family != AF_INET && dst->sa_family != AF_INET6)
    {
        return 0;
    }

    int sock = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (sock < 0)
    {
        return 0;
    }

    // Hard ceiling on how long we wait for the kernel. RTM_GETROUTE
    // round-trips are normally <1 ms but we MUST not block the caller.
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 500000;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Build the request: nlmsghdr | rtmsg | RTA_DST attribute.
    char reqbuf[512];
    memset(reqbuf, 0, sizeof(reqbuf));
    struct nlmsghdr* nlh = reinterpret_cast<struct nlmsghdr*>(reqbuf);
    struct rtmsg* rtm = reinterpret_cast<struct rtmsg*>(NLMSG_DATA(nlh));

    const uint32_t my_seq = 1;
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq = my_seq;
    nlh->nlmsg_pid = 0;  // 0 = let the kernel assign; we filter on seq.
    rtm->rtm_family = dst->sa_family;

    char* rta_area =
        reinterpret_cast<char*>(nlh) + NLMSG_SPACE(sizeof(struct rtmsg));
    struct rtattr* rta = reinterpret_cast<struct rtattr*>(rta_area);
    rta->rta_type = RTA_DST;

    if (dst->sa_family == AF_INET)
    {
        const struct sockaddr_in* sin =
            reinterpret_cast<const struct sockaddr_in*>(dst);
        rta->rta_len = RTA_LENGTH(sizeof(struct in_addr));
        memcpy(RTA_DATA(rta), &sin->sin_addr, sizeof(struct in_addr));
        rtm->rtm_dst_len = 32;
    }
    else
    {
        const struct sockaddr_in6* sin6 =
            reinterpret_cast<const struct sockaddr_in6*>(dst);
        rta->rta_len = RTA_LENGTH(sizeof(struct in6_addr));
        memcpy(RTA_DATA(rta), &sin6->sin6_addr, sizeof(struct in6_addr));
        rtm->rtm_dst_len = 128;
    }
    nlh->nlmsg_len = NLMSG_SPACE(sizeof(struct rtmsg)) + RTA_ALIGN(rta->rta_len);
    (void)nlmsg_align;  // silence unused-function warning across gcc/clang

    if (send(sock, reqbuf, nlh->nlmsg_len, 0) < 0)
    {
        close(sock);
        return 0;
    }

    // Drain replies until we find one matching our seq (other consumers
    // of NETLINK_ROUTE on the same socket are not possible since we own
    // it exclusively, but a kernel-initiated multicast can still slip in
    // on broken kernels — be defensive).
    char rbuf[8192];
    uint32_t result = 0;
    for (int tries = 0; tries < 16; ++tries)
    {
        ssize_t n = recv(sock, rbuf, sizeof(rbuf), 0);
        if (n <= 0)
        {
            break;
        }
        size_t len = static_cast<size_t>(n);
        struct nlmsghdr* nh = reinterpret_cast<struct nlmsghdr*>(rbuf);
        bool stop = false;
        for (; NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len))
        {
            if (nh->nlmsg_seq != my_seq)
            {
                continue;
            }
            if (nh->nlmsg_type == NLMSG_DONE ||
                nh->nlmsg_type == NLMSG_ERROR)
            {
                stop = true;
                break;
            }
            if (nh->nlmsg_type != RTM_NEWROUTE)
            {
                continue;
            }
            struct rtmsg* rt =
                reinterpret_cast<struct rtmsg*>(NLMSG_DATA(nh));
            int rtl = RTM_PAYLOAD(nh);
            for (struct rtattr* attr = RTM_RTA(rt); RTA_OK(attr, rtl);
                 attr = RTA_NEXT(attr, rtl))
            {
                if (attr->rta_type == RTA_OIF)
                {
                    result = *reinterpret_cast<uint32_t*>(RTA_DATA(attr));
                    stop = true;
                    break;
                }
            }
            if (stop)
            {
                break;
            }
        }
        if (stop || result != 0)
        {
            break;
        }
    }

    close(sock);
    return result;
}

#else  // !(Linux non-Android)

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* /*dst*/,
                                            socklen_t /*dst_len*/,
                                            uint32_t /*scope_ifindex*/)
{
    return TT_ROUTE_IFINDEX_UNKNOWN;
}

#endif  // Linux non-Android

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
