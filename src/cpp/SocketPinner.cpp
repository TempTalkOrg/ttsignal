///////////////////////////////////////////////////////////////////////////////
// file   : SocketPinner.cpp
// author : anto
//
// 平台相关的 socket 网卡绑定实现。代码从 UDPSender::_InitSocket 与
// UDPSender::_TryClearInterfaceBinding 抽出，行为保持一致。
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "SocketPinner.h"
#include "Utils.h"

#include <string.h>
#include <errno.h>

#ifdef OS_ANDROID
#include <dlfcn.h>
#include <android/api-level.h>
#endif

#if defined(__APPLE__)
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#if defined(__linux__) && !defined(OS_ANDROID)
#include <netinet/in.h>
#include <sys/socket.h>
#include <net/if.h>     // if_indextoname / IF_NAMESIZE for SO_BINDTODEVICE
// IPV6_UNICAST_IF 在 5.7 之前的 uapi 头里没有，但它是稳定的内核 ABI。
#ifndef IPV6_UNICAST_IF
#define IPV6_UNICAST_IF 76
#endif
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

// outMethod 允许为 NULL；统一走这个 helper 避免每个分支都判空。
static void _set_method(char* outMethod, size_t len, const char* name)
{
    if (outMethod == NULL || len == 0) return;
    snprintf(outMethod, len, "%s", name ? name : "");
}

static void _set_errno(int* outErrno, int value)
{
    if (outErrno) *outErrno = value;
}

TTPinResult tt_socket_pin(const TTPinRequest* req,
                          char* outMethod, size_t outMethodLen,
                          int* outErrno)
{
    _set_method(outMethod, outMethodLen, "");
    _set_errno(outErrno, 0);

    if (req == NULL) {
        return TT_PIN_FAILED;
    }

#ifdef OS_ANDROID
    if (req->androidNetHandle == 0) {
        return TT_PIN_NOT_NEEDED;
    }
#else
    if (req->ifIndex == 0) {
        return TT_PIN_NOT_NEEDED;
    }
#endif

    if (req->fd < 0) {
        _set_errno(outErrno, EBADF);
        return TT_PIN_FAILED;
    }

#ifdef OS_ANDROID
    if (android_get_device_api_level() >= 23)
    {
        typedef int (*pfn_android_setsocknetwork)(uint64_t, int);
        static pfn_android_setsocknetwork fn = (pfn_android_setsocknetwork)
            dlsym(RTLD_DEFAULT, "android_setsocknetwork");
        if (fn)
        {
            int ret = fn((uint64_t)req->androidNetHandle, req->fd);
            LogQ(req->loggerCtx, _DEBUG_,
                "UDP Sender: android_setsocknetwork(handle=%lld, fd=%d) = %d, errno=%d",
                (long long)req->androidNetHandle, req->fd, ret, ret == 0 ? 0 : errno);
            if (ret == 0)
            {
                _set_method(outMethod, outMethodLen, "android_setsocknetwork");
                return TT_PIN_OK;
            }
            _set_errno(outErrno, errno);
            return TT_PIN_FAILED;
        }
        else
        {
            LogQ(req->loggerCtx, _WARN_,
                "UDP Sender: android_setsocknetwork not found via dlsym");
            return TT_PIN_UNSUPPORTED;
        }
    }
    return TT_PIN_UNSUPPORTED;
#elif defined(__APPLE__)
    // macOS + iOS: bind the UDP socket to a specific interface so the kernel
    // stops following the system "best path" automatically. The caller
    // (AppleNetworkMonitor) passes an ifIndex from if_nametoindex() through
    // the SMPConnection::Restart -> UDPSender::Restart chain as
    // m_nNetworkHandle. IP_BOUND_IF is the Apple equivalent of Android's
    // android_setsocknetwork. Fake-IP / TUN-mode-VPN handling lives in
    // UDPSender::Connect (it's the only entry point that learns the peer
    // address), so we just install the pin unconditionally here.
    {
        uint32_t ifIndex = req->ifIndex;
        int fd = req->fd;
        int retV4 = setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &ifIndex, sizeof(ifIndex));
        int errV4 = (retV4 == 0) ? 0 : errno;
        int retV6 = 0;
        int errV6 = 0;
        if (req->ipv6)
        {
            retV6 = setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &ifIndex, sizeof(ifIndex));
            errV6 = (retV6 == 0) ? 0 : errno;
        }
        LogQ(req->loggerCtx, _DEBUG_,
            "UDP Sender: setsockopt(IP_BOUND_IF, ifIndex=%u, fd=%d) v4=%d/errno=%d v6=%d/errno=%d",
            ifIndex, fd, retV4, errV4, retV6, errV6);
        if (retV4 == 0 || (req->ipv6 && retV6 == 0))
        {
            _set_method(outMethod, outMethodLen, "IP_BOUND_IF");
            return TT_PIN_OK;
        }
        _set_errno(outErrno, (retV4 != 0) ? errV4 : errV6);
        return TT_PIN_FAILED;
    }
