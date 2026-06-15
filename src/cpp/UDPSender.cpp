///////////////////////////////////////////////////////////////////////////////
// file : UDPSender.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "UDPSender.h"
#include <BC/BCLog.h>
#include <BC/Utils.h>
#include "Utils.h"
#ifdef OS_ANDROID
#include <android/api-level.h>
#include <dlfcn.h>
#include <cerrno>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
// IP_BOUND_IF / IPV6_BOUND_IF — bind a socket to a specific network interface
// by ifIndex (returned by NWPathMonitor / if_nametoindex). Shared by iOS and
// macOS: iOS picks cellular vs wifi, macOS picks ethernet vs wifi vs VPN.
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>  // inet_ntop for fake-IP diagnostic logging
#include <cerrno>
#endif
// Cross-platform kernel route-table query (tt_route_lookup_ifindex).
// macOS uses PF_ROUTE / RTM_GET, Linux uses NETLINK_ROUTE / RTM_GETROUTE,
// Windows uses GetBestInterfaceEx. iOS / Android / etc. return 0 from a
// stub. UDPSender uses this on macOS and Windows to actively (proactive)
// un-pin IP_BOUND_IF / IP_UNICAST_IF on peer/interface mismatch — both
// kernels produce a hard "source IP from pinned interface, route via
// real interface" split that black-holes packets through TUN-mode
// proxies. On Linux it is used as a diagnostic warning only; Linux
// IP_UNICAST_IF on a connected UDP socket is either silently ignored
// (kernel < 6.0.16 / 6.1.2 / 6.2) or rejects the connect() with an
// explicit errno (kernel ≥ that), so a wrong hint either has no effect
// or surfaces as ENETUNREACH that the reactive _OnConnectDone /
// _SendDoneCallback path will catch and unpin.
#include "NetworkRouteLookup.h"
#if defined(__linux__) && !defined(OS_ANDROID)
// IP_UNICAST_IF / IPV6_UNICAST_IF — Linux equivalent of IP_BOUND_IF. Index
// is in HOST byte order on Linux (no htonl, unlike Windows). Used by the
// LinuxNetlinkMonitor restart path.
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>  // inet_ntop for route-lookup diagnostic logging
#include <cerrno>
// IPV6_UNICAST_IF was added to the Linux kernel uapi headers in 5.7. Older
// cross-toolchain sysroots (e.g. the homebrew aarch64-unknown-linux-gnu
// bottle, which still ships pre-5.7 headers) don't declare it even though
// any reasonably recent runtime kernel implements it. The numeric value is
// stable kernel ABI (linux/in6.h: #define IPV6_UNICAST_IF 76), so define
// it ourselves when the toolchain headers fall short.
#ifndef IPV6_UNICAST_IF
#define IPV6_UNICAST_IF 76
#endif
#endif
#if defined(_WIN32)
// IP_UNICAST_IF / IPV6_UNICAST_IF — Windows equivalent. Per Microsoft docs,
// IPv4 ifIndex must be passed in NETWORK byte order (htonl); IPv6 is in
// host order. Used by the WinIpChangeMonitor restart path.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cerrno>
#endif



///////////////////////////////////////////////////////////////////////////////
// Macro & typedefs
///////////////////////////////////////////////////////////////////////////////


#define SNDR_STATE_FREED		0
#define SNDR_STATE_INACTIVE		1
#define SNDR_STATE_RESTARTING	2
#define SNDR_STATE_READY		3
#define SNDR_STATE_READING		4
#define SNDR_STATE_MAX			9

#if defined(__APPLE__)
// Is `addr` a TUN-mode-VPN fake-IP — i.e. a numerically valid IPv4 whose
// route only exists *inside* a transparent proxy and whose packets would
// land in a black hole if we forced them out via a physical interface?
//
// We check the two well-known default ranges that catch the entire
// out-of-the-box install base of fake-IP proxies on macOS / iOS:
//
//   * 198.18.0.0/15  RFC 2544 BENCHMARK. Default for Clash (Verge, Stash,
//                    Mihomo), Surge ≥4, V2Ray Xray, sing-box, Loon,
//                    Shadowrocket, etc.
//   * 28.0.0.0/8     Surge legacy default before they migrated to 198.18,
//                    still common on older configs.
//
// IPv6 fake-IPs are not a thing in the wild (proxies stick to v4
// fake-IPs even when the real backend is dual-stack), so we deliberately
// ignore AF_INET6 here. Users whose proxy uses a non-default fake-IP
// range can still escape via `bypassVpn=false` on the connector.
static bool _IsFakeIPPeer(const BCSockAddrS& addr)
{
	if (addr.type.sa.sa_family != AF_INET) return false;
	uint32_t ip = ntohl(addr.type.sin.sin_addr.s_addr);
	if ((ip & 0xFFFE0000u) == 0xC6120000u) return true; // 198.18.0.0/15
	if ((ip & 0xFF000000u) == 0x1C000000u) return true; // 28.0.0.0/8
	return false;
}
#endif



typedef enum EventTypeE
{
	SNDRM_STARTWORK				= 1,
	SNDRM_START_RECV			= 2,
	SNDRM_PAUSEWORK				= 3,
	SNDRM_STOPWORK				= 4,
	SNDRM_CHECKACTIVE			= 5,
	SNDRM_CLIENT_SHUTDOWN		= 6,
	SNDRM_RESTART_WORK			= 7,
	// Number of events
	SNDRM_NUMBER				= 8,
}EventTypeE;

///////////////////////////////////////////////////////////////////////////////
// Class : UDPSender
///////////////////////////////////////////////////////////////////////////////

#define _set_state(conn, _state, _status)	\
	(conn)->_SetState(_state, __LINE__);(conn)->m_nCloseStatus = _status

