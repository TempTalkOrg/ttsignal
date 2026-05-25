///////////////////////////////////////////////////////////////////////////////
// file : HTTPConnOut.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include <BC/BCLog.h>
#include "HTTPConnector.h"
#include "HTTPConnOut.h"



///////////////////////////////////////////////////////////////////////////////
// Namespace :
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

#ifdef DEBUG_HTTPD
#define ENTER(x)	do { fprintf(stderr, "ENTER %s\n", (x)); } while (0)
#define EXIT(x)		do { fprintf(stderr, "EXIT %s\n", (x)); } while (0)
#define NOTICE(x)	do { fprintf(stderr, "NOTICE %s\n", (x)); } while (0)
#else
#define ENTER(x)	do { } while(0)
#define EXIT(x)		do { } while(0)
#define NOTICE(x)	do { } while(0)
#endif

#define MSGSTATE_FREED						0
#define MSGSTATE_INACTIVE					1
#define MSGSTATE_READY						2
#define MSGSTATE_READING					3
#define MSGSTATE_WORKING					4
#define MSGSTATE_MAX						9


#define EDGESTATE_UPDATE			1

#define EDGESTATE_MSGLEN			18

enum
{
	HTTPTSK_STARTWORK				= 1,
	HTTPTSK_STOPWORK				= 2,
	HTTPTSK_CHECKACTIVE				= 3,
	HTTPTSK_SENDPACKET				= 4,
	HTTPTSK_RECVPACKET				= 5,
	// Number of events
	HTTPTSK_NUMBER
};

enum
{
	BCM_SOCKET_CONNECT		= HTTPTSK_NUMBER + 1,
	BCM_SOCKET_ONCONNECT,
	BCM_SOCKET_DISCONNECT,
	BCM_SOCKET_ONDISCONNECT,

	BCM_SOCKET_NUMBER
};

///////////////////////////////////////////////////////////////////////////////
// class : HTTPConnOut
///////////////////////////////////////////////////////////////////////////////

#define _set_state(conn, _state, _status)	\
	(conn)->_SetState(_state, __LINE__);(conn)->m_nCloseStatus = _status

IMPLEMENT_FIXED_ALLOC(HTTPConnOut, 100);

HTTPConnOut::HTTPConnOut()
	: m_pConnMgr(NULL)
	, m_pSocketMgr(NULL)
	, m_pSocket(NULL)
	, m_pRecvBuffer(NULL)
	, m_bNeedDestroy(false)
	, m_nLatestNetActionTime(0)
	, m_nConnectTime(0)
	, m_nPendingConnect(0)
	, m_nPendingRecv(0)
	, m_nPendingSend(0)
	, m_eIoState(RECV_HEADER)
	, m_bCtrlStop(false)
	, m_bCtrlConnet(false)
	, m_bCtrlHeader(false)
	, m_nEOFCounter(0)
	, m_eState(MSGSTATE_INACTIVE)
	, m_nNewState(MSGSTATE_MAX)
	, m_nCloseStatus(BC_R_SUCCESS)
	, m_nExitCode(0)
	, m_pCurrSendItem(NULL)
	, m_nTotalRecv(0)
	, m_nTotalSend(0)
	, m_pResponder(NULL)
	, m_nContentRecv(0)
	, m_nTimeoutUSecs(5000000)
	, m_nConnectTimer(0)
	, m_bIPv6(false)
{
	m_pRecvBuffer = m_sChunkParser.GetRecvBuf();
}

HTTPConnOut::~HTTPConnOut()
{
	//
}

BCRESULT HTTPConnOut::Create(
	BCTimerMgr *pTimerMgr,
	BCTaskMgr *pTaskMgr,
	HTTPConnOutMgr *pConnMgr,
	BCSocketMgr *pSocketMgr,
	IHTTPResponder *pResponder)
{
	BCRESULT result;

	ASSERT(pConnMgr != NULL);
	ASSERT(pTaskMgr != NULL);
	ASSERT(pSocketMgr != NULL);
	ASSERT(pResponder != NULL);

	m_pConnMgr			= pConnMgr;
	m_pSocketMgr		= pSocketMgr;
	m_pRecvBuffer		= m_sChunkParser.GetRecvBuf();
	m_nNewState			= MSGSTATE_MAX;
	m_nPendingRecv		= 0;
	m_nPendingSend		= 0;
	m_pResponder		= pResponder;

	result = m_sChunkParser.Create(this);
	if (result != BC_R_SUCCESS)
	{
		ASSERT(0);
		return result;
	}

	result = BCEventQueue::Create(pTimerMgr, pTaskMgr, "HTTPConnOut", this);
	if (result != BC_R_SUCCESS)
	{
		ASSERT(0);
		return result;
	}

	m_sRecvEvent.ev_type = BC_SOCKEVENT_RECVDONE;
	m_sRecvEvent.ev_action = _RecvDoneCallback;
	m_sRecvEvent.ev_arg = this;
	m_sSendEvent.ev_type = BC_SOCKEVENT_SENDDONE;
	m_sSendEvent.ev_action = _SendDoneCallback;
	m_sSendEvent.ev_arg = this;

	return BC_R_SUCCESS;
}

void HTTPConnOut::_Cleanup()
{
	if (m_pSocket != NULL)
	{
		m_pSocket->Detach(&m_pSocket);
	}
}

void HTTPConnOut::SetAside()
{
	m_pConnMgr->_Aside(this);
}

