///////////////////////////////////////////////////////////////////////////////
// file : HTTPConnector.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include "HTTPConnOut.h"
#include "HTTPConnector.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

///////////////////////////////////////////////////////////////////////////////
// class : HTTPConnector
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(HTTPConnector, 2);

HTTPConnector::HTTPConnector()
	: m_bDeleteRes(FALSE)
	, m_pTaskMgr(NULL)
	, m_pTimerMgr(NULL)
	, m_pSockMgr(NULL)
	, m_pConnMgr(NULL)
	, m_bIPv6(FALSE)
{
	//
}

HTTPConnector::~HTTPConnector()
{
	//
}

BCRESULT HTTPConnector::Create(uint32_t nWorkers, BOOL bIPv6 /*= FALSE*/)
{
	BCRESULT result;

	// Create task manager
	m_pTaskMgr = new BCTaskMgr();
	if (m_pTaskMgr == NULL)
	{
		result = BC_R_NOMEMORY;
		goto quit;
	}
	result = m_pTaskMgr->Create(nWorkers, 0);
	if (result != BC_R_SUCCESS)
	{
		goto destroy_taskmgr;
	}

	// Create timer manager
	m_pTimerMgr = NULL;
	result = BCTimerMgr::Create(&m_pTimerMgr);
	if (result != BC_R_SUCCESS)
	{
		goto destroy_taskmgr;
	}

	// Create socket manager
	m_pSockMgr = new BCSocketMgr();
	if (m_pSockMgr == NULL)
	{
		result = BC_R_NOMEMORY;
		goto destroy_timermgr;
	}
	result = m_pSockMgr->Create((uint32_t)0);
	if (result != BC_R_SUCCESS)
	{
		goto destroy_sockmgr;
	}

	// Create NetConnMgr
	m_pConnMgr = new HTTPConnOutMgr();
	if (m_pConnMgr == NULL)
	{
		result = BC_R_NOMEMORY;
		goto destroy_sockmgr;
	}
	result = m_pConnMgr->Create(m_pTaskMgr, m_pTimerMgr, this);
	if (result != BC_R_SUCCESS)
	{
		goto destroy_connmgr;
	}

	m_bIPv6 = bIPv6;
	m_bDeleteRes = TRUE;

	m_pConnMgr->Start();

	return BC_R_SUCCESS;

destroy_connmgr:
	HTTPConnOutMgr::Destroy(&m_pConnMgr);
destroy_sockmgr:
	BCSocketMgr::Destroy(&m_pSockMgr);
destroy_timermgr:
	BCTimerMgr::Destroy(&m_pTimerMgr);
destroy_taskmgr:
	BCTaskMgr::Destroy(&m_pTaskMgr);
quit:
	return result;
}

BCRESULT HTTPConnector::Create(
	BCTaskMgr *pTaskMgr,
	BCTimerMgr *pTimerMgr,
	BCSocketMgr *pSockMgr,
	BOOL bIPv6 /* = FALSE */)
{
	BCRESULT result;

	ASSERT(pTaskMgr != NULL);
	ASSERT(pTimerMgr != NULL);
	ASSERT(pSockMgr != NULL);

	m_pTaskMgr = pTaskMgr;
	m_pTimerMgr = pTimerMgr;
	m_pSockMgr = pSockMgr;

	// Create NetConnMgr
	m_pConnMgr = new HTTPConnOutMgr();
	if (m_pConnMgr == NULL)
	{
		result = BC_R_NOMEMORY;
		goto quit;
	}
	result = m_pConnMgr->Create(m_pTaskMgr, m_pTimerMgr, this);
	if (result != BC_R_SUCCESS)
	{
		goto destroy_connmgr;
	}

	m_bIPv6 = bIPv6;
	m_bDeleteRes = FALSE;

	m_pConnMgr->Start();

	return BC_R_SUCCESS;

destroy_connmgr:
	HTTPConnOutMgr::Destroy(&m_pConnMgr);
quit:
	return result;
}

void HTTPConnector::Destroy()
{
	if (m_pConnMgr)
	{
		HTTPConnOutMgr::Destroy(&m_pConnMgr);
	}
	if (m_bDeleteRes)
	{
		if (m_pSockMgr)
		{
			BCSocketMgr::Destroy(&m_pSockMgr);
		}
		if (m_pTimerMgr)
		{
			BCTimerMgr::Destroy(&m_pTimerMgr);
		}
		if (m_pTaskMgr)
		{
			BCTaskMgr::Destroy(&m_pTaskMgr);
		}
	}
}

void HTTPConnector::SetConnTimeout(uint32_t nTimeout)
{
	BCSpinMutex::Owner lock(m_sLock);
	ASSERT(m_pConnMgr);
	m_pConnMgr->SetConnTimeout(nTimeout);
}

void HTTPConnector::SetMaxConnTimeout(uint32_t nTimeout)
{
	BCSpinMutex::Owner lock(m_sLock);
	ASSERT(m_pConnMgr);
	m_pConnMgr->SetMaxConnTimeout(nTimeout);
}

HTTPConnOut *HTTPConnector::CreateHTTPConn(IHTTPResponder *pResponder)
{
	BCRESULT result;
	HTTPConnOut *pConn;

	BCSpinMutex::Owner lock(m_sLock);
	// Create new CDNConn instance
	pConn = m_pConnMgr->Allocate();
	if (pConn == NULL)
	{
		goto return_null;
	}
	result = pConn->Create(m_pTimerMgr, m_pTaskMgr, m_pConnMgr, 
		m_pSockMgr, pResponder);
	if (result != BC_R_SUCCESS)
	{
		goto destroy_conn;
	}
	pConn->Start();

	return pConn;

destroy_conn:
	BC_SAFE_DELETE_PTR(pConn);
return_null:

	return NULL;
}

BCRESULT HTTPConnector::OnMgrMsg(uint32_t nMsg, void *wParam, void *lParam)
{
	UNUSED(nMsg);
	UNUSED(wParam);
	UNUSED(lParam);

	return BC_R_SUCCESS;
}

BCRESULT HTTPConnector::OnHTTPMsg(uint32_t nMsg, void *wParam, void *lParam)
{
	UNUSED(nMsg);
	UNUSED(wParam);
	UNUSED(lParam);

	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPConnector.cpp
///////////////////////////////////////////////////////////////////////////////