#elif defined(__linux__) && !defined(OS_ANDROID)
    // Linux (non-Android): bind via IP_UNICAST_IF. Caller (LinuxNetlinkMonitor)
    // passes ifIndex obtained from RTM_GETROUTE/RTA_OIF in m_nNetworkHandle.
    // Index is in HOST byte order on Linux — DO NOT htonl here (Windows is
    // the only platform that requires byte-swapping).
    {
        uint32_t ifIndex = req->ifIndex;
        int fd = req->fd;

        // force-physical 下优先用 SO_BINDTODEVICE。
        //
        // 这是 Linux 上唯一能真正保证"包从这块网卡出去"的手段。
        // IP_UNICAST_IF 在 kernel < 6.0.16 / 6.1.2 / 6.2 上对已 connect 的
        // UDP socket 被静默忽略（路由在 connect 时被缓存，缓存绕过
        // fib_lookup），等于没绑。
        //
        // 而"只绑源 IP"是不够的：路由仍走 tun 时，包带着物理网卡的源 IP
        // 进入 VPN，会被 NAT 改写或直接丢弃——服务端照样看不到真实 IP。
        //
        // SO_BINDTODEVICE 需要 CAP_NET_RAW（或 root）。没权限时回落到
        // IP_UNICAST_IF 并告警：此时 force-physical 只能保证"失败可见"，
        // 不能保证"拿到真实 IP"。
        bool bound_to_device = false;
        if (req->policy == TT_VPN_POLICY_FORCE_PHYSICAL)
        {
            char ifname[IF_NAMESIZE] = {0};
            if (if_indextoname(ifIndex, ifname) != NULL)
            {
                int r = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                    ifname, (socklen_t)strlen(ifname));
                if (r == 0)
                {
                    bound_to_device = true;
                    LogQ(req->loggerCtx, _INFO_,
                        "UDP Sender: setsockopt(SO_BINDTODEVICE, %s, fd=%d) 成功，"
                        "force-physical 已硬绑物理网卡", ifname, fd);
                    _set_method(outMethod, outMethodLen, "SO_BINDTODEVICE");
                }
                else
                {
                    if (req->isTcp)
                    {
                        LogQ(req->loggerCtx, _INFO_,
                            "TCP: setsockopt(SO_BINDTODEVICE, %s) 失败 errno=%d"
                            "（缺 CAP_NET_RAW / 非 root）。回落 IP_UNICAST_IF——"
                            "该选项对 connect 前的 TCP socket 有效，force-physical "
                            "仍可保证走物理网卡。",
                            ifname, errno);
                    }
                    else
                    {
                        LogQ(req->loggerCtx, _WARN_,
                            "UDP Sender: setsockopt(SO_BINDTODEVICE, %s) 失败 errno=%d"
                            "（缺 CAP_NET_RAW / 非 root）。回落 IP_UNICAST_IF——该选项"
                            "在 kernel < 6.0.16/6.1.2/6.2 上对已 connect 的 UDP socket "
                            "无效，TUN 全局代理下 force-physical 可能拿不到真实 IP，"
                            "只能保证失败可见。",
                            ifname, errno);
                    }
                }
            }
            else
            {
                LogQ(req->loggerCtx, _WARN_,
                    "UDP Sender: if_indextoname(%u) 失败 errno=%d，"
                    "无法使用 SO_BINDTODEVICE，回落 IP_UNICAST_IF",
                    ifIndex, errno);
            }
        }

        if (!bound_to_device)
        {
            int retV4 = setsockopt(fd, IPPROTO_IP, IP_UNICAST_IF, &ifIndex, sizeof(ifIndex));
            int errV4 = (retV4 == 0) ? 0 : errno;
            int retV6 = 0;
            int errV6 = 0;
            if (req->ipv6)
            {
                retV6 = setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_IF, &ifIndex, sizeof(ifIndex));
                errV6 = (retV6 == 0) ? 0 : errno;
            }
            LogQ(req->loggerCtx, _DEBUG_,
                "UDP Sender: setsockopt(IP_UNICAST_IF, ifIndex=%u, fd=%d) v4=%d/errno=%d v6=%d/errno=%d",
                ifIndex, fd, retV4, errV4, retV6, errV6);
            if (retV4 == 0 || (req->ipv6 && retV6 == 0))
            {
                _set_method(outMethod, outMethodLen, "IP_UNICAST_IF");
                return TT_PIN_OK;
            }
            _set_errno(outErrno, (retV4 != 0) ? errV4 : errV6);
            return TT_PIN_FAILED;
        }
        return TT_PIN_OK;
    }