void HTTPConnOut::Start()
{
	PostEvent(MAKEEVENT(HTTPTSK_STARTWORK, 0, 0));
}

BCRESULT HTTPConnOut::Connect(LPCSTR szUrl, uint32_t nTimeoutUSec, BOOL bIPv6/* = FALSE*/)
{
	BCEventItemS sEvent(MAKEEVENT(BCM_SOCKET_CONNECT, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyString(szUrl);
	sEvent.lParam = nTimeoutUSec;
	sEvent.vParams[0] = bIPv6;
	PostEvent(sEvent);

	return BC_R_SUCCESS;
}

BCRESULT HTTPConnOut::GetSockName(BCSockAddrS &refAddr)
{
	if (m_pSocket)
	{
		return m_pSocket->GetSockName(&refAddr);
	}
	return BC_R_FAILURE;
}

BCRESULT HTTPConnOut::GetPeerName(BCSockAddrS &refAddr)
{
	if (m_pSocket)
	{
		return m_pSocket->GetPeerName(&refAddr);
	}
	return BC_R_FAILURE;
}

void HTTPConnOut::Send(BCBuffer &refBuffer)
{
	BCBuffer *pPacket;

	pPacket = refBuffer.Clone();
	if (pPacket)
	{
		PostEvent(MAKEEVENT(HTTPTSK_SENDPACKET, 0, 0), NULL,
			pPacket, _PacketEventDtor);
	}
}

void HTTPConnOut::SendEx(BCBuffer &refBuffer)
{
	BCBuffer *pPacket;

	pPacket = new BCBuffer();
	if (pPacket)
	{
		refBuffer.RemoveConsumed();
		refBuffer.Extract(pPacket, INFINITE);
		PostEvent(MAKEEVENT(HTTPTSK_SENDPACKET, 0, 0), NULL,
			pPacket, _PacketEventDtor);
	}
}

void HTTPConnOut::Recv()
{
	PostEvent(MAKEEVENT(HTTPTSK_RECVPACKET, 0, 0));
}

void HTTPConnOut::Disconnect(uint32_t nExitCode)
{
	PostEvent(MAKEEVENT(HTTPTSK_STOPWORK, 0, 0), nExitCode, BC_R_SUCCESS);
}

BOOL HTTPConnOut::_ExitCheck()
{
	if (m_eState <= m_nNewState)
		return (FALSE); /* Business as usual. */

	ASSERT(m_nNewState < MSGSTATE_WORKING);

	BCTask *pTask = GetTask();

	if (m_eState == MSGSTATE_WORKING)
	{
		ASSERT(m_nNewState <= MSGSTATE_READING);

		/*
		* We are trying to abort connect processing.
		*/
		if (m_nPendingConnect > 0)
		{
			m_pSocket->Cancel(pTask, BC_SOCKCANCEL_CONNECT);
		}

		if (! (m_nPendingConnect == 0))
		{
			/*
			* Still waiting for I/O cancel completion.
			* or lingering references.
			*/
			return (TRUE);
		}

		/*
		* We are trying to abort send processing.
		*/
		if (m_nPendingSend > 0)
		{
			m_pSocket->Cancel(pTask, BC_SOCKCANCEL_SEND);
		}

		if (! (m_nPendingSend == 0))
		{
			/*
			* Still waiting for I/O cancel completion.
			* or lingering references.
			*/
			return (TRUE);
		}
		/*
		* I/O cancel is complete.  Burn down all state
		* related to the current request.  Ensure that
		* the client is on the active list and not the
		* recursing list.
		*/

		m_eState = MSGSTATE_READING;

		if (MSGSTATE_READING == m_nNewState)
		{
			return (TRUE); /* We're done. */
		}
	}

	if (m_eState == MSGSTATE_READING)
	{
		/*
		* We are trying to abort the current TCP connection,
		* if any.
		*/
		ASSERT(m_nNewState <= MSGSTATE_READY);

		if (m_nPendingRecv > 0)
		{
			m_pSocket->Cancel(pTask, BC_SOCKCANCEL_RECV);
		}
		if (! (m_nPendingRecv == 0))
		{
			/* Still waiting for read cancel completion. */
			return (TRUE);
		}

		if (m_pSocket != NULL)
		{
			m_pSocket->Detach(&m_pSocket);
		}

		if (m_nConnectTimer > 0)
		{
			UnscheduleTask(m_nConnectTimer);
		}

		m_eState = MSGSTATE_READY;

		if (MSGSTATE_READY == m_nNewState)
		{
			m_nNewState = MSGSTATE_MAX;
			return (TRUE);
		}
	}

	if (m_eState == MSGSTATE_READY)
	{
		ASSERT(m_nNewState <= MSGSTATE_INACTIVE);

		// Stop all type control events
		_Stop();

		if (m_nCtrls > 0)
		{
			/* Still waiting for control event to be delivered */
			return (TRUE);
		}

		m_eState = MSGSTATE_INACTIVE;

		if (m_eState == m_nNewState)
		{
			// Continue work
		}
	}

	if (m_eState == MSGSTATE_INACTIVE)
	{
		ASSERT(m_nNewState == MSGSTATE_FREED);
		/*
		* We are trying to free the client.
		*
		* When "shuttingdown" is true, either the task has received
		* its shutdown event or no shutdown event has ever been
		* set up.  Thus, we have no outstanding shutdown
		* event at this point.
		*/
		ASSERT(m_eState == MSGSTATE_INACTIVE);

		/*
		* Detaching the task must be done after unlinking from
		* the manager's lists because the manager accesses
		* this->task.
		*/
		BCEventQueue::Detach();

		// Make sure Detach only called once.
		m_eState = MSGSTATE_FREED;
	}

	return TRUE;
}

void HTTPConnOut::_TCP_RecvResponse()
{
	BCRESULT result;

	if (_ExitCheck())
	{
		return;
	}

	result = m_pSocket->RecvV2(m_pRecvBuffer, 1,
		GetTask(), &m_sRecvEvent, 0);
	if (result == BC_R_SUCCESS || result == BC_R_INPROGRESS)
	{
		ASSERT(m_nPendingRecv == 0);
		m_nPendingRecv++;
		result = BC_R_SUCCESS;
	}
	else
	{
		ASSERT(0);
		// Do some recycle for exit process

		// Set exit signal
		_set_state(this, MSGSTATE_FREED, result);
	}
	(void)_ExitCheck();
}

void HTTPConnOut::_TCP_SendV(BCBuffer *pBuffer)
{
	BCRESULT result;

	if (_ExitCheck())
	{
		return;
	}

	result = m_pSocket->SendToV2(pBuffer, GetTask(),
		NULL, NULL, (BCSockEvent *)&m_sSendEvent, 0);
	if (result == BC_R_SUCCESS || result == BC_R_INPROGRESS)
	{
		ASSERT(m_nPendingSend == 0);
		m_nPendingSend++;
	}
	else
	{
		// Do some recycle for exit process

		// Set exit signal
		_set_state(this, MSGSTATE_FREED, result);
	}

	(void)_ExitCheck();
}

void HTTPConnOut::_TCP_Connect()
{
	BCRESULT result;
	BCSockAddrS sSockAddrS;
	std::string strIP, strLocation;
	uint16_t nPort;
    int32_t iNetType = AF_UNSPEC;
	bool bSSL = false;

	if (m_nPendingConnect > 0)
	{
		return;
	}

	result = HTTPProtocol::ParseAddrFromUrl(m_strUrl.c_str(), strIP, 
		nPort, iNetType, strLocation, bSSL);
	if (result == BC_R_SUCCESS)
	{
		if (iNetType == AF_INET6)
		{
			struct in6_addr in6a;
			if (bc_net_pton(PF_INET6, strIP.c_str(), &in6a) <= 0)
			{
				result = BC_R_HOSTUNREACH;
			}
			else
			{
				bc_sockaddr_fromin6(&sSockAddrS, &in6a, nPort);
				m_bIPv6 = true;
			}
		}
		else
		{
			struct in_addr ina;
			if (bc_net_pton(PF_INET, strIP.c_str(), &ina) <= 0)
			{
				result = BC_R_HOSTUNREACH;
			}
			else
			{
				bc_sockaddr_fromin(&sSockAddrS, &ina, nPort);
				m_bIPv6 = false;
			}
		}
	}
	if (result != BC_R_SUCCESS)
	{
		if (m_pResponder)
		{
			m_pResponder->OnHTTPConnect(result, this);
		}
		return;
	}

	m_eState = MSGSTATE_WORKING;
	if (!m_pSocket)
	{
		BCSocket *pSocket;

		pSocket = new BCSocket();
		if (pSocket == NULL)
		{
			goto return_error;
		}
		result = pSocket->Create(m_pSocketMgr,
			m_bIPv6 ? PF_INET6 : PF_INET, bc_sockettype_tcp);
		if (result != BC_R_SUCCESS)
		{
			goto return_error;
		}
		m_pSocket = pSocket;
	}
	/*
	* Queue up connect event.
	*/
	result = m_pSocket->Connect(&sSockAddrS,
		GetTask(), _ConnectDoneCallback, this);
	if (result == BC_R_SUCCESS || result == BC_R_INPROGRESS)
	{
		ASSERT(m_nPendingConnect == 0);
		m_nPendingConnect++;
	}
	if (!m_nConnectTimer)
	{
		ScheduleTask(m_nConnectTimer, [this](int32_t nTimerId) {
			Disconnect(BC_R_TIMEDOUT);
		}, m_nTimeoutUSecs);
	}

return_error:
	return;
}

void HTTPConnOut::_CLI_StartWork()
{
	// Get latest net action time, to avoid HTTPConnOutMgr
	// active check error occur
	bc_stdtime_get(&m_nLatestNetActionTime);
	m_nConnectTime = m_nLatestNetActionTime;
	// Set connection state
	m_eState = MSGSTATE_WORKING;
	// Active this connection to subscribe active check events
	m_pConnMgr->_Active(this);
}

void HTTPConnOut::_CLI_StopWork(uint32_t nExitCode, BCRESULT result)
{
	if (!m_nExitCode)
	{
		m_nExitCode = nExitCode;
	}
	_set_state(this, MSGSTATE_FREED, result);
}

void HTTPConnOut::_ConnectDoneCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCSockOCEvent *pSockEv = (BCSockOCEvent *)pEvent;
	HTTPConnOut *_this = (HTTPConnOut *)pEvent->ev_arg;

	UNUSED(pTask);
	ASSERT(_this != NULL);

	if (_this->m_nPendingConnect > 0)
	{
		_this->m_nPendingConnect--;
	}

	_this->_OnConnectDone((BCSocket *)pSockEv->ev_sender, pSockEv->result);
	_this->_ExitCheck();
	pEvent->Destroy();
}

void HTTPConnOut::_RecvDoneCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCSockEvent *pSockEv = (BCSockEvent *)pEvent;
	HTTPConnOut *_this = (HTTPConnOut *)pSockEv->ev_arg;

	ASSERT(_this != NULL);
	// Get latest
	pTask->GetCurTime(&_this->m_nLatestNetActionTime);
	// Check pending recv ref count
	ASSERT (_this->m_nPendingRecv == 1);
	_this->m_nPendingRecv--;

	if (pSockEv->result != BC_R_SUCCESS)
	{
		switch (pSockEv->result)
		{
		case BC_R_EOF:
		case BC_R_CANCELED:
			break;
		default:
			//Set free signal
			_set_state(_this, MSGSTATE_FREED, pSockEv->result);
			break;
		}
	}

	if (_this->_ExitCheck())
	{
		return;
	}

	if (pSockEv->result == BC_R_EOF)
	{
		if (_this->m_nEOFCounter >= _this->m_pConnMgr->GetEOFTimes())
		{
			//Set free signal
			_set_state(_this, MSGSTATE_FREED, pSockEv->result);
		}
		else
		{
			_this->m_nEOFCounter++;
		}
	}
	else
	{
		_this->m_nEOFCounter = 0;

		/*
		* Success.
		*/
#ifndef _DEBUG
		try
		{
#endif
			switch (_this->m_eIoState)
			{
			case RECV_HEADER:
				_this->_OnRecvHeader();
				break;
			case RECV_BODY:
			case RECV_CHUNK:
				_this->_OnRecvBody();
				break;
			default:
				break;
			}
#ifndef _DEBUG
		}
		catch (...)
		{
			LogFatal(_LOCAL_, "Unexcepted error occurred!");
			_set_state(_this, MSGSTATE_FREED, BC_R_UNEXPECTED);
		}
#endif
	}

	if (_this->m_nPendingRecv == 0)
	{
		_this->_TCP_RecvResponse();
	}

	(void)_this->_ExitCheck();
}

