
#include "BC/BCLog.h"
#include <BC/BCAcceptor.h>




///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

#define ACCEPTSTATE_FREED			0
#define ACCEPTSTATE_INACTIVE		1
#define ACCEPTSTATE_ACCEPTING		2
#define ACCEPTSTATE_MAX				3
#define DEFAULT_ACCEPT_DELAY		50

enum
{
	BCM_STARTWORK				= 1,
	BCM_STOPWORK				= 2,
	BCM_PAUSEWORK				= 3,
	BCM_OPTCHANGED				= 4,
	// Number of events
	BCM_NUMBER					= 4,
};

///////////////////////////////////////////////////////////////////////////////
// Options : 
///////////////////////////////////////////////////////////////////////////////

BC_OPTION_IMPLEMENT(OPT_BCACCEPTOR_HOST);
BC_OPTION_IMPLEMENT(OPT_BCACCEPTOR_PORT);
BC_OPTION_IMPLEMENT(OPT_BCACCEPTOR_ENABLEIPV6);
BC_OPTION_IMPLEMENT(OPT_BCACCEPTOR_ACCEPTINTERVAL);
BC_OPTION_IMPLEMENT(OPT_BCACCEPTOR_REUSEADDRESS);
BC_OPTION_IMPLEMENT(OPT_BCACCEPTOR_BACKLOG);

///////////////////////////////////////////////////////////////////////////////
// class : BCAcceptor
///////////////////////////////////////////////////////////////////////////////

#define _set_state(conn, _state) (conn)->_SetState(_state, __LINE__)

IMPLEMENT_FIXED_ALLOC(BCAcceptor, 2);

BCAcceptor::BCAcceptor()
	: m_pOptions(NULL)
	, m_pSocket(NULL)
	, m_pTimer(NULL)
	, m_pActiveChkTimer(NULL)
	, m_pAction(NULL)
	, m_pCBArg(NULL)
	, m_nPendingAccept(0)
	, m_nAcceptInterval(DEFAULT_ACCEPT_DELAY)
	, m_bReuseAddress(TRUE)
	, m_nBacklog(0)
	, m_bPaused(FALSE)
	, m_bTimerSet(FALSE)
	, m_nState(ACCEPTSTATE_INACTIVE)
	, m_nNewState(ACCEPTSTATE_MAX)
	, m_nStateLineNo(0)
	, m_bUseIPv6(FALSE)
	, m_nPort(0)
	, m_sExitCond(&m_sExitLock)
{
	memzero(m_szHost, sizeof(m_szHost));
}

BCAcceptor::~BCAcceptor()
{
	//
}