UDPSender::UDPSender()
    : m_pLoggerCtx(NULL)
	, m_pTaskMgr(NULL)
	, m_pSockMgr(NULL)
	, m_pTimerMgr(NULL)
	, m_pSocket(NULL)
	, m_pRecvBuffer1(NULL)
	, m_pRecvBuffer2(NULL)
	, m_bAlterBuffer(false)
	, m_szHost{0}
	, m_nPort(8000)
	, m_nLatestNetActionTime(0)
	, m_nPendingConnect(0)
	, m_nPendingRecv(0)
	, m_nPendingSend(0)
	, m_eState(SNDR_STATE_INACTIVE)
	, m_nNewState(SNDR_STATE_MAX)
	, m_nStateLineNo(0)
	, m_nCloseStatus(BC_R_SUCCESS)
	, m_sExitCond(&m_sExitLock)
	, m_pHandler(NULL)
	, m_bBindIP(false)
	, m_bBindPort(false)
	, m_nPendingRestart(0)
	, m_bCheckAvailable(false)
	, m_nCheckAvailableTimerId(0)
	, m_nRecvDataCount(0)
	, m_nNetworkHandle(0)
	, m_bInterfaceBindingActive(false)
{
	memset(&m_sSelfAddr, 0, sizeof(BCSockAddrS));
	memset(&m_sSockAddr, 0, sizeof(BCSockAddrS));
}

UDPSender::~UDPSender()
{
}

BCRESULT UDPSender::Create(
	void *logger_ctx,
	BCTaskMgr *pTaskMgr,
	BCTimerMgr *pTimerMgr,
	BCSocketMgr *pSockMgr,
	BCFObject *pConfig,
	IUDPSenderHandler *pHandler,
	bool bindIP,
	bool bindPort)
{
	BCRESULT result;

	if (!pTaskMgr || !pTimerMgr || !pSockMgr || !pConfig || !pHandler)
	{
		return BC_R_INVALIDARG;
	}

	m_sConfig.Init(pConfig);
	//if (!m_sConfig.host || !m_sConfig.port)
	//{
	//	return BC_R_INVALIDARG;
	//}
	result = BCEventQueue::Create(pTimerMgr, pTaskMgr, "UDPSender", this);
	if (result != BC_R_SUCCESS)
	{
		goto out;
	}

	m_pLoggerCtx = logger_ctx;
	m_sRecvEvent.ev_sender = this;
	m_sRecvEvent.ev_type = BC_SOCKEVENT_RECVDONE;
	m_sRecvEvent.ev_action = _RecvDoneCallback;
	m_sRecvEvent.ev_arg = this;
	m_sSendEvent.ev_sender = this;
	m_sSendEvent.ev_type = BC_SOCKEVENT_SENDDONE;
	m_sSendEvent.ev_action = _SendDoneCallback;
	m_sSendEvent.ev_arg = this;
	m_pConfig 			= pConfig;
	m_pTaskMgr			= pTaskMgr;
	m_pTimerMgr			= pTimerMgr;
	m_pSockMgr			= pSockMgr;
	m_pRecvBuffer1		= new BCBuffer();
	m_pRecvBuffer2		= new BCBuffer();
	m_nNewState			= SNDR_STATE_MAX;
	m_nPendingRecv		= 0;
	m_pHandler			= pHandler;
	m_bBindIP 			= bindIP;
	m_bBindPort 		= bindPort;

	result = _InitSocket();
	if (result != BC_R_SUCCESS)
	{
		goto detach_task;
	}
	m_eState = SNDR_STATE_READING;

	return BC_R_SUCCESS;

detach_task:
	Detach(true);
out:
	return result;
}

BCRESULT UDPSender::Restart(bool checkAvailable, int64_t networkHandle)
{ 
	PostEvent(MAKEEVENT(SNDRM_RESTART_WORK, 0, 0), checkAvailable?1:0, (uint64_t)networkHandle);
	return BC_R_SUCCESS;
}

