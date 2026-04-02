///////////////////////////////////////////////////////////////////////////////
// file : HTTPConnOut.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef HTTP_HTTPCONNOUT_H_INCLUDED__
#define HTTP_HTTPCONNOUT_H_INCLUDED__

#include <string>
#include <HTTP/Exports.h>
#include <BC/BCEventQueue.h>
#include <BC/BCTimer.h>
#include <BC/BCUserData.h>
#include <HTTP/IHTTPHandler.h>
#include <HTTP/HTTPProtocol.h>
#include <BC/BCSocket.h>


using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

enum
{
	    HTTP_RECV_DATA          = 201
};

namespace HTTP
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

class HTTPConnOutMgr;

///////////////////////////////////////////////////////////////////////////////
// class : HTTPConnOut
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPConnOut
	: public BCNodeList::Node
	, public BCEventQueue
	, public IHTTPChunkHandler
	, public BCUserData
{
	DECLARE_FIXED_ALLOC(HTTPConnOut);

	friend class HTTPConnOutMgr;
public:
	typedef enum NetIoStateE
	{
		RECV_HEADER		= 0,
		RECV_BODY		= 1,
		RECV_CHUNK		= 2,
		SEND_HEADER		= 3,
		SEND_BODY		= 4
	}NetIoStateE;
public:
	HTTPConnOut();
	~HTTPConnOut();

	BCRESULT			Create(
							BCTimerMgr *pTimerMgr, 
							BCTaskMgr *pTaskMgr,
							HTTPConnOutMgr *pConnMgr,
							BCSocketMgr *pSocketMgr,
							IHTTPResponder *pResponder);
	void				SetAside();
	void				Start();
	BCRESULT			Connect(LPCSTR szUrl, uint32_t nTimeoutUSec, BOOL bIPv6 = FALSE);
	BCRESULT			GetSockName(BCSockAddrS &refAddr);
	BCRESULT			GetPeerName(BCSockAddrS &refAddr);
	void				Send(BCBuffer &refBuffer);
	void				SendEx(BCBuffer &refBuffer);
	void				Recv();
	void				Disconnect(uint32_t nExitCode);
	// Properties
	// static functions
	static HTTPConnOut *	Create(IHTTPResponder *pResponder);
    bool                IsIPv6() const { return m_bIPv6; }
protected:
	inline void			_SetState(uint32_t eState, uint32_t nLineNumber)
	{
		m_nNewState		= eState;
		m_nStateLineNo	= nLineNumber;
	}
	void				_TCP_Connect();
	void				_CLI_StartWork();
	void				_CLI_StopWork(uint32_t nExitCode, BCRESULT result);
	BOOL				_ExitCheck();
	void				_ActiveCheck();
	void				_TCP_RecvResponse();
	void				_TCP_SendV(BCBuffer *pBuffer);
	void				_Stop();
	void				_Cleanup();
	// Chunk send utilities
	void				_SendChunkFromQueue();
	void				_AppendPacket(BCBuffer &refBody);
	void				_AppendPacketEx(BCBuffer &refBody);
	void				_AppendNotifier(HTTPNotifier *pNotifier);
	// Connect
	static void			_ConnectDoneCallback(BCTask *, BCTaskEvent *);
	void				_OnConnectDone(BCSocket *pSocket, BCRESULT result);
	// Recv
	static void			_RecvDoneCallback(BCTask *, BCTaskEvent *);
	void				_OnRecvHeader();
	void				_OnRecvBody();
	void				_ProcessResponse();
	// Send
	static void			_SendDoneCallback(BCTask *, BCTaskEvent *);
	void				_OnSendDone(BCBuffer *pBuffer);
	// Override IHTTPChunkHandler interface
	void				OnChunkRecv(BCBuffer *pChunk) override;
	// Override BCEventFactory interface
	bool				OnEventProcess(BCEventItemS &refEvent) override;
	void				OnEventProcShutdown() override;
	// static functions
	static void			_PacketSentCB(HTTPNotifier &refNotifier);
	static void			_PacketEventDtor(BCEventItemS &refEvent);
private:
	DECLARE_NO_COPY_CLASS(HTTPConnOut);
	HTTPConnOutMgr		*	m_pConnMgr;
	BCSocketMgr			*	m_pSocketMgr;
	BCSocket			*	m_pSocket;
	HTTPChunkParser			m_sChunkParser;
	BCBuffer			*	m_pRecvBuffer;
	HTTPReq					m_sHeaderParser;
	// Network events
	BCSockEvent				m_sRecvEvent;
	BCSockEvent				m_sSendEvent;
	// Asynch control event
	bool					m_bNeedDestroy;
	// Latest network action reminder, used to check connection activity
	uint32_t				m_nLatestNetActionTime;
	// Connect time
	uint32_t				m_nConnectTime;
	// Net status
	uint32_t				m_nPendingConnect;
	uint32_t				m_nPendingRecv;
	uint32_t				m_nPendingSend;
	NetIoStateE				m_eIoState;
	bool					m_bCtrlStop;
	bool					m_bCtrlConnet;
	bool					m_bCtrlHeader;
	uint32_t				m_nEOFCounter;
	// Asynch state
	uint32_t				m_eState;
	uint32_t				m_nStateLineNo;
	uint32_t				m_nNewState;
	uint32_t				m_nCloseStatus;
	// Exit code
	uint32_t				m_nExitCode;
	// packet queue
	HTTPMQueue				m_sMsgQueue;
	HTTPMItem			*	m_pCurrSendItem;
	BCBuffer				m_sSendBuffer;
	// Statistics
	uint64_t				m_nTotalRecv;
	uint64_t				m_nTotalSend;

	IHTTPResponder		*	m_pResponder;
	uint64_t				m_nContentRecv;
	std::string				m_strUrl;
	uint32_t				m_nTimeoutUSecs;
	int32_t					m_nConnectTimer;
	bool					m_bIPv6;
};