BCRESULT BCAcceptor::Create(
	BCOptions *pOptions,
	BCTaskMgr *pTaskMgr,
	BCSocketMgr *pSockMgr,
	BCTimerMgr *pTimerMgr,
	LPFN_BCAcceptAction pAction,
	void *pArg)
{
	BCRESULT result;
	BCTask *pTask;
	BCFVar *pValue;
	BCPString strHost;

	ASSERT(pOptions);
	ASSERT(pSockMgr);
	ASSERT(pTaskMgr);
	ASSERT(pTimerMgr);

	m_pOptions = pOptions;
	m_pAction = pAction;
	m_pCBArg = pArg;

	// Get port to listen at
	BC_GET_INT_OPTION(OPT_BCACCEPTOR_PORT, m_nPort);
	// Get if enable ipv6
	BC_GET_BOOL_OPTION(OPT_BCACCEPTOR_ENABLEIPV6, m_bUseIPv6);
	// Get delay accept value
	BC_GET_INT_OPTION(OPT_BCACCEPTOR_ACCEPTINTERVAL, m_nAcceptInterval);
	// Get if reuse address
	BC_GET_BOOL_OPTION(OPT_BCACCEPTOR_REUSEADDRESS, m_bReuseAddress);
	// Get backlog value
	BC_GET_INT_OPTION(OPT_BCACCEPTOR_BACKLOG, m_nBacklog);
	// Get host to bind
	BC_GET_STRING_OPTION(OPT_BCACCEPTOR_HOST, strHost);
	memzero(m_szHost, sizeof(m_szHost));
	strncpy(m_szHost, strHost, sizeof(m_szHost) - 1);
	if (m_bUseIPv6)
	{
		struct in6_addr in6a;

		if (strlen(m_szHost) == 0)
		{
			in6a = in6addr_any;
		}
		else if (bc_net_pton(PF_INET6, m_szHost, &in6a) <= 0)
		{
			return BC_R_FAMILYMISMATCH;
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
			return BC_R_FAMILYMISMATCH;
		}
		bc_sockaddr_fromin(&m_sSockAddr, &ina, m_nPort);
	}

	// Add option change event listeners
	m_pOptions->AddListener(OPT_BCACCEPTOR_HOST, this);
	m_pOptions->AddListener(OPT_BCACCEPTOR_PORT, this);
	m_pOptions->AddListener(OPT_BCACCEPTOR_ENABLEIPV6, this);
	m_pOptions->AddListener(OPT_BCACCEPTOR_ACCEPTINTERVAL, this);
	m_pOptions->AddListener(OPT_BCACCEPTOR_REUSEADDRESS, this);
	m_pOptions->AddListener(OPT_BCACCEPTOR_BACKLOG, this);

	// Create task
	result = BCEventFactory::Create(pTaskMgr, "BCAcceptor", this);
	if (result != BC_R_SUCCESS)
	{
		ASSERT(0);
		return result;
	}
	pTask = BCEventFactory::GetTask();

	// Create socket
	m_pSocket = new BCSocket();
	if (m_pSocket == NULL)
	{
		result = BC_R_NOMEMORY;
		goto detach_task;
	}
	result = m_pSocket->Create(pSockMgr, m_bUseIPv6?PF_INET6:PF_INET,
		bc_sockettype_tcp);
	if (result != BC_R_SUCCESS)
	{
		goto detach_socket;
	}

	// Create timer
	m_pTimer = new BCTimer();
	if (m_pTimer == NULL)
	{
		goto detach_socket;
	}
	result = m_pTimer->Create(pTimerMgr, bc_timertype_inactive,
		NULL, NULL, pTask, _TimeoutCallback, this);
	if (result != BC_R_SUCCESS)
	{
		goto detach_timer;
	}

	// Create active check timer
	m_pActiveChkTimer = new BCTimer();
	if (m_pActiveChkTimer == NULL)
	{
		goto detach_timer;
	}
	result = m_pActiveChkTimer->Create(pTimerMgr, bc_timertype_inactive,
		NULL, NULL, pTask, _ActiveChkCallback, this);
	if (result != BC_R_SUCCESS)
	{
		goto detach_achk_timer;
	}

	return BC_R_SUCCESS;

detach_achk_timer:
	m_pActiveChkTimer->Detach(&m_pActiveChkTimer);
detach_timer:
	m_pTimer->Detach(&m_pTimer);
detach_socket:
	m_pSocket->Detach(&m_pSocket);
detach_task:
	BCEventFactory::Detach();

	return result;
}

void BCAcceptor::Start()
{
	BCEventFactory::PostEvent(MAKEEVENT(BCM_STARTWORK, 0, 0));
}

void BCAcceptor::Pause(BOOL bPause)
{
	BCEventFactory::PostEvent(MAKEEVENT(BCM_PAUSEWORK, bPause, 0));
}

void BCAcceptor::Stop()
{
	BCEventFactory::PostEvent(MAKEEVENT(BCM_STOPWORK, 0, 0));
	BCMutex::Owner lock(m_sExitLock);
	m_sExitCond.Wait();
}

