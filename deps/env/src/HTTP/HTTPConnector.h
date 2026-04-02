///////////////////////////////////////////////////////////////////////////////
// file : HTTPConnector.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef HTTP_HTTPCONNECTOR_H_INCLUDED__
#define HTTP_HTTPCONNECTOR_H_INCLUDED__

#include <HTTP/Exports.h>
#include <HTTP/IHTTPHandler.h>
#include <HTTP/HTTPConnOut.h>

namespace BC
{
	class BCTask;
	class BCTaskMgr;
	class BCTimer;
	class BCTimerMgr;
	class BCSocket;
	class BCSocketMgr;
	class BCAcceptor;
}

///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{

class HTTPConnOut;
class HTTPConnOutMgr;

///////////////////////////////////////////////////////////////////////////////
// class : HTTPConnector
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPConnector : public IHTTPHandler
{
	DECLARE_FIXED_ALLOC(HTTPConnector);
public:
	HTTPConnector();
	virtual ~HTTPConnector();

	BCRESULT		Create(uint32_t nWorkers, BOOL bIPv6 = FALSE);
	BCRESULT		Create(
						BCTaskMgr *pTaskMgr,
						BCTimerMgr *pTimerMgr,
						BCSocketMgr *pSockMgr,
						BOOL bIPv6 = FALSE);
	void			SetConnTimeout(uint32_t nTimeout);
	void			SetMaxConnTimeout(uint32_t nTimeout);
	HTTPConnOut	*	CreateHTTPConn(IHTTPResponder *pResponder);
	void			Destroy();
protected:
	virtual BCRESULT	OnMgrMsg(uint32_t nMsg, void *wParam, void *lParam);
	virtual BCRESULT	OnHTTPMsg(uint32_t nMsg, void *wParam, void *lParam);

	BCSpinMutex				m_sLock;
	BOOL					m_bDeleteRes;
	BCTaskMgr			*	m_pTaskMgr;
	BCTimerMgr			*	m_pTimerMgr;
	BCSocketMgr			*	m_pSockMgr;
	HTTPConnOutMgr		*	m_pConnMgr;
	BOOL					m_bIPv6;
private:
	DECLARE_NO_COPY_CLASS(HTTPConnector);
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

#endif // HTTP_HTTPCONNECTOR_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPConnector.h
///////////////////////////////////////////////////////////////////////////////
