///////////////////////////////////////////////////////////////////////////////
// file : HTTPClient.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef HTTP_HTTPCLIENT_H_INCLUDED__
#define HTTP_HTTPCLIENT_H_INCLUDED__

#include <HTTP/Exports.h>
#include <HTTP/IHTTPHandler.h>


namespace BC
{
	class BCEventDispatcher;
}

using namespace BC;


///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

enum
{
	BCTM_UPDATE			= 1000,
	BCTM_FAILED			= 1001,
	BCTM_STOP			= 1002,
};

class HTTPConnOut;

///////////////////////////////////////////////////////////////////////////////
// class : HTTPClient
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPClient : public IHTTPResponder
{
public:
	HTTPClient();
	~HTTPClient();

	BCRESULT		Create(
						BCEventDispatcher *pOwner,
						LPCSTR szUrl = NULLPSTRING,
						uint16_t nPort = 0);
	void			SetServerInfo(
						LPCSTR szUrl,
						uint16_t nPort);
	BCRESULT		Connect();
	BCRESULT		SendTask();
	void			Disconnect();
	void			Destroy();
	BOOL			IsConnected();
protected:
	BCRESULT		OnHTTPMessage(uint32_t nMsg, void *wParam, void *lParam);
	BCRESULT		OnHTTPConnect(BCRESULT result, void *lParam);
	void			OnRequestReady(HTTPReq &refReq, void *lParam);
	void			OnResponseHeader(HTTPReq &refResp, void *lParam);
	void			OnResponseBody(BCBuffer *pBuffer, void *lParam);
	void			OnPacketSent(void *lpParam);
	void			OnHTTPStop(void *lpParam, uint32_t nExitCode);
private:
	DECLARE_NO_COPY_CLASS(HTTPClient);
	BCSpinMutex				m_sLock;
	BCEventDispatcher	*	m_pOwner;
	HTTPConnOut			*	m_pConn;
	uint32_t				m_nState;
	uint32_t				m_nQueryCount;
	char					m_szSvrIP[64];
	uint16_t				m_nSvrPort;
	//BOOL					m_bShutdown;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////


} // End of namespace HTTP

#endif // HTTP_HTTPCLIENT_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPClient.h
///////////////////////////////////////////////////////////////////////////////
