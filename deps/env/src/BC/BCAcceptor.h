///////////////////////////////////////////////////////////////////////////////
// File : BCAcceptor.h
///////////////////////////////////////////////////////////////////////////////
  
#ifndef BCACCEPTOR_INCLUDED__
#define BCACCEPTOR_INCLUDED__

#include <BC/Exports.h>
#include <BC/BCTask.h>
#include <BC/BCSocket.h>
#include <BC/BCTimer.h>
#include <BC/EventQueue.h>
#include <BC/BCOptions.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

/*
 * return : TRUE  - Continue accept new connection
 *          FALSE - Stop accept new connection
 */
typedef BOOL (*LPFN_BCAcceptActionPtr)(BCSocket *pSocket, void *pArg);
typedef LPFN_BCAcceptActionPtr	LPFN_BCAcceptAction;

///////////////////////////////////////////////////////////////////////////////
// Options : 
///////////////////////////////////////////////////////////////////////////////

BC_OPTION_DEFINE(OPT_BCACCEPTOR_HOST);
BC_OPTION_DEFINE(OPT_BCACCEPTOR_PORT);
BC_OPTION_DEFINE(OPT_BCACCEPTOR_ENABLEIPV6);
BC_OPTION_DEFINE(OPT_BCACCEPTOR_ACCEPTINTERVAL);
BC_OPTION_DEFINE(OPT_BCACCEPTOR_REUSEADDRESS);
BC_OPTION_DEFINE(OPT_BCACCEPTOR_BACKLOG);

///////////////////////////////////////////////////////////////////////////////
// class : BCAcceptor
///////////////////////////////////////////////////////////////////////////////

class BC_API BCAcceptor 
	: public BCEventFactory
	, public BCOptionListener
{
	DECLARE_FIXED_ALLOC(BCAcceptor);
public:
	BCAcceptor();
	virtual ~BCAcceptor();

	BCRESULT			Create(
							BCOptions *pOptions,
							BCTaskMgr *pTaskMgr,
							BCSocketMgr *pSockMgr,
							BCTimerMgr *pTimerMgr,
							LPFN_BCAcceptAction pAction,
							void *pArg);
	void				Start();
	void				Pause(BOOL bPause);
	void				Stop();
	static void			Destroy(BCAcceptor **ppAcceptor);
protected:
	inline void		_SetState(uint32_t eState, uint32_t nLineNumber)
	{
		m_nNewState		= eState;
		m_nStateLineNo	= nLineNumber;
	}
	BCRESULT			_Start(BOOL bInit);
	void				_Pause(BOOL bPause);
	BCRESULT			_Accept(uint32_t nMsDelay = 0);
	void				_Detach();
	void				_OnAcceptDone(BCSocket **ppNewConn);
	void				_OnTimeout();
	bool				OnEventProcess(BCEventItemS &refEvent);
	void				OnEventProcShutdown();
	BOOL				_ExitCheck();
	BCRESULT			_ScheduleTimer(uint32_t nMsDelay, BCTimerTypeE eType);
	void				_ActiveCheck();
	void				_ChangePort(uint16_t nPort);
protected:
	static void		_AcceptDoneCallback(BCTask *, BCTaskEvent *);
	static void		_TimeoutCallback(BCTask *, BCTaskEvent *);
	static void		_ActiveChkCallback(BCTask *, BCTaskEvent *);
	// Override option change event handler
	void			OnOptionChanged(const BCPString &strKey, BCFVar *pValue);
private:
	DECLARE_NO_COPY_CLASS(BCAcceptor);
	BCOptions			*	m_pOptions;
	BCSocket			*	m_pSocket;
	BCTimer				*	m_pTimer;
	BCTimer				*	m_pActiveChkTimer;
	LPFN_BCAcceptAction		m_pAction;
	void				*	m_pCBArg;
	uint32_t				m_nPendingAccept;
	uint32_t				m_nAcceptInterval;
	BOOL					m_bReuseAddress;
	uint32_t				m_nBacklog;
	BOOL					m_bPaused;
	BOOL					m_bTimerSet;
	uint32_t				m_nState;
	uint32_t				m_nNewState;
	uint32_t				m_nStateLineNo;
	BOOL					m_bUseIPv6;
	char					m_szHost[MAX_PATH];
	BCSockAddrS				m_sSockAddr;
	uint16_t				m_nPort;
	BCMutex					m_sExitLock;
	BCCondition				m_sExitCond;
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file : BCAcceptor.h
///////////////////////////////////////////////////////////////////////////////