BCRESULT UDPSender::Start(LPCSTR szHost, uint16_t nPort)
{
	BCEventItemS sEvent(MAKEEVENT(SNDRM_STARTWORK, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyString(szHost);
	sEvent.lParam = nPort;
	PostEvent(sEvent);

	return BC_R_SUCCESS;
}

BCRESULT UDPSender::StartRecv()
{
	PostEvent(MAKEEVENT(SNDRM_START_RECV, 0, 0));

	return BC_R_SUCCESS;
}

BCRESULT UDPSender::Send(
	BCSockAddrS& refSockAddr,
	LPCVOID lpData,
	size_t nSize)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (_ExitCheck())
	{
		return BC_R_SHUTTINGDOWN;
	}
	return _UDP_Send(refSockAddr, lpData, nSize);
}

BCRESULT UDPSender::Connect(BCSockAddrS& refSockAddr)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (_ExitCheck() || !m_pSocket)
	{
		return BC_R_SHUTTINGDOWN;
	}
	// Cross-platform route-table validation of the IP_BOUND_IF /
	// IP_UNICAST_IF pin _InitSocket put on this socket.
	//
	// Per platform:
	//   * macOS — IP_BOUND_IF is a HARD bind. If the peer's natural
	//     egress interface (per the kernel route table right now) is
	//     not the one we pinned, packets are silently black-holed by
	//     the pinned interface's default gateway (TUN-mode proxies
	//     like Clash, Surge, V2Ray; iCloud Private Relay; some VPNs).
	//     connect() / send() still return success, so the reactive
	//     NETUNREACH / HOSTUNREACH fallback can't recover. We must
	//     un-pin proactively. Fall back to the fake-IP heuristic if
	//     the route lookup itself fails (sandboxed / EPERM).
	//   * Windows — IP_UNICAST_IF is documented as a hint, but the
	//     implementation forces the source IP to come from the hinted
	//     interface even when the route picks a different egress NIC.
	//     Reproduces 1:1 with Clash / mihomo / Surge TUN running on
	//     the host: the path monitor's GetBestInterfaceEx(0.0.0.0)
	//     selects the physical adapter while the peer's actual route
	//     is the TUN adapter; we end up sending packets out via TUN
	//     with a source IP from the physical NIC, the proxy's NAT
	//     table can't symmetrise the flow, and PATH_RESPONSE never
	//     comes back. Same blackhole shape as macOS, treated the same
	//     way: un-pin proactively when the route lookup disagrees.
	//   * Linux (non-Android) — IP_UNICAST_IF on a connected UDP
	//     socket is either silently ignored (kernel < 6.0.16 / 6.1.2
	//     / 6.2) or makes connect() return ENETUNREACH/EHOSTUNREACH
	//     when the hint can't reach the peer. Either way there is no
	//     source/route split like on Windows, so we don't unpin
	//     proactively — the reactive _OnConnectDone path will catch
	//     the failure and call _TryClearInterfaceBinding for us.
	//     Diagnostic warning only here.
	//   * iOS / Android — tt_route_lookup_ifindex is a stub returning
	//     0, so the whole block is a no-op.
	if (m_bInterfaceBindingActive)
	{
		socklen_t peer_len =
			(refSockAddr.type.sa.sa_family == AF_INET6)
				? static_cast<socklen_t>(sizeof(refSockAddr.type.sin6))
				: static_cast<socklen_t>(sizeof(refSockAddr.type.sin));
		uint32_t natural_idx =
			tt_route_lookup_ifindex(&refSockAddr.type.sa, peer_len);

		const bool mismatch =
			(natural_idx != 0 &&
			 natural_idx != static_cast<uint32_t>(m_nNetworkHandle));
#if defined(__APPLE__)
		const bool fake_ip_fallback =
			(natural_idx == 0 && _IsFakeIPPeer(refSockAddr));
#else
		const bool fake_ip_fallback = false;
#endif

		if (mismatch || fake_ip_fallback)
		{
			char ipbuf[INET6_ADDRSTRLEN] = {0};
			const void* ip_src = (refSockAddr.type.sa.sa_family == AF_INET6)
				? static_cast<const void*>(&refSockAddr.type.sin6.sin6_addr)
				: static_cast<const void*>(&refSockAddr.type.sin.sin_addr);
			inet_ntop(refSockAddr.type.sa.sa_family, ip_src,
				ipbuf, sizeof(ipbuf));

#if defined(__APPLE__)
			if (mismatch)
			{
				LogQ(m_pLoggerCtx, _WARN_,
					"UDP Sender: kernel routes peer %s via ifIndex=%u "
					"but we pinned IP_BOUND_IF=%lld; unpinning to "
					"avoid blackhole (TUN-mode proxy / VPN / Private "
					"Relay). Set bypassVpn=false on the connector to "
					"silence this for your topology.",
					ipbuf, natural_idx, (long long)m_nNetworkHandle);
			}
			else
			{
				LogQ(m_pLoggerCtx, _WARN_,
					"UDP Sender: route lookup failed for peer %s; "
					"peer falls in TUN-mode fake-IP range "
					"(198.18.0.0/15 or 28.0.0.0/8); proactively "
					"clearing IP_BOUND_IF=%lld via heuristic "
					"fallback.",
					ipbuf, (long long)m_nNetworkHandle);
			}
			// BC_R_SUCCESS as trigger reason — no kernel error here,
			// the setsockopt(IP_BOUND_IF=0) machinery is just being
			// reused as the "unpin" primitive.
			_TryClearInterfaceBinding(BC_R_SUCCESS);
#elif defined(_WIN32)
			// See the per-platform comment above: Windows produces a
			// hard source-IP / route split through TUN-mode proxies
			// even though IP_UNICAST_IF advertises itself as a hint.
			// Drop the hint so the next sendto() picks the source IP
			// from the actual egress interface.
			LogQ(m_pLoggerCtx, _WARN_,
				"UDP Sender: kernel routes peer %s via ifIndex=%u "
				"but we hinted IP_UNICAST_IF=%lld; clearing hint to "
				"avoid source/route split (TUN-mode proxy / VPN). "
				"Set bypassVpn=false on the connector to silence "
				"this for your topology.",
				ipbuf, natural_idx, (long long)m_nNetworkHandle);
			_TryClearInterfaceBinding(BC_R_SUCCESS);
#else
			// Linux: IP_UNICAST_IF is a true soft hint, the kernel
			// will auto-fall-back to the real route AND adjust the
			// source IP, so we don't touch the socket. Surface the
			// discrepancy in logs only.
			LogQ(m_pLoggerCtx, _WARN_,
				"UDP Sender: kernel routes peer %s via ifIndex=%u "
				"but we hinted IP_UNICAST_IF=%lld; source IP may be "
				"wrong on the wire. Diagnostic only — kernel will "
				"route correctly via the real outbound interface.",
				ipbuf, natural_idx, (long long)m_nNetworkHandle);
#endif
		}
	}
	return m_pSocket->Connect(&refSockAddr, GetTask(), _ConnectDoneCB, this);
}

BCRESULT UDPSender::SendMMsg(
	BCSockAddrS& refSockAddr,
	BCRegionS *io_vec,
	size_t iovec_len)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (_ExitCheck())
	{
		return BC_R_SHUTTINGDOWN;
	}
	return _UDP_SendMMsg(refSockAddr, io_vec, iovec_len);
}

BCRESULT UDPSender::GetSockName(BCSockAddrS& refAddr)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_pSocket)
	{
		refAddr = m_sSelfAddr;
		return BC_R_SUCCESS;
	}
	return BC_R_FAILURE;
}

void UDPSender::Close()
{
	PostEvent(MAKEEVENT(SNDRM_STOPWORK, 0, 0));
}

void UDPSender::Destroy(UDPSender **ppSender)
{
	UDPSender *pSender;

	ASSERT(ppSender && *ppSender);
	pSender = *ppSender;
	ASSERT(pSender == this);

	// Destory members
	{
		pSender->PostEvent(MAKEEVENT(SNDRM_STOPWORK, 0, 0));
		BCMutex::Owner lock(m_sExitLock);
		m_sExitCond.Wait();
	}

	delete pSender;
	*ppSender = NULL;
}