void HTTPConnOut::_SendDoneCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCSockEvent *pSockEv = (BCSockEvent *)pEvent;
	HTTPConnOut *_this = (HTTPConnOut *)pSockEv->ev_arg;

	ASSERT(_this != NULL);
	// Get latest
	pTask->GetCurTime(&_this->m_nLatestNetActionTime);
	// Check pending send ref count
	ASSERT(_this->m_nPendingSend == 1);
	_this->m_nPendingSend--;

	if (pSockEv->result != BC_R_SUCCESS)
	{
		//Do some recycle work for exit process


		//Set free signal
		_set_state(_this, MSGSTATE_FREED, pSockEv->result);
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
		_this->_OnSendDone(pSockEv->bufferlist);
#ifndef _DEBUG
	}
	catch(...)
	{
		LogError(_LOCAL_, "Unexcepted error occurred!");
		_set_state(_this, MSGSTATE_FREED, BC_R_UNEXPECTED);
	}
#endif

	(void)_this->_ExitCheck();
}

void HTTPConnOut::_OnConnectDone(BCSocket *pSocket, BCRESULT result)
{
	BCRESULT Ret = BC_R_SUCCESS;
	HTTPNotifier sNotifier;

	UNUSED(pSocket);

	if (m_pResponder)
	{
		m_bCtrlConnet = true;
		Ret = m_pResponder->OnHTTPConnect(result, this);
	}
	if (result != BC_R_SUCCESS)
	{
		switch(result)
		{
		case BC_R_TIMEDOUT:
		case BC_R_NETUNREACH:
		case BC_R_CONNREFUSED:
		case BC_R_CANCELED:
			if (Ret == BC_R_SUCCESS)
			{
				_TCP_Connect();
				break;
			}
		default:
			_set_state(this, MSGSTATE_FREED, result);
			break;
		}
		goto exit_;
	}
	// Get connect time
	bc_stdtime_get(&m_nConnectTime);
	// Send http request
	// Prepare request header data
	//m_sHeaderParser.Response();
	m_pResponder->OnRequestReady(m_sHeaderParser, this);
	//m_sHeaderParser.EndHeaders();  /* done */

	/*
	 * Link the data buffer into our send queue, should we have any data
	 * rendered into it.  If no data is present, we won't do anything
	 * with the buffer.
	 */
	_AppendPacket(m_sHeaderParser.m_sHeaderBuffer);
	sNotifier.Init(HTTPM_TASK, _PacketSentCB, this);
	_AppendNotifier(&sNotifier);
	_SendChunkFromQueue();
exit_:
	return;
}

