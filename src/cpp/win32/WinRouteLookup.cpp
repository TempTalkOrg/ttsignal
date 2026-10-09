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
//
// scope_ifindex 在这里被忽略：GetBestInterfaceEx 没有"限定在某块网卡的
// 路由表里查"这个概念，Windows 也没有对应的公开 API。查询因此始终等价于
// scope_ifindex=0，即全局路由表。这与 IP_UNICAST_IF 的行为是匹配的——
// Windows 上它同样不是硬绑（见 UDPSender::Connect 的平台注释：实现会强制
// 源 IP 取自被提示的网卡，但路由仍可能落到别的出口），所以全局查询正是
// 检出"源/路由劈叉"所需要的那个问题。
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
                                            socklen_t /*dst_len*/,
                                            uint32_t /*scope_ifindex*/)
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
                                            socklen_t /*dst_len*/,
                                            uint32_t /*scope_ifindex*/)
{
    return TT_ROUTE_IFINDEX_UNKNOWN;
}

#endif  // _WIN32

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