#elif defined(_WIN32)
    // Windows: IP_UNICAST_IF requires the IPv4 ifIndex in NETWORK byte order
    // (htonl). IPv6 stays in host order. Caller (WinIpChangeMonitor) supplies
    // ifIndex from GetBestInterfaceEx in m_nNetworkHandle.
    {
        DWORD ifIndex = (DWORD)req->ifIndex;
        DWORD ifIndexBE = htonl(ifIndex);
        int fd = req->fd;
        int retV4 = setsockopt((SOCKET)fd, IPPROTO_IP, IP_UNICAST_IF,
            (const char*)&ifIndexBE, sizeof(ifIndexBE));
        int errV4 = (retV4 == 0) ? 0 : WSAGetLastError();
        int retV6 = 0;
        int errV6 = 0;
        if (req->ipv6)
        {
            retV6 = setsockopt((SOCKET)fd, IPPROTO_IPV6, IPV6_UNICAST_IF,
                (const char*)&ifIndex, sizeof(ifIndex));
            errV6 = (retV6 == 0) ? 0 : WSAGetLastError();
        }
        LogQ(req->loggerCtx, _DEBUG_,
            "UDP Sender: setsockopt(IP_UNICAST_IF, ifIndex=%u, fd=%d) v4=%d/wsa=%d v6=%d/wsa=%d",
            (unsigned)ifIndex, fd, retV4, errV4, retV6, errV6);
        if (retV4 == 0 || (req->ipv6 && retV6 == 0))
        {
            _set_method(outMethod, outMethodLen, "IP_UNICAST_IF");
            return TT_PIN_OK;
        }
        _set_errno(outErrno, (retV4 != 0) ? errV4 : errV6);
        return TT_PIN_FAILED;
    }
#else
    return TT_PIN_UNSUPPORTED;
#endif
}