bool HTTPConnOut::OnEventProcess(BCEventItemS &refEvent)
{
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
		case HTTPTSK_STARTWORK:
			_CLI_StartWork();
			break;
		case HTTPTSK_STOPWORK:
			_CLI_StopWork((uint64_t)refEvent.wParam, (uint64_t)refEvent.lParam);
			break;
		case HTTPTSK_CHECKACTIVE:
			_ActiveCheck();
			break;
		case HTTPTSK_SENDPACKET:
			{
				BCBuffer *pPacket = (BCBuffer *)refEvent.lParam;
				if (pPacket)
				{
					HTTPNotifier sNotifier;

					_AppendPacketEx(*pPacket);
					sNotifier.Init(HTTPM_TASK, _PacketSentCB, this);
					_AppendNotifier(&sNotifier);
					_SendChunkFromQueue();
				}
			}
			break;
		case HTTPTSK_RECVPACKET:
			if (m_nPendingRecv == 0)
			{
				_TCP_RecvResponse();
			}
			break;
		case BCM_SOCKET_CONNECT:
			if (refEvent.wParam)
			{
				m_strUrl = (LPCSTR)refEvent.wParam;
				m_nTimeoutUSecs = refEvent.lParam;
				m_bIPv6 = !!refEvent.vParams[0];
				_TCP_Connect();
			}
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
		_set_state(this, MSGSTATE_FREED, BC_R_UNEXPECTED);
	}
