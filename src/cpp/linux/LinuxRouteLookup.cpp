




































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





inline size_t nlmsg_align(size_t n)
{
    return (n + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
}

}  

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* dst,
                                            socklen_t dst_len,
                                            uint32_t scope_ifindex)
{
    
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

    
    
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 500000;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    
    char reqbuf[512];
    memset(reqbuf, 0, sizeof(reqbuf));
    struct nlmsghdr* nlh = reinterpret_cast<struct nlmsghdr*>(reqbuf);
    struct rtmsg* rtm = reinterpret_cast<struct rtmsg*>(NLMSG_DATA(nlh));

    const uint32_t my_seq = 1;
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq = my_seq;
    nlh->nlmsg_pid = 0;  
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
    (void)nlmsg_align;  

    if (send(sock, reqbuf, nlh->nlmsg_len, 0) < 0)
    {
        close(sock);
        return 0;
    }

    
    
    
    
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

#else  

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* ,
                                            socklen_t ,
                                            uint32_t )
{
    return TT_ROUTE_IFINDEX_UNKNOWN;
}

#endif  