BCRESULT UDPSender::_InitSocket()
{ 
	BCRESULT result;

	// Each _InitSocket() builds a fresh BCSocket; the new fd has never been
	// setsockopt'd yet, so any pin that may have been cleared on the
	// previous socket is irrelevant here.
	m_bInterfaceBindingActive = false;

	m_pSocket = new BCSocket();
	if (m_pSocket == NULL)
	{
		return BC_R_NOMEMORY;
	}
	result = m_pSocket->Create(m_pSockMgr, m_sConfig.ipv6?PF_INET6:PF_INET,
		bc_sockettype_udp);
	if (result != BC_R_SUCCESS)
	{
        // Already freed socket instance
		goto return_error;
	}
	if (m_sConfig.ipv6)
	{
		bc_sockaddr_any6(&m_sSelfAddr);
		if (m_sConfig.host && m_bBindIP)
		{
			struct in6_addr in6a;

			if (bc_net_pton(PF_INET6, m_sConfig.host, &in6a) <= 0)
			{
				return BC_R_UNEXPECTED;
			}
			bc_sockaddr_fromin6(&m_sSelfAddr, &in6a, bc_sockaddr_getport(&m_sSelfAddr));
		}
	}
	else
	{
		bc_sockaddr_any(&m_sSelfAddr);
		if (m_sConfig.host && m_bBindIP)
		{
			struct in_addr ina;

			if (bc_net_pton(PF_INET, m_sConfig.host, &ina) <= 0)
			{
				return BC_R_UNEXPECTED;
			}
			bc_sockaddr_fromin(&m_sSelfAddr, &ina, bc_sockaddr_getport(&m_sSelfAddr));
		}
	}
	if (m_sConfig.port && m_bBindIP)
	{
		bc_sockaddr_setport(&m_sSelfAddr, m_sConfig.port);
	}
	result = m_pSocket->Bind(&m_sSelfAddr, BC_SOCKET_REUSEADDRESS);
	if (result != BC_R_SUCCESS)
	{
		goto delete_socket;
	}
	m_pSocket->GetSockName(&m_sSelfAddr);
#ifdef OS_ANDROID
	if (m_nNetworkHandle != 0 && android_get_device_api_level() >= 23)
	{
		typedef int (*pfn_android_setsocknetwork)(uint64_t, int);
		static pfn_android_setsocknetwork fn = (pfn_android_setsocknetwork)
			dlsym(RTLD_DEFAULT, "android_setsocknetwork");
		if (fn)
		{
			int ret = fn((uint64_t)m_nNetworkHandle, m_pSocket->GetFd());
			LogQ(m_pLoggerCtx, _DEBUG_,
				"UDP Sender: android_setsocknetwork(handle=%lld, fd=%d) = %d, errno=%d",
				(long long)m_nNetworkHandle, m_pSocket->GetFd(), ret, ret == 0 ? 0 : errno);
			if (ret == 0) m_bInterfaceBindingActive = true;
		}
		else
		{
			LogQ(m_pLoggerCtx, _WARN_,
				"UDP Sender: android_setsocknetwork not found via dlsym");
		}
	}
#elif defined(__APPLE__)
	// macOS + iOS: bind the UDP socket to a specific interface so the kernel
	// stops following the system "best path" automatically. The caller
	// (AppleNetworkMonitor) passes an ifIndex from if_nametoindex() through
	// the SMPConnection::Restart -> UDPSender::Restart chain as
	// m_nNetworkHandle. IP_BOUND_IF is the Apple equivalent of Android's
	// android_setsocknetwork. Fake-IP / TUN-mode-VPN handling lives in
	// UDPSender::Connect (it's the only entry point that learns the peer
	// address), so we just install the pin unconditionally here.
	if (m_nNetworkHandle != 0)
	{
		uint32_t ifIndex = (uint32_t)m_nNetworkHandle;
		int fd = m_pSocket->GetFd();
		int retV4 = setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &ifIndex, sizeof(ifIndex));
		int errV4 = (retV4 == 0) ? 0 : errno;
		int retV6 = 0;
		int errV6 = 0;
		if (m_sConfig.ipv6)
		{
			retV6 = setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &ifIndex, sizeof(ifIndex));
			errV6 = (retV6 == 0) ? 0 : errno;
		}
		LogQ(m_pLoggerCtx, _DEBUG_,
			"UDP Sender: setsockopt(IP_BOUND_IF, ifIndex=%u, fd=%d) v4=%d/errno=%d v6=%d/errno=%d",
			ifIndex, fd, retV4, errV4, retV6, errV6);
		if (retV4 == 0 || (m_sConfig.ipv6 && retV6 == 0))
			m_bInterfaceBindingActive = true;
	}
#elif defined(__linux__) && !defined(OS_ANDROID)
	// Linux (non-Android): bind via IP_UNICAST_IF. Caller (LinuxNetlinkMonitor)
	// passes ifIndex obtained from RTM_GETROUTE/RTA_OIF in m_nNetworkHandle.
	// Index is in HOST byte order on Linux — DO NOT htonl here (Windows is
	// the only platform that requires byte-swapping).
	if (m_nNetworkHandle != 0)
	{
		uint32_t ifIndex = (uint32_t)m_nNetworkHandle;
		int fd = m_pSocket->GetFd();
		int retV4 = setsockopt(fd, IPPROTO_IP, IP_UNICAST_IF, &ifIndex, sizeof(ifIndex));
		int errV4 = (retV4 == 0) ? 0 : errno;
		int retV6 = 0;
		int errV6 = 0;
		if (m_sConfig.ipv6)
		{
			retV6 = setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_IF, &ifIndex, sizeof(ifIndex));
			errV6 = (retV6 == 0) ? 0 : errno;
		}
		LogQ(m_pLoggerCtx, _DEBUG_,
			"UDP Sender: setsockopt(IP_UNICAST_IF, ifIndex=%u, fd=%d) v4=%d/errno=%d v6=%d/errno=%d",
			ifIndex, fd, retV4, errV4, retV6, errV6);
		if (retV4 == 0 || (m_sConfig.ipv6 && retV6 == 0))
			m_bInterfaceBindingActive = true;
	}
#elif defined(_WIN32)
	// Windows: IP_UNICAST_IF requires the IPv4 ifIndex in NETWORK byte order
	// (htonl). IPv6 stays in host order. Caller (WinIpChangeMonitor) supplies
	// ifIndex from GetBestInterfaceEx in m_nNetworkHandle.
	if (m_nNetworkHandle != 0)
	{
		DWORD ifIndex = (DWORD)m_nNetworkHandle;
		DWORD ifIndexBE = htonl(ifIndex);
		int fd = m_pSocket->GetFd();
		int retV4 = setsockopt((SOCKET)fd, IPPROTO_IP, IP_UNICAST_IF,
			(const char*)&ifIndexBE, sizeof(ifIndexBE));
		int errV4 = (retV4 == 0) ? 0 : WSAGetLastError();
		int retV6 = 0;
		int errV6 = 0;
		if (m_sConfig.ipv6)
		{
			retV6 = setsockopt((SOCKET)fd, IPPROTO_IPV6, IPV6_UNICAST_IF,
				(const char*)&ifIndex, sizeof(ifIndex));
			errV6 = (retV6 == 0) ? 0 : WSAGetLastError();
		}
		LogQ(m_pLoggerCtx, _DEBUG_,
			"UDP Sender: setsockopt(IP_UNICAST_IF, ifIndex=%u, fd=%d) v4=%d/wsa=%d v6=%d/wsa=%d",
			(unsigned)ifIndex, fd, retV4, errV4, retV6, errV6);
		if (retV4 == 0 || (m_sConfig.ipv6 && retV6 == 0))
			m_bInterfaceBindingActive = true;
	}