///////////////////////////////////////////////////////////////////////////////
// class : HTTPConnOutMgr
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPConnOutMgr : public BCEventQueue
{
	friend class HTTPConnOut;
public:
	HTTPConnOutMgr();
	~HTTPConnOutMgr();

	BCRESULT			Create(
							BCTaskMgr *pTaskMgr,
							BCTimerMgr *pTimerMgr,
							IHTTPHandler *pHandler);
	void				Start();
	void				Stop();
	static void			Destroy(HTTPConnOutMgr **ppMgr);
	HTTPConnOut		*	Allocate();
	uint32_t			GetActiveConn() const;
	void				SetConnTimeout(uint32_t nTimeout);
	void				SetMaxConnTimeout(uint32_t nTimeout);
	void				SetWorkFolder(LPCSTR lpWorkFolder);
	const BCPString &	GetWorkFolder() const;
	uint32_t			GetEOFTimes() const;
protected:
	inline void			_SetState(uint32_t eState, uint32_t nLineNumber)
	{
		m_nNewState		= eState;
		m_nStateLineNo	= nLineNumber;
	}
	void				_Active(HTTPConnOut *pConn);
	void				_InActive(HTTPConnOut *pConn);
	void				_Aside(HTTPConnOut *pConn);
	void				_Recycle(HTTPConnOut *pConn);
	void				_ResetTimer(bool bStart = false);
	void				_ActiveCheck();
	void				_Detach();
	void				_OnStateChange();
	BOOL				_ExitCheck();
#ifdef HAVE_DUMP_STATE
	void				Dump() const;
#endif
	static void			_ActiveCheckCB(BCTask *, BCTaskEvent *);
	// Override BCEventFactory interface
	bool				OnEventProcess(BCEventItemS &refEvent) override;
	void				OnEventProcShutdown() override;
private:
	DECLARE_NO_COPY_CLASS(HTTPConnOutMgr);

	BCSpinMutex				m_sLock;
	BCTaskMgr			*	m_pTaskMgr;
	BCTimerMgr			*	m_pTimerMgr;
	BCTimer				*	m_pActChkTimer;
	/* Locked by lock. */
	bool					m_bExisting;
	typedef TNodeList<HTTPConnOut>		HttpConnOutList;
	HttpConnOutList			m_lstActive;		/*%< Active clients */
	HttpConnOutList			m_lstInactive;		/*%< To be recycled */
	HttpConnOutList			m_lstAside;			/*%< Don't do active check*/
	uint32_t				m_nInstCount;
	BCMutex					m_sExitLock;
	BCCondition				m_sExitCond;
	uint32_t				m_nTimeout;
	uint32_t				m_nMaxTimeout;
	BCPString				m_strWorkFolder;
	IHTTPHandler		*	m_pHandler;
	// Asynch state
	uint32_t				m_nState;
	uint32_t				m_nNewState;
	uint32_t				m_nStateLineNo;
	uint32_t				m_nCloseStatus;
	// TokenFlush timer
	uint32_t				m_nLastFlashTime;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

#endif // HTTP_HTTPCONNOUT_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPConnOut.h
///////////////////////////////////////////////////////////////////////////////
