///////////////////////////////////////////////////////////////////////////////
// file : UDPSender.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "UDPSender.h"
#include <BC/BCLog.h>
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
#include <cerrno>
#endif
#if defined(__linux__) && !defined(OS_ANDROID)
// IP_UNICAST_IF / IPV6_UNICAST_IF — Linux equivalent of IP_BOUND_IF. Index
// is in HOST byte order on Linux (no htonl, unlike Windows). Used by the
// LinuxNetlinkMonitor restart path.
#include <netinet/in.h>
#include <sys/socket.h>
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
	// android_setsocknetwork.
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
		switch (pSockEv->result)
		{
			case BC_R_NETUNREACH:
			case BC_R_CANCELED:
				break;
			default:
				//Set free signal
				_set_state(_this, SNDR_STATE_FREED, pSockEv->result);
				break;
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
		switch (pSockEv->result)
		{
			case BC_R_NETUNREACH:
			case BC_R_CANCELED:
				break;
			default:
				//Set free signal
				_set_state(_this, SNDR_STATE_FREED, pSockEv->result);
				break;
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

void UDPSender::_OnConnectDone(BCRESULT result)
{
	printf("_OnConnectDone %u\n", result);
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
