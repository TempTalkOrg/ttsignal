
#include <BC/BCTask.h>
#include <BC/BCTimer.h>
#include <BC/BCSocket.h>
#include <BC/BCRuntime.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Class : BCRuntime
///////////////////////////////////////////////////////////////////////////////

BCRuntime::BCRuntime()
	: m_pUgTaskMgr(NULL)
	, m_pTaskMgr(NULL)
	, m_pTimerMgr(NULL)
	, m_nRTCFlags(0)
{
	//
}

BCRuntime::~BCRuntime()
{
	//
}

BCRESULT BCRuntime::Create(
	uint32_t nFlags,
	uint32_t nUgWorkers, 
	uint32_t nWorkers)
{
	BCRESULT result;
	
	m_nRTCFlags = nFlags;
	if (m_nRTCFlags == 0)
	{
		return BC_R_SUCCESS;
	}

	if (nFlags & BCRTC_UGTSKMGR)
	{
		// Create ugent task manager
		m_pUgTaskMgr = new BCTaskMgr();
		if (m_pUgTaskMgr == NULL)
		{
			result = BC_R_NOMEMORY;
			goto quit;
		}
		result = m_pUgTaskMgr->Create(nUgWorkers, 0, BCThread::PRIORITY_HIGH);
		if (result != BC_R_SUCCESS)
		{
			goto destroy_ugtaskmgr;
		}
	}
	if (nFlags & BCRTC_TSKMGR)
	{
		// Create task manager
		m_pTaskMgr = new BCTaskMgr();
		if (m_pTaskMgr == NULL)
		{
			result = BC_R_NOMEMORY;
			goto destroy_ugtaskmgr;
		}
		result = m_pTaskMgr->Create(nWorkers, 0);
		if (result != BC_R_SUCCESS)
		{
			goto destroy_taskmgr;
		}
	}
	if (nFlags & BCRTC_TIMERMGR)
	{
		// Create timer manager
		m_pTimerMgr = NULL;
		result = BCTimerMgr::Create(&m_pTimerMgr);
		if (result != BC_R_SUCCESS)
		{
			goto destroy_taskmgr;
		}
	}
	if (nFlags & BCRTC_SOCKMGR)
	{
		// Create socket manager
		m_pSockMgr = new BCSocketMgr();
		if (m_pSockMgr == NULL)
		{
			result = BC_R_NOMEMORY;
			goto destroy_timermgr;
		}
		result = m_pSockMgr->Create();
		if (result != BC_R_SUCCESS)
		{
			goto destroy_sockmgr;
		}
	}

	return BC_R_SUCCESS;

destroy_sockmgr:
	if (nFlags & BCRTC_SOCKMGR)
	{
		BCSocketMgr::Destroy(&m_pSockMgr);
	}
destroy_timermgr:
	if (nFlags & BCRTC_TIMERMGR)
	{
		BCTimerMgr::Destroy(&m_pTimerMgr);
	}
destroy_taskmgr:
	if (nFlags & BCRTC_TSKMGR)
	{
		BCTaskMgr::Destroy(&m_pTaskMgr);
	}
destroy_ugtaskmgr:
	if (nFlags & BCRTC_UGTSKMGR)
	{
		BCTaskMgr::Destroy(&m_pUgTaskMgr);
	}
quit:
	return result;
}

BCRESULT BCRuntime::Create(
	BCTaskMgr *pTaskMgr, 
	BCTaskMgr *pUgTaskMgr, 
	BCTimerMgr *pTimerMgr,
	BCSocketMgr *pSockMgr)
{
	ASSERT(pTaskMgr);
	ASSERT(pUgTaskMgr);
	ASSERT(pTimerMgr);
	ASSERT(pSockMgr);
	m_nRTCFlags		= 0;
	m_pTaskMgr		= pTaskMgr;
	m_pUgTaskMgr	= pUgTaskMgr;
	m_pTimerMgr		= pTimerMgr;
	m_pSockMgr		= pSockMgr;

	return BC_R_SUCCESS;
}

void BCRuntime::Destroy()
{
	if ((m_nRTCFlags & BCRTC_SOCKMGR) && m_pSockMgr)
	{
		BCSocketMgr::Destroy(&m_pSockMgr);
	}
	else
	{
		m_pSockMgr = NULL;
	}
	if ((m_nRTCFlags & BCRTC_TIMERMGR) && m_pTimerMgr)
	{
		BCTimerMgr::Destroy(&m_pTimerMgr);
	}
	else
	{
		m_pTimerMgr = NULL;
	}
	if ((m_nRTCFlags & BCRTC_TSKMGR) && m_pTaskMgr)
	{
		BCTaskMgr::Destroy(&m_pTaskMgr);
	}
	else
	{
		m_pTaskMgr = NULL;
	}
	if ((m_nRTCFlags & BCRTC_UGTSKMGR) && m_pUgTaskMgr)
	{
		BCTaskMgr::Destroy(&m_pUgTaskMgr);
	}
	else
	{
		m_pUgTaskMgr = NULL;
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

///////////////////////////////////////////////////////////////////////////////
// End of file : BCRuntime.cpp
///////////////////////////////////////////////////////////////////////////////
