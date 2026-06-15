///////////////////////////////////////////////////////////////////////////////
// file   : WinRouteLookup.cpp
// author : anto
//
// Windows implementation of tt_route_lookup_ifindex(). Trivially backed
// by iphlpapi's GetBestInterfaceEx, which is the documented one-line
// equivalent of macOS PF_ROUTE / Linux RTM_GETROUTE: "given a remote
// destination, which network interface would IPv4/IPv6 routing pick?".
//
// GetBestInterfaceEx has been available since Windows XP / Server 2003
// SP1 and is the same syscall WinIpChangeMonitor already uses to track
// the default route — see win32/WinIpChangeMonitor.cpp.
///////////////////////////////////////////////////////////////////////////////

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
                                            socklen_t /*dst_len*/)
{
    if (dst == NULL)
    {
        return 0;
    }
    if (dst->sa_family != AF_INET && dst->sa_family != AF_INET6)
    {
        return 0;
    }

    // GetBestInterfaceEx wants a non-const sockaddr*. The API does not
    // actually mutate the destination — the const_cast is purely to
    // match a stale prototype that predates C-correctness conventions.
    DWORD ifIndex = 0;
    DWORD rc = GetBestInterfaceEx(
        const_cast<struct sockaddr*>(dst), &ifIndex);
    if (rc != NO_ERROR)
    {
        return 0;
    }
    return static_cast<uint32_t>(ifIndex);
}

#else  // !_WIN32

extern "C" uint32_t tt_route_lookup_ifindex(const struct sockaddr* /*dst*/,
                                            socklen_t /*dst_len*/)
{
    return 0;
}

#endif  // _WIN32

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
