///////////////////////////////////////////////////////////////////////////////
// file : UDPSender.h
// author : anto
///////////////////////////////////////////////////////////////////////////////
#ifndef UDPSENDER_H_INCLUDED__
#define UDPSENDER_H_INCLUDED__

#include "BC/BCTimer.h"
#include "BC/BCSocket.h"
#include "BC/BCFCodec.h"
#include "BC/BCEventQueue.h"
#include "VpnPolicy.h"


///////////////////////////////////////////////////////////////////////////////
// typedef & macros
///////////////////////////////////////////////////////////////////////////////

class UDPSender;

///////////////////////////////////////////////////////////////////////////////
// Class : IUDPSenderHandler
///////////////////////////////////////////////////////////////////////////////

class IUDPSenderHandler
{
public:
	IUDPSenderHandler() {}
	virtual ~IUDPSenderHandler() {}

	virtual void	OnSendData(
						uint32_t nWrite, 
						UDPSender *pSender)			= 0;
	virtual void	OnRecvData(
						BCBuffer* pBuffer, 
						BCSockAddrS& refSrcAddr)	= 0;
	virtual void	OnCheckAvailable()				= 0;
	virtual void	OnRestart(BCRESULT result)		= 0;
	virtual void	OnUdpClosed()					= 0;
};

///////////////////////////////////////////////////////////////////////////////
// Class : UDPSender
///////////////////////////////////////////////////////////////////////////////

class UDPSender : public BCEventQueue
{
	///////////////////////////////////////////////////////////////////////////////
	// class : Config
	///////////////////////////////////////////////////////////////////////////////

	class Config
	{
	public:
		Config() : ipv6(false), publishId(5), host(NULL), port(0)
			, checkAvailableInterval(2000000)
		{
		}

		Config(const Config &other)
		{
			operator=(other);
		}

		~Config()
		{
		}

		Config & operator=(const Config &other)
		{
			ipv6 = other.ipv6;
			publishId = other.publishId;
			host = pool_.Strdup(other.host);
			port = other.port;
			checkAvailableInterval = other.checkAvailableInterval;
			return *this;
		}

		bool				ipv6;
		uint32_t			publishId;
		LPCSTR				host;
		uint16_t			port;
		uint32_t			checkAvailableInterval;

