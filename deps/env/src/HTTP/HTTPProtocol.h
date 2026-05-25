///////////////////////////////////////////////////////////////////////////////
// file : HTTPProtocol.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef HTTP_HTTPPROTOCOL_H_INCLUDED__
#define HTTP_HTTPPROTOCOL_H_INCLUDED__

#include <vector>
#include <map>
#include <string>
#include <HTTP/Exports.h>
#include <BC/BCStrPtrLen.h>
#include <BC/BCUserData.h>
#include <BC/BCEventQueue.h>
#include <BC/BCBuffer.h>
#include <BC/BCSockAddr.h>

///////////////////////////////////////////////////////////////////////////////
// file :
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// Namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

namespace HTTP
{


#define HTTP_RECVLEN			1024
#define HTTP_SENDGROW			1024
#define HTTP_SEND_MAXLEN		10240

/*%
 * HTTP methods.
 */
#define BC_HTTPD_METHODUNKNOWN	0
#define BC_HTTPD_METHODGET		1
#define BC_HTTPD_METHODPOST		2
#define BC_HTTPD_METHODPUT		3



#define HTTPD_CLOSE			0x0001 /* Got a Connection: close header */
#define HTTPD_FOUNDHOST		0x0002 /* Got a Host: header */

/*%
 * Client states.
 *
 * _IDLE	The client is not doing anything at all.  This state should
 *		only occur just after creation, and just before being
 *		destroyed.
 *
 * _RECV	The client is waiting for data after issuing a socket recv().
 *
 * _RECVDONE	Data has been received, and is being processed.
 *
 * _SEND	All data for a response has completed, and a reply was
 *		sent via a socket send() call.
 *
 * _SENDDONE	Send is completed.
 *
 * Badly formatted state table:
 *
 *	IDLE -> RECV when client has a recv() queued.
 *
 *	RECV -> RECVDONE when recvdone event received.
 *
 *	RECVDONE -> SEND if the data for a reply is at hand.
 *
 *	SEND -> RECV when a senddone event was received.
 *
 *	At any time -> RECV on error.  If RECV fails, the client will
 *	self-destroy, closing the socket and freeing memory.
 */
#define BC_HTTPD_STATEIDLE		0
#define BC_HTTPD_STATERECV		1
#define BC_HTTPD_STATERECVDONE	2
#define BC_HTTPD_STATESEND		3
#define BC_HTTPD_STATESENDDONE	4

#define BC_HTTPD_ISRECV(c)		((c)->m_eIoState == BC_HTTPD_STATERECV)
#define BC_HTTPD_ISRECVDONE(c)	((c)->m_eIoState == BC_HTTPD_STATERECVDONE)
#define BC_HTTPD_ISSEND(c)		((c)->m_eIoState == BC_HTTPD_STATESEND)
#define BC_HTTPD_ISSENDDONE(c)	((c)->m_eIoState == BC_HTTPD_STATESENDDONE)

/*%
 * Overall magic test that means we're not idle.
 */
#define BC_HTTPD_SETRECV(c)		((c)->m_eIoState = BC_HTTPD_STATERECV)
#define BC_HTTPD_SETRECVDONE(c)	((c)->m_eIoState = BC_HTTPD_STATERECVDONE)
#define BC_HTTPD_SETSEND(c)		((c)->m_eIoState = BC_HTTPD_STATESEND)
#define BC_HTTPD_SETSENDDONE(c)	((c)->m_eIoState = BC_HTTPD_STATESENDDONE)


HTTPDLLEXPORT_DATA(extern const char) G_szHTTP10[];
HTTPDLLEXPORT_DATA(extern const char) G_szHTTP11[];

typedef struct KVPairS
{
	LPCSTR		szArg;
	LPCSTR		szValue;

	KVPairS() : szArg(NULL), szValue(NULL) {}
	~KVPairS(){};
}KVPairS;


// Versions
enum
{
	http09version = 0,
	http10Version = 1,
	http11Version = 2,

	httpNumVersions = 3,
	httpIllegalVersion = 3
};
typedef uint32_t HTTPVersion;

// Methods
enum
{
	httpGetMethod           = 0,
	httpHeadMethod          = 1,
	httpPostMethod          = 2,
	httpOptionsMethod       = 3,
	httpPutMethod           = 4,
	httpDeleteMethod        = 5,
	httpTraceMethod         = 6,
	httpConnectMethod       = 7,

