///////////////////////////////////////////////////////////////////////////////
// file : HTTPClient.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include "HTTPConnOut.h"
#include "HTTPClient.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{


///////////////////////////////////////////////////////////////////////////////
// class : HTTPClient
///////////////////////////////////////////////////////////////////////////////

#define IRDCSTATE_IDLE			0
#define IRDCSTATE_WORKING		1

HTTPClient::HTTPClient()
	: m_pOwner(NULL)
	, m_pConn(NULL)
	, m_nState(IRDCSTATE_IDLE)
	, m_nQueryCount(0)
	, m_nSvrPort(0)
	//, m_bShutdown(FALSE)
{
	memzero(m_szSvrIP, sizeof(m_szSvrIP));
}

HTTPClient::~HTTPClient()
{
	//
}

BCRESULT HTTPClient::Create(
	BCEventDispatcher *pOwner,
	LPCSTR szUrl,
	uint16_t nPort)
{
	BCRESULT result;

	BCSpinMutex::Owner lock(m_sLock);

	ASSERT(pOwner != NULL);
	ASSERT(m_nState == IRDCSTATE_IDLE);

	m_pOwner = pOwner;
	strncpy(m_szSvrIP, szUrl, BCMIN(strlen(szUrl), sizeof(m_szSvrIP) - 1));
	m_nSvrPort = nPort;

	result = BC_R_SUCCESS;

	return result;
}

void HTTPClient::SetServerInfo(
	LPCSTR szUrl,
	uint16_t nPort)
{
	BCSpinMutex::Owner lock(m_sLock);

	ASSERT(m_nState == IRDCSTATE_IDLE);

	strncpy(m_szSvrIP, szUrl, BCMIN(strlen(szUrl), sizeof(m_szSvrIP) - 1));
	m_nSvrPort = nPort;
}

BCRESULT HTTPClient::Connect()
{
	BCSpinMutex::Owner lock(m_sLock);

	//if (m_bShutdown)
	//{
	//	return BC_R_FAILURE;
	//}

	ASSERT(m_pConn == NULL);
	m_pConn = HTTPConnOut::Create(this);
	if (m_pConn == NULL)
	{
		return BC_R_FAILURE;
	}

	return m_pConn->Connect(m_szSvrIP, m_nSvrPort);
}

BCRESULT HTTPClient::SendTask()
{
	BCSpinMutex::Owner lock(m_sLock);

	if (m_nState != IRDCSTATE_WORKING || m_pConn == NULL)
	{
		return BC_R_FAILURE;
	}
	if (m_nQueryCount == 0)
	{
		uint32_t nTimeNow;

		bc_stdtime_get(&nTimeNow);
		m_nQueryCount++;
	}

	return BC_R_SUCCESS;
}

void HTTPClient::Disconnect()
{
	BCSpinMutex::Owner lock(m_sLock);

	if (m_pConn)
	{
		m_pConn->Disconnect(0);
	}
}

void HTTPClient::Destroy()
{
	//
}

BOOL HTTPClient::IsConnected()
{
	BOOL bConnected;

	m_sLock.Lock();
	bConnected = m_pConn != NULL;
	m_sLock.Unlock();

	return bConnected;
}

BCRESULT HTTPClient::OnHTTPMessage(uint32_t nMsg, void *wParam, void *lParam)
{
	UNUSED(nMsg);
	UNUSED(wParam);
	UNUSED(lParam);

	return BC_R_SUCCESS;
}

BCRESULT HTTPClient::OnHTTPConnect(BCRESULT result, void *lParam)
{
	BCSpinMutex::Owner lock(m_sLock);

	return result;
}

void HTTPClient::OnRequestReady(HTTPReq &refReq, void *lParam)
{

	BCSpinMutex::Owner lock(m_sLock);

	UNUSED(lParam);

	// test
	if (refReq.CreateRequest(BC_HTTPD_METHODGET, "vod.html") != BC_R_SUCCESS)
	{
		return;
	}
	refReq.BeginHeader();
	refReq.AddParam("output", "720x576");
	refReq.AddParamUInt("ratio", 1);
	refReq.EndParams();
	refReq.AddHeader("Accept: */*", NULL);
	refReq.AddHeader("Accept-Language: zh-cn", NULL);
	refReq.AddHeader("User-Agent: BCTWorker/1.0", NULL);
	refReq.AddHeader("Host", m_szSvrIP);
	refReq.AddHeader("Connection: Keep-Alive", NULL);
	refReq.EndHeaders();  /* done */
}

void HTTPClient::OnResponseHeader(HTTPReq &refResp, void *lParam)
{
	HTTPConnOut *pConn = (HTTPConnOut *)lParam;
	BCSpinMutex::Owner lock(m_sLock);

	UNUSED(refResp);

	// TODO :
	if (pConn)
	{
		pConn->Disconnect(0);
	}
}

void HTTPClient::OnResponseBody(BCBuffer *pBuffer, void *lParam)
{
	BC_SAFE_DELETE_PTR(pBuffer);
	UNUSED(lParam);
}

void HTTPClient::OnPacketSent(void *lpParam)
{
	UNUSED(lpParam);
}

void HTTPClient::OnHTTPStop(void *lpParam, uint32_t nExitCode)
{
	BCSpinMutex::Owner lock(m_sLock);

	UNUSED(lpParam);

	m_nState		= IRDCSTATE_IDLE;
	m_pConn			= NULL;
	m_nQueryCount	= 0;
	//Connect();
	ASSERT(m_pOwner);
	m_pOwner->PostEvent(MAKEEVENT(BCTM_STOP, 0, 0));
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPClient.cpp
///////////////////////////////////////////////////////////////////////////////