void BCAcceptor::Destroy(BCAcceptor **ppAcceptor)
{
	BCAcceptor *pAcceptor;
	BCTask *pTask;

	ASSERT(ppAcceptor != NULL && *ppAcceptor != NULL);
	pAcceptor = *ppAcceptor;
	pTask = pAcceptor->GetTask();
	if (pTask != NULL)
	{
		pAcceptor->Stop();
	}
	delete pAcceptor;
	*ppAcceptor = NULL;
}

BCRESULT BCAcceptor::_Start(BOOL bInit)
{
	BCRESULT result;
	BCIntervalS sInterval;

	if (bInit)
	{
		uint32_t options = m_bReuseAddress?BC_SOCKET_REUSEADDRESS:0;
		result = m_pSocket->Bind(&m_sSockAddr, options);
		if (result != BC_R_SUCCESS)
		{
			return result;
		}

		result = m_pSocket->Listen(m_nBacklog);
		if (result != BC_R_SUCCESS)
		{
			return result;
		}

		bc_interval_set(&sInterval, 1, 0);
		result = m_pActiveChkTimer->Reset(bc_timertype_ticker,
			0, &sInterval, FALSE);
		if (result != BC_R_SUCCESS)
		{
			return result;
		}
	}

	/*
	* Queue up the first accept pEvent.
	*/
	m_nState = ACCEPTSTATE_ACCEPTING;
	return _Accept();
}

BCRESULT BCAcceptor::_Accept(uint32_t nMsDelay /*= 0*/)
{
	BCRESULT result;

	if (m_bPaused)
	{
		return BC_R_SUCCESS;
	}

	if (nMsDelay == 0)
	{
		/*
		* Queue up accept pEvent.
		*/
		result = m_pSocket->Accept(BCEventFactory::GetTask(),
			_AcceptDoneCallback, this);
		if (result == BC_R_SUCCESS)
		{
			ASSERT(m_nPendingAccept == 0);
			m_nPendingAccept++;
		}
	}
	else
	{
		result = _ScheduleTimer(nMsDelay, bc_timertype_once);
		if (result == BC_R_SUCCESS)
		{
			m_bTimerSet = TRUE;
		}
	}
	return result;
}

void BCAcceptor::_Pause(BOOL bPause)
{
	if (m_bPaused == bPause)
	{
		return;
	}
	if (bPause)	// Pause acceptor
	{
		if (m_nPendingAccept > 0)
		{
			m_pSocket->Cancel(BCEventFactory::GetTask(),
				BC_SOCKCANCEL_ACCEPT);
		}

		_set_state(this, ACCEPTSTATE_INACTIVE);
	}
	else	// Resume acceptor
	{
		_set_state(this, ACCEPTSTATE_MAX);
		m_nState = ACCEPTSTATE_ACCEPTING;
		_Accept(0);
	}
	m_bPaused = bPause;
}

void BCAcceptor::_Detach()
{
	if (m_pTimer != NULL)
	{
		m_pTimer->Detach(&m_pTimer);
	}
	if (m_pActiveChkTimer != NULL)
	{
		m_pActiveChkTimer->Detach(&m_pActiveChkTimer);
	}
	if (m_pSocket != NULL)
	{
		m_pSocket->Detach(&m_pSocket);
	}
	BCEventFactory::Detach();
}

void BCAcceptor::_AcceptDoneCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCSockICEvent *pSockEv = (BCSockICEvent *)pEvent;
	BCAcceptor *_this = (BCAcceptor *)pEvent->ev_arg;

	UNUSED(pTask);

	ASSERT(_this != NULL);

	ASSERT(_this->m_nPendingAccept == 1);
	_this->m_nPendingAccept--;
	if (pSockEv->result != BC_R_SUCCESS)
	{
		if ((pSockEv->result == BC_R_CANCELED && _this->m_bPaused) || 
			pSockEv->result == BC_R_NETUNREACH)
		{
			goto check_exit;
		}
		else
		{
			//_set_state(_this, ACCEPTSTATE_FREED);
			LogError(_LOCAL_, "BCAcceptor got[%" _U32BITARG_"] result, "
				"continue to accept new connection.", pSockEv->result);
		}
	}

	if (_this->_ExitCheck())
	{
		goto free_event;
	}

	_this->_OnAcceptDone(&pSockEv->newsocket);