#endif

	(void)_ExitCheck();
	return true;
}

void HTTPConnOut::OnEventProcShutdown()
{
	// Do some clean up work, as well as release some memory resources
	_Cleanup();

	if (m_pResponder)
	{
		m_pResponder->OnHTTPStop(this, m_nExitCode);
	}

	m_pConnMgr->_Recycle(this);
}

void HTTPConnOut::_OnRecvHeader()
{
	BCRESULT result;

	// TODO : Parse http request
	result = m_sHeaderParser.ParseResponse(*m_pRecvBuffer);
	if (result == BC_R_NOTFOUND)
	{
		goto out;
	}
	else if (result != BC_R_SUCCESS)
	{
		// Close connection
		_set_state(this, MSGSTATE_FREED, result);
		goto out;
	}

	m_eIoState = RECV_BODY;

	_ProcessResponse();
out:
	EXIT("recv");
}

void HTTPConnOut::_OnRecvBody()
{
	switch(m_eIoState)
	{
	case RECV_BODY:
		if (m_pResponder)
		{
			BCBuffer *pBuffer = new BCBuffer();
			BCBOStream sWriter(pBuffer);
			m_nContentRecv += m_pRecvBuffer->RemainingLength();
			m_pRecvBuffer->WriteTo(sWriter);
			m_pResponder->OnResponseBody(pBuffer, this);
			m_pRecvBuffer->Reset();
			if (m_nContentRecv == m_sHeaderParser.GetContentLength())
			{
				m_pResponder->OnResponseEnd(this);
			}
		}
		break;
	case RECV_CHUNK:
		m_sChunkParser.Parse();
		if(m_pResponder)
		{
			m_pResponder->OnHTTPMessage(HTTP_RECV_DATA, NULL, this);
		}
		break;
	default:
		break;
	}
}

void HTTPConnOut::OnChunkRecv(BCBuffer *pChunk)
{
	ScopedPointer<BCBuffer> dtor(pChunk);
	if (m_pResponder)
	{
		if (pChunk->RemainingLength() > 0)
		{
			m_pResponder->OnResponseBody(dtor.Release(), this);
		} 
		else
		{
			m_pResponder->OnResponseEnd(this);
		}
	}
}

void HTTPConnOut::_ProcessResponse()
{
	if (m_pResponder)
	{
		uint64_t nContentLen = 0;

		m_bCtrlHeader = true;
		m_pResponder->OnResponseHeader(m_sHeaderParser, this);
		nContentLen = m_sHeaderParser.GetContentLength();
		if (nContentLen > 0)
		{
			m_eIoState = RECV_BODY;
			if (m_pRecvBuffer->RemainingLength() > 0)
			{
				BCBuffer *pBuffer = new BCBuffer();
				BCBOStream sWriter(pBuffer);
				m_nContentRecv += m_pRecvBuffer->RemainingLength();
				m_pRecvBuffer->WriteTo(sWriter);
				m_pResponder->OnResponseBody(pBuffer, this);
				m_pRecvBuffer->Reset();
				if (m_nContentRecv == m_sHeaderParser.GetContentLength())
				{
					m_pResponder->OnResponseEnd(this);
				}
			}
		}
		else if (m_sHeaderParser.IsChunkedBody())
		{
			m_eIoState = RECV_CHUNK;
			m_sChunkParser.Parse();
		}
	}
}

void HTTPConnOut::_OnSendDone(BCBuffer *pBuffer)
{
	uint32_t nDataLen;

	ASSERT(pBuffer == &m_sSendBuffer);

	nDataLen = pBuffer->UsedLength();
	// Record send data count
	m_nTotalSend += nDataLen;

	//LogBin(_LOCAL_, refRegion.base, refRegion.length);
	m_sSendBuffer.Reset(0);
	// Continue send
	_SendChunkFromQueue();
}

void HTTPConnOut::_SendChunkFromQueue()
{
	uint32_t nDataLen;

	while(m_nNewState >= MSGSTATE_WORKING
		&& m_nPendingSend == 0)
	{
		nDataLen = m_sSendBuffer.RemainingLength();
		// last send wasn't fulfill due to band width limitation
		if (nDataLen > 0)
		{
			// Ready to send
			_TCP_SendV(&m_sSendBuffer);
		}
		// last packet wasn't finish send
		else if (m_pCurrSendItem != NULL)
		{
			ASSERT(m_pCurrSendItem->m_eType != HTTPM_NOTIFIER);
			m_pCurrSendItem->m_sBuffer.Extract(&m_sSendBuffer,
				BC_SOCKET_MAXSCATTERGATHER);
			if (m_pCurrSendItem->m_sBuffer.RemainingLength() == 0)
			{
				BC_SAFE_DELETE_PTR(m_pCurrSendItem);
			}
			nDataLen = m_sSendBuffer.RemainingLength();
			ASSERT(nDataLen > 0);
			// Ready to send
			_TCP_SendV(&m_sSendBuffer);
		}
		else if(!m_sMsgQueue.IsEmpty())
		{
			BC_SAFE_DELETE_PTR(m_pCurrSendItem);
			m_pCurrSendItem = m_sMsgQueue.PopFront();
			ASSERT(m_pCurrSendItem != NULL);
			switch(m_pCurrSendItem->m_eType)
			{
			case HTTPM_NOTIFIER:
				{
					HTTPNotifier sNotifier = m_pCurrSendItem->m_sNotifier;
					// We MUST deallocate this packet before action,
					// for this action may change the pointer's value
					// and cause memory leak
					BC_SAFE_DELETE_PTR(m_pCurrSendItem);
					switch(sNotifier.m_eType)
					{
					case HTTPM_TASK:
						sNotifier.Notify();
						break;
					default:
						break;
					}
				}
				break;
			default:
				break;
			}
		}
		else
		{
			break;
		}
	}
}