	httpNumMethods          = 8,
	httpIllegalMethod       = 8
};
typedef uint32_t HTTPMethod;

// Headers
enum
{
	// VIP headers
	httpConnectionHeader        = 0, // general header
	httpDateHeader              = 1, // general header
	httpAuthorizationHeader     = 2, // request header
	httpIfModifiedSinceHeader   = 3, // request header
	httpServerHeader            = 4, // response header
	httpWWWAuthenticateHeader   = 5, // response header
	httpExpiresHeader           = 6, // entity header
	httpLastModifiedHeader      = 7, // entity header
	httpNumVIPHeaders           = 8,

	//Other general http headers
	httpCacheControlHeader      = 8,
	httpPragmaHeader            = 9,
	httpTrailerHeader           = 10,
	httpTransferEncodingHeader  = 11,
	httpUpgradeHeader           = 12,
	httpViaHeader               = 13,
	httpWarningHeader           = 14,

	// Other request headers
	httpAcceptHeader            = 15,
	httpAcceptCharsetHeader     = 16,
	httpAcceptEncodingHeader    = 17,
	httpAcceptLanguageHeader    = 18,
	httpExpectHeader            = 19,
	httpFromHeader              = 20,
	httpHostHeader              = 21,
	httpIfMatchHeader           = 22,
	httpIfNoneMatchHeader       = 23,
	httpIfRangeHeader           = 24,
	httpIfUnmodifiedSinceHeader = 25,
	httpMaxForwardsHeader       = 26,
	httpProxyAuthorizationHeader= 27,
	httpRangeHeader             = 28,
	httpRefererHeader           = 29,
	httpTEHeader                = 30,
	httpUserAgentHeader         = 31,

	// Other response headers
	httpAcceptRangesHeader      = 32,
	httpAgeHeader               = 33,
	httpETagHeader              = 34,
	httpLocationHeader          = 35,
	httpProxyAuthenticateHeader = 36,
	httpRetryAfterHeader        = 37,
	httpVaryHeader              = 38,

	// Other entity headers
	httpAllowHeader             = 39,
	httpContentEncodingHeader   = 40,
	httpContentLanguageHeader   = 41,
	httpContentLengthHeader     = 42,
	httpContentLocationHeader   = 43,
	httpContentMD5Header        = 44,
	httpContentRangeHeader      = 45,
	httpContentTypeHeader       = 46,

	// QTSS Specific headers
	// Add headers that are not part of the HTTP spec here
	// Make sure and up the number of headers and httpIllegalHeader number
	httpSessionCookieHeader     = 47,           // Used for HTTP tunnelling
	httpServerIPAddressHeader   = 48,