TTPinResult tt_socket_unpin(int fd, int ipv6, void* loggerCtx,
                            char* outMethod, size_t outMethodLen,
                            int* outErrno)
{
    _set_method(outMethod, outMethodLen, "");
    _set_errno(outErrno, 0);

    if (fd < 0)
    {
        return TT_PIN_FAILED;
    }

#if defined(__APPLE__)
    int retV4 = 0;
    int errV4 = 0;
    int retV6 = 0;
    int errV6 = 0;
    uint32_t zero = 0;
    retV4 = setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &zero, sizeof(zero));
    errV4 = (retV4 == 0) ? 0 : errno;
    if (ipv6)
    {
        retV6 = setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &zero, sizeof(zero));
        errV6 = (retV6 == 0) ? 0 : errno;
    }
    const char* opt_name = "IP_BOUND_IF";
    LogQ(loggerCtx, _DEBUG_,
        "SocketPinner: clearing %s fd=%d v4=%d/err=%d v6=%d/err=%d",
        opt_name, fd, retV4, errV4, retV6, errV6);
    _set_method(outMethod, outMethodLen, opt_name);
    _set_errno(outErrno, (errV4 != 0) ? errV4 : errV6);
    return (retV4 == 0 || (ipv6 && retV6 == 0)) ? TT_PIN_OK : TT_PIN_FAILED;
#elif defined(_WIN32)
    int retV4 = 0;
    int errV4 = 0;
    int retV6 = 0;
    int errV6 = 0;
    // IP_UNICAST_IF: IPv4 takes a network-byte-order DWORD, IPv6 takes a
    // host-order DWORD. Zero is byte-order-agnostic so we pass it as-is.
    DWORD zero = 0;
    retV4 = setsockopt((SOCKET)fd, IPPROTO_IP, IP_UNICAST_IF,
        (const char*)&zero, sizeof(zero));
    errV4 = (retV4 == 0) ? 0 : WSAGetLastError();
    if (ipv6)
    {
        retV6 = setsockopt((SOCKET)fd, IPPROTO_IPV6, IPV6_UNICAST_IF,
            (const char*)&zero, sizeof(zero));
        errV6 = (retV6 == 0) ? 0 : WSAGetLastError();
    }
    const char* opt_name = "IP_UNICAST_IF";
    LogQ(loggerCtx, _DEBUG_,
        "SocketPinner: clearing %s fd=%d v4=%d/err=%d v6=%d/err=%d",
        opt_name, fd, retV4, errV4, retV6, errV6);
    _set_method(outMethod, outMethodLen, opt_name);
    _set_errno(outErrno, (errV4 != 0) ? errV4 : errV6);
    return (retV4 == 0 || (ipv6 && retV6 == 0)) ? TT_PIN_OK : TT_PIN_FAILED;
#elif defined(__linux__) && !defined(OS_ANDROID)
    int retV4 = 0;
    int errV4 = 0;
    int retV6 = 0;
    int errV6 = 0;
    // Linux: IP_UNICAST_IF takes a host-order uint32 (no htonl), and 0
    // means "no hint". Same option for v4 and v6 (IPV6_UNICAST_IF == 76).
    uint32_t zero = 0;
    retV4 = setsockopt(fd, IPPROTO_IP, IP_UNICAST_IF, &zero, sizeof(zero));
    errV4 = (retV4 == 0) ? 0 : errno;
    if (ipv6)
    {
        retV6 = setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_IF, &zero, sizeof(zero));
        errV6 = (retV6 == 0) ? 0 : errno;
    }
    const char* opt_name = "IP_UNICAST_IF";
    LogQ(loggerCtx, _DEBUG_,
        "SocketPinner: clearing %s fd=%d v4=%d/err=%d v6=%d/err=%d",
        opt_name, fd, retV4, errV4, retV6, errV6);
    _set_method(outMethod, outMethodLen, opt_name);
    _set_errno(outErrno, (errV4 != 0) ? errV4 : errV6);
    return (retV4 == 0 || (ipv6 && retV6 == 0)) ? TT_PIN_OK : TT_PIN_FAILED;
#else
    (void)ipv6;
    (void)loggerCtx;
    return TT_PIN_UNSUPPORTED;
#endif
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
