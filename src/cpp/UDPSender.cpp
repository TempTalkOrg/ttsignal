



#include "StdAfx.h"
#include "UDPSender.h"
#include <BC/BCLog.h>
#include <BC/Utils.h>
#include "Utils.h"
#include "SocketPinner.h"
#include <atomic>
#ifdef OS_ANDROID
#include <android/api-level.h>
#include <dlfcn.h>
#include <cerrno>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>



#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>  
#include <cerrno>
#endif













#include "NetworkRouteLookup.h"
#if defined(__linux__) && !defined(OS_ANDROID)



#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>  
#include <net/if.h>     
#include <cerrno>






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
#include <cerrno>
#endif








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
	
	SNDRM_NUMBER				= 8,
}EventTypeE;





#define _set_state(conn, _state, _status)	\
	(conn)->_SetState(_state, __LINE__);(conn)->m_nCloseStatus = _status

static std::atomic<int> s_nLiveSenders{0};

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
	, m_eVpnPolicy(TT_VPN_POLICY_UNSET)
{
	s_nLiveSenders.fetch_add(1);
	memset(&m_sSelfAddr, 0, sizeof(BCSockAddrS));
	memset(&m_sSockAddr, 0, sizeof(BCSockAddrS));
}

int UDPSender::LiveCount()
{
	return s_nLiveSenders.load();
}

UDPSender::~UDPSender()
{
	s_nLiveSenders.fetch_sub(1);
}