check_exit:
	(void)_this->_ExitCheck();

free_event:
	pEvent->Destroy();
}

void BCAcceptor::_OnAcceptDone(BCSocket **ppNewConn)
{
	BOOL bContinue = FALSE;

	if (*ppNewConn)
	{
		if (m_pAction != NULL)
		{
#if 0
			bContinue = (m_pAction)(*ppNewConn, m_pCBArg);
#else // TEST : Always accept
			(m_pAction)(*ppNewConn, m_pCBArg);
			bContinue = TRUE;
#endif // 
		}
		(*ppNewConn)->Detach(ppNewConn);
	}
	else
	{
		bContinue = TRUE;
	}

	/*
	* Queue up another accept event.
	*/
	if (bContinue && m_nState == ACCEPTSTATE_ACCEPTING)
	{
		_Accept(m_nAcceptInterval);
	}
	else
	{
		m_nState = ACCEPTSTATE_INACTIVE;
	}
}

void BCAcceptor::_TimeoutCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCAcceptor *_this = (BCAcceptor *)pEvent->ev_arg;

	UNUSED(pTask);

	ASSERT(_this != NULL);

	if (_this->_ExitCheck())
	{
		goto free_event;
	}

	_this->_OnTimeout();

	(void)_this->_ExitCheck();
free_event:
	pEvent->Destroy();
}

void BCAcceptor::_OnTimeout()
{
	m_bTimerSet = FALSE;
	if (m_nState == ACCEPTSTATE_ACCEPTING)
	{
		_Accept();
	}
}

bool BCAcceptor::OnEventProcess(BCEventItemS &refEvent)
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
		case BCM_STARTWORK:
			_Start(TRUE);
			break;
		case BCM_STOPWORK:
			_set_state(this, ACCEPTSTATE_FREED);
			break;
		case BCM_PAUSEWORK:
			_Pause(refEvent.wParam != 0);
			break;
		case BCM_OPTCHANGED:
			if (refEvent.wParam && refEvent.lParam)
			{
				BCPString strKey((LPCSTR)refEvent.wParam);
				ScopedPointer<BCFVar> pValue((BCFVar *)refEvent.lParam);

				if (strKey == OPT_BCACCEPTOR_PORT)
				{
					if (IS_BCF_INT(pValue.Get()))
					{
						_ChangePort(GET_BCF_INT(pValue.Get()));
					}
				} 
				else if (strKey == OPT_BCACCEPTOR_BACKLOG)
				{
					if (IS_BCF_INT(pValue.Get()))
					{
						m_nBacklog = GET_BCF_INT(pValue.Get());
					}
				}
				else if (strKey == OPT_BCACCEPTOR_ACCEPTINTERVAL)
				{
					if (IS_BCF_INT(pValue.Get()))
					{
						m_nAcceptInterval = GET_BCF_INT(pValue.Get());
					}
				}
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
		_set_state(this, ACCEPTSTATE_FREED);
	}
#endif

	(void)_ExitCheck();
	return true;
}

void BCAcceptor::OnEventProcShutdown()
{
	// Do some clean up work, as well as release some memory resources
	BCMutex::Owner lock(m_sExitLock);
	m_sExitCond.Signal();
}

