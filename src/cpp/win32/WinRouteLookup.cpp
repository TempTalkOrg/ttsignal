



















#include "../NetworkRouteLookup.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* dst,
                                            socklen_t ,
                                            uint32_t )
{
    if (dst == NULL)
    {
        return 0;
    }
    if (dst->sa_family != AF_INET && dst->sa_family != AF_INET6)
    {
        return 0;
    }

    
    
    
    DWORD ifIndex = 0;
    DWORD rc = GetBestInterfaceEx(
        const_cast<struct sockaddr*>(dst), &ifIndex);
    if (rc != NO_ERROR)
    {
        return 0;
    }
    return static_cast<uint32_t>(ifIndex);
}

#else  

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* ,
                                            socklen_t ,
                                            uint32_t )
{
    return TT_ROUTE_IFINDEX_UNKNOWN;
}

#endif  