void HTTPConnOut::_AppendPacket(BCBuffer &refBody)
{
	BOOL result;

	result = m_sMsgQueue.Append(refBody);
	if (!result)
	{
		_set_state(this, MSGSTATE_FREED, result);
	}
}

void HTTPConnOut::_AppendPacketEx(BCBuffer &refBody)
{
	BOOL result;

	result = m_sMsgQueue.AppendEx(refBody);
	if (!result)
	{
		_set_state(this, MSGSTATE_FREED, result);
	}
}

void HTTPConnOut::_AppendNotifier(HTTPNotifier *pNotifier)
{
	m_sMsgQueue.Append(pNotifier);
}

void HTTPConnOut::_ActiveCheck()
{
	// Check connection activity
	// TODO :
	_CLI_StopWork(0, BC_R_TIMEDOUT);
}

void HTTPConnOut::_Stop()
{
	if (m_bCtrlStop)
	{
		return;
	}

	/* response data had recviced */
	if(m_bCtrlConnet && m_bCtrlHeader && 
		((m_pRecvBuffer->RemainingLength() > 0 && m_eIoState == RECV_BODY) || m_eIoState == RECV_CHUNK))
	{
		if(m_eIoState == RECV_CHUNK)
		{
			m_sChunkParser.Parse();
			m_sChunkParser.Cleanup();
		}
		else if(m_pResponder)
		{
			BCBuffer *pBuffer = new BCBuffer();
			BCBOStream sWriter(pBuffer);
			m_pRecvBuffer->WriteTo(sWriter);
			m_pResponder->OnResponseBody(pBuffer, this);
			m_pRecvBuffer->Reset();
		}
	}
	/* Deactivate the client. unsubscribe active check timer */
	m_pConnMgr->_InActive(this);

	m_bCtrlStop = true;
}

void HTTPConnOut::_PacketSentCB(HTTPNotifier &refNotifier)
{
	HTTPConnOut *_this;

	_this = (HTTPConnOut *)refNotifier.m_wParam;
	ASSERT(_this != NULL);
	if (_this->m_pResponder)
	{
		_this->m_pResponder->OnPacketSent(_this);
	}
}