BCRESULT UDPSender::Create(
	void *logger_ctx,
	BCTaskMgr *pTaskMgr,
	BCTimerMgr *pTimerMgr,
	BCSocketMgr *pSockMgr,
	BCFObject *pConfig,
	IUDPSenderHandler *pHandler,
	bool bindIP,
	bool bindPort,
	int64_t initialNetworkHandle)
{
	BCRESULT result;

	if (!pTaskMgr || !pTimerMgr || !pSockMgr || !pConfig || !pHandler)
	{
		return BC_R_INVALIDARG;
	}

	m_sConfig.Init(pConfig);
	
	
	
	
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
	
	m_nNetworkHandle	= initialNetworkHandle;

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
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	if (m_bInterfaceBindingActive)
	{
		socklen_t peer_len =
			(refSockAddr.type.sa.sa_family == AF_INET6)
				? static_cast<socklen_t>(sizeof(refSockAddr.type.sin6))
				: static_cast<socklen_t>(sizeof(refSockAddr.type.sin));
		const uint32_t bound_idx = static_cast<uint32_t>(m_nNetworkHandle);

		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		
		const uint32_t scoped_idx = tt_route_lookup_ifindex(
			&refSockAddr.type.sa, peer_len, bound_idx);
		const uint32_t global_idx = tt_route_lookup_ifindex(
			&refSockAddr.type.sa, peer_len, 0);

#if defined(__APPLE__)
		
		
		
		const uint32_t peer_class =
			tt_peer_address_class(&refSockAddr.type.sa, (size_t)peer_len);
#else
		const uint32_t peer_class = TT_PEER_CLASS_GLOBAL;
#endif

		
		
		
		TTPinRecheckReason recheck_reason = TT_PIN_REASON_OK;
		const TTPinVerdict verdict = tt_vpn_pin_recheck(
			m_eVpnPolicy, bound_idx, scoped_idx, global_idx,
			peer_class, &recheck_reason);

		
		
		
		const bool force_diag =
			(m_eVpnPolicy == TT_VPN_POLICY_FORCE_PHYSICAL) &&
			(((global_idx != TT_ROUTE_IFINDEX_UNKNOWN &&
			   global_idx != bound_idx)) ||
			 (global_idx == TT_ROUTE_IFINDEX_UNKNOWN &&
			  (peer_class & TT_PEER_CLASS_FAKE_IP) != 0));

		if (verdict == TT_PIN_VERDICT_UNPIN || force_diag)
		{
			char ipbuf[INET6_ADDRSTRLEN] = {0};
			const void* ip_src = (refSockAddr.type.sa.sa_family == AF_INET6)
				? static_cast<const void*>(&refSockAddr.type.sin6.sin6_addr)
				: static_cast<const void*>(&refSockAddr.type.sin.sin_addr);
			inet_ntop(refSockAddr.type.sa.sa_family, ip_src,
				ipbuf, sizeof(ipbuf));

			if (m_eVpnPolicy == TT_VPN_POLICY_FORCE_PHYSICAL)
			{
				
				
				
				
				LogQ(m_pLoggerCtx, _WARN_,
					"UDP Sender: 全局路由表认为对端 %s 应走 ifIndex=%u，与已绑定的 "
					"ifIndex=%lld 不一致；vpnPolicy=force-physical，保持绑定不解除。"
					"这条日志本身不代表出错：VPN 抢占默认路由时全局表必然报不一致，"
					"而 macOS 的 IP_BOUND_IF 是硬绑定，包仍从物理网卡出去、服务端看到"
					"的仍是真实 IP。判断实际是否可达请看 scoped 路由表的答案："
					"scoped(ifIndex=%lld) -> %u（%u 表示查不出来，%u 表示该网卡的 "
					"scoped 表里确实没有到对端的路由，那才是真不可达）。",
					ipbuf, global_idx, (long long)m_nNetworkHandle,
					(long long)m_nNetworkHandle, scoped_idx,
					(unsigned)TT_ROUTE_IFINDEX_UNKNOWN,
					(unsigned)TT_ROUTE_IFINDEX_NO_ROUTE);
			}
			else
			{
#if defined(__APPLE__)
			if (recheck_reason == TT_PIN_REASON_BLACKHOLE_RISK)
			{
				LogQ(m_pLoggerCtx, _WARN_,
					"UDP Sender: 对端 %s 不是全局可路由地址（peerClass=0x%x："
					"0x1=TUN 代理 fake-IP 段 198.18.0.0/15 / 28.0.0.0/8，"
					"0x2=私网 / CGNAT / 链路本地 / 环回 / 保留段），而全局路由表把它"
					"判给 ifIndex=%u（%u 表示查不出来），与已绑定的 IP_BOUND_IF=%lld "
					"不一致。这类地址在公网上不可达，硬绑在物理网卡上发出去是静默"
					"黑洞，主动解绑回落系统路由。policy=%s。",
					ipbuf, (unsigned)peer_class, global_idx,
					(unsigned)TT_ROUTE_IFINDEX_UNKNOWN,
					(long long)m_nNetworkHandle,
					tt_vpn_policy_to_string(m_eVpnPolicy));
			}
			else if (recheck_reason == TT_PIN_REASON_SCOPED_NO_ROUTE)
			{
				LogQ(m_pLoggerCtx, _WARN_,
					"UDP Sender: 已绑定 IP_BOUND_IF=%lld，但内核在这块网卡的 scoped "
					"路由表里找不到到 %s 的路由（该网卡没有可用默认路由 / 地址族不"
					"匹配）。这是确定性不可达，解绑回落系统路由。policy=%s。",
					(long long)m_nNetworkHandle, ipbuf,
					tt_vpn_policy_to_string(m_eVpnPolicy));
			}
			else
			{
				
				LogQ(m_pLoggerCtx, _WARN_,
					"UDP Sender: 路由复核判定绑定无效（reason=%s，scoped=%u，"
					"global=%u），与已绑定的 IP_BOUND_IF=%lld 不一致，解绑以避免"
					"黑洞（TUN 模式代理 / VPN / Private Relay）。对端 %s，policy=%s。",
					tt_vpn_pin_reason_to_string(recheck_reason),
					scoped_idx, global_idx, (long long)m_nNetworkHandle,
					ipbuf, tt_vpn_policy_to_string(m_eVpnPolicy));
			}
			
			
			
			_TryClearInterfaceBinding(BC_R_SUCCESS);
#elif defined(_WIN32)
			
			
			
			
			
			
			
			
			LogQ(m_pLoggerCtx, _WARN_,
				"UDP Sender: kernel routes peer %s via ifIndex=%u "
				"but we hinted IP_UNICAST_IF=%lld; clearing hint to "
				"avoid source/route split (TUN-mode proxy / VPN). "
				"Set bypassVpn=false on the connector to silence "
				"this for your topology. (reason=%s)",
				ipbuf, global_idx, (long long)m_nNetworkHandle,
				tt_vpn_pin_reason_to_string(recheck_reason));
			_TryClearInterfaceBinding(BC_R_SUCCESS);
#else
			
			
			
			
			
			
			
			LogQ(m_pLoggerCtx, _WARN_,
				"UDP Sender: kernel routes peer %s via ifIndex=%u "
				"but we hinted IP_UNICAST_IF=%lld; source IP may be "
				"wrong on the wire. Diagnostic only — kernel will "
				"route correctly via the real outbound interface. "
				"(reason=%s)",
				ipbuf, global_idx, (long long)m_nNetworkHandle,
				tt_vpn_pin_reason_to_string(recheck_reason));
#endif
			}
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
	
	{
		TTPinRequest pin;
		memset(&pin, 0, sizeof(pin));
		pin.fd               = m_pSocket->GetFd();
		pin.ifIndex          = (uint32_t)m_nNetworkHandle;
		pin.androidNetHandle = (uint64_t)m_nNetworkHandle;
		pin.ipv6             = m_sConfig.ipv6 ? 1 : 0;
		pin.isTcp            = 0;
		pin.policy           = m_eVpnPolicy;
		pin.loggerCtx        = m_pLoggerCtx;

		char method[32] = "";
		int  pinErrno   = 0;
		TTPinResult pinResult =
			tt_socket_pin(&pin, method, sizeof(method), &pinErrno);
		if (pinResult == TT_PIN_OK)
		{
			m_bInterfaceBindingActive = true;
		}
	}
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

	
	BCEventQueue::_FlushEvents();
	BC_SAFE_DELETE_PTR(m_pRecvBuffer1);
	BC_SAFE_DELETE_PTR(m_pRecvBuffer2);
}

BOOL UDPSender::_ExitCheck()
{
	if (m_eState <= m_nNewState)
		return (FALSE); 

	ASSERT(m_nNewState < SNDR_STATE_READING);

	BCTask *pTask = GetTask();

	if (m_eState == SNDR_STATE_READING)
	{
		



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
			
			return (TRUE);
		}

		if (m_pSocket != NULL)
		{
			m_pSocket->Detach(&m_pSocket);
		}

		
		_Stop();

		if (m_nCtrls > 0)
		{
			
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
		







		if (m_nCheckAvailableTimerId > 0)
		{
			UnscheduleTask(m_nCheckAvailableTimerId);
			m_nCheckAvailableTimerId = 0;
		}

		




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

	
	_this->_ExitCheck();
}

void UDPSender::_RecvDoneCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCSockEvent *pSockEv = (BCSockEvent *)pEvent;
	UDPSender *_this = (UDPSender *)pSockEv->ev_arg;

	BCSpinMutex::Owner lock(_this->m_sLock);

	ASSERT(_this != NULL);
	
	pTask->GetCurTime(&_this->m_nLatestNetActionTime);
	
	ASSERT (_this->m_nPendingRecv == 1);
	_this->m_nPendingRecv--;

	if (pSockEv->result != BC_R_SUCCESS && _this->m_nPendingRestart == 0)
	{
		
		
		
		
		
		
		
		
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
	
	if (_this->m_nPendingSend > 0)
	{
		_this->m_nPendingSend--;
	}

	if (pSockEv->result != BC_R_SUCCESS && _this->m_nPendingRestart == 0)
	{
		
		
		
		
		
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
			
			
			
			
			_this->_TryClearInterfaceBinding(pSockEv->result);
		}
	}

	if (_this->_ExitCheck())
	{
		return;
	}

	


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


	(void)_ExitCheck();
	return true;
}

void UDPSender::OnEventProcShutdown()
{
	
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

	
	
	
	char method[32] = "";
	int  unpinErrno = 0;
	tt_socket_unpin(fd, m_sConfig.ipv6 ? 1 : 0, m_pLoggerCtx,
		method, sizeof(method), &unpinErrno);

	
	
	
	
	m_bInterfaceBindingActive = false;

	LogQ(m_pLoggerCtx, _WARN_,
		"UDP Sender: clearing %s after trigger=%u (%s) fd=%d errno=%d. "
		"Likely a TUN-mode VPN (Clash / Surge / V2Ray) where the server hostname "
		"resolved to a fake-IP / a route only present on the proxy adapter; "
		"falling back to OS default routing.",
		method, (unsigned)triggerResult, BC::bc_result2string(triggerResult),
		fd, unpinErrno);
	return true;
#else
	(void)triggerResult;
	return false;
#endif
}

void UDPSender::_OnConnectDone(BCRESULT result)
{
	
	
	
	
	
	
	LogQ(m_pLoggerCtx,
		result == BC_R_SUCCESS ? _INFO_ : _WARN_,
		"UDP Sender: connect done result=%u (%s)",
		(unsigned)result, BC::bc_result2string(result));

	
	
	
	
	
	
	
	
	
	if (result == BC_R_NETUNREACH || result == BC_R_HOSTUNREACH ||
		result == BC_R_ADDRNOTAVAIL)
	{
		if (m_eVpnPolicy == TT_VPN_POLICY_FORCE_PHYSICAL)
		{
			
			
			LogQ(m_pLoggerCtx, _WARN_,
				"UDP Sender: connect 失败 result=%u (%s) 且已绑定 ifIndex=%lld，"
				"但 vpnPolicy=force-physical，不解除绑定，错误如实上抛。",
				(unsigned)result, BC::bc_result2string(result),
				(long long)m_nNetworkHandle);
		}
		else
		{
			_TryClearInterfaceBinding(result);
		}
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
	
}