#endif
	char local_addr_str_[128];
	bc_sockaddr_format(&m_sSelfAddr, local_addr_str_, sizeof(local_addr_str_));
	LogQ(m_pLoggerCtx, _INFO_, "UDP Sender: started at %s (networkHandle=%lld)",
		local_addr_str_, (long long)m_nNetworkHandle);

	return BC_R_SUCCESS;

delete_socket:
	m_pSocket->Detach(&m_pSocket);
return_error:
	return result;
}

void UDPSender::_Cleanup()
{
	if (m_pSocket != NULL)
	{
		m_pSocket->Detach(&m_pSocket);
	}

	// Destroy all unsend events
	BCEventQueue::_FlushEvents();
	BC_SAFE_DELETE_PTR(m_pRecvBuffer1);
	BC_SAFE_DELETE_PTR(m_pRecvBuffer2);
}

BOOL UDPSender::_ExitCheck()
{
	if (m_eState <= m_nNewState)
		return (FALSE); /* Business as usual. */

	ASSERT(m_nNewState < SNDR_STATE_READING);

	BCTask *pTask = GetTask();

	if (m_eState == SNDR_STATE_READING)
	{
		/*
		* We are trying to abort the current UDP connection,
		* if any.
		*/
		ASSERT(m_nNewState <= SNDR_STATE_READY);

		if (m_nPendingRecv > 0)
		{
			m_pSocket->Cancel(pTask, BC_SOCKCANCEL_RECV);
		}
		if (m_nPendingSend > 0)
		{
			m_pSocket->Cancel(pTask, BC_SOCKCANCEL_SEND);
		}

		m_eState = SNDR_STATE_READY;
	}

	if (m_eState == SNDR_STATE_READY)
	{
		ASSERT(m_nNewState <= SNDR_STATE_READY);

		if (!(m_nPendingRecv == 0 && m_nPendingSend == 0))
		{
			/* Still waiting for read cancel completion. */
			return (TRUE);
		}

		if (m_pSocket != NULL)
		{
			m_pSocket->Detach(&m_pSocket);
		}

		// Stop all type control events
		_Stop();

		if (m_nCtrls > 0)
		{
			/* Still waiting for control event to be delivered */
			return (TRUE);
		}
		m_eState = SNDR_STATE_INACTIVE;

		if (m_eState == m_nNewState)
		{
			if (m_nPendingRestart > 0)
			{
				if (m_nCheckAvailableTimerId > 0)
				{
					UnscheduleTask(m_nCheckAvailableTimerId);
					m_nCheckAvailableTimerId = 0;
				}
				_Restart();
				m_eState = SNDR_STATE_READING;
				_set_state(this, SNDR_STATE_MAX, BC_R_SUCCESS);
			}
			return FALSE;
		}
	}

	if (m_eState == SNDR_STATE_INACTIVE)
	{
		ASSERT(m_nNewState == SNDR_STATE_FREED);
		/*
		* We are trying to free the client.
		*
		* When "shuttingdown" is true, either the task has received
		* its shutdown event or no shutdown event has ever been
		* set up.  Thus, we have no outstanding shutdown
		* event at this point.
		*/
		if (m_nCheckAvailableTimerId > 0)
		{
			UnscheduleTask(m_nCheckAvailableTimerId);
			m_nCheckAvailableTimerId = 0;
		}

		/*
		* Detaching the task must be done after unlinking from
		* the manager's lists because the manager accesses
		* this->task.
		*/
		Detach();
	}

	return TRUE;
}

void UDPSender::_Restart()
{
	BCRESULT result = _InitSocket();
	if (result == BC_R_SUCCESS)
	{
		if (m_bCheckAvailable)
		{
			m_nRecvDataCount = 0;
			m_pHandler->OnCheckAvailable();
			_UDP_RecvChunk();
			result = ScheduleTask(m_nCheckAvailableTimerId, [this](int32_t timer_id) {
				BCSpinMutex::Owner lock(m_sLock);
				if (_ExitCheck())
				{
					return;
				}
				UnscheduleTask(m_nCheckAvailableTimerId);
				if (m_nRecvDataCount > 0)
				{
					m_nRecvDataCount = 0;
					m_nPendingRestart--;
					m_bCheckAvailable = false;
					m_pHandler->OnRestart(BC_R_SUCCESS);
				}
				else 
				{
					LogQ(m_pLoggerCtx, _INFO_, "UDP Sender: check available timeout");
					m_nPendingRestart--;
					m_bCheckAvailable = false;
					_set_state(this, SNDR_STATE_INACTIVE, BC_R_NETUNREACH);
				}
				_ExitCheck();
			}, m_sConfig.checkAvailableInterval, false);
			if (result != BC_R_SUCCESS)
			{
				m_nPendingRestart--;
				m_bCheckAvailable = false;
				m_pHandler->OnRestart(result);
			}
		}
		else
		{
			m_nPendingRestart--;
			m_bCheckAvailable = false;
			m_pHandler->OnRestart(BC_R_SUCCESS);
		}
	}
	else
	{
		m_nPendingRestart--;
		m_bCheckAvailable = false;
		m_pHandler->OnRestart(result);
	}
}

void UDPSender::_UDP_RecvChunk()
{
	BCRESULT result;

	if (_ExitCheck() || m_nPendingRecv > 0)
	{
		return;
	}

	BCBuffer *pBuffer = m_bAlterBuffer ? m_pRecvBuffer2 : m_pRecvBuffer1;
	pBuffer->Reset();
	result = m_pSocket->RecvV2(pBuffer, 1, GetTask(), &m_sRecvEvent, 0);
	if (result == BC_R_SUCCESS || result == BC_R_INPROGRESS)
	{
		m_nPendingRecv++;
		m_bAlterBuffer = !m_bAlterBuffer;
		result = BC_R_SUCCESS;
	}
	(void)_ExitCheck();
}