BOOL BCAcceptor::_ExitCheck()
{
	if (m_nState <= m_nNewState)
		return (FALSE); /* Business as usual. */

	ASSERT(m_nNewState < ACCEPTSTATE_ACCEPTING);

	if (m_nState == ACCEPTSTATE_ACCEPTING)
	{
		ASSERT(m_nNewState <= ACCEPTSTATE_INACTIVE);

		/*
		* We are trying to abort accept processing.
		*/
		if (m_nPendingAccept > 0)
		{
			m_pSocket->Cancel(BCEventFactory::GetTask(),
				BC_SOCKCANCEL_ACCEPT);
		}

		if (m_nPendingAccept != 0)
		{
			/*
			* Still waiting for I/O cancel completion.
			* or lingering references.
			*/
			return (TRUE);
		}
		/*
		* Stop timers
		*/
		// Stop delay accept timer
		m_pTimer->Reset(bc_timertype_inactive, 0, 0, TRUE);
		m_bTimerSet = FALSE;
		// Stop active check timer
		m_pActiveChkTimer->Reset(bc_timertype_inactive, 0, 0, TRUE);

		m_nState = ACCEPTSTATE_INACTIVE;

		if (ACCEPTSTATE_INACTIVE == m_nNewState)
		{
			return (TRUE); /* We're done. */
		}
	}

	if (m_nState == ACCEPTSTATE_INACTIVE)
	{
		ASSERT(m_nNewState == ACCEPTSTATE_FREED);
		/*
		* We are trying to free the acceptor.
		*
		* When "shuttingdown" is true, either the task has received
		* its shutdown event or no shutdown event has ever been
		* set up.  Thus, we have no outstanding shutdown
		* event at this point.
		*/
		ASSERT(m_nState == ACCEPTSTATE_INACTIVE);

		_Detach();
	}

	return TRUE;
}

BCRESULT BCAcceptor::_ScheduleTimer(uint32_t nMsDelay, BCTimerTypeE eType)
{
	BCIntervalS sInterval = {0};

	bc_interval_set(&sInterval, nMsDelay/1000, (nMsDelay%1000)*1000000);
	return m_pTimer->Reset(eType, 0, &sInterval, TRUE);
}

void BCAcceptor::_ActiveCheck()
{
	if (!m_bTimerSet
		&& m_nPendingAccept == 0
		&& m_nState == ACCEPTSTATE_ACCEPTING)
	{
		_Accept(m_nAcceptInterval);
	}
}

void BCAcceptor::_ChangePort(uint16_t nPort)
{
	UNUSED(nPort);
	//BCRESULT result;

	//bc_sockaddr_setport(&m_sSockAddr, nPort);
	//uint32_t options = m_bReuseAddress?BC_SOCKET_REUSEADDRESS:0;
	//result = m_pSocket->Bind(&m_sSockAddr, options);
	//if (BC_R_SUCCESS != result)
	//{
	//	LogError(_LOCAL_, "Failed to set listen port to [%"_U32BITARG_"]", nPort);
	//	bc_sockaddr_setport(&m_sSockAddr, m_nPort);
	//	result = m_pSocket->Bind(&m_sSockAddr, options);
	//	if (BC_R_SUCCESS != result)
	//	{
	//		LogFatal(_LOCAL_, "Failed to restore listen port back to [%"_U32BITARG_"]", m_nPort);
	//		return;
	//	}
	//}
	//else
	//{
	//	m_nPort = nPort;
	//}

	//result = m_pSocket->Listen(m_nBacklog);
	//if (result != BC_R_SUCCESS)
	//{
	//	LogFatal(_LOCAL_, "Failed to listen at port[%"_U32BITARG_"]", m_nPort);
	//}
}

void BCAcceptor::_ActiveChkCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCAcceptor *_this = (BCAcceptor *)pEvent->ev_arg;

	UNUSED(pTask);

	ASSERT(_this != NULL);

	if (_this->_ExitCheck())
	{
		goto free_event;
	}

	_this->_ActiveCheck();

	(void)_this->_ExitCheck();
free_event:
	pEvent->Destroy();
}

void BCAcceptor::OnOptionChanged(const BCPString &strKey, BCFVar *pValue)
{
	BCEventItemS sEvent(MAKEEVENT(BCM_OPTCHANGED, 0, 0));
	sEvent.wParam = (uint64_t)sEvent.CopyString(strKey);
	sEvent.lParam = (uint64_t)pValue;
	PostEvent(sEvent);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