		BCRESULT		Init(BCFObject *pConfig)
		{
			BCFVar *pVar;

			pVar = pConfig->Get("ipv6");
			if (IS_BCF_BOOL(pVar))
			{
				ipv6 = GET_BCF_BOOL(pVar);
			}
			pVar = pConfig->Get("publishId");
			if (IS_BCF_NUMBER(pVar))
			{
				publishId = (uint32_t)GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("host");
			if (IS_BCF_STRING(pVar))
			{
				host = pool_.Strdup(GET_BCF_STRING(pVar));
			}
			pVar = pConfig->Get("port");
			if (IS_BCF_NUMBER(pVar))
			{
				port = (uint32_t)GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("checkAvailableInterval");
			if (IS_BCF_NUMBER(pVar))
			{
				checkAvailableInterval = (uint32_t)GET_BCF_INT(pVar);
			}
			return BC_R_SUCCESS;
		}

	private:
		KBPool		pool_;
	};
public:
	UDPSender();
	virtual ~UDPSender();

	BCRESULT		Create(
						void *logger_ctx,
						BCTaskMgr *pTaskMgr,
						BCTimerMgr *pTimerMgr,
						BCSocketMgr *pSockMgr,
						BCFObject *pConfig,
						IUDPSenderHandler *pHandler,
						bool bindIP = false,
						bool bindPort = false,
						// 建 socket 之前就设好的网卡句柄（Apple/Windows/Linux
						// 是 ifIndex，Android 是 network_handle_t）。0 表示
						// 不绑定，由内核按默认路由决定出口——这也是历史行为。
						//
						// 为什么需要它：_InitSocket 在 Create 里就执行，那时
						// m_nNetworkHandle 还是 0，所以初始 socket 从来不绑定
						// 网卡；只有后续 Restart(ifIndex) 才会绑。结果是网络
						// 稳定、从不切网的场景下 vpnPolicy 完全不起作用，包照样
						// 走内核默认路由（TUN 代理下就是 VPN）。用 Restart() 补
						// 救不行——它是异步投递事件，和 Connect 存在竞态。
						int64_t initialNetworkHandle = 0);
	BCRESULT 		Restart(bool checkAvailable = false, int64_t networkHandle = 0);
	BCRESULT		Start(LPCSTR szHost, uint16_t nPort);
	BCRESULT		StartRecv();
	BCRESULT		Connect(BCSockAddrS& refSockAddr);
	BCRESULT		Send(
						BCSockAddrS& refSockAddr,
						LPCVOID lpData,
						size_t nSize);
	BCRESULT		SendMMsg(
						BCSockAddrS& refSockAddr,
						BCRegionS *io_vec,
						size_t iovec_len);
	BCRESULT		GetSockName(BCSockAddrS& refAddr);
	// 设置生效的 VPN 策略。必须在 Connect() 之前调用；SMPConnection::Create
	// 在 UDPSenderGroup::Create 成功后立刻调用，此时 socket 已建好但还没
	// connect，正是时机。
	//
	// TT_VPN_POLICY_FORCE_PHYSICAL 下，本对象拒绝撤销已有的网卡绑定——
	// 无论是 Connect() 里的路由表主动校验，还是 _OnConnectDone 收到
	// ENETUNREACH 的被动回收。业务显式要求"必须走物理网卡"，那么"连不上"
	// 就是正确结果，静默回落到 VPN 才是错的。
	void			SetVpnPolicy(TTVpnPolicy policy) { m_eVpnPolicy = policy; }
	void			Close();
	void			Destroy(UDPSender **ppSender);

protected:
	inline void		_SetState(uint32_t eState, uint32_t nLineNumber)
	{
		m_nNewState		= eState;
		m_nStateLineNo	= nLineNumber;
	}
	BCRESULT 		_InitSocket();
	BCRESULT		_StartWork(LPCSTR szHost, uint16_t nPort);
	void			_StopWork();
	void			_Cleanup();
	BOOL			_ExitCheck();
	void			_Restart();
	// If the current UDP socket is pinned to a specific physical interface
	// (Apple IP_BOUND_IF / Windows + Linux IP_UNICAST_IF / Android
	// android_setsocknetwork) and either (a) the kernel reported
	// "network unreachable" / "address not available" / "host unreachable"
	// on this socket, or (b) the proactive route-table check in Connect()
	// found the peer routes through a different interface, the pin is the
	// reason we can't reach the peer (typical case: Clash / Surge / V2Ray
	// TUN mode resolves the server hostname to a fake-IP whose route only
	// exists on the proxy adapter; pinning the socket to the physical NIC
	// strands the fake-IP — and on Windows even forces the source IP to
	// the wrong interface, which the proxy's stateful NAT can't undo).
	// Clear the pin in-place so the kernel falls back to default routing
	// on the next send. Returns true if the pin was just cleared, false
	// if there was nothing to clear or the platform doesn't support
	// runtime un-pinning (currently only Android — its hard pin via
	// android_setsocknetwork has no public API to undo). Caller must
	// hold m_sLock.
	bool			_TryClearInterfaceBinding(BCRESULT triggerResult);
	void			_UDP_RecvChunk();
	BCRESULT		_UDP_Send(
						BCSockAddrS &refSockAddr, 
						LPCVOID lpData, 
						size_t nSize);
	BCRESULT		_UDP_SendMMsg(
						BCSockAddrS &refSockAddr, 
						BCRegionS *io_vec, 
						size_t iovec_len);

	static void		_ConnectDoneCB(BCTask *, BCTaskEvent *);
	static void		_RecvDoneCallback(BCTask *, BCTaskEvent *);
	static void		_SendDoneCallback(BCTask *, BCTaskEvent *);
	// Override BCEventFactory interfaces
	bool			OnEventProcess(BCEventItemS &refEvent) override;
	void			OnEventProcShutdown() override;
	// Chunk receive
	void			_OnConnectDone(BCRESULT result);
	void			_OnDataRecv(BCBuffer *pBuffer, BCSockAddrS &refSrcAddr);
	void			_OnSendDone(uint32_t nWrite, BCRESULT result);
	// Stop all event factory
	void			_Stop();
private:
	BCSpinMutex				m_sLock;
	void				*	m_pLoggerCtx;
	BCFObject			*	m_pConfig;
	Config					m_sConfig;
	BCTaskMgr			*	m_pTaskMgr;
	BCSocketMgr			*	m_pSockMgr;
	BCTimerMgr			*	m_pTimerMgr;
	BCSocket			*	m_pSocket;
	BCBuffer			*	m_pRecvBuffer1;
	BCBuffer			*	m_pRecvBuffer2;
	bool					m_bAlterBuffer;
	char					m_szHost[MAX_PATH];
	BCSockAddrS				m_sSelfAddr;
	BCSockAddrS				m_sSockAddr;
	uint16_t				m_nPort;
	// Network events
	BCSockEvent				m_sRecvEvent;
	BCSockEvent				m_sSendEvent;
	// Latest network action reminder, used to check connection activity
	uint32_t				m_nLatestNetActionTime;
	// Net status
	uint32_t				m_nPendingConnect;
	uint32_t				m_nPendingRecv;
	uint32_t				m_nPendingSend;
	// Asynch state
	uint32_t				m_eState;
	uint32_t				m_nNewState;
	uint32_t				m_nStateLineNo;
	uint32_t				m_nCloseStatus;
	BCMutex					m_sExitLock;
	BCCondition				m_sExitCond;
	IUDPSenderHandler	*	m_pHandler;
	bool 					m_bBindIP;
	bool 					m_bBindPort;
	uint32_t 				m_nPendingRestart;
	bool 					m_bCheckAvailable;
	int32_t 				m_nCheckAvailableTimerId;
	uint32_t 				m_nRecvDataCount;
	// Platform-overloaded handle used by _InitSocket() to bind the UDP socket
	// to a specific network interface during connection migration:
	//   * Android : opaque uint64_t network_handle_t (consumed by
	//               android_setsocknetwork(handle, fd))
	//   * iOS     : kernel ifIndex (consumed by setsockopt(IP_BOUND_IF))
	//   * macOS   : kernel ifIndex (consumed by setsockopt(IP_BOUND_IF))
	//   * Linux   : kernel ifIndex (consumed by setsockopt(IP_UNICAST_IF),
	//               host byte order — no htonl)
	//   * Windows : NET_IFINDEX    (consumed by setsockopt(IP_UNICAST_IF),
	//               IPv4 byte-swapped via htonl, IPv6 host order)
	// Always int64 in storage so a single Restart() signature works for both
	// the Android handle path and the ifIndex platforms. Zero means "don't
	// bind" — OS picks default.
	int64_t 				m_nNetworkHandle;
	// Set true by _InitSocket() after a successful platform-specific bind
	// (setsockopt(IP_BOUND_IF / IP_UNICAST_IF) or android_setsocknetwork).
	// Reset to false by _TryClearInterfaceBinding() once we've decided to
	// un-pin because the route is unreachable on the pinned interface, and
	// also reset (implicitly via re-bind) on the next _InitSocket() call.
	// Guards _TryClearInterfaceBinding from un-pinning twice per socket
	// life so we don't flap setsockopt under a flood of NETUNREACH errors.
	bool 					m_bInterfaceBindingActive;
	// 生效的 VPN 策略，由 SMPConnection::Create 经 UDPSenderGroup 下发。
	// 默认 UNSET，等同于"不抑制解绑"，也就是改动前的历史行为——SMPServer
	// 等不走 connector 配置的调用方因此不受影响。
	TTVpnPolicy				m_eVpnPolicy;
};

#endif // UDPSENDER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : UDPSender.h
///////////////////////////////////////////////////////////////////////////////