BCRESULT UDPSender::_UDP_Send(
	BCSockAddrS &refSockAddr,
	LPCVOID lpData, 
	size_t nSize)
{
	BCRESULT result;
#if 0
	BCBuffer *pBuffer = new BCBuffer();

	pBuffer->Write(lpData, nSize);
	result = m_pSocket->SendToV(pBuffer, GetTask(),
		_SendDoneCallback, this, &refSockAddr, NULL);
#else
	BCRegionS region(lpData, nSize);
	result = m_pSocket->SendTo(&region, 1, GetTask(),
		_SendDoneCallback, this, &refSockAddr, NULL);
#endif
	if (result != BC_R_SUCCESS)
	{
		switch (result)
		{
		case BC_R_INPROGRESS:
			m_nPendingSend++;
			break;
		default:
			break;
		}
		return result;
	}
	m_nPendingSend++;
	return result;
}

BCRESULT UDPSender::_UDP_SendMMsg(
	BCSockAddrS &refSockAddr,
	BCRegionS *io_vec, 
	size_t iovec_len)
{
	BCRESULT result;

	if (iovec_len > BC_SOCKET_MAXSCATTERGATHER)
	{
		return BC_R_INVALIDARG;
	}
	result = m_pSocket->SendTo(io_vec, iovec_len, GetTask(),
		_SendDoneCallback, this, &refSockAddr, NULL);
	if (result != BC_R_SUCCESS)
	{
		switch (result)
		{
		case BC_R_INPROGRESS:
			m_nPendingSend++;
			break;
		default:
			break;
		}
		return result;
	}
	m_nPendingSend++;
	return result;
}

BCRESULT UDPSender::_StartWork(LPCSTR szHost, uint16_t nPort)
{
	if (szHost)
	{
		strncpy(m_szHost, szHost, sizeof(m_szHost) - 1);
	}
	else
	{
		memzero(m_szHost, sizeof(m_szHost));
	}
	m_nPort = nPort;
	if (m_sConfig.ipv6)
	{
		struct in6_addr in6a;

		if (strlen(m_szHost) == 0)
		{
			in6a = in6addr_any;
		}
		else if (bc_net_pton(PF_INET6, m_szHost, &in6a) <= 0)
		{
			return BC_R_UNEXPECTED;
		}
		bc_sockaddr_fromin6(&m_sSockAddr, &in6a, m_nPort);
	}
	else
	{
		struct in_addr ina;

		if (strlen(m_szHost) == 0)
		{
			ina.s_addr = INADDR_ANY;
		}
		else if (bc_net_pton(PF_INET, m_szHost, &ina) <= 0)
		{
			return BC_R_UNEXPECTED;
		}
		bc_sockaddr_fromin(&m_sSockAddr, &ina, m_nPort);
	}
	//result = _UDP_Send(m_sSockAddr, &message[0], message.size());
	//if (result != BC_R_SUCCESS)
	//{
	//	return result;
	//}
	_UDP_RecvChunk();

	return BC_R_SUCCESS;
}

void UDPSender::_StopWork()
{
	_set_state(this, SNDR_STATE_FREED, BC_R_SUCCESS);
}

void UDPSender::_ConnectDoneCB(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCSockOCEvent *pSockEv = (BCSockOCEvent *)pEvent;
	UDPSender *_this = (UDPSender *)pEvent->ev_arg;
	ScopedPointer<BCSockOCEvent> dtor(pSockEv);

	UNUSED(pTask);

	ASSERT(_this != NULL);

	BCSpinMutex::Owner lock(_this->m_sLock);

	if (_this->m_nPendingConnect > 0)
	{
		_this->m_nPendingConnect--;
	}

	_this->_OnConnectDone(pSockEv->result);

	//check_exit:
	_this->_ExitCheck();
}

void UDPSender::_RecvDoneCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCSockEvent *pSockEv = (BCSockEvent *)pEvent;
	UDPSender *_this = (UDPSender *)pSockEv->ev_arg;

	BCSpinMutex::Owner lock(_this->m_sLock);

	ASSERT(_this != NULL);
	// Get latest
	pTask->GetCurTime(&_this->m_nLatestNetActionTime);
	// Check pending recv ref count
	ASSERT (_this->m_nPendingRecv == 1);
	_this->m_nPendingRecv--;

	if (pSockEv->result != BC_R_SUCCESS && _this->m_nPendingRestart == 0)
	{
		// Treat anything that just means "the network is having a moment"
		// as transient: keep the socket open and let the NWPathMonitor /
		// netlink monitor decide whether to migrate. Self-destructing on
		// these errors used to mean a brief Wi-Fi flap (during which
		// recvfrom returns EADDRNOTAVAIL / EHOSTUNREACH / ENETDOWN before
		// the OS publishes the new IP) tore down the QUIC connection
		// outright. Anything else (NOMEMORY, IOERROR, UNEXPECTED, ...)
		// is genuinely fatal and still triggers FREED.
		bool transient = false;
		switch (pSockEv->result)
		{
			case BC_R_TIMEDOUT:
			case BC_R_ADDRNOTAVAIL:
			case BC_R_NETUNREACH:
			case BC_R_HOSTUNREACH:
			case BC_R_NETDOWN:
			case BC_R_HOSTDOWN:
			case BC_R_CANCELED:
				transient = true;
				break;
			default:
				break;
		}
		LogQ(_this->m_pLoggerCtx, _WARN_,
			"UDP Sender: recv error result=%u (%s)%s",
			(unsigned)pSockEv->result,
			BC::bc_result2string(pSockEv->result),
			transient ? " - transient, keep socket" : " - fatal, freeing");
		if (!transient)
		{
			_set_state(_this, SNDR_STATE_FREED, pSockEv->result);
		}
	}

	if (_this->_ExitCheck())
	{
		return;
	}

	//LogBin(_LOCAL_, pSockEv->region.base, pSockEv->region.length);

	/*
	* Success.
	*/
#ifndef _DEBUG
	try
	{
#endif
		BCBuffer *pBuffer = pSockEv->bufferlist;
		BCSockAddrS sAddr = pSockEv->address;
		_this->_UDP_RecvChunk();
		_this->_OnDataRecv(pBuffer, sAddr);
#ifndef _DEBUG
	}
	catch(...)
	{
		LogError(_LOCAL_, "Unexcepted error occurred!");
		_set_state(_this, SNDR_STATE_FREED, BC_R_UNEXPECTED);
	}