void HTTPConnOut::_PacketEventDtor(BCEventItemS &refEvent)
{
	if (refEvent.lParam)
	{
		BCBuffer *lpBuffer = (BCBuffer *)refEvent.lParam;
		delete lpBuffer;
		refEvent.lParam = 0;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Static functions
///////////////////////////////////////////////////////////////////////////////

static BCOnceS				g_sInitConnector = BC_ONCE_INIT;
static HTTPConnector	*	g_pConnctor = NULL;

static void init_connector(void*)
{
	if (g_pConnctor == NULL)
	{
		BCRESULT result;

		g_pConnctor = new HTTPConnector();
		ASSERT(g_pConnctor);
		result = g_pConnctor->Create(1, FALSE);
		ASSERT(result == BC_R_SUCCESS);
	}
}

HTTPConnOut *HTTPConnOut::Create(IHTTPResponder *pResponder)
{
	bc_once_do(&g_sInitConnector, init_connector, NULL);
	return g_pConnctor->CreateHTTPConn(pResponder);
}
///////////////////////////////////////////////////////////////////////////////
// class : HTTPConnOutMgr
///////////////////////////////////////////////////////////////////////////////

#define MGRSTATE_FREED						0
#define MGRSTATE_INACTIVE					1
#define MGRSTATE_WORKING					2
#define MGRSTATE_MAX						9

#define MGRM_STARTWORK						1
#define MGRM_STOPWORK						2
#define MGRM_RECYCLE						3

HTTPConnOutMgr::HTTPConnOutMgr()
	: m_pTaskMgr(NULL)
	, m_pTimerMgr(NULL)
	, m_pActChkTimer(NULL)
	, m_nInstCount(0)
	, m_sExitCond(&m_sExitLock)
	, m_nTimeout(60)
	, m_nMaxTimeout(3600)
	, m_pHandler(NULL)
	, m_nState(MSGSTATE_INACTIVE)
	, m_nNewState(MSGSTATE_MAX)
	, m_nStateLineNo(0)
	, m_nCloseStatus(BC_R_SUCCESS)
	, m_nLastFlashTime(0)
{
	//
}

HTTPConnOutMgr::~HTTPConnOutMgr()
{
	_Detach();
}

void HTTPConnOutMgr::_Detach()
{
	if (m_pActChkTimer != NULL)
	{
		m_pActChkTimer->Detach(&m_pActChkTimer);
	}
	BCEventQueue::Detach();
}

BCRESULT HTTPConnOutMgr::Create(
	BCTaskMgr *pTaskMgr,
	BCTimerMgr *pTimerMgr,
	IHTTPHandler *pHandler)
{
	BCRESULT result;

	ASSERT(pTaskMgr != NULL);
	ASSERT(pTimerMgr != NULL);

	m_pTaskMgr	= pTaskMgr;
	m_pTimerMgr = pTimerMgr;
	m_pHandler	= pHandler;
	m_bExisting = FALSE;

	result = BCEventQueue::Create(pTimerMgr, pTaskMgr, "HTTPConnOutMgr", this);
	if (result != BC_R_SUCCESS)
	{
		ASSERT(0);
		return result;
	}

	m_pActChkTimer = new BCTimer();
	if (m_pActChkTimer == NULL)
	{
		result = BC_R_NOMEMORY;
		goto detach_task;
	}
	result = m_pActChkTimer->Create(pTimerMgr, bc_timertype_inactive,
		0, 0, GetTask(), _ActiveCheckCB, this);
	if (result != BC_R_SUCCESS)
	{
		goto detach_actchktimer;
	}

	return result;

detach_actchktimer:
	m_pActChkTimer->Detach(&m_pActChkTimer);
detach_task:
	BCEventQueue::Detach();

	return result;
}

void HTTPConnOutMgr::Start()
{
	PostEvent(MAKEEVENT(MGRM_STARTWORK, 0, 0));
}

void HTTPConnOutMgr::Stop()
{
	PostEvent(MAKEEVENT(MGRM_STOPWORK, 0, 0));
	BCMutex::Owner lock(m_sExitLock);
	m_sExitCond.Wait();
}

void HTTPConnOutMgr::Destroy(HTTPConnOutMgr **ppMgr)
{
	HTTPConnOutMgr *_this;
	BCTask *pTask;

	ASSERT(ppMgr != NULL && *ppMgr != NULL);
	_this = *ppMgr;
	pTask = _this->GetTask();
	if (pTask != NULL)
	{
		_this->Stop();
	}
	delete _this;
	*ppMgr = NULL;
}

/*
 * Connection state change routine :
 *
 *                                        ----->delete
 *                                       /
 * start --> new --> active -> inactive -----> recursing
 *                      \                          /
 *                       ----------<---------------
 *
 * We must obay this routine to avoid memory leak & timer confusion.
 */

void HTTPConnOutMgr::_Active(HTTPConnOut *pConn)
{
	BCSpinMutex::Owner lock(m_sLock);
	// Active a conn from newly creation, or recursiving list, and m_lstInactive list
	pConn->RemoveFromList();
	m_lstActive.PushBack(pConn);
	_ResetTimer(true);
	_OnStateChange();
}

void HTTPConnOutMgr::_InActive(HTTPConnOut *pConn)
{
	BCSpinMutex::Owner lock(m_sLock);
	// InActive a conn from recursiving list, or m_lstActive list
	pConn->RemoveFromList();
	m_lstInactive.PushBack(pConn);
	pConn->m_eState = MSGSTATE_INACTIVE;
	//ResetTimer(false);
	_OnStateChange();
}

void HTTPConnOutMgr::_Aside(HTTPConnOut *pConn)
{
	BCSpinMutex::Owner lock(m_sLock);
	// InActive a conn from recursiving list, or m_lstActive list
	pConn->RemoveFromList();
	m_lstAside.PushBack(pConn);
	//ResetTimer(false);
	_OnStateChange();
}

void HTTPConnOutMgr::_Recycle(HTTPConnOut *pConn)
{
	BCSpinMutex::Owner lock(m_sLock);
	// Recursive a conn from newly creation, or m_lstActive list, and m_lstInactive list
	pConn->RemoveFromList();
	// Only to call destructor
	delete pConn;
	// Dec instance counter
	m_nInstCount--;
	_OnStateChange();
	PostEvent(MAKEEVENT(MGRM_RECYCLE, 0, 0));
}

HTTPConnOut *HTTPConnOutMgr::Allocate()
{
	HTTPConnOut *pConn = NULL;

	BCSpinMutex::Owner lock(m_sLock);
	pConn = new HTTPConnOut();
	m_nInstCount++;

	return pConn;
}

uint32_t HTTPConnOutMgr::GetActiveConn() const
{
	return m_lstActive.Count();
}

void HTTPConnOutMgr::SetConnTimeout(uint32_t nTimeout)
{
	BCSpinMutex::Owner lock(m_sLock);
	m_nTimeout = nTimeout;
}

void HTTPConnOutMgr::SetMaxConnTimeout(uint32_t nTimeout)
{
	BCSpinMutex::Owner lock(m_sLock);
	m_nMaxTimeout = nTimeout;
}

void HTTPConnOutMgr::SetWorkFolder(LPCSTR lpFolderPath)
{
	BCSpinMutex::Owner lock(m_sLock);
	m_strWorkFolder = lpFolderPath;
}

const BCPString &HTTPConnOutMgr::GetWorkFolder() const
{
	return m_strWorkFolder;
}

uint32_t HTTPConnOutMgr::GetEOFTimes() const
{
	return 5;
}

void HTTPConnOutMgr::_ResetTimer(bool bStart /*= false*/)
{
	uint32_t nActiveCount;

	nActiveCount = m_lstActive.Count();
	if (bStart && nActiveCount == 1)
	{
		BCIntervalS sInterval;
		bc_interval_set(&sInterval, 10, 0);
		m_pActChkTimer->Reset(bc_timertype_ticker, 0, &sInterval, FALSE);
	}
	else if (nActiveCount == 0)
	{
		m_pActChkTimer->Reset(bc_timertype_inactive, 0, 0, FALSE);
	}
}

void HTTPConnOutMgr::_ActiveCheck()
{
	HTTPConnOut *pConn, *pIterEnd;
	uint32_t nNow;

	if (m_nTimeout == 0)
	{
		return;
	}

	bc_stdtime_get(&nNow);

	pConn = m_lstActive.Begin();
	pIterEnd = m_lstActive.End();
	for (; pConn != pIterEnd; pConn = m_lstActive.Next(pConn))
	{
		if (nNow >= pConn->m_nLatestNetActionTime + m_nTimeout)
		{
			pConn->PostEvent(MAKEEVENT(HTTPTSK_CHECKACTIVE, 0, 0));
		}
		if (nNow >= pConn->m_nConnectTime + m_nMaxTimeout)
		{
			pConn->PostEvent(MAKEEVENT(HTTPTSK_STOPWORK, 0, 0));
		}
	}
}

void HTTPConnOutMgr::_ActiveCheckCB(BCTask *pTask, BCTaskEvent *pEvent)
{
	HTTPConnOutMgr *_this = (HTTPConnOutMgr *)pEvent->ev_arg;

	UNUSED(pTask);
	ASSERT(_this != NULL);

	BCSpinMutex::Owner lock(_this->m_sLock);

	if (_this->_ExitCheck())
	{
		return;
	}
	_this->_ActiveCheck();
	(void)_this->_ExitCheck();
	// Destroy event
	pEvent->Destroy();
}

bool HTTPConnOutMgr::OnEventProcess(BCEventItemS &refEvent)
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
		case MGRM_STARTWORK:
			m_nState = MGRSTATE_WORKING;
			break;
		case MGRM_STOPWORK:
			_set_state(this, MGRSTATE_FREED, BC_R_SUCCESS);
			break;
		case MGRM_RECYCLE:
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
		_set_state(this, MSGSTATE_FREED, BC_R_UNEXPECTED);
	}
#endif

	(void)_ExitCheck();
	return true;
}

