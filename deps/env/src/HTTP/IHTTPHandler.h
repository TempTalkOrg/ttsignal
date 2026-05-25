///////////////////////////////////////////////////////////////////////////////
// file : IHTTPHandler.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef HTTP_IHTTPHANDLER_H_INCLUDED__
#define HTTP_IHTTPHANDLER_H_INCLUDED__

#include <HTTP/Exports.h>
#include <BC/BCBuffer.h>
#include <BC/BCNodeList.h>
#include <BC/BCTask.h>
#include <BC/BCEventQueue.h>


namespace BC
{
	class BCSocket;
}

///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

#define HTTPM_CONNECTFAILED				1
#define HTTPM_CONNECTSUCCESS			2
#define HTTPM_NEWHTTPCONN				3
#define HTTPM_FILELISTFAILED			4
#define HTTPM_FILELISTSTART				5
#define HTTPM_CONNECTCLOSED				6

///////////////////////////////////////////////////////////////////////////////
// class : IHTTPHandler
///////////////////////////////////////////////////////////////////////////////

class HTTP_API IHTTPHandler
{
public:
    virtual ~IHTTPHandler(){}
public:
	virtual BCRESULT	OnMgrMsg(uint32_t nMsg, void *wParam, void *lParam)		= 0;
	virtual BCRESULT	OnHTTPMsg(uint32_t nMsg, void *wParam, void *lParam)	= 0;
};

#define MGRM_CONNCOUNT_CHANGED			1

///////////////////////////////////////////////////////////////////////////////
// class : HTTPResponder
//       - Used to handle outgoing http connection
///////////////////////////////////////////////////////////////////////////////

class HTTPReq;

class HTTP_API IHTTPResponder
{
public:
	IHTTPResponder(){}
	virtual ~IHTTPResponder(){}

	virtual BCRESULT	OnHTTPMessage(uint32_t nMsg, void *wParam, void *lParam)	= 0;
	virtual BCRESULT	OnHTTPConnect(BCRESULT result, void *lParam)				= 0;
	virtual void		OnRequestReady(HTTPReq &refReq, void *lParam)				= 0;
	virtual void		OnResponseHeader(HTTPReq &refResp, void *lParam)			= 0;
	virtual void		OnResponseBody(BCBuffer *pBuffer, void *lParam)				= 0;
	virtual void		OnResponseEnd(void *lParam)									= 0;
	virtual void		OnPacketSent(void *lpParam)									= 0;
	virtual void		OnHTTPStop(void *lpParam, uint32_t nExitCode)				= 0;
};

///////////////////////////////////////////////////////////////////////////////
// class : HTTPFilter
//       - Used to handle incoming http connection
///////////////////////////////////////////////////////////////////////////////

#define HTTP_FILTER_EV			5

class HTTPResp;

class HTTP_API HTTPFilter : public BCNodeList::Node
{
	friend class HTTPConn;
	friend class HTTPConnMgr;
public:
	HTTPFilter(){m_szUrl[0] = '\0';};
	virtual ~HTTPFilter(){};

	virtual BOOL		IsMatch(LPCSTR lpUrl) const       = 0;
	virtual BCRESULT	OnRequestHeader(
							HTTPResp &refResp,
							BCTask *pTask,
							void *lParam)                 = 0;
	virtual void		OnRequestBody(
							BCBuffer &refBuffer,
							void *lParam)                 = 0;
	virtual void		OnFilterEvent(
							BCEventItemS &refEvent,
							void *lParam)                 = 0;
	virtual void		OnResponseSent(void *lParam)      = 0;
	virtual void		OnResponseStop(void *lParam)      = 0;
	virtual void		OnResponseShutdown(void *lParam)  = 0;
protected:
private:
	DECLARE_NO_COPY_CLASS(HTTPFilter);
	char			m_szUrl[MAX_PATH];
};

typedef TNodeList<HTTPFilter>	HTTPUrlList;

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // end of namespace HTTP

#endif // HTTP_IHTTPHANDLER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : IHTTPHandler.h
///////////////////////////////////////////////////////////////////////////////
