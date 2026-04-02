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
			return *this;
		}

		bool				ipv6;
		uint32_t			publishId;
		LPCSTR				host;
		uint16_t			port;

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
						bool bindPort = false);
	BCRESULT 		Restart();
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
	bool 					m_bRestart;
};

#endif // UDPSENDER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : UDPSender.h
///////////////////////////////////////////////////////////////////////////////
