








































#include "../NetworkRouteLookup.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif








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




inline size_t sa_roundup(const struct sockaddr* sa)
{
    size_t len = sa->sa_len;
    if (len == 0)
    {
        len = sizeof(uint32_t);
    }
    return (len + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
}






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

}  

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

    
    
    
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 500000;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    
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
            
            
            sa_dst->sa_len = static_cast<uint8_t>(dst_len);
        }
    }

    
    
    
    
    const pid_t my_pid = getpid();
    hdr->rtm_msglen = static_cast<u_short>(sizeof(*hdr) + dst_align);
    hdr->rtm_version = RTM_VERSION;
    hdr->rtm_type = RTM_GET;
    hdr->rtm_addrs = RTA_DST | RTA_IFP;
    hdr->rtm_pid = my_pid;
    hdr->rtm_seq = 1;
    hdr->rtm_flags = RTF_UP;

    
    
    
    if (scope_ifindex != 0)
    {
        hdr->rtm_flags |= RTF_IFSCOPE;
        hdr->rtm_index = static_cast<u_short>(scope_ifindex);
    }

    if (write(sock, buf, hdr->rtm_msglen) < 0)
    {
        
        
        
        
        
        const int werr = errno;
        close(sock);
        if (scope_ifindex != 0 &&
            (werr == ESRCH || werr == ENETUNREACH || werr == EHOSTUNREACH))
        {
            return TT_ROUTE_IFINDEX_NO_ROUTE;
        }
        return TT_ROUTE_IFINDEX_UNKNOWN;
    }

    
    
    
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

#else  



extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* ,
                                            socklen_t ,
                                            uint32_t )
{
    return TT_ROUTE_IFINDEX_UNKNOWN;
}

#endif  