#endif

//quit:
	(void)_this->_ExitCheck();
}

void UDPSender::_SendDoneCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCSockEvent *pSockEv = (BCSockEvent *)pEvent;
	UDPSender *_this = (UDPSender *)pSockEv->ev_arg;
	ScopedPointer<BCSockEvent> dtor(pSockEv);
	ScopedPointer<BCBuffer> pBuffer(pSockEv->bufferlist);

	ASSERT(_this != NULL);

	BCSpinMutex::Owner lock(_this->m_sLock);
	// Check pending send ref count
	if (_this->m_nPendingSend > 0)
	{
		_this->m_nPendingSend--;
	}

	if (pSockEv->result != BC_R_SUCCESS && _this->m_nPendingRestart == 0)
	{
		// Same policy as _RecvDoneCallback: keep the socket on any
		// "network blip" error so that the upper monitor (NWPathMonitor /
		// netlink) gets a chance to migrate us. Otherwise a 1-2s Wi-Fi
		// handover during which sendto returns EADDRNOTAVAIL would tear
		// down a perfectly recoverable QUIC connection.
		bool transient = false;
		switch (pSockEv->result)
		{
			case BC_R_TIMEDOUT:
			case BC_R_ADDRNOTAVAIL:
			case BC_R_NETUNREACH:
			case BC_R_HOSTUNREACH:
			case BC_R_NETDOWN:
			case BC_R_HOSTDOWN:
			case BC_R_CANCELED:
				transient = true;
				break;
			default:
				break;
		}
		LogQ(_this->m_pLoggerCtx, _WARN_,
			"UDP Sender: send error result=%u (%s) n=%u%s",
			(unsigned)pSockEv->result,
			BC::bc_result2string(pSockEv->result),
			(unsigned)pSockEv->n,
			transient ? " - transient, keep socket" : " - fatal, freeing");
		if (!transient)
		{
			_set_state(_this, SNDR_STATE_FREED, pSockEv->result);
		}
		else if (pSockEv->result == BC_R_NETUNREACH ||
			pSockEv->result == BC_R_HOSTUNREACH ||
			pSockEv->result == BC_R_ADDRNOTAVAIL)
		{
			// Same heuristic as _OnConnectDone: if every send out of this
			// socket comes back "no route", the interface pin we put on
			// in _InitSocket() is likely the reason — drop it once and
			// let the kernel pick a working route.
			_this->_TryClearInterfaceBinding(pSockEv->result);
		}
	}

	if (_this->_ExitCheck())
	{
		return;
	}

	/*
	* Success
	*/
#ifndef _DEBUG
	try
	{
#endif
		_this->_OnSendDone(pSockEv->n, pSockEv->result);
#ifndef _DEBUG
	}
	catch (...)
	{
		LogFatal(_LOCAL_, "Unexcepted error occurred!");
		_set_state(_this, SNDR_STATE_FREED, BC_R_UNEXPECTED);
	}
#endif

	(void)_this->_ExitCheck();
}

bool UDPSender::OnEventProcess(BCEventItemS &refEvent)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (_ExitCheck())
	{
		return true;
	}

	/*
	* Success
	*/
#ifndef _DEBUG
	try
	{
#endif
		switch(EVENTMAJOR(refEvent.eType))
		{
		case SNDRM_STARTWORK:
			_StartWork((LPCSTR)refEvent.wParam, (uint16_t)refEvent.lParam);
			break;
		case SNDRM_START_RECV:
			_UDP_RecvChunk();
			break;
		case SNDRM_STOPWORK:
			_StopWork();
			break;
		case SNDRM_CHECKACTIVE:
			//_ActiveCheck();
			break;
		case SNDRM_CLIENT_SHUTDOWN:
			break;
		case SNDRM_RESTART_WORK:
			if (m_nPendingRestart == 0)
			{
				_set_state(this, SNDR_STATE_INACTIVE, BC_R_SUCCESS);
				m_nPendingRestart++;
			}
			else
			{
				m_nPendingRestart++;
			}
			m_bCheckAvailable = !!refEvent.wParam;
			m_nNetworkHandle = (int64_t)refEvent.lParam;
			m_nRecvDataCount = 0;
			break;
		default:
			BCDefEventProc(refEvent);
			break;
		}
#ifndef _DEBUG
	}
	catch(...)
	{
		LogError(_LOCAL_, "Unexcepted error occurred!");
		_set_state(this, SNDR_STATE_FREED, BC_R_UNEXPECTED);
	}
#endif

//quit:
	(void)_ExitCheck();
	return true;
}

void UDPSender::OnEventProcShutdown()
{
	// Do some clean up work, as well as release some memory resources
	{
		BCMutex::Owner lock(m_sExitLock);
		BCSpinMutex::Owner lock2(m_sLock);
		_Cleanup();
		m_sExitCond.Signal();
	}
	if (m_pHandler)
	{
		m_pHandler->OnUdpClosed();
	}
}