void HTTPConnOutMgr::OnEventProcShutdown()
{
	BCMutex::Owner lock(m_sExitLock);
	m_sExitCond.Signal();
}

void HTTPConnOutMgr::_OnStateChange()
{
#define DUMP_HTTPMSG_STATE
#if (defined(HAVE_DUMP_STATE) && defined(DUMP_HTTPMSG_STATE))
	Dump();
#endif
	if (m_pHandler)
	{
		m_pHandler->OnMgrMsg(MGRM_CONNCOUNT_CHANGED,
			(void *)m_lstActive.Count(), NULL);
	}
	//Global::PostToMaster(Global::G_nMsgConnCountChanged, m_lstActive.Count(), NULL);
}

#ifdef HAVE_DUMP_STATE
void HTTPConnOutMgr::Dump() const
{
	LogDebug(_LOCAL_, "Total instance count : %4d ; Active count : %4d ;"
		"Inactive count : %4d ;Aside count : %4d.", m_nInstCount,
		m_lstActive.Count(), m_lstInactive.Count(), m_lstAside.Count());
}
#endif

BOOL HTTPConnOutMgr::_ExitCheck()
{
	if (m_nState <= m_nNewState)
		return (FALSE); /* Business as usual. */

	ASSERT(m_nNewState < MGRSTATE_WORKING);

	if (m_nState == MGRSTATE_WORKING)
	{
		ASSERT(m_nNewState <= MGRSTATE_INACTIVE);

		if (m_lstActive.Count() > 0 || m_lstAside.Count() > 0)
		{
			HTTPConnOut *pConn, *pIterEnd;

			pConn = m_lstActive.Begin();
			pIterEnd = m_lstActive.End();
			for (; pConn != pIterEnd; pConn = m_lstActive.Next(pConn))
			{
				pConn->PostEvent(MAKEEVENT(HTTPTSK_STOPWORK, 0, 0), (void *)true);
			}

			pConn = m_lstAside.Begin();
			pIterEnd = m_lstAside.End();
			for (; pConn != pIterEnd; pConn = m_lstAside.Next(pConn))
			{
				pConn->PostEvent(MAKEEVENT(HTTPTSK_STOPWORK, 0, 0), (void *)true);
			}

			return TRUE;
		}

		/*
		* I/O cancel is complete.  Burn down all state
		* related to the current request.  Ensure that
		* the client is on the active list and not the
		* recursing list.
		*/

		m_pActChkTimer->Reset(bc_timertype_inactive, 0, 0, TRUE);

		m_nState = MGRSTATE_INACTIVE;

		if (MGRSTATE_INACTIVE == m_nNewState)
		{
			return (TRUE); /* We're done. */
		}
	}

	if (m_nState == MGRSTATE_INACTIVE)
	{
		ASSERT(m_nNewState == MGRSTATE_FREED);
		/*
		* We are trying to free the acceptor.
		*
		* When "shuttingdown" is true, either the task has received
		* its shutdown event or no shutdown event has ever been
		* set up.  Thus, we have no outstanding shutdown
		* event at this point.
		*/
		ASSERT(m_nState == MGRSTATE_INACTIVE);

		_Detach();

		m_nState = MGRSTATE_FREED;
	}

	return TRUE;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPConnOut.cpp
///////////////////////////////////////////////////////////////////////////////