	httpNumHeaders              = 49,
	httpIllegalHeader           = 49
};
typedef uint32_t HTTPHeader;

// Status codes
enum
{
	httpContinue                    = 0,            //100
	httpSwitchingProtocols          = 1,            //101
	httpOK                          = 2,            //200
	httpCreated                     = 3,            //201
	httpAccepted                    = 4,            //202
	httpNonAuthoritativeInformation = 5,            //203
	httpNoContent                   = 6,            //204
	httpResetContent                = 7,            //205
	httpPartialContent              = 8,            //206
	httpMultipleChoices             = 9,            //300
	httpMovedPermanently            = 10,           //301
	httpFound                       = 11,           //302
	httpSeeOther                    = 12,           //303
	httpNotModified                 = 13,           //304
	httpUseProxy                    = 14,           //305
	httpTemporaryRedirect           = 15,           //307
	httpBadRequest                  = 16,           //400
	httpUnAuthorized                = 17,           //401
	httpPaymentRequired             = 18,           //402
	httpForbidden                   = 19,           //403
	httpNotFound                    = 20,           //404
	httpMethodNotAllowed            = 21,           //405
	httpNotAcceptable               = 22,           //406
	httpProxyAuthenticationRequired = 23,           //407
	httpRequestTimeout              = 24,           //408
	httpConflict                    = 25,           //409
	httpGone                        = 26,           //410
	httpLengthRequired              = 27,           //411
	httpPreconditionFailed          = 28,           //412
	httpRequestEntityTooLarge       = 29,           //413
	httpRequestURITooLarge          = 30,           //414
	httpUnsupportedMediaType        = 31,           //415
	httpRequestRangeNotSatisfiable  = 32,           //416
	httpExpectationFailed           = 33,           //417
	httpInternalServerError         = 34,           //500
	httpNotImplemented              = 35,           //501
	httpBadGateway                  = 36,           //502
	httpServiceUnavailable          = 37,           //503
	httpGatewayTimeout              = 38,           //504
	httpHTTPVersionNotSupported     = 39,           //505
	httpNumStatusCodes              = 40
};
typedef uint32_t HTTPStatusCode;

///////////////////////////////////////////////////////////////////////////////
// class : HTTPReq
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPReq
{
public:
	HTTPReq();
	~HTTPReq();

	BCRESULT		ParseResponse(BCBuffer &refBuffer);
	BCRESULT		Response();
	BCRESULT		CreateRequest(uint32_t eMethod, LPCSTR szUrl, LPCSTR szProtocol = G_szHTTP11);
	BCRESULT		BeginHeader(BOOL bEncodeUrl = TRUE, BOOL bPCS = FALSE);
	BCRESULT		AddParam(const char *name, const char *var);
	BCRESULT		AddParamUInt(const char *name, int val);
	BCRESULT		AddParamInt64(const char *name, int64_t val);
	BCRESULT		EndParams(BOOL host = TRUE);
	BCRESULT		AddHeader(const char *name,	const char *val);
	BCRESULT		AddHeaderUInt(const char *name, int val);
	BCRESULT		AddHeaderInt64(const char *name, int64_t val);
	BCRESULT		EndHeaders();
	void			Reset();
	BCRESULT		Render(HTTPStatusCode eStatus);
	BCRESULT		Render404();
	BCRESULT		Render500();
	LPCSTR			GetParam(LPCSTR szArg, BOOL bCaseSensitive = FALSE);
	LPCSTR			GetHeader(LPCSTR szArg, BOOL bCaseSensitive = FALSE);
	uint64_t		GetContentLength();
	void			GetMD5(char* md5);
	void			GetETag(char* etag);
	bool			IsChunkedBody();
protected:
public:

	uint32_t			m_eIoState;
	bool				m_bRequest;

	/*%
	 * Received data state.
	 */
	uint32_t			m_eMethod;
	char			*	m_szUrl;
	char			*	m_szHost;
	const char		*	m_szProtocol;
	uint32_t			m_nStatusCode;
	char			*	m_szStatusCode;

	/*
	 * Flags on the httpd client.
	 */
	int32_t				m_nFlags;

	BCBuffer			m_sHeaderBuffer;

	const char		*	m_szMimeType;
	uint32_t			m_nRetCode;
	const char		*	m_szRetMsg;
	BCBuffer			m_sBodyBuffer;
	typedef std::vector<KVPairS>	ArgArray;
	ArgArray			m_arrayArgs;
	ArgArray			m_arrayHeaders;
	KBPool				m_sPool;
};


///////////////////////////////////////////////////////////////////////////////
// class : HTTPResp
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPResp
{
public:
	typedef struct RangeS
	{
		bool		bSet;
		int64_t		nStart;
		int64_t		nEnd;

		RangeS() : bSet(false), nStart(0), nEnd(-1){}
	}RangeS;
public:
	HTTPResp();
	~HTTPResp();

	BCRESULT		ParseRequest(BCBuffer &refBuffer);
	BCRESULT		Response();
	BCRESULT		AddHeader(const char *name,	const char *val);
	BCRESULT		AddHeaderBool(const char* name, bool val);
	BCRESULT		AddHeaderUInt(const char *name, int val);
	BCRESULT		AddHeaderInt64(const char *name, int64_t val);
	BCRESULT		EndHeaders();
	void			Reset();
	BCRESULT		Render(HTTPStatusCode eStatus, BOOL bWriteRetMsg = TRUE);
	BCRESULT		Render404();
	BCRESULT		Render500();
	LPCSTR			GetParam(LPCSTR szArg, BOOL bCaseSensitive = FALSE);
	LPCSTR			GetHeader(LPCSTR szArg, BOOL bCaseSensitive = FALSE);
	RangeS			GetRange();
	uint64_t		GetContentLength();
protected:
public:

	uint32_t			m_eIoState;

	/*%
	 * Received data state.
	 */
	uint32_t			m_eMethod;
	char			*	m_szUrl;
	char			*	m_szProtocol;

	/*
	 * Flags on the httpd client.
	 */
	int32_t				m_nFlags;

	BCBuffer			m_sHeaderBuffer;

	const char	    *	m_szMimeType;
	uint32_t			m_nRetCode;
	const char	    *	m_szRetMsg;
	BCBuffer			m_sBodyBuffer;
	KBPool				m_sPool;
	typedef std::vector<KVPairS>	ArgArray;
	ArgArray			m_arrayArgs;
	ArgArray			m_arrayHeaders;
	BCSockAddrS			m_sRemoteAddr;
	BCSockAddrS			m_sLocalAddr;
};

///////////////////////////////////////////////////////////////////////////////
// Class : HTTPProtocol
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPProtocol
{
public:
	// Methods
	static HTTPMethod        GetMethod(const BCStrPtrLen* inMethodStr);
	static BCStrPtrLen    *  GetMethodString(HTTPMethod inMethod) { return &sMethods[inMethod]; }
	// Headers
	static HTTPHeader        GetHeader(const BCStrPtrLen* inHeaderStr);
	static BCStrPtrLen    *  GetHeaderString(HTTPHeader inHeader) { return &sHeaders[inHeader]; }
	// Status codes
	static BCStrPtrLen    *  GetStatusCodeString(HTTPStatusCode inStat) { return &sStatusCodeStrings[inStat]; }
	static int32_t           GetStatusCodeByIETFCode(int inStat);
	static int32_t           GetStatusCode(HTTPStatusCode inStat) { return sStatusCodes[inStat]; }
	static BCStrPtrLen    *  GetStatusCodeAsString(HTTPStatusCode inStat) { return &sStatusCodeAsStrings[inStat]; }
	// Versions
	static HTTPVersion       GetVersion(BCStrPtrLen* versionStr);
	static BCStrPtrLen    *  GetVersionString(HTTPVersion version) { return &sVersionStrings[version]; }
	// Url
	static BCRESULT          ParseHost(
								LPCSTR lpszUrl, 
								BCStrPtrLen &refStrHost, 
								BCStrPtrLen &refStrPort,
								BCStrPtrLen &refRelativeUrl,
								bool& bSSL,
								uint16_t& refPort);
	// Url encode & decode
	static size_t			URLEncode(const char *lpszIn, char *lpszOut, size_t size_out);
	static size_t			URLEncode_RFC(const char *lpszIn, char *lpszOut, size_t size_out);
	static size_t			URLDecode(const char *lpszIn, char *lpszOut, size_t size_out);
	// DNS Parse
	static BCRESULT			ParseAddrFromUrl(
								LPCSTR lpszHttpUrl,
								std::string &strIP,
                                uint16_t &refPort,
                                int32_t &refNetType,
								std::string &strRelativeUrl,
								bool &bSSL);
	static BCRESULT			ParseLocationWithParams(
								LPCSTR lpszLocation,
								std::string &strPath,
                                std::map<std::string, std::string> &params);
private:
	static BCStrPtrLen          sMethods[];
	static BCStrPtrLen          sHeaders[];
	static BCStrPtrLen          sStatusCodeStrings[];
	static BCStrPtrLen          sStatusCodeAsStrings[];
	static int32_t              sStatusCodes[];
	static BCStrPtrLen          sVersionStrings[];
};

///////////////////////////////////////////////////////////////////////////////
// enum : HTTPMtypeE
///////////////////////////////////////////////////////////////////////////////

typedef enum HTTPMTypeE
{
	HTTPM_UNKNOWN				=	0,
	// User defined message type - User data
	HTTPM_USERDATA				= 0x89,
	// User defined message type - User notifier
	HTTPM_NOTIFIER				= 0x8A,
	// User defined message type - User task
	HTTPM_TASK					= 0x8B,
	// User defined message type - Virtual send
	HTTPM_DELAYSEND				= 0x8C,
	// User defined message type - wait signal
	HTTPM_SIGNALWAIT			= 0x8D,
}HTTPMTypeE;

class HTTPNotifier;

typedef void (*LPFN_HTTPNotificationPtr)(HTTPNotifier &refNotifier);
typedef LPFN_HTTPNotificationPtr		LPFN_HTTPNotification;

///////////////////////////////////////////////////////////////////////////////
// class : IHTTPChunkHandler
///////////////////////////////////////////////////////////////////////////////

class HTTP_API IHTTPChunkHandler
{
	friend class HTTPChunkParser;
public:
	IHTTPChunkHandler(){};
	virtual ~IHTTPChunkHandler(){};

protected:
	virtual void OnChunkRecv(BCBuffer *pChunk)		= 0;
private:
	DECLARE_NO_COPY_CLASS(IHTTPChunkHandler);
};

///////////////////////////////////////////////////////////////////////////////
// class : NMParser
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPChunkParser
{
	typedef enum RecvStateE
	{
		RECV_SIZE		= 0,
		RECV_CHUNK		= 1
	}RecvStateE;
public:
	HTTPChunkParser();
	virtual ~HTTPChunkParser();

	BCRESULT		Create(IHTTPChunkHandler *pHandler);
	BOOL			Parse();
	BCBuffer	*	GetRecvBuf();
	void			Cleanup();
protected:
	BOOL			_RequireData(
						uint32_t nSize,
						uint32_t nForward,
						RecvStateE eRecvState);
	// message receive
	BOOL			_RequireSize();
	BOOL			_RequireChunk();
	BOOL			_ParseSize();
	BOOL			_FinishChunk();
	// utilities
	uint64_t		_Hex2Int(LPCSTR lpIn);
protected:
	IHTTPChunkHandler	*	m_pHandler;
	uint64_t				m_nChunkSize;
	BCBuffer				m_sChunk;
	BCBIStream				m_sReader;
	// Asynch state
	RecvStateE				m_eRecvState;
	uint32_t				m_nRequireDataSize;
	bool					m_bChunkParseCtrl;
private:
	DECLARE_NO_COPY_CLASS(HTTPChunkParser);
};

///////////////////////////////////////////////////////////////////////////////
// class : HTTPNotifier
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPNotifier
{
	DECLARE_FIXED_ALLOC(HTTPNotifier);
public:
	HTTPNotifier();
	HTTPNotifier(
		uint32_t eType,
		LPFN_HTTPNotification pAction,
		void *wParam,
		uint64_t lParam);
	virtual ~HTTPNotifier();

	uint32_t				m_eType;
	LPFN_HTTPNotification	m_pAction;
	void				*	m_wParam;
	uint64_t				m_lParam;

	HTTPNotifier &	operator = (const HTTPNotifier &refOther);
	void			Init(
						uint32_t eType,
						LPFN_HTTPNotification pAction,
						void *wParam,
						uint64_t lParam = 0);
	void			Notify();
};

///////////////////////////////////////////////////////////////////////////////
// class : SendItem
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPMItem : public BCNodeList::Node
{
	DECLARE_FIXED_ALLOC(HTTPMItem);
public:
	HTTPMItem(uint32_t eType = HTTPM_UNKNOWN);
	~HTTPMItem();

	uint32_t			m_eType;
	BCBuffer			m_sBuffer;
	HTTPNotifier		m_sNotifier;
};

typedef TNodeList<HTTPMItem>		SendItemList;

///////////////////////////////////////////////////////////////////////////////
// class : SendBuffer
///////////////////////////////////////////////////////////////////////////////

class HTTP_API HTTPMQueue
{
public:
	HTTPMQueue();
	virtual ~HTTPMQueue();

	BOOL			Append(BCBuffer &refBody);
	BOOL			AppendEx(BCBuffer &refBody);
	BOOL			Append(HTTPNotifier *pNotifier);
	HTTPMItem *		PopFront();
	void			Cleanup();
	BOOL			IsEmpty() const;
protected:
private:
	DECLARE_NO_COPY_CLASS(HTTPMQueue);
	SendItemList			m_lstItems;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : HTTP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace HTTP

#endif // HTTP_HTTPPROTOCOL_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : HTTPProtocol.h
///////////////////////////////////////////////////////////////////////////////