bool UDPSender::_TryClearInterfaceBinding(BCRESULT triggerResult)
{
	// Only platforms where _InitSocket() left the socket with a "source IP
	// pinned to a specific interface" need an "unpin" path here.
	//
	//   * Apple   — IP_BOUND_IF is a HARD bind (route + source). Clear by
	//               setsockopt(IP_BOUND_IF, 0).
	//   * Windows — IP_UNICAST_IF is documented as a routing hint, but the
	//               implementation forces the source IP to come from the
	//               hinted interface even when the kernel route ends up
	//               using a different egress (canonical trigger: Clash /
	//               Surge / mihomo / V2Ray TUN where the peer's route is
	//               on the TUN adapter but the hint points at the physical
	//               NIC). Same blackhole shape as Apple. Clear by
	//               setsockopt(IP_UNICAST_IF, 0) — Microsoft documents
	//               INADDR_ANY (0) as the way to undo the option.
	//   * Linux (non-Android) — IP_UNICAST_IF behaves differently across
	//               kernel versions for connected UDP: pre-v6.0.16 / 6.1.2
	//               / 6.2 the option is silently ignored once a UDP socket
	//               is connected (the route gets cached at connect() and
	//               the cache bypasses fib_lookup), so a stale hint is a
	//               no-op. Starting with that fix [Richard Gobert,
	//               net-next 2022-08-29] connect() actually consults
	//               uc_index in fib_lookup and returns ENETUNREACH /
	//               EHOSTUNREACH when the hinted interface has no route
	//               to the peer — which is exactly what happens with
	//               Clash / mihomo TUN running on the host. Clearing the
	//               hint with setsockopt(IP_UNICAST_IF, 0) lets the next
	//               connect() / send() pick the real default-route
	//               interface. Note: Linux does NOT split source/route
	//               like Windows, so the failure mode is an explicit
	//               errno (which the reactive _SendDoneCallback /
	//               _OnConnectDone path catches) rather than a silent
	//               blackhole — there is no need for proactive unpinning
	//               in Connect().
	//   * Android — android_setsocknetwork is a hard pin too but there's
	//               no public API to undo it on a live fd; the JNI layer
	//               should just create a new socket via Network.bindSocket().
#if defined(__APPLE__) || defined(_WIN32) || (defined(__linux__) && !defined(OS_ANDROID))
	if (!m_bInterfaceBindingActive || m_pSocket == NULL)
	{
		return false;
	}

	int fd = m_pSocket->GetFd();
	if (fd < 0)
	{
		m_bInterfaceBindingActive = false;
		return false;
	}

	int retV4 = 0;
	int errV4 = 0;
	int retV6 = 0;
	int errV6 = 0;

#if defined(__APPLE__)
	uint32_t zero = 0;
	retV4 = setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &zero, sizeof(zero));
	errV4 = (retV4 == 0) ? 0 : errno;
	if (m_sConfig.ipv6)
	{
		retV6 = setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &zero, sizeof(zero));
		errV6 = (retV6 == 0) ? 0 : errno;
	}
	const char* opt_name = "IP_BOUND_IF";
#elif defined(_WIN32)
	// IP_UNICAST_IF: IPv4 takes a network-byte-order DWORD, IPv6 takes a
	// host-order DWORD. Zero is byte-order-agnostic so we pass it as-is.
	DWORD zero = 0;
	retV4 = setsockopt((SOCKET)fd, IPPROTO_IP, IP_UNICAST_IF,
		(const char*)&zero, sizeof(zero));
	errV4 = (retV4 == 0) ? 0 : WSAGetLastError();
	if (m_sConfig.ipv6)
	{
		retV6 = setsockopt((SOCKET)fd, IPPROTO_IPV6, IPV6_UNICAST_IF,
			(const char*)&zero, sizeof(zero));
		errV6 = (retV6 == 0) ? 0 : WSAGetLastError();
	}
	const char* opt_name = "IP_UNICAST_IF";
#else  // Linux non-Android
	// Linux: IP_UNICAST_IF takes a host-order uint32 (no htonl), and 0
	// means "no hint". Same option for v4 and v6 (IPV6_UNICAST_IF == 76).
	uint32_t zero = 0;
	retV4 = setsockopt(fd, IPPROTO_IP, IP_UNICAST_IF, &zero, sizeof(zero));
	errV4 = (retV4 == 0) ? 0 : errno;
	if (m_sConfig.ipv6)
	{
		retV6 = setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_IF, &zero, sizeof(zero));
		errV6 = (retV6 == 0) ? 0 : errno;
	}
	const char* opt_name = "IP_UNICAST_IF";
#endif

	// Mark as cleared even if one of the setsockopt calls failed: we still
	// don't want to spam this fallback path again on the next NETUNREACH
	// from the same socket. Worst case the next packet still fails and the
	// upper-layer NetworkMonitor / idle timeout will drive a real restart.
	m_bInterfaceBindingActive = false;

	LogQ(m_pLoggerCtx, _WARN_,
		"UDP Sender: clearing %s after trigger=%u (%s) fd=%d v4=%d/err=%d v6=%d/err=%d. "
		"Likely a TUN-mode VPN (Clash / Surge / V2Ray) where the server hostname "
		"resolved to a fake-IP / a route only present on the proxy adapter; "
		"falling back to OS default routing.",
		opt_name,
		(unsigned)triggerResult, BC::bc_result2string(triggerResult),
		fd, retV4, errV4, retV6, errV6);
	return true;
#else
	(void)triggerResult;
	return false;
#endif
}

void UDPSender::_OnConnectDone(BCRESULT result)
{
	// UDP "connect" just records the peer address on the socket and (on
	// Linux) lets the kernel pick the source IP/route, so a failure here
	// usually means the freshly-arrived path isn't actually routable
	// yet — most often ENETUNREACH while the OS is still finishing
	// DHCP/SLAAC on a new Wi-Fi association. NWPathMonitor will fire
	// again once the route is installed and we'll restart.
	LogQ(m_pLoggerCtx,
		result == BC_R_SUCCESS ? _INFO_ : _WARN_,
		"UDP Sender: connect done result=%u (%s)",
		(unsigned)result, BC::bc_result2string(result));

	// If we just pinned to a specific interface (Apple/Windows
	// bypassVpn=1) and the peer is unreachable from that interface, undo
	// the pin so the next sendto() can flow through whatever default
	// route the OS picks (which, on a Clash/Surge/V2Ray TUN-mode host,
	// will be the proxy adapter — the only path on which a fake-IP peer
	// address makes sense). On Windows this reactive path is mostly a
	// safety net: the proactive route-table check in Connect() already
	// catches the source/route split before the first packet goes out;
	// a real connect failure here is rare but still worth recovering.
	if (result == BC_R_NETUNREACH || result == BC_R_HOSTUNREACH ||
		result == BC_R_ADDRNOTAVAIL)
	{
		_TryClearInterfaceBinding(result);
	}
}

void UDPSender::_OnDataRecv(BCBuffer *pBuffer, BCSockAddrS &refSrcAddr)
{
	ASSERT(pBuffer != NULL);
	m_nRecvDataCount += pBuffer->RemainingLength();
	if (m_pHandler)
	{
		m_sLock.Unlock();
		m_pHandler->OnRecvData(pBuffer, refSrcAddr);
		m_sLock.Lock();
	}
}

void UDPSender::_OnSendDone(uint32_t nWrite, BCRESULT result)
{
	if (m_pHandler)
	{
		m_sLock.Unlock();
		m_pHandler->OnSendData(nWrite, this);
		m_sLock.Lock();
	}
}

void UDPSender::_Stop()
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// End of file : UDPSender.cpp
///////////////////////////////////////////////////////////////////////////////
