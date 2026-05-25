
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_   /* Prevent inclusion of winsock.h in windows.h */
#endif

#include <WinSock2.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <MSWSock.h>
#include "BC/BCTask.h"
#include "BC/BCBuffer.h"
#include "BC/BCLog.h"
#include "BC/win32/os.h"
#include "BC/win32/BCSocket.h"



#ifndef EWOULDBLOCK
#define EWOULDBLOCK             WSAEWOULDBLOCK
#endif // EWOULDBLOCK


#ifndef _DEBUG
#pragma warning(disable:4002)
#define LogError(...)
#define LogFatal(...)
#endif // _DEBUG

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////


/*
 * How in the world can Microsoft exist with APIs like this?
 * We can't actually call this directly, because it turns out
 * no library exports this function.  Instead, we need to
 * issue a runtime call to get the address.
 */
LPFN_CONNECTEX BCConnectEx;
LPFN_ACCEPTEX BCAcceptEx;
LPFN_GETACCEPTEXSOCKADDRS BCGetAcceptExSockaddrs;

/*
 * Run expensive internal consistency checks.
 */
#ifdef BC_SOCKET_CONSISTENCY_CHECKS
#define CONSISTENT(sock) (sock)->_Consistent()
#else
#define CONSISTENT(sock) do {} while (0)
#endif

/*
 * Define this macro to control the behavior of connection
 * resets on UDP sockets.  See Microsoft KnowledgeBase Article Q263823
 * for details.
 * NOTE: This requires that Windows 2000 systems install Service Pack 2
 * or later.
 */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR,12)
#endif

/*
 * Some systems define the socket length argument as an int, some as size_t,
 * some as socklen_t.  This is here so it can be easily changed if needed.
 */
#ifndef BC_SOCKADDR_LEN_T
#define BC_SOCKADDR_LEN_T unsigned int
#endif

/*
 * Define what the possible "soft" errors can be.  These are non-fatal returns
 * of various network related functions, like recv() and so on.
 */
#define SOFT_ERROR(e)	((e) == WSAEINTR || \
			 (e) == WSAEWOULDBLOCK || \
			 (e) == EWOULDBLOCK || \
			 (e) == EINTR || \
			 (e) == EAGAIN || \
			 (e) == 0)

/*
 * Pending errors are not really errors and should be
 * kept separate
 */
#define PENDING_ERROR(e) ((e) == WSA_IO_PENDING || (e) == 0)

#define DOIO_SUCCESS	  0       /* i/o ok, event sent */
#define DOIO_SOFT         1       /* i/o ok, soft error, no event sent */
#define DOIO_HARD	      2       /* i/o error, event sent */
#define DOIO_EOF	      3       /* EOF, no event sent */
#define DOIO_PENDING	  4       /* status when i/o is in process */
#define DOIO_NEEDMORE	  5       /* IO was processed, but we need more due to minimum */


/*
 * Socket State
 */
enum
{
	SOCK_INITIALIZED,	/* Socket Initialized */
	SOCK_OPEN,			/* Socket opened but nothing yet to do */
	SOCK_DATA,			/* Socket sending or receiving data */
	SOCK_LISTEN,		/* TCP Socket listening for connects */
	SOCK_ACCEPT,		/* TCP socket is waiting to accept */
	SOCK_CONNECT,		/* TCP Socket connecting */
	SOCK_CLOSED,		/* Socket has been closed */
};

#define SOCKET_MAGIC		BC_MAGIC('I', 'O', 'i', 'o')
#define VALID_SOCKET(t)		BC_MAGIC_VALID(t, SOCKET_MAGIC)

/*
 * IPv6 control information.  If the socket is an IPv6 socket we want
 * to collect the destination address and interface so the client can
 * set them on outgoing packets.
 */
#ifdef BC_PLATFORM_HAVEIPV6
#ifndef USE_CMSG
#define USE_CMSG	1
#endif
#endif

/*
 * We really  don't want to try and use these control messages. Win32
 * doesn't have this mechanism before XP.
 */
#undef USE_CMSG

/*
 * Message header for recvmsg and sendmsg calls.
 * Used value-result for recvmsg, value only for sendmsg.
 */
struct msghdr
{
	SOCKADDR_STORAGE	to_addr;		/* UDP send/recv address */
	int					to_addr_len;	/* length of the address */
	WSABUF			*	msg_iov;		/* scatter/gather array */
	u_int				msg_iovlen;     /* # elements in msg_iov */
	void			*	msg_control;    /* ancillary data, see below */
	u_int				msg_controllen; /* ancillary data buffer len */
	int					msg_totallen;	/* total length of this message */
} msghdr;

/*
 * The size to raise the receive buffer to.
 */
#define RCVBUFSIZE (32*1024)

/*
 * The number of times a send operation is repeated if the result
 * is WSAEINTR.
 */
#define NRETRIES 10


#define _set_state(sock, _state) (sock)->_SetState(_state, __LINE__)


/*
 * I/O Completion ports Info structures
 */
class SockIoCompletionInfo
{
	DECLARE_FIXED_ALLOC(SockIoCompletionInfo);
public:
	SockIoCompletionInfo() : dev(NULL), cdev(NULL), adev(NULL),
			acceptbuffer(NULL), received_bytes(0), request_type(0)
	{
		memzero(&overlapped, sizeof(overlapped));
		memzero(&messagehdr, sizeof(messagehdr));
	}
	~SockIoCompletionInfo() {}
public:
	OVERLAPPED				overlapped;
	BCSockEvent			*	dev;  /* send()/recv() done event */
	BCSockOCEvent		*	cdev; /* connect() done event */
	BCSockICEvent		*	adev; /* accept() done event */
	void				*	acceptbuffer;
	DWORD					received_bytes;
	int						request_type;
	struct msghdr			messagehdr;
} ;

IMPLEMENT_FIXED_ALLOC(SockIoCompletionInfo, 2000);

class SockAcceptBuffer
{
	DECLARE_FIXED_ALLOC(SockAcceptBuffer);
public:
	SockAcceptBuffer()
	{
		memzero(&buffer, sizeof(buffer));
	}
	~SockAcceptBuffer() {}
public:
	char		buffer[(sizeof(SOCKADDR_STORAGE) + 16) * 2];
};

IMPLEMENT_FIXED_ALLOC(SockAcceptBuffer, 2);

/*
 * Define a maximum number of I/O Completion Port worker threads
 * to handle the load on the Completion Port. The actual number
 * used is the number of CPU's + 1.
 */
#define MAX_IOCPTHREADS 20

#define SOCKET_MANAGER_MAGIC	BC_MAGIC('I', 'O', 'm', 'g')
#define VALID_MANAGER(m)		BC_MAGIC_VALID(m, SOCKET_MANAGER_MAGIC)

enum
{
	SOCKET_RECV,
	SOCKET_SEND,
	SOCKET_ACCEPT,
	SOCKET_CONNECT
};

/*
 * send() and recv() iovec counts
 */
#define MAXSCATTERGATHER_SEND	(BC_SOCKET_MAXSCATTERGATHER)
#define MAXSCATTERGATHER_RECV	(BC_SOCKET_MAXSCATTERGATHER)



static BCOnceS initialise_once = BC_ONCE_INIT;
static BOOL initialised = FALSE;

static void
initialise(void*)
{
	WORD wVersionRequested;
	WSADATA wsaData;
	int err;
	SOCKET sock;
	GUID GUIDConnectEx = WSAID_CONNECTEX;
	GUID GUIDAcceptEx = WSAID_ACCEPTEX;
	GUID GUIDGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
	DWORD dwBytes;

	/* Need Winsock 2.2 or better */
	wVersionRequested = MAKEWORD(2, 2);

	err = WSAStartup(wVersionRequested, &wsaData);
	if (err != 0)
	{
		char strbuf[BC_STRERRORSIZE];
		bc_strerror(err, strbuf, sizeof(strbuf));
		LogFatal(_LOCAL_, "WSAStartup(): %s", strbuf);
		exit(1);
	}
	/*
	 * The following APIs do not exist as functions in a library, but we must
	 * ask winsock for them.  They are "extensions" -- but why they cannot be
	 * actual functions is beyond me.  So, ask winsock for the pointers to the
	 * functions we need.
	 */
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	ASSERT(sock != INVALID_SOCKET);
	err = WSAIoctl(sock,  SIO_GET_EXTENSION_FUNCTION_POINTER,
	               &GUIDConnectEx, sizeof(GUIDConnectEx),
	               &BCConnectEx, sizeof(BCConnectEx),
	               &dwBytes, NULL, NULL);
	ASSERT(err == 0);

	err = WSAIoctl(sock,  SIO_GET_EXTENSION_FUNCTION_POINTER,
	               &GUIDAcceptEx, sizeof(GUIDAcceptEx),
	               &BCAcceptEx, sizeof(BCAcceptEx),
	               &dwBytes, NULL, NULL);
	ASSERT(err == 0);

	err = WSAIoctl(sock,  SIO_GET_EXTENSION_FUNCTION_POINTER,
	               &GUIDGetAcceptExSockaddrs, sizeof(GUIDGetAcceptExSockaddrs),
	               &BCGetAcceptExSockaddrs, sizeof(BCGetAcceptExSockaddrs),
	               &dwBytes, NULL, NULL);
	ASSERT(err == 0);

	closesocket(sock);

	initialised = TRUE;
}

/*
 * Initialize socket services
 */
static void
InitSockets(void)
{
	BCRESULT result;

	result = bc_once_do(&initialise_once, initialise, NULL);
	if (result != BC_R_SUCCESS)
	{
		exit(1);
	}
	if (!initialised)
	{
		exit(1);
	}
}

/*
 * Make an fd SOCKET non-blocking.
 */
static BCRESULT
make_nonblock(SOCKET fd)
{
	int ret;
	unsigned long flags = 1;
	char strbuf[BC_STRERRORSIZE];

	/* Set the socket to non-blocking */
	ret = ioctlsocket(fd, FIONBIO, &flags);

	if (ret == -1)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
				 "ioctlsocket(%d, FIOBIO, %d): %s",
				 fd, flags, strbuf);

		return (BC_R_UNEXPECTED);
	}

	return (BC_R_SUCCESS);
}

/*
 * Windows 2000 systems incorrectly cause UDP sockets using WSARecvFrom
 * to not work correctly, returning a WSACONNRESET error when a WSASendTo
 * fails with an "ICMP port unreachable" response and preventing the
 * socket from using the WSARecvFrom in subsequent operations.
 * The function below fixes this, but requires that Windows 2000
 * Service Pack 2 or later be installed on the system.  NT 4.0
 * systems are not affected by this and work correctly.
 * See Microsoft Knowledge Base Article Q263823 for details of this.
 */
BCRESULT
connection_reset_fix(SOCKET fd)
{
	DWORD dwBytesReturned = 0;
	BOOL  bNewBehavior = FALSE;
	int status;

	if (bc_win32os_majorversion() < 5)
		return (BC_R_SUCCESS); /*  NT 4.0 has no problem */

	/* disable bad behavior using IOCTL: SIO_UDP_CONNRESET */
	status = WSAIoctl(fd, SIO_UDP_CONNRESET, &bNewBehavior,
			  sizeof(bNewBehavior), NULL, 0,
			  &dwBytesReturned, NULL, NULL);
	if (status != SOCKET_ERROR)
		return (BC_R_SUCCESS);
	else
	{
		LogError(_LOCAL_,
				 "WSAIoctl(SIO_UDP_CONNRESET, oldBehaviour) %s",
				 "failed");
		return (BC_R_UNEXPECTED);
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : BCSockEvent
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCSockEvent, 1000);

BCSockEvent::BCSockEvent()
		: BCTaskEvent()
		, result(BC_R_SUCCESS)
		, minimum(0)
		, n(0)
		, offset(0)
		, region_size(0)
		, bufferlist(NULL)
		, bufferlist_size(0)
		, timestamp(0)
		, attributes(0)
{
	memzero(&region, sizeof(region));
	memzero(&address, sizeof(address));
	memzero(&pktinfo, sizeof(pktinfo));
}

BCSockEvent::BCSockEvent(
	void *sender,
	BCEventType type,
	LPFN_BCTaskAction action,
	const void *arg)
		: BCTaskEvent(sender, type, action, arg)
		, result(BC_R_SUCCESS)
		, minimum(0)
		, n(0)
		, offset(0)
		, region_size(0)
		, bufferlist(NULL)
		, timestamp(0)
		, attributes(0)
{
	memzero(&region, sizeof(region));
	memzero(&address, sizeof(address));
	memzero(&pktinfo, sizeof(pktinfo));
}

BCSockEvent::~BCSockEvent()
{
	//
}

void BCSockEvent::SetAddress(BCSocket *sock, BCSockAddrS *pAddress)
{
	if (sock->m_eType == bc_sockettype_udp)
	{
		if (pAddress != NULL)
			address = *pAddress;
		else
			address = sock->m_sAddress;
	}
	else if (sock->m_eType == bc_sockettype_tcp)
	{
		ASSERT(pAddress == NULL);
		address = sock->m_sAddress;
	}
}

void BCSockEvent::Destroy()
{
	delete this;
}

#ifdef _DEBUG
ULONGLONG BCSockEvent::GetAllocCount()
{
	return s_alloc.GetAllocCount();
}
#endif

///////////////////////////////////////////////////////////////////////////////
// class : BCSockICEvent
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCSockICEvent, 10);

BCSockICEvent::BCSockICEvent()
		: BCTaskEvent()
		, newsocket(NULL)
		, result(BC_R_SUCCESS)
{
	memzero(&address, sizeof(address));
}

BCSockICEvent::BCSockICEvent(
	void *sender,
	BCEventType type,
	LPFN_BCTaskAction action,
	const void *arg)
		: BCTaskEvent(sender, type, action, arg)
		, newsocket(NULL)
		, result(BC_R_SUCCESS)
{
	memzero(&address, sizeof(address));
}

BCSockICEvent::~BCSockICEvent()
{
	//
}

void BCSockICEvent::Destroy()
{
	delete this;
}

#ifdef _DEBUG
ULONGLONG BCSockICEvent::GetAllocCount()
{
	return s_alloc.GetAllocCount();
}
#endif

///////////////////////////////////////////////////////////////////////////////
// class : BCSockOCEvent
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCSockOCEvent, 10);

BCSockOCEvent::BCSockOCEvent()
		: BCTaskEvent()
		, result(BC_R_SUCCESS)
{
	//
}

BCSockOCEvent::BCSockOCEvent(
	void *sender,
	BCEventType type,
	LPFN_BCTaskAction action,
	const void *arg)
		: BCTaskEvent(sender, type, action, arg)
		, result(BC_R_SUCCESS)
{
	//
}

BCSockOCEvent::~BCSockOCEvent()
{
	//
}

void BCSockOCEvent::Destroy()
{
	delete this;
}

#ifdef _DEBUG
ULONGLONG BCSockOCEvent::GetAllocCount()
{
	return s_alloc.GetAllocCount();
}
#endif

///////////////////////////////////////////////////////////////////////////////
// class : BCSocket
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCSocket, 200);

BCSocket::BCSocket()
		: BCMagic(SOCKET_MAGIC)
		, m_pMgr(NULL)
		, m_eType(bc_sockettype_unknown)
		, m_nRef(0)
		, m_nFD(INVALID_SOCKET)
		, m_nPF(0)
		, m_pTag(NULL)
		, m_pConnectEvent(NULL)
		, m_bListener(0)
		, m_bConnected(0)
		, m_bPendingConnect(0)
		, m_bBound(0)
		, m_nPendingIocp(0)
		, m_nPendingRecv(0)
		, m_nPendingSend(0)
		, m_nPendingAccept(0)
		, m_eState(0)
		, m_nStateLineNO(0)
		, m_nInRecoveryCount(0)
{
	memzero(m_sIoVector, sizeof(m_sIoVector));
	memzero(m_szName, sizeof(m_szName));
	memzero(&m_sRecvBuf, sizeof(m_sRecvBuf));
	memzero(&m_sAddress, sizeof(m_sAddress));
	_set_state(this, SOCK_INITIALIZED);
	memzero(m_szBuffer, sizeof(m_szBuffer));
	m_sRecvBuf.len = BCSOCKET_RECVBUF_SIZE;
	m_sRecvBuf.consume_position = m_sRecvBuf.base;
	m_sRecvBuf.remaining = 0;
	m_sRecvBuf.base = m_szBuffer;
}

BCSocket::~BCSocket()
{
	//
}

void BCSocket::_Destroy()
{
	delete this;
}

/*
 * Associate a socket with an IO Completion Port.  This allows us to queue events for it
 * and have our worker pool of threads process them.
 */
void BCSocket::_IocpUpdate()
{
	HANDLE hiocp;
	char strbuf[BC_STRERRORSIZE];

	ASSERT(VALID_SOCKET(this));

	hiocp = CreateIoCompletionPort((HANDLE)m_nFD,
	                               m_pMgr->m_hIoCompletionPort, (ULONG_PTR)this, 0);

	if (hiocp == NULL)
	{
		DWORD errval = GetLastError();
		bc_strerror(errval, strbuf, sizeof(strbuf));
		LogFatal(_LOCAL_,
		         "iocompletionport_update: failed to open"
		         " io completion port: %s",
		         strbuf);

		/* XXXMLG temporary hack to make failures detected.
		 * This function should return errors to the caller, not
		 * exit here.
		 */
		LogFatal(_LOCAL_,
		         "CreateIoCompletionPort() failed "
		         "during initialization: %s",
		         strbuf);
		exit(1);
	}

	InterlockedIncrement(&m_pMgr->m_nIocpTotal);
}

/*
 * Routine to cleanup and then close the socket.
 * Only close the socket here if it is NOT associated
 * with an event, otherwise the WSAWaitForMultipleEvents
 * may fail due to the fact that the Wait should not
 * be running while closing an event or a socket.
 * The socket is locked before calling this function
 */
void BCSocket::_Close()
{
	if (m_nFD != INVALID_SOCKET)
	{
		closesocket(m_nFD);
		m_nFD = INVALID_SOCKET;
		_set_state(this, SOCK_CLOSED);
		InterlockedDecrement(&m_pMgr->m_nTotalSockets);
	}
}

int32_t BCSocket::_InternalSendMsg(
	SockIoCompletionInfo *lpo,
	struct msghdr *messagehdr,
	int flags,
	int *Error)
{
	int Result;
	DWORD BytesSent;
	DWORD Flags = flags;
	int total_sent;

	*Error = 0;
	Result = WSASendTo(m_nFD, messagehdr->msg_iov,
			   messagehdr->msg_iovlen, &BytesSent,
			   Flags, (SOCKADDR *)&messagehdr->to_addr,
			   messagehdr->to_addr_len, (LPWSAOVERLAPPED)lpo,
			   NULL);

	total_sent = (int)BytesSent;

	/* Check for errors.*/
	if (Result == SOCKET_ERROR)
	{
		*Error = WSAGetLastError();

		switch (*Error)
		{
		case WSA_IO_INCOMPLETE:
		case WSA_WAIT_IO_COMPLETION:
		case WSA_IO_PENDING:
		case NO_ERROR:		/* Strange, but okay */
			m_nPendingIocp++;
			m_nPendingSend++;
			break;

		default:
			return (-1);
			break;
		}
	}
	else
	{
		m_nPendingIocp++;
		m_nPendingSend++;
	}

	if (lpo != NULL)
		return (0);
	else
		return (total_sent);
}

void BCSocket::_QueueReceiveRequest()
{
	DWORD Flags = 0;
	DWORD NumBytes = 0;
	int total_bytes = 0;
	int Result;
	int Error;
	BOOL need_recovering = FALSE;
	WSABUF iov[1];
	SockIoCompletionInfo *lpo;
	BCRESULT bc_result;

	/*
	 * If we already have a receive pending, do nothing.
	 */
	if (m_nPendingRecv > 0)
		return;

	/*
	 * If no one is waiting, do nothing.
	 */
	if (m_lstRecvEvents.IsEmpty())
		return;

	ASSERT(m_sRecvBuf.remaining == 0);
	ASSERT(m_nFD != INVALID_SOCKET);

	iov[0].len = m_sRecvBuf.len;
	iov[0].buf = m_sRecvBuf.base;

	lpo = new SockIoCompletionInfo();
	ASSERT(lpo != NULL);
	lpo->request_type = SOCKET_RECV;

	m_sRecvBuf.from_addr_len = sizeof(m_sRecvBuf.from_addr);

	Error = 0;
	Result = WSARecvFrom((SOCKET)m_nFD, iov, 1,
			     &NumBytes, &Flags,
			     (SOCKADDR *)&m_sRecvBuf.from_addr,
			     &m_sRecvBuf.from_addr_len,
			     (LPWSAOVERLAPPED)lpo, NULL);

	/* Check for errors. */
	if (Result == SOCKET_ERROR) {
		Error = WSAGetLastError();

		switch (Error) {
		case WSA_IO_PENDING:
			m_nPendingIocp++;
			m_nPendingRecv++;
			break;

		case ERROR_HOST_UNREACHABLE:
			if (m_eType == bc_sockettype_udp)
			{
				LogError(_LOCAL_,
					 "WSARecvFrom ERROR_HOST_UNREACHABLE: trying to recover");
				need_recovering = TRUE;
				break;
			} else
				goto fail;

		case WSAENETRESET:
			if (m_eType == bc_sockettype_udp)
			{
				LogError(_LOCAL_,
					 "WSARecvFrom WSAENETRESET: trying to recover");
				need_recovering = TRUE;
				break;
			} else
				goto fail;

		case WSAECONNRESET:
			if (m_eType == bc_sockettype_udp)
			{
				LogError(_LOCAL_,
					 "WSARecvFrom WSAECONNRESET: trying to recover");
				need_recovering = TRUE;
				break;
			} else
				goto fail;

		default:
		fail:
			bc_result = bc_errno2resultx(Error, __FILE__, __LINE__);
			if ((bc_result == BC_R_UNEXPECTED) ||
			    (bc_result == BC_R_CONNECTIONRESET) ||
			    (bc_result == BC_R_HOSTUNREACH))
			{
				//LogError(_LOCAL_,
				//	"WSARecvFrom: Windows error code: %d, nw result %d",
				//	Error, bc_result);
			}
			_SendRecvDoneAbort(bc_result);
			break;
		}
	} else {
		/*
		 * The recv() finished immediately, but we will still get
		 * a completion event.  Rather than duplicate code, let
		 * that thread handle sending the data along its way.
		 */
		m_nPendingIocp++;
		m_nPendingRecv++;
		m_nInRecoveryCount = 0;
	}

#if 0
	LogError(_LOCAL_, "queue_io_request: fd %d result %d error %d",
		   m_nFD, Result, Error);
#endif

	CONSISTENT(this);

	if (need_recovering)
		_RecoverReceiveRequest(&lpo);
}

/*
 * (placeholder) Hard recovery, doing nothing useful today
 * (other than to avoid unlimited recursion).
 */
void BCSocket::_HardRecoverReceiveRequest()
{
	LogError(_LOCAL_,
			 "can't recover fd %d sock %p",
			 m_nFD, this);
	_SendRecvDoneAbort(BC_R_UNEXPECTED);
}

/*
 * Recovery from a Windows 2008 Server bug
 * (WSARecvFrom() getting an ERROR_HOST_UNREACHABLE).
 * Free the overlapped pointer and requeue a receive request.
 */
void BCSocket::_RecoverReceiveRequest(SockIoCompletionInfo **lplpo)
{
	if (*lplpo != NULL)
		delete *lplpo;
	*lplpo = NULL;

	/* limit recursion to 20 */
	if (m_nInRecoveryCount++ < 20)
		_QueueReceiveRequest();
	else
		_HardRecoverReceiveRequest();
}

void BCSocket::_BuildMsghdrSend(
	BCSockEvent *dev,
	struct msghdr *msg,
	char *cmsg,
	WSABUF *iov,
	SockIoCompletionInfo  *lpo)
{
	uint32_t iovcount, nBlockSize;
	size_t write_count;
	void *pBuffer;

	memzero(msg, sizeof(*msg));

	memcpy2(&msg->to_addr, &dev->address.type, dev->address.length);
	msg->to_addr_len = dev->address.length;

	iovcount = 0;
	write_count = 0;

	/*
	 * Single buffer I/O?  Skip what we've done so far in this region.
	 */
	if (dev->bufferlist == NULL)
	{
		uint32_t skip_count = dev->n;
		for (uint32_t i = 0; i < dev->region_size; i++)
		{
			if (skip_count >= dev->region[i].length)
			{
				skip_count -= dev->region[i].length;
			}
			else if (skip_count > 0)
			{
				write_count += dev->region[i].length - skip_count;
				iov[iovcount].buf = (char *)(dev->region[i].base + skip_count);
				iov[iovcount].len = dev->region[i].length - skip_count;
				skip_count = 0;
				iovcount++;
			}
			else
			{
				write_count += dev->region[i].length;
				iov[iovcount].buf = (char *)(dev->region[i].base);
				iov[iovcount].len = dev->region[i].length;
				iovcount++;
			}
		}
	}
	else
	{
		/*
		 * Multibuffer I/O.
		 * Skip the data in the buffer list that we have already written.
		 */
		dev->bufferlist->Rewind();
		dev->bufferlist->Forward(dev->n);
		pBuffer = dev->bufferlist->ReadBlock(INFINITE, nBlockSize);
		while(pBuffer != NULL && nBlockSize > 0)
		{
			ASSERT(iovcount < MAXSCATTERGATHER_SEND);

			iov[iovcount].buf = (char *)pBuffer;
			iov[iovcount].len = nBlockSize;
			write_count += nBlockSize;
			iovcount++;

			pBuffer = dev->bufferlist->ReadBlock(INFINITE, nBlockSize);
		}
	}

	msg->msg_iov = iov;
	msg->msg_iovlen = iovcount;
	msg->msg_totallen = write_count;
}


/*
 * map the error code
 */
int32_t BCSocket::_MapError(
	int32_t windows_errno,
	int32_t *bc_errno,
	char *errorstring,
	size_t bufsize)
{
	int32_t doreturn;
	switch (windows_errno)
	{
	case WSAECONNREFUSED:
		*bc_errno = BC_R_CONNREFUSED;
		if (m_bConnected)
			doreturn = DOIO_HARD;
		else
			doreturn = DOIO_SOFT;
		break;
	case WSAENETUNREACH:
	case ERROR_NETWORK_UNREACHABLE:
		*bc_errno = BC_R_NETUNREACH;
		if (m_bConnected)
			doreturn = DOIO_HARD;
		else
			doreturn = DOIO_SOFT;
		break;
	case ERROR_PORT_UNREACHABLE:
	case ERROR_HOST_UNREACHABLE:
	case WSAEHOSTUNREACH:
		*bc_errno = BC_R_HOSTUNREACH;
		if (m_bConnected)
			doreturn = DOIO_HARD;
		else
			doreturn = DOIO_SOFT;
		break;
	case WSAENETDOWN:
		*bc_errno = BC_R_NETDOWN;
		if (m_bConnected)
			doreturn = DOIO_HARD;
		else
			doreturn = DOIO_SOFT;
		break;
	case WSAEHOSTDOWN:
		*bc_errno = BC_R_HOSTDOWN;
		if (m_bConnected)
			doreturn = DOIO_HARD;
		else
			doreturn = DOIO_SOFT;
		break;
	case WSAEACCES:
		*bc_errno = BC_R_NOPERM;
		if (m_bConnected)
			doreturn = DOIO_HARD;
		else
			doreturn = DOIO_SOFT;
		break;
	case WSAECONNRESET:
	case WSAENETRESET:
	case WSAECONNABORTED:
	case WSAEDISCON:
		*bc_errno = BC_R_CONNECTIONRESET;
		if (m_bConnected)
			doreturn = DOIO_HARD;
		else
			doreturn = DOIO_SOFT;
		break;
	case WSAENOTCONN:
		*bc_errno = BC_R_NOTCONNECTED;
		if (m_bConnected)
			doreturn = DOIO_HARD;
		else
			doreturn = DOIO_SOFT;
		break;
	case ERROR_OPERATION_ABORTED:
	case ERROR_CONNECTION_ABORTED:
	case ERROR_REQUEST_ABORTED:
		*bc_errno = BC_R_CONNECTIONRESET;
		doreturn = DOIO_HARD;
		break;
	case WSAENOBUFS:
		*bc_errno = BC_R_NORESOURCES;
		doreturn = DOIO_HARD;
		break;
	case WSAEAFNOSUPPORT:
		*bc_errno = BC_R_FAMILYNOSUPPORT;
		doreturn = DOIO_HARD;
		break;
	case WSAEADDRNOTAVAIL:
		*bc_errno = BC_R_ADDRNOTAVAIL;
		doreturn = DOIO_HARD;
		break;
	case WSAEDESTADDRREQ:
		*bc_errno = BC_R_BADADDRESSFORM;
		doreturn = DOIO_HARD;
		break;
	case ERROR_NETNAME_DELETED:
		*bc_errno = BC_R_NETDOWN;
		doreturn = DOIO_HARD;
		break;
	default:
		*bc_errno = BC_R_IOERROR;
		doreturn = DOIO_HARD;
		break;
	}
	if (doreturn == DOIO_HARD)
	{
		bc_strerror(windows_errno, errorstring, bufsize);
	}
	return (doreturn);
}

void BCSocket::_FillRecv(BCSockEvent *dev)
{
	int copylen;

	ASSERT(dev->n < dev->minimum);
	ASSERT(m_sRecvBuf.remaining > 0);
	ASSERT(m_nPendingRecv == 0);

	if (m_eType == bc_sockettype_udp)
	{
		dev->address.length = m_sRecvBuf.from_addr_len;
		memcpy2(&dev->address.type, &m_sRecvBuf.from_addr,
		    m_sRecvBuf.from_addr_len);
		if (bc_sockaddr_getport(&dev->address) == 0)
		{
			m_sRecvBuf.remaining = 0;
			return;
		}
	}
	else if (m_eType == bc_sockettype_tcp)
	{
		dev->address = m_sAddress;
	}

	/*
	 * Run through the list of buffers we were given, and find the
	 * first one with space.  Once it is found, loop through, filling
	 * the buffers as much as possible.
	 */
	if (dev->bufferlist != NULL)
	{ // Multi-buffer receive
		dev->bufferlist->Write(m_sRecvBuf.consume_position, m_sRecvBuf.remaining);
		dev->n += m_sRecvBuf.remaining;
		m_sRecvBuf.consume_position = m_sRecvBuf.base;
		m_sRecvBuf.remaining = 0;
	}
	else if (dev->region_size == 1)
	{ // Single-buffer receive
		copylen = BCMIN(dev->region[0].length - dev->n, m_sRecvBuf.remaining);
		memcpy2(dev->region[0].base + dev->n, m_sRecvBuf.consume_position, copylen);
		m_sRecvBuf.consume_position += copylen;
		m_sRecvBuf.remaining -= copylen;
		dev->n += copylen;
	}
	else
	{
		ASSERT(0);
	}

	/*
	 * UDP receives are all-consuming.  That is, if we have 4k worth of
	 * data in our receive buffer, and the caller only gave us
	 * 1k of space, we will toss the remaining 3k of data.  TCP
	 * will keep the extra data around and use it for later requests.
	 */
	if (m_eType == bc_sockettype_udp)
		m_sRecvBuf.remaining = 0;
}

/*
 * Copy out as much data from the internal buffer to done events.
 * As each done event is filled, send it along its way.
 */
void BCSocket::_CompleteIoRecv()
{
	BCSockEvent *dev;

	/*
	 * If we are in the process of filling our buffer, we cannot
	 * touch it yet, so don't.
	 */
	if (m_nPendingRecv > 0)
		return;

	while (m_sRecvBuf.remaining > 0 && !m_lstRecvEvents.IsEmpty())
	{
		dev = m_lstRecvEvents.Begin();

		/*
		 * See if we have sufficient data in our receive buffer
		 * to handle this.  If we do, copy out the data.
		 */
		_FillRecv(dev);

		/*
		 * Did we satisfy it?
		 */
		if (dev->n >= dev->minimum)
		{
			dev->result = BC_R_SUCCESS;
			_SendRecvDoneEvent(&dev);
		}
	}
}

/*
 * Returns:
 *	DOIO_SUCCESS	The operation succeeded.  dev->result contains
 *			BC_R_SUCCESS.
 *
 *	DOIO_HARD	A hard or unexpected I/O error was encountered.
 *			dev->result contains the appropriate error.
 *
 *	DOIO_SOFT	A soft I/O error was encountered.  No senddone
 *			event was sent.  The operation should be retried.
 *
 *	No other return values are possible.
 */
int32_t BCSocket::_CompleteIoSend(
	BCSockEvent *dev,
	struct msghdr *messagehdr,
	int32_t cc,
	int32_t send_errno)
{
	char addrbuf[BC_SOCKADDR_FORMATSIZE];
	char strbuf[BC_STRERRORSIZE];

	if (send_errno != 0)
	{
		if (SOFT_ERROR(send_errno))
			return (DOIO_SOFT);

		return (_MapError(send_errno, (int32_t *)&dev->result,
			strbuf, sizeof(strbuf)));

		/*
		 * The other error types depend on whether or not the
		 * socket is UDP or TCP.  If it is UDP, some errors
		 * that we expect to be fatal under TCP are merely
		 * annoying, and are really soft errors.
		 *
		 * However, these soft errors are still returned as
		 * a status.
		 */
		bc_sockaddr_format(&dev->address, addrbuf, sizeof(addrbuf));
		bc_strerror(send_errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "completeio_send: %s: %s", addrbuf, strbuf);
		dev->result = bc_errno2resultx(send_errno, __FILE__, __LINE__);
		return (DOIO_HARD);
	}

	/*
	 * If we write less than we expected, update counters, poke.
	 */
	dev->n += cc;
	if (cc != messagehdr->msg_totallen)
		return (DOIO_SOFT);

	/*
	 * Exactly what we wanted to write.  We're done with this
	 * entry.  Post its completion event.
	 */
	dev->result = BC_R_SUCCESS;
	return (DOIO_SUCCESS);
}

int32_t BCSocket::_StartIoSend(
	BCSockEvent *dev,
	int32_t *nbytes,
	int32_t *send_errno)
{
	char *cmsg = NULL;
	char strbuf[BC_STRERRORSIZE];
	SockIoCompletionInfo *lpo;
	int status;
	struct msghdr *msg_hdr;

	lpo = new SockIoCompletionInfo();
	ASSERT(lpo != NULL);
	lpo->request_type = SOCKET_SEND;
	lpo->dev = dev;
	msg_hdr = &lpo->messagehdr;
	memset(msg_hdr, 0, sizeof(struct msghdr));

	_BuildMsghdrSend(dev, msg_hdr, cmsg, m_sIoVector, lpo);

	*nbytes = _InternalSendMsg(lpo, msg_hdr, 0, send_errno);

	if (*nbytes < 0)
	{
		/*
		 * I/O has been initiated
		 * completion will be through the completion port
		 */
		if (PENDING_ERROR(*send_errno))
		{
			status = DOIO_PENDING;
			goto done;
		}

		if (SOFT_ERROR(*send_errno))
		{
			status = DOIO_SOFT;
			goto done;
		}

		/*
		 * If we got this far then something is wrong
		 */
		if (0)
		{
			bc_strerror(*send_errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
				   "startio_send: internal_sendmsg(%d) %d "
				   "bytes, err %d/%s",
				   m_nFD, *nbytes, *send_errno, strbuf);
		}
		status = DOIO_HARD;
		// Below code is add by antoniozhou
		{
			delete lpo;
		}
		// add end.
		goto done;
	}
	dev->result = BC_R_SUCCESS;
	status = DOIO_SOFT;
 done:
	_set_state(this, SOCK_DATA);
	return (status);
}

/*
 * Maybe free the socket.
 *
 * This function will verify tht the socket is no longer in use in any way,
 * either internally or externally.  This is the only place where this
 * check is to be made; if some bit of code believes that IT is done with
 * the socket (e.g., some reference counter reaches zero), it should call
 * this function.
 *
 * When calling this function, the socket must be locked, and the manager
 * must be unlocked.
 *
 * When this function returns, *socketp will be NULL.  No tricks to try
 * to hold on to this pointer are allowed.
 */
void BCSocket::_MaybeFree(BCSocket *pSocket, int lineno)
{
	ASSERT(VALID_SOCKET(pSocket));
	CONSISTENT(pSocket);

	if (pSocket->m_nPendingIocp > 0
	    || pSocket->m_nPendingRecv > 0
	    || pSocket->m_nPendingSend > 0
	    || pSocket->m_nPendingAccept > 0
	    || pSocket->m_nRef > 0
	    || pSocket->m_bPendingConnect == 1
	    || !pSocket->m_lstRecvEvents.IsEmpty()
	    || !pSocket->m_lstSendEvents.IsEmpty()
	    || !pSocket->m_lstAcceptEvents.IsEmpty()
	    || pSocket->m_nFD != INVALID_SOCKET)
	{
		pSocket->m_sLock.Unlock();
		return;
	}
	pSocket->m_sLock.Unlock();

	_Free(pSocket, lineno);
}

void BCSocket::_MaybeFree(BCSocket **ppSocket, int32_t lineno)
{
	BCSocket *pSocket = *ppSocket;
	*ppSocket = NULL;
	_MaybeFree(pSocket, lineno);
}

void BCSocket::_Free(BCSocket *pSocket, int lineno)
{
	BCSocketMgr *pMgr;

	pMgr = pSocket->m_pMgr;
	/*
	 * Seems we can free the socket after all.
	 */
	pSocket->m_nMagic = 0;

	pMgr->m_sLock.Lock();
	pSocket->RemoveFromList();
	pSocket->_Destroy();

	if (pMgr->m_lstSockets.IsEmpty())
		pMgr->m_sCondShutdownOK.Signal();
	pMgr->m_sLock.Unlock();
}

void BCSocket::_Free(BCSocket **ppSocket, int32_t lineno)
{
	BCSocket *pSocket = *ppSocket;
	*ppSocket = NULL;
	_Free(pSocket, lineno);
}

/*
 * Create a new 'type' socket managed by 'manager'.  Events
 * will be posted to 'task' and when dispatched 'action' will be
 * called with 'arg' as the arg value.  The new socket is returned
 * in 'socketp'.
 */
BCRESULT BCSocket::Create(
	BCSocketMgr *pMgr,
	int nPf,
	BCSocketTypeE eType)
{
	BCRESULT result;
#if defined(USE_CMSG)
	int on = 1;
#endif
#if defined(SO_RCVBUF)
	BC_SOCKADDR_LEN_T optlen;
	int size;
#endif
	int socket_errno;
	char strbuf[BC_STRERRORSIZE];

	ASSERT(VALID_MANAGER(pMgr));
	ASSERT(m_eType != bc_sockettype_fdwatch);

	m_pMgr = pMgr;
	m_eType = eType;
	m_nPF = nPf;

	switch (m_eType)
	{
	case bc_sockettype_udp:
		m_nFD = socket(m_nPF, SOCK_DGRAM, IPPROTO_UDP);
		if (m_nFD != INVALID_SOCKET)
		{
			result = connection_reset_fix(m_nFD);
			if (result != BC_R_SUCCESS)
			{
				LogError(_LOCAL_,
					"closed %d %d %d con_reset_fix_failed",
					m_nPendingRecv, m_nPendingSend,
					m_nRef);
				closesocket(m_nFD);
				_set_state(this, SOCK_CLOSED);
				m_nFD = INVALID_SOCKET;
				_Free(this, __LINE__);
				return (result);
			}
		}
		break;
	case bc_sockettype_tcp:
		m_nFD = socket(m_nPF, SOCK_STREAM, IPPROTO_TCP);
		break;
	}

	if (m_nFD == INVALID_SOCKET)
	{
		socket_errno = WSAGetLastError();
		_Free(this, __LINE__);

		switch (socket_errno)
		{
		case WSAEMFILE:
		case WSAENOBUFS:
			return (BC_R_NORESOURCES);

		case WSAEPROTONOSUPPORT:
		case WSAEPFNOSUPPORT:
		case WSAEAFNOSUPPORT:
			return (BC_R_FAMILYNOSUPPORT);

		default:
			bc_strerror(socket_errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_, "socket() %s: %s", "failed", strbuf);
			return (BC_R_UNEXPECTED);
		}
	}

	result = make_nonblock(m_nFD);
	if (result != BC_R_SUCCESS)
	{
		LogError(_LOCAL_,
			"closed %d %d %d make_nonblock_failed",
			m_nPendingRecv, m_nPendingSend,
			m_nRef);
		closesocket(m_nFD);
		m_nFD = INVALID_SOCKET;
		_Free(this, __LINE__);
		return (result);
	}


#if defined(USE_CMSG) || defined(SO_RCVBUF)
	if (m_eType == bc_sockettype_udp) {

#if defined(USE_CMSG)
#if defined(BC_PLATFORM_HAVEIPV6)
#ifdef IPV6_RECVPKTINFO
		/* 2292bis */
		if ((m_nPF == AF_INET6)
		    && (setsockopt(m_nFD, IPPROTO_IPV6, IPV6_RECVPKTINFO,
				   (char *)&on, sizeof(on)) < 0))
		{
			bc_strerror(WSAGetLastError(), strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
					 "setsockopt(%d, IPV6_RECVPKTINFO) "
					 "%s: %s", m_nFD, "failed", strbuf);
		}
#else
		/* 2292 */
		if ((m_nPF == AF_INET6)
		    && (setsockopt(m_nFD, IPPROTO_IPV6, IPV6_PKTINFO,
				   (char *)&on, sizeof(on)) < 0))
		{
			bc_strerror(WSAGetLastError(), strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
					 "setsockopt(%d, IPV6_PKTINFO) %s: %s",
					 m_nFD, "failed", strbuf);
		}
#endif /* IPV6_RECVPKTINFO */
#ifdef IPV6_USE_MIN_MTU	/*2292bis, not too common yet*/
		/* use minimum MTU */
		if (m_nPF == AF_INET6)
		{
			(void)setsockopt(m_nFD, IPPROTO_IPV6,
					 IPV6_USE_MIN_MTU,
					 (char *)&on, sizeof(on));
		}
#endif
#endif /* BC_PLATFORM_HAVEIPV6 */
#endif /* defined(USE_CMSG) */

#if defined(SO_RCVBUF)
	       optlen = sizeof(size);
	       if (getsockopt(m_nFD, SOL_SOCKET, SO_RCVBUF,
			      (char *)&size, (int *)&optlen) >= 0 &&
		    size < RCVBUFSIZE)
		   {
		       size = RCVBUFSIZE;
		       (void)setsockopt(m_nFD, SOL_SOCKET, SO_RCVBUF,
					(char *)&size, sizeof(size));
	       }
#endif

	}
#endif /* defined(USE_CMSG) || defined(SO_RCVBUF) */

	_set_state(this, SOCK_OPEN);
	m_nRef = 1;

	_IocpUpdate();

	/*
	 * Note we don't have to lock the socket like we normally would because
	 * there are no external references to it yet.
	 */
	m_pMgr->m_sLock.Lock();
	m_pMgr->m_lstSockets.PushBack(this);
	InterlockedIncrement(&m_pMgr->m_nTotalSockets);
	m_pMgr->m_sLock.Unlock();

	return (BC_R_SUCCESS);
}

/*
 * Attach to a socket.  Caller must explicitly detach when it is done.
 */
void BCSocket::Attach(BCSocket **ppSocket)
{
	ASSERT(VALID_SOCKET(this));
	ASSERT(ppSocket != NULL && *ppSocket == NULL);

	m_sLock.Lock();
	CONSISTENT(this);
	m_nRef++;
	m_sLock.Unlock();

	*ppSocket = this;
}

/*
 * Dereference a socket.  If this is the last reference to it, clean things
 * up by destroying the socket.
 */
void BCSocket::Detach(BCSocket **ppSocket)
{
	BCSocket *pSocket;
	BOOL kill_socket = FALSE;

	ASSERT(ppSocket != NULL);
	pSocket = (BCSocket *)*ppSocket;
	ASSERT(VALID_SOCKET(pSocket));
	ASSERT(pSocket == this);
	ASSERT(m_eType != bc_sockettype_fdwatch);

	m_sLock.Lock();
	CONSISTENT(this);
	ASSERT(m_nRef > 0);
	m_nRef--;

	if (m_nRef == 0 && m_nFD != INVALID_SOCKET)
	{
		closesocket(m_nFD);
		m_nFD = INVALID_SOCKET;
		_set_state(this, SOCK_CLOSED);
	}

	_MaybeFree(this, __LINE__);

	*ppSocket = NULL;
}

/*
 * Dequeue an item off the given socket's read queue, set the result code
 * in the done event to the one provided, and send it to the task it was
 * destined for.
 *
 * If the event to be sent is on a list, remove it before sending.  If
 * asked to, send and detach from the task as well.
 *
 * Caller must have the socket locked if the event is attached to the socket.
 */
void BCSocket::_SendRecvDoneEvent(BCSockEvent **dev)
{
	BCTask *pTask;

	ASSERT(dev != NULL && *dev != NULL);

	pTask = (BCTask *)(*dev)->ev_sender;
	(*dev)->ev_sender = this;

	(*dev)->RemoveFromList();

	if (((*dev)->attributes & BC_SOCKEVENTATTR_ATTACHED)
	    == BC_SOCKEVENTATTR_ATTACHED)
	{
		pTask->SendAndDetach((BCTaskEvent **)dev);
	}
	else
	{
		pTask->Send((BCTaskEvent **)dev);
	}

	*dev = NULL;

	CONSISTENT(this);
}

/*
 * See comments for send_recvdone_event() above.
 */
void BCSocket::_SendSendDoneEvent(BCSockEvent **dev)
{
	BCTask *pTask;

	ASSERT(dev != NULL && *dev != NULL);

	pTask = (BCTask *)(*dev)->ev_sender;
	(*dev)->ev_sender = this;

	(*dev)->RemoveFromList();

	if (((*dev)->attributes & BC_SOCKEVENTATTR_ATTACHED)
	    == BC_SOCKEVENTATTR_ATTACHED)
	{
		pTask->SendAndDetach((BCTaskEvent **)dev);
	}
	else
	{
		pTask->Send((BCTaskEvent **)dev);
	}

	*dev = NULL;

	CONSISTENT(this);
}

/*
 * See comments for send_recvdone_event() above.
 */
void BCSocket::_SendAcceptDoneEvent(BCSockICEvent **adev)
{
	BCTask *pTask;

	ASSERT(adev != NULL && *adev != NULL);

	pTask = (BCTask *)(*adev)->ev_sender;
	(*adev)->ev_sender = this;

	(*adev)->RemoveFromList();

	pTask->SendAndDetach((BCTaskEvent **)adev);

	*adev = NULL;

	CONSISTENT(this);
}

/*
 * See comments for send_recvdone_event() above.
 */
void BCSocket::_SendConnectDoneEvent(BCSockOCEvent **cdev)
{
	BCTask *pTask;

	ASSERT(cdev != NULL && *cdev != NULL);

	pTask = (BCTask *)(*cdev)->ev_sender;
	(*cdev)->ev_sender = this;

	m_pConnectEvent = NULL;

	pTask->SendAndDetach((BCTaskEvent **)cdev);

	*cdev = NULL;

	CONSISTENT(this);
}

/*
 * On entry to this function, the event delivered is the internal
 * readable event, and the first item on the accept_list should be
 * the done event we want to send.  If the list is empty, this is a no-op,
 * so just close the new connection, unlock, and return.
 *
 * Note the socket is locked before entering here
 */
void BCSocket::_InternalAccept(SockIoCompletionInfo *lpo, int32_t accept_errno)
{
	BCSockICEvent *adev;
	BCRESULT result = BC_R_SUCCESS;
	BCSocket *nsock;
	struct sockaddr *localaddr;
	int localaddr_len = sizeof(*localaddr);
	struct sockaddr *remoteaddr;
	int remoteaddr_len = sizeof(*remoteaddr);
	int ret;

	ASSERT(VALID_SOCKET(this));
	m_sLock.Lock();
	CONSISTENT(this);

	ASSERT(m_bListener);

	ASSERT(m_nPendingIocp > 0);
	m_nPendingIocp--;
	ASSERT(m_nPendingAccept > 0);
	m_nPendingAccept--;

	adev = lpo->adev;

	/*
	 * If the event is no longer in the list we can just return.
	 */
	if (!_IsAcceptDoneActive(adev))
		goto done;

	nsock = adev->newsocket;

	/*
	 * Pull off the done event.
	 */
	adev->RemoveFromList();

	/*
	 * Extract the addresses from the socket, copy them into the structure,
	 * and return the new socket.
	 */
	BCGetAcceptExSockaddrs(lpo->acceptbuffer, 0,
		sizeof(SOCKADDR_STORAGE) + 16, sizeof(SOCKADDR_STORAGE) + 16,
		(LPSOCKADDR *)&localaddr, &localaddr_len,
		(LPSOCKADDR *)&remoteaddr, &remoteaddr_len);
	memcpy2(&adev->address.type, remoteaddr, remoteaddr_len);
	adev->address.length = remoteaddr_len;
	nsock->m_sAddress = adev->address;
	nsock->m_nPF = adev->address.type.sa.sa_family;

	result = make_nonblock(adev->newsocket->m_nFD);
	ASSERT(result == BC_R_SUCCESS);

	ret = setsockopt(nsock->m_nFD, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
		(char *)&m_nFD, sizeof(m_nFD));
	ASSERT(ret == 0);

	/*
	 * Hook it up into the manager.
	 */
	nsock->m_bBound = 1;
	nsock->m_bConnected = 1;
	_set_state(nsock, SOCK_OPEN);

	nsock->m_pMgr->m_sLock.Lock();
	nsock->m_pMgr->m_lstSockets.PushBack(nsock);
	InterlockedIncrement(&nsock->m_pMgr->m_nTotalSockets);
	nsock->m_pMgr->m_sLock.Unlock();

	adev->result = result;
	_SendAcceptDoneEvent(&adev);

done:
	CONSISTENT(this);
	m_sLock.Unlock();

	delete (SockAcceptBuffer *)lpo->acceptbuffer;
	lpo->acceptbuffer = NULL;
}

/*
 * Called when a socket with a pending connect() finishes.
 * Note that the socket is locked before entering.
 */
void BCSocket::_InternalConnect(SockIoCompletionInfo *lpo, int32_t connect_errno)
{
	BCSockOCEvent *cdev;
	char strbuf[BC_STRERRORSIZE];
	int ret;

	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();

	ASSERT(m_nPendingIocp > 0);
	m_nPendingIocp--;
	ASSERT(m_bPendingConnect == 1);
	m_bPendingConnect = 0;

	/*
	 * Has this event been canceled?
	 */
	cdev = lpo->cdev;
	if (!_IsConnectDoneActive(cdev))
	{
		m_bPendingConnect = 0;
		if (m_nFD != INVALID_SOCKET)
		{
			closesocket(m_nFD);
			m_nFD = INVALID_SOCKET;
			_set_state(this, SOCK_CLOSED);
		}
		CONSISTENT(this);
		m_sLock.Unlock();
		return;
	}

	/*
	 * Check possible Windows network event error status here.
	 */
	if (connect_errno != 0)
	{
		/*
		 * If the error is SOFT, just try again on this
		 * fd and pretend nothing strange happened.
		 */
		if (SOFT_ERROR(connect_errno) ||
		    connect_errno == WSAEINPROGRESS)
		{
			m_bPendingConnect = 1;
			CONSISTENT(this);
			m_sLock.Unlock();
			return;
		}

		/*
		 * Translate other errors into BC_R_* flavors.
		 */
		switch (connect_errno)
		{
#define ERROR_MATCH(a, b) case a: cdev->result = b; break;
			ERROR_MATCH(WSAEACCES, BC_R_NOPERM);
			ERROR_MATCH(WSAEADDRNOTAVAIL, BC_R_ADDRNOTAVAIL);
			ERROR_MATCH(WSAEAFNOSUPPORT, BC_R_ADDRNOTAVAIL);
			ERROR_MATCH(WSAECONNREFUSED, BC_R_CONNREFUSED);
			ERROR_MATCH(WSAEHOSTUNREACH, BC_R_HOSTUNREACH);
			ERROR_MATCH(WSAEHOSTDOWN, BC_R_HOSTDOWN);
			ERROR_MATCH(WSAENETUNREACH, BC_R_NETUNREACH);
			ERROR_MATCH(WSAENETDOWN, BC_R_NETDOWN);
			ERROR_MATCH(WSAENOBUFS, BC_R_NORESOURCES);
			ERROR_MATCH(WSAECONNRESET, BC_R_CONNECTIONRESET);
			ERROR_MATCH(WSAECONNABORTED, BC_R_CONNECTIONRESET);
			ERROR_MATCH(WSAETIMEDOUT, BC_R_TIMEDOUT);
#undef ERROR_MATCH
		default:
			cdev->result = BC_R_UNEXPECTED;
			bc_strerror(connect_errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
					 "internal_connect: connect() %s", strbuf);
		}
	}
	else
	{
		ret = setsockopt(m_nFD, SOL_SOCKET,
			SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
		ASSERT(ret == 0);
		cdev->result = BC_R_SUCCESS;
		m_bConnected = 1;
	}

	_SendConnectDoneEvent(&cdev);

	m_sLock.Unlock();
}

/*
 * Loop through the socket, returning BC_R_EOF for each done event pending.
 */
void BCSocket::_SendRecvDoneAbort(BCRESULT result)
{
	BCSockEvent *dev;

	while (!m_lstRecvEvents.IsEmpty())
	{
		dev = m_lstRecvEvents.Begin();
		dev->result = result;
		_SendRecvDoneEvent(&dev);
	}
}

/*
 * Take the data we received in our private buffer, and if any recv() calls on
 * our list are satisfied, send the corresponding done event.
 *
 * If we need more data (there are still items on the recv_list after we consume all
 * our data) then arrange for another system recv() call to fill our buffers.
 */
void BCSocket::_InternalRecv(int32_t nbytes)
{
	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * If we got here, the I/O operation succeeded.  However, we might still have removed this
	 * event from our notification list (or never placed it on it due to immediate completion.)
	 * Handle the reference counting here, and handle the cancellation event just after.
	 */
	ASSERT(m_nPendingIocp > 0);
	m_nPendingIocp--;
	ASSERT(m_nPendingRecv > 0);
	m_nPendingRecv--;

	/*
	 * The only way we could have gotten here is that our I/O has successfully completed.
	 * Update our pointers, and move on.  The only odd case here is that we might not
	 * have received enough data on a TCP stream to satisfy the minimum requirements.  If
	 * this is the case, we will re-issue the recv() call for what we need.
	 *
	 * We do check for a recv() of 0 bytes on a TCP stream.  This means the remote end
	 * has closed.
	 */
	if (nbytes == 0 && m_eType == bc_sockettype_tcp)
	{
		_SendRecvDoneAbort(BC_R_EOF);
		_MaybeFree(this, __LINE__);
		return;
	}
	m_sRecvBuf.remaining = nbytes;
	m_sRecvBuf.consume_position = m_sRecvBuf.base;
	_CompleteIoRecv();

	/*
	 * If there are more receivers waiting for data, queue another receive
	 * here.
	 */
	_QueueReceiveRequest();

	/*
	 * Unlock and/or destroy if we are the last thing this socket has left to do.
	 */
	_MaybeFree(this, __LINE__);
}

void BCSocket::_InternalSend(
	BCSockEvent *dev,
	struct msghdr *messagehdr,
	int32_t nbytes,
	int32_t send_errno,
	SockIoCompletionInfo *lpo)
{
	/*
	 * Find out what socket this is and lock it.
	 */
	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();
	CONSISTENT(this);

	ASSERT(m_nPendingIocp > 0);
	m_nPendingIocp--;
	ASSERT(m_nPendingSend > 0);
	m_nPendingSend--;

	/* If the event is no longer in the list we can just return */
	if (!_IsSendDoneActive(dev))
		goto done;

	/*
	 * Set the error code and send things on its way.
	 */
	switch (_CompleteIoSend(dev, messagehdr, nbytes, send_errno))
	{
	case DOIO_SOFT:
		break;
	case DOIO_HARD:
	case DOIO_SUCCESS:
		_SendSendDoneEvent(&dev);
		break;
	}

 done:
	_MaybeFree(this, __LINE__);
}

/*
 * These return if the done event passed in is on the list (or for connect, is
 * the one we're waiting for.  Using these ensures we will not double-send an
 * event.
 */
BOOL BCSocket::_IsSendDoneActive(BCSockEvent *dev)
{
	return m_lstSendEvents.IsExist(dev);
}

BOOL BCSocket::_IsAcceptDoneActive(BCSockICEvent *dev)
{
	return m_lstAcceptEvents.IsExist(dev);
}

BOOL BCSocket::_IsConnectDoneActive(BCSockOCEvent *dev)
{
	return (m_pConnectEvent == dev ? TRUE : FALSE);
}

void BCSocket::_QueueReceiveEvent(
	BCTask *task,
	BCSockEvent *dev)
{
	BCTask *ntask = NULL;

	task->Attach(&ntask);
	dev->attributes |= BC_SOCKEVENTATTR_ATTACHED;

	/*
	 * Enqueue the request.
	 */
	dev->RemoveFromList();
	m_lstRecvEvents.PushBack(dev);
}

/*
 * Check the pending receive queue, and if we have data pending, give it to this
 * caller.  If we have none, queue an I/O request.  If this caller is not the first
 * on the list, then we will just queue this event and return.
 *
 * Caller must have the socket locked.
 */
BCRESULT BCSocket::_Recv(
	BCSockEvent *dev,
	BCTask *task,
	uint32_t flags)
{
	int cc = 0;
	BCTask *ntask = NULL;
	BCRESULT result = BC_R_SUCCESS;
	int recv_errno = 0;

	dev->ev_sender = task;

	if (m_nFD == INVALID_SOCKET)
		return (BC_R_EOF);

	/*
	 * Queue our event on the list of things to do.  Call our function to
	 * attempt to fill buffers as much as possible, and return done events.
	 * We are going to lie about our handling of the BC_SOCKFLAG_IMMEDIATE
	 * here and tell our caller that we could not satisfy it immediately.
	 */
	_QueueReceiveEvent(task, dev);
	if ((flags & BC_SOCKFLAG_IMMEDIATE) != 0)
		result = BC_R_INPROGRESS;

	_CompleteIoRecv();

	/*
	 * If there are more receivers waiting for data, queue another receive
	 * here.  If the
	 */
	_QueueReceiveRequest();

	return (result);
}

BCRESULT BCSocket::Recv(
	BCRegionS *region,
	uint32_t minimum,
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg)
{
	BCSockEvent *dev;
	BCRESULT ret;

	ASSERT(VALID_SOCKET(this));
	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}
	ASSERT(action != NULL);

	ASSERT(VALID_MANAGER(m_pMgr));

	ASSERT(m_bBound);

	dev = new BCSockEvent(this, BC_SOCKEVENT_RECVDONE, action, arg);
	if (dev == NULL)
	{
		m_sLock.Unlock();
		return (BC_R_NOMEMORY);
	}

	ret = Recv2(region, minimum, task, dev, 0);
	m_sLock.Unlock();
	return (ret);
}

BCRESULT BCSocket::Recv2(
	BCRegionS *pRegion,
	uint32_t minimum,
	BCTask *task,
	BCSockEvent *pEvent,
	uint32_t nFlags)
{
	BCRESULT ret;

	ASSERT(VALID_SOCKET(this));
	m_sLock.Lock();
	CONSISTENT(this);

	pEvent->result = BC_R_UNEXPECTED;
	pEvent->ev_sender = this;
	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	pEvent->bufferlist = NULL;
	pEvent->region[0] = *pRegion;
	pEvent->region_size = 1;
	pEvent->n = 0;
	pEvent->offset = 0;
	pEvent->attributes = 0;

	/*
	 * UDP sockets are always partial read.
	 */
	if (m_eType == bc_sockettype_udp)
	{
		pEvent->minimum = 1;
	}
	else
	{
		if (minimum == 0)
		{
			pEvent->minimum = pRegion->length;
		}
		else
		{
			pEvent->minimum = minimum;
		}
	}

	ret = _Recv(pEvent, task, nFlags);
	m_sLock.Unlock();
	return (ret);
}

BCRESULT BCSocket::RecvV(
	BCBuffer *buflist,
	uint32_t minimum,
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg)
{
	BCSockEvent *dev;
	uint32_t iocount;
	BCRESULT ret;

	ASSERT(VALID_SOCKET(this));
	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * Make sure that the socket is not closed.  XXXMLG change error here?
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	ASSERT(buflist != NULL);
	ASSERT(VALID_BCBUFFER(buflist));
	ASSERT(task != NULL);
	ASSERT(action != NULL);

	ASSERT(VALID_MANAGER(m_pMgr));

	iocount = INFINITE;
	(void)buflist->GetWritableBlock(iocount);

	ASSERT(iocount > 0);

	ASSERT(m_bBound);

	dev = new BCSockEvent(this, BC_SOCKEVENT_RECVDONE, action, arg);
	if (dev == NULL)
	{
		m_sLock.Unlock();
		return (BC_R_NOMEMORY);
	}

	/*
	 * UDP sockets are always partial read
	 */
	if (m_eType == bc_sockettype_udp)
	{
		dev->minimum = 1;
	}
	else
	{
		if (minimum == 0)
		{
			dev->minimum = iocount;
		}
		else
		{
			dev->minimum = minimum;
		}
	}

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	dev->bufferlist = buflist;

	ret = _Recv(dev, task, 0);

	m_sLock.Unlock();
	return (ret);
}

BCRESULT BCSocket::RecvV2(
	BCBuffer *buflist,
	uint32_t minimum,
	BCTask *task,
	BCSockEvent *pEvent,
	uint32_t nFlags)
{
	uint32_t iocount;
	BCRESULT ret;

	ASSERT(VALID_SOCKET(this));
	m_sLock.Lock();
	CONSISTENT(this);

	pEvent->result = BC_R_UNEXPECTED;
	pEvent->ev_sender = this;

	/*
	 * Make sure that the socket is not closed.  XXXMLG change error here?
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	ASSERT(buflist != NULL);
	ASSERT(VALID_BCBUFFER(buflist));
	ASSERT(task != NULL);
	ASSERT(VALID_MANAGER(m_pMgr));

	pEvent->n = 0;
	pEvent->offset = 0;
	pEvent->attributes = 0;

	iocount = INFINITE;
	(void)buflist->GetWritableBlock(iocount);

	ASSERT(iocount > 0);

	ASSERT(m_bBound);

	/*
	 * UDP sockets are always partial read
	 */
	if (m_eType == bc_sockettype_udp)
	{
		pEvent->minimum = 1;
	}
	else
	{
		if (minimum == 0)
		{
			pEvent->minimum = iocount;
		}
		else
		{
			pEvent->minimum = minimum;
		}
	}

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	pEvent->bufferlist = buflist;

	ret = _Recv(pEvent, task, 0);

	m_sLock.Unlock();
	return (ret);
}

/*
 * Caller must have the socket locked.
 */
BCRESULT BCSocket::_Send(
	BCSockEvent *dev,
	BCTask *task,
	BCSockAddrS *pAddress,
	struct in6_pktinfo *pktinfo,
	uint32_t flags)
{
	int io_state;
	int send_errno = 0;
	int cc = 0;
	BCTask *ntask = NULL;
	BCRESULT result = BC_R_SUCCESS;

	ASSERT(!pAddress || (pAddress && pAddress->length > 0));
	dev->ev_sender = task;

	dev->SetAddress(this, pAddress);
	if (pktinfo != NULL)
	{
		LogError(_LOCAL_,
			   "pktinfo structure provided, ifindex %u (set to 0)",
			   pktinfo->ipi6_ifindex);

		dev->attributes |= BC_SOCKEVENTATTR_PKTINFO;
		dev->pktinfo = *pktinfo;
		/*
		 * Set the pktinfo index to 0 here, to let the kernel decide
		 * what interface it should send on.
		 */
		dev->pktinfo.ipi6_ifindex = 0;
	}

	io_state = _StartIoSend(dev, &cc, &send_errno);
	switch (io_state)
	{
	case DOIO_PENDING:	/* I/O started. Nothing more to do */
	case DOIO_SOFT:
		/*
		 * We couldn't send all or part of the request right now, so
		 * queue it unless BC_SOCKFLAG_NORETRY is set.
		 */
		if ((flags & BC_SOCKFLAG_NORETRY) == 0)
		{
			task->Attach(&ntask);
			dev->attributes |= BC_SOCKEVENTATTR_ATTACHED;

			/*
			 * Enqueue the request.
			 */
			m_lstSendEvents.PushBack(dev);

			if ((flags & BC_SOCKFLAG_IMMEDIATE) != 0)
				result = BC_R_INPROGRESS;
			break;
		}

	case DOIO_SUCCESS:
		break;
	// Below code was added by antoniozhou
	case DOIO_HARD:
		result = BC_R_UNEXPECTED;
		break;
	// Add end.
	}

	return (result);
}

BCRESULT BCSocket::Send(
	BCRegionS *region,
	uint32_t region_size,
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg)
{
	/*
	 * ASSERT() checking is performed in SendTo().
	 */
	return (SendTo(region, region_size, task, action, arg, NULL, NULL));
}

BCRESULT BCSocket::SendTo(
	BCRegionS *region,
	uint32_t region_size,
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg,
	BCSockAddrS *pAddress,
	struct in6_pktinfo *pktinfo)
{
	BCSockEvent *dev;
	BCRESULT ret;

	ASSERT(VALID_SOCKET(this));
	ASSERT(m_eType != bc_sockettype_fdwatch);

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}
	ASSERT(region != NULL);
	ASSERT(region_size <= BC_SOCKET_MAXSCATTERGATHER);
	ASSERT(task != NULL);
	ASSERT(action != NULL);

	ASSERT(VALID_MANAGER(m_pMgr));

	ASSERT(m_bBound);

	dev = new BCSockEvent(this, BC_SOCKEVENT_SENDDONE, action, arg);
	if (dev == NULL)
	{
		m_sLock.Unlock();
		return (BC_R_NOMEMORY);
	}
	for (uint32_t i = 0; i < region_size; i++)
	{
		dev->region[i] = region[i];
		dev->region_size++;
	}

	ret = _Send(dev, task, pAddress, pktinfo, 0);
	m_sLock.Unlock();
	return (ret);
}

BCRESULT BCSocket::SendTo2(
	BCRegionS *region, 
	uint32_t region_size,
	BCTask *task,
	BCSockAddrS *address,
	struct in6_pktinfo *pktinfo,
	BCSockEvent *pEvent,
	uint32_t flags)
{
	BCRESULT ret;

	ASSERT(VALID_SOCKET(this));
	m_sLock.Lock();
	CONSISTENT(this);

	ASSERT((flags & ~(BC_SOCKFLAG_IMMEDIATE|BC_SOCKFLAG_NORETRY)) == 0);
	if ((flags & BC_SOCKFLAG_NORETRY) != 0)
		ASSERT(m_eType == bc_sockettype_udp);
	pEvent->ev_sender = this;
	pEvent->result = BC_R_UNEXPECTED;
	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}
	ASSERT(region != NULL);
	ASSERT(region_size <= BC_SOCKET_MAXSCATTERGATHER);
	ASSERT(task != NULL);
	ASSERT(pEvent != NULL);

	for (uint32_t i = 0; i < region_size; i++)
	{
		pEvent->region[i] = region[i];
		pEvent->region_size++;
	}
	pEvent->bufferlist = NULL;
	pEvent->n = 0;
	pEvent->offset = 0;
	pEvent->attributes = 0;

	ret = _Send(pEvent, task, address, pktinfo, flags);
	m_sLock.Unlock();
	return (ret);
}

BCRESULT BCSocket::SendV(
	BCBuffer *buflist,
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg)
{
	return (SendToV(buflist, task, action, arg, NULL, NULL));
}

BCRESULT BCSocket::SendToV(
	BCBuffer *buflist,
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg,
	BCSockAddrS *address,
	struct in6_pktinfo *pktinfo)
{
	BCSockEvent *dev;
	uint32_t iocount;
	BCRESULT ret;

	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}
	ASSERT(buflist != NULL);
	ASSERT(VALID_BCBUFFER(buflist));
	ASSERT(task != NULL);
	ASSERT(action != NULL);
	ASSERT(!address || (address && address->length > 0));

	ASSERT(VALID_MANAGER(m_pMgr));

	iocount = buflist->RemainingLength();
	ASSERT(iocount > 0);

	dev = new BCSockEvent(this, BC_SOCKEVENT_SENDDONE, action, arg);
	if (dev == NULL)
	{
		m_sLock.Unlock();
		return (BC_R_NOMEMORY);
	}

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	dev->bufferlist = buflist;

	ret = _Send(dev, task, address, pktinfo, 0);
	m_sLock.Unlock();
	return (ret);
}

BCRESULT BCSocket::SendToV2(
	BCBuffer *buflist,
	BCTask *task,
	BCSockAddrS *address,
	struct in6_pktinfo *pktinfo,
	BCSockEvent *pEvent,
	uint32_t flags)
{
	uint32_t iocount;
	BCRESULT ret;

	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();
	CONSISTENT(this);

	ASSERT((flags & ~(BC_SOCKFLAG_IMMEDIATE|BC_SOCKFLAG_NORETRY)) == 0);
	if ((flags & BC_SOCKFLAG_NORETRY) != 0)
	{
		ASSERT(m_eType == bc_sockettype_udp);
	}
	pEvent->ev_sender = this;
	pEvent->result = BC_R_UNEXPECTED;

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}
	ASSERT(buflist != NULL);
	ASSERT(VALID_BCBUFFER(buflist));
	ASSERT(VALID_MANAGER(m_pMgr));

	iocount = buflist->RemainingLength();
	ASSERT(iocount > 0);

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	pEvent->bufferlist = buflist;
	pEvent->n = 0;
	pEvent->offset = 0;
	pEvent->attributes = 0;

	ret = _Send(pEvent, task, address, pktinfo, 0);
	m_sLock.Unlock();
	return (ret);
}

BCRESULT BCSocket::Bind(
	BCSockAddrS *sockaddr,
	uint32_t options)
{
	int32_t bind_errno;
	char strbuf[BC_STRERRORSIZE];
	int32_t on = 1;

	ASSERT(VALID_SOCKET(this));
	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	ASSERT(!m_bBound);

	if (m_nPF != sockaddr->type.sa.sa_family)
	{
		m_sLock.Unlock();
		return (BC_R_FAMILYMISMATCH);
	}
	/*
	 * Only set SO_REUSEADDR when we want a specific port.
	 */
	if ((options & BC_SOCKET_REUSEADDRESS) != 0 &&
	    bc_sockaddr_getport(sockaddr) != (in_port_t)0 &&
	    setsockopt(m_nFD, SOL_SOCKET, SO_REUSEADDR, (char *)&on,
		       sizeof(on)) < 0)
	{
		LogError(_LOCAL_, "setsockopt(%d) %s", m_nFD, "failed");
		/* Press on... */
	}
	if (bind(m_nFD, &sockaddr->type.sa, sockaddr->length) < 0)
	{
		bind_errno = WSAGetLastError();
		m_sLock.Unlock();
		switch (bind_errno)
		{
		case WSAEACCES:
			return (BC_R_NOPERM);
		case WSAEADDRNOTAVAIL:
			return (BC_R_ADDRNOTAVAIL);
		case WSAEADDRINUSE:
			return (BC_R_ADDRINUSE);
		case WSAEINVAL:
			return (BC_R_BOUND);
		default:
			bc_strerror(bind_errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_, "bind: %s", strbuf);
			return (BC_R_UNEXPECTED);
		}
	}

	m_bBound = 1;

	m_sLock.Unlock();
	return (BC_R_SUCCESS);
}

BCRESULT BCSocket::Filter(const char *filter)
{
	UNUSED(filter);

	ASSERT(VALID_SOCKET(this));
	return (BC_R_NOTIMPLEMENTED);
}

/*
 * Set up to listen on a given socket.  We do this by creating an internal
 * event that will be dispatched when the socket has read activity.  The
 * watcher will send the internal event to the task when there is a new
 * connection.
 *
 * Unlike in read, we don't preallocate a done event here.  Every time there
 * is a new connection we'll have to allocate a new one anyway, so we might
 * as well keep things simple rather than having to track them.
 */
BCRESULT BCSocket::Listen(uint32_t backlog)
{
	char strbuf[BC_STRERRORSIZE];

	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	ASSERT(!m_bListener);
	ASSERT(m_bBound);
	ASSERT(m_eType == bc_sockettype_tcp);

	if (backlog == 0)
		backlog = SOMAXCONN;

	if (listen(m_nFD, (int)backlog) < 0)
	{
		m_sLock.Unlock();
		bc_strerror(WSAGetLastError(), strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "listen: %s", strbuf);
		return (BC_R_UNEXPECTED);
	}

	m_bListener = 1;
	_set_state(this, SOCK_LISTEN);

	m_sLock.Unlock();
	return (BC_R_SUCCESS);
}

/*
 * This should try to do aggressive accept() XXXMLG
 */
BCRESULT BCSocket::Accept(
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg)
{
	BCSockICEvent *adev;
	BCTask *ntask = NULL;
	BCSocket *nsock;
	SockIoCompletionInfo *lpo;

	ASSERT(VALID_SOCKET(this));

	ASSERT(VALID_MANAGER(m_pMgr));

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	ASSERT(m_bListener);

	/*
	 * Sender field is overloaded here with the task we will be sending
	 * this event to.  Just before the actual event is delivered the
	 * actual ev_sender will be touched up to be the socket.
	 */
	adev = new BCSockICEvent(task, BC_SOCKEVENT_NEWCONN, action, arg);
	if (adev == NULL)
	{
		m_sLock.Unlock();
		return (BC_R_NOMEMORY);
	}

	nsock = new BCSocket();
	if (nsock == NULL)
	{
		adev->Destroy();
		m_sLock.Unlock();
		return (BC_R_NOMEMORY);
	}
	nsock->m_pMgr = m_pMgr;
	nsock->m_eType = m_eType;

	/*
	 * AcceptEx() requires we pass in a socket.
	 */
	nsock->m_nFD = socket(m_nPF, SOCK_STREAM, IPPROTO_TCP);
	if (nsock->m_nFD == INVALID_SOCKET)
	{
		_Free(&nsock, __LINE__);
		adev->Destroy();
		m_sLock.Unlock();
		return (BC_R_FAILURE); // XXXMLG need real error message
	}

	/*
	 * Attach to socket and to task.
	 */
	task->Attach(&ntask);
	nsock->m_nRef++;

	adev->ev_sender = ntask;
	adev->newsocket = nsock;
	_set_state(nsock, SOCK_ACCEPT);

	/*
	 * Queue io completion for an accept().
	 */
	lpo = new SockIoCompletionInfo();
	ASSERT(lpo != NULL);
	lpo->acceptbuffer = (void *)new SockAcceptBuffer();
	ASSERT(lpo->acceptbuffer != NULL);

	lpo->adev = adev;
	lpo->request_type = SOCKET_ACCEPT;

	BCAcceptEx(m_nFD,
		    nsock->m_nFD,					/* Accepted Socket */
		    lpo->acceptbuffer,				/* Buffer for initial Recv */
		    0,								/* Length of Buffer */
		    sizeof(SOCKADDR_STORAGE) + 16,	/* Local address length + 16 */
		    sizeof(SOCKADDR_STORAGE) + 16,	/* Remote address lengh + 16 */
		    (LPDWORD)&lpo->received_bytes,	/* Bytes Recved */
		    (LPOVERLAPPED)lpo				/* Overlapped structure */
		    );
	nsock->_IocpUpdate();

	/*
	 * Enqueue the event
	 */
	m_lstAcceptEvents.PushBack(adev);
	m_nPendingAccept++;
	m_nPendingIocp++;

	m_sLock.Unlock();
	return (BC_R_SUCCESS);
}

BCRESULT BCSocket::Connect(
	BCSockAddrS *addr,
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg)
{
	char strbuf[BC_STRERRORSIZE];
	BCSockOCEvent *cdev;
	BCTask *ntask = NULL;
	SockIoCompletionInfo *lpo;
	int32_t bind_errno;

	ASSERT(VALID_SOCKET(this));
	ASSERT(addr != NULL);
	ASSERT(task != NULL);
	ASSERT(action != NULL);

	ASSERT(VALID_MANAGER(m_pMgr));
	ASSERT(addr != NULL);

	if (bc_sockaddr_ismulticast(addr))
	{
		return (BC_R_MULTICAST);
	}

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	/*
	 * Windows sockets won't connect unless the socket is bound.
	 */
	if (!m_bBound)
	{
		BCSockAddrS any;

		bc_sockaddr_anyofpf(&any, bc_sockaddr_pf(addr));
		if (bind(m_nFD, &any.type.sa, any.length) < 0)
		{
			bind_errno = WSAGetLastError();
			m_sLock.Unlock();
			switch (bind_errno)
			{
			case WSAEACCES:
				return (BC_R_NOPERM);
			case WSAEADDRNOTAVAIL:
				return (BC_R_ADDRNOTAVAIL);
			case WSAEADDRINUSE:
				return (BC_R_ADDRINUSE);
			case WSAEINVAL:
				return (BC_R_BOUND);
			default:
				bc_strerror(bind_errno, strbuf, sizeof(strbuf));
				LogError(_LOCAL_, "bind: %s", strbuf);
				return (BC_R_UNEXPECTED);
			}
		}
		m_bBound = 1;
	}

	ASSERT(!m_bPendingConnect);

	cdev = new BCSockOCEvent(this, BC_SOCKEVENT_CONNECT, action, arg);
	if (cdev == NULL)
	{
		m_sLock.Unlock();
		return (BC_R_NOMEMORY);
	}

	if (m_eType == bc_sockettype_tcp)
	{
		/*
		 * Queue io completion for an accept().
		 */
		lpo = new SockIoCompletionInfo();
		lpo->cdev = cdev;
		lpo->request_type = SOCKET_CONNECT;

		m_sAddress = *addr;
		BCConnectEx(m_nFD, &addr->type.sa, addr->length,
			NULL, 0, NULL, (LPOVERLAPPED)lpo);

		/*
		 * Attach to task.
		 */
		task->Attach(&ntask);
		cdev->ev_sender = ntask;

		m_bPendingConnect = 1;
		_set_state(this, SOCK_CONNECT);

		/*
		 * Enqueue the request.
		 */
		m_pConnectEvent = cdev;
		m_nPendingIocp++;
	}
	else
	{
		BCTaskEvent *pTEvent;
		WSAConnect(m_nFD, &addr->type.sa, addr->length, NULL, NULL, NULL, NULL);
		cdev->result = BC_R_SUCCESS;
		pTEvent = cdev;
		task->Send(&pTEvent);
	}
	CONSISTENT(this);
	m_sLock.Unlock();

	return (BC_R_SUCCESS);
}

BCRESULT BCSocket::GetPeerName(BCSockAddrS *addressp)
{
	BCRESULT result;

	ASSERT(VALID_SOCKET(this));
	ASSERT(addressp != NULL);

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	if (m_bConnected)
	{
		*addressp = m_sAddress;
		result = BC_R_SUCCESS;
	}
	else
	{
		result = BC_R_NOTCONNECTED;
	}

	m_sLock.Unlock();

	return (result);
}

BCRESULT BCSocket::GetSockName(BCSockAddrS *addressp)
{
	BC_SOCKADDR_LEN_T len;
	BCRESULT result;
	char strbuf[BC_STRERRORSIZE];

	ASSERT(VALID_SOCKET(this));
	ASSERT(addressp != NULL);

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (BC_R_CONNREFUSED);
	}

	if (!m_bBound)
	{
		result = BC_R_NOTBOUND;
		goto out;
	}

	result = BC_R_SUCCESS;

	len = sizeof(addressp->type.sa);
	if (getsockname(m_nFD, &addressp->type.sa, (int *)&len) < 0)
	{
		bc_strerror(WSAGetLastError(), strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "getsockname: %s", strbuf);
		result = BC_R_UNEXPECTED;
		goto out;
	}
	addressp->length = (unsigned int)len;

out:
	m_sLock.Unlock();

	return (result);
}

/*
 * Run through the list of events on this socket, and cancel the ones
 * queued for task "task" of type "how".  "how" is a bitmask.
 */
void BCSocket::Cancel(BCTask *task, uint32_t how)
{
	ASSERT(VALID_SOCKET(this));

	/*
	 * Quick exit if there is nothing to do.  Don't even bother locking
	 * in this case.
	 */
	if (how == 0)
		return;

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return;
	}

	/*
	 * All of these do the same thing, more or less.
	 * Each will:
	 *	o If the internal event is marked as "posted" try to
	 *	  remove it from the task's queue.  If this fails, mark it
	 *	  as canceled instead, and let the task clean it up later.
	 *	o For each I/O request for that task of that type, post
	 *	  its done event with status of "BC_R_CANCELED".
	 *	o Reset any state needed.
	 */

	if ((how & BC_SOCKCANCEL_RECV) == BC_SOCKCANCEL_RECV)
	{
		BCSockEvent		*dev, *next, *end_event;
		BCTask			*current_task;

		end_event = m_lstRecvEvents.End();
		for (dev = m_lstRecvEvents.Begin();
			 dev != end_event;
			 dev = next)
		{
			current_task = (BCTask *)dev->ev_sender;
			next = m_lstRecvEvents.Next(dev);
			if ((task == NULL) || (task == current_task))
			{
				dev->result = BC_R_CANCELED;
				_SendRecvDoneEvent(&dev);
			}
		}
	}
	how &= ~BC_SOCKCANCEL_RECV;

	if ((how & BC_SOCKCANCEL_SEND) == BC_SOCKCANCEL_SEND)
	{
		BCSockEvent		*dev, *next, *end_event;
		BCTask			*current_task;

		end_event = m_lstSendEvents.End();
		for (dev = m_lstSendEvents.Begin();
			 dev != end_event;
			 dev = next)
		{
			current_task = (BCTask *)dev->ev_sender;
			next = m_lstSendEvents.Next(dev);
			if ((task == NULL) || (task == current_task))
			{
				dev->result = BC_R_CANCELED;
				_SendSendDoneEvent(&dev);
			}
		}
	}
	how &= ~BC_SOCKCANCEL_SEND;

	if (((how & BC_SOCKCANCEL_ACCEPT) == BC_SOCKCANCEL_ACCEPT)
	    && !m_lstAcceptEvents.IsEmpty())
	{
		BCSockICEvent	*dev, *next, *end_event;
		BCTask			*current_task;

		end_event = m_lstAcceptEvents.End();
		for (dev = m_lstAcceptEvents.Begin();
		     dev != end_event;
			 dev = next)
		{
			current_task = (BCTask *)dev->ev_sender;
			next = m_lstAcceptEvents.Next(dev);
			if ((task == NULL) || (task == current_task))
			{
				dev->newsocket->m_nRef--;
				closesocket(dev->newsocket->m_nFD);
				dev->newsocket->m_nFD = INVALID_SOCKET;
				_Free(&dev->newsocket, __LINE__);

				dev->result = BC_R_CANCELED;
				_SendAcceptDoneEvent(&dev);
			}
		}
	}
	how &= ~BC_SOCKCANCEL_ACCEPT;

	/*
	 * Connecting is not a list.
	 */
	if (((how & BC_SOCKCANCEL_CONNECT) == BC_SOCKCANCEL_CONNECT)
	    && m_pConnectEvent != NULL)
	{
		BCSockOCEvent		*dev;
		BCTask				*current_task;

		ASSERT(m_bPendingConnect);

		dev = m_pConnectEvent;
		current_task = (BCTask *)dev->ev_sender;

		if ((task == NULL) || (task == current_task))
		{
			closesocket(m_nFD);
			m_nFD = INVALID_SOCKET;
			_set_state(this, SOCK_CLOSED);

			m_pConnectEvent = NULL;
			dev->result = BC_R_CANCELED;
			_SendConnectDoneEvent(&dev);
		}
	}
	how &= ~BC_SOCKCANCEL_CONNECT;

	_MaybeFree(this, __LINE__);
}

BCSocketTypeE BCSocket::GetType()
{
	BCSocketTypeE eType;

	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (bc_sockettype_unknown);
	}

	eType = m_eType;
	m_sLock.Unlock();
	return (eType);
}

BOOL BCSocket::IsBound()
{
	BOOL val;

	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();
	CONSISTENT(this);

	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		m_sLock.Unlock();
		return (FALSE);
	}

	val = ((m_bBound) ? TRUE : FALSE);
	m_sLock.Unlock();

	return (val);
}

void BCSocket::IPv6only(BOOL yes)
{
#if defined(IPV6_V6ONLY)
	int onoff = yes ? 1 : 0;
#else
	UNUSED(yes);
#endif

	ASSERT(VALID_SOCKET(this));

#ifdef IPV6_V6ONLY
	if (m_nPF == AF_INET6)
	{
		(void)setsockopt(m_nFD, IPPROTO_IPV6, IPV6_V6ONLY,
			(char *)&onoff, sizeof(onoff));
	}
#endif
}

void BCSocket::SetName(const char *szName, void *pTag)
{
	/*
	 * Name 'socket'.
	 */

	ASSERT(VALID_SOCKET(this));

	m_sLock.Lock();
	memzero(m_szName, sizeof(m_szName));
	strncpy(m_szName, szName, sizeof(m_szName) - 1);
	m_pTag = pTag;
	m_sLock.Unlock();
}

const char * BCSocket::GetName()
{
	return (m_szName);
}

void * BCSocket::GetTag()
{
	return (m_pTag);
}

BCSocketMgr *BCSocket::GetManager() const
{
	return (m_pMgr);
}

#ifdef _DEBUG
ULONGLONG BCSocket::GetAllocCount()
{
	return s_alloc.GetAllocCount();
}
#endif

#ifdef BC_SOCKET_CONSISTENCY_CHECKS

/*
 * Verify that the socket state is consistent.
 */
void BCSocket::_Consistent()
{
	uint32_t count;
	const char *crash_reason;
	BOOL crash = FALSE;

	ASSERT(m_nPendingIocp == m_nPendingRecv + m_nPendingSend
		+ m_nPendingAccept + m_bPendingConnect);

	count = m_lstSendEvents.Count();
	if (count > m_nPendingSend)
	{
		crash = TRUE;
		crash_reason = "m_lstSendEvents > m_nPendingSend";
	}

	count = m_lstAcceptEvents.Count();
	if (count > m_nPendingAccept)
	{
		crash = TRUE;
		crash_reason = "m_lstAcceptEvents > m_nPendingAccept";
	}

	if (crash)
	{
		LogError(_LOCAL_, "SOCKET INCONSISTENT: %s", crash_reason);
		_Dump();
		ASSERT(crash == FALSE);
	}
}

/*
 * This is used to dump the contents of the sock structure
 * You should make sure that the sock is locked before
 * dumping it. Since the code uses simple printf() statements
 * it should only be used interactively.
 */
void BCSocket::_Dump()
{
	BCSockEvent *ldev, *lendev;
	BCSockICEvent *ndev, *nendev;

#if 0
	BCSockAddrS addr;
	char socktext[256];

	GetPeerName(&addr);
	bc_sockaddr_format(&addr, socktext, sizeof(socktext));
	printf("Remote Socket: %s\n", socktext);
	GetSockName(&addr);
	bc_sockaddr_format(&addr, socktext, sizeof(socktext));
	printf("This Socket: %s\n", socktext);
#endif

	printf("\n\t\tSock Dump\n");
	printf("\t\tfd: %" _U64BITARG_ "\n", (uint64_t)m_nFD);
	printf("\t\treferences: %d\n", m_nRef);
	printf("\t\tpending_accept: %d\n", m_nPendingAccept);
	printf("\t\tconnecting: %d\n", m_bPendingConnect);
	printf("\t\tconnected: %d\n", m_bConnected);
	printf("\t\tbound: %d\n", m_bBound);
	printf("\t\tpending_iocp: %d\n", m_nPendingIocp);
	printf("\t\tsocket type: %d\n", m_eType);

	printf("\n\t\tSock Recv List\n");
	ldev = m_lstRecvEvents.Begin();
	lendev = m_lstRecvEvents.End();
	for (;ldev != lendev;ldev = m_lstRecvEvents.Next(ldev))
	{
		printf("\t\tdev: %p\n", ldev);
	}

	printf("\n\t\tSock Send List\n");
	ldev = m_lstSendEvents.Begin();
	lendev = m_lstSendEvents.End();
	for (;ldev != lendev;ldev = m_lstSendEvents.Next(ldev))
	{
		printf("\t\tdev: %p\n", ldev);
	}

	printf("\n\t\tSock Accept List\n");
	ndev = m_lstAcceptEvents.Begin();
	nendev = m_lstAcceptEvents.End();
	for (;ndev != nendev;ndev = m_lstAcceptEvents.Next(ndev))
	{
		printf("\t\tdev: %p\n", ndev);
	}
}

#endif // BC_SOCKET_CONSISTENCY_CHECKS

///////////////////////////////////////////////////////////////////////////////
// class : BCSocketMgr
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCSocketMgr, 8);

BCSocketMgr::BCSocketMgr()
		: BCMagic(SOCKET_MANAGER_MAGIC)
		, m_bShutdown(FALSE)
		, m_sCondShutdownOK(&m_sLock)
		, m_hIoCompletionPort(NULL)
		, m_nMaxIocpThreads(0)
		, m_nTotalSockets(0)
		, m_nIocpTotal(0)
{
	memzero(m_hIocpThreads, sizeof(m_hIocpThreads));
}

BCSocketMgr::~BCSocketMgr()
{
	//
}

BCRESULT BCSocketMgr::Create(uint32_t nMaxSocks /* = 0 */)
{
	if (nMaxSocks != 0)
		return (BC_R_NOTIMPLEMENTED);

	InitSockets();

	_IocpInitialize();	/* Create the Completion Ports */

	return (BC_R_SUCCESS);
}

void BCSocketMgr::Destroy(BCSocketMgr **ppMgr)
{
	BCSocketMgr *pMgr;
	int32_t i;

	/*
	 * Destroy a socket manager.
	 */

	ASSERT(ppMgr != NULL && *ppMgr != NULL);
	pMgr = *ppMgr;
	ASSERT(VALID_MANAGER(pMgr));

	pMgr->m_sLock.Lock();

	/*
	 * Wait for all sockets to be destroyed.
	 */
	while (!pMgr->m_lstSockets.IsEmpty())
	{
		pMgr->m_sCondShutdownOK.Wait();
	}

	pMgr->m_sLock.Unlock();

	/*
	 * Here, we need to had some wait code for the completion port
	 * thread.
	 */
	pMgr->_SignalIocpExit();
	pMgr->m_bShutdown = TRUE;

	/*
	 * Wait for threads to exit.
	 */
	for (i = 0; i < pMgr->m_nMaxIocpThreads; i++)
	{
		pMgr->m_hIocpThreads[i]->Join(NULL);
	}
	/*
	 * Clean up.
	 */

	memzero(pMgr->m_hIocpThreads, sizeof(pMgr->m_hIocpThreads));

	pMgr->m_nMagic = 0;

	pMgr->_Destroy();

	*ppMgr = NULL;
}

uint32_t BCSocketMgr::GetSocketCount() const
{
	return m_lstSockets.Count();
}

void BCSocketMgr::_Destroy()
{
	delete this;
}

/*  This function will add an entry to the I/O completion port
 *  that will signal the I/O thread to exit (gracefully)
 */
void BCSocketMgr::_SignalIocpExit()
{
	int i;
	int errval;
	char strbuf[BC_STRERRORSIZE];

	ASSERT(VALID_MANAGER(this));
	for (i = 0; i < m_nMaxIocpThreads; i++)
	{
		if (!PostQueuedCompletionStatus(m_hIoCompletionPort,
		                                0, 0, 0))
		{
			errval = GetLastError();
			bc_strerror(errval, strbuf, sizeof(strbuf));
			LogFatal(_LOCAL_,
			         "Can't request service thread to exit: %s",
			         strbuf);
		}
	}
}

/*
 * Create the worker threads for the I/O Completion Port
 */
void BCSocketMgr::_IocpCreateThreads(int32_t total_threads)
{
	int errval;
	char strbuf[BC_STRERRORSIZE];
	int i;

	ASSERT(total_threads > 0);
	ASSERT(VALID_MANAGER(this));
	/*
	 * We need at least one
	 */
	for (i = 0; i < total_threads; i++)
	{
		m_hIocpThreads[i] = new BCThread(_SocketIoThread, this, 
			BCThread::PRIORITY_NORMAL, "BCSocket-WinIocpThread");
		if (m_hIocpThreads[i] == NULL)
		{
			errval = GetLastError();
			bc_strerror(errval, strbuf, sizeof(strbuf));
			LogFatal(_LOCAL_,
			         "Can't create IOCP thread: %s",
			         strbuf);
			exit(1);
		}
		else
		{
			m_hIocpThreads[i]->Start();
		}
	}
}

/*
 *  Create/initialise the I/O completion port
 */
void BCSocketMgr::_IocpInitialize()
{
	int errval;
	char strbuf[BC_STRERRORSIZE];

	ASSERT(VALID_MANAGER(this));

	m_nMaxIocpThreads = BCMIN(bc_os_ncpus() + 1, MAX_IOCPTHREADS);

	/* Now Create the Completion Port */
	m_hIoCompletionPort = CreateIoCompletionPort(
	                        INVALID_HANDLE_VALUE, NULL,
	                        0, m_nMaxIocpThreads);
	if (m_hIoCompletionPort == NULL)
	{
		errval = GetLastError();
		bc_strerror(errval, strbuf, sizeof(strbuf));
		LogFatal(_LOCAL_,
		         "CreateIoCompletionPort() failed "
		         "during initialization: %s",
		         strbuf);
		exit(1);
	}

	/*
	 * Worker threads for servicing the I/O
	 */
	_IocpCreateThreads(m_nMaxIocpThreads);
}

/*
 * This is the I/O Completion Port Worker Function. It loops forever
 * waiting for I/O to complete and then forwards them for further
 * processing. There are a number of these in separate threads.
 */
void *BCSocketMgr::_SocketIoThread(LPVOID ThreadContext)
{
	BCSocketMgr *pMgr = (BCSocketMgr *)ThreadContext;
	BOOL bSuccess = FALSE;
	DWORD nbytes;
	SockIoCompletionInfo *lpo = NULL;
	BCSocket *sock = NULL;
	int request;
	struct msghdr *messagehdr = NULL;
	int errval;
	char strbuf[BC_STRERRORSIZE];
	int errstatus;

	ASSERT(VALID_MANAGER(pMgr));

	/*
	 * Set the thread priority high enough so I/O will
	 * preempt normal recv packet processing, but not
	 * higher than the timer sync thread.
	 */
	if (!SetThreadPriority(GetCurrentThread(),
			       THREAD_PRIORITY_ABOVE_NORMAL))
	{
		errval = GetLastError();
		bc_strerror(errval, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "Can't set thread priority: %s", strbuf);
	}

	/*
	 * Loop forever waiting on I/O Completions and then processing them
	 */
	while (TRUE)
	{
		bSuccess = GetQueuedCompletionStatus(
						pMgr->m_hIoCompletionPort,
						&nbytes, (PULONG_PTR)&sock,
						(LPWSAOVERLAPPED *)&lpo,
						INFINITE);
		if (lpo == NULL) /* Received request to exit */
			break;

		ASSERT(VALID_SOCKET(sock));

		request = lpo->request_type;

		errstatus = 0;
		if (!bSuccess)
		{
			BCRESULT bc_result;

			/*
			 * Did the I/O operation complete?
			 */
			errstatus = GetLastError();
			bc_result = bc_errno2resultx(errstatus, __FILE__, __LINE__);

			sock->m_sLock.Lock();
			CONSISTENT(sock);
			switch (request)
			{
			case SOCKET_RECV:
				ASSERT(sock->m_nPendingIocp > 0);
				sock->m_nPendingIocp--;
				ASSERT(sock->m_nPendingRecv > 0);
				sock->m_nPendingRecv--;
				if ((sock->m_eType == bc_sockettype_udp) &&
				    (errstatus == ERROR_HOST_UNREACHABLE))
				{
					//LogError(_LOCAL_,
					//		 "SOCKET_RECV ERROR_HOST_UNREACHABLE: trying to recover");
					sock->_RecoverReceiveRequest(&lpo);
					break;
				}
				if ((sock->m_eType == bc_sockettype_udp) &&
				    (errstatus == WSAENETRESET))
				{
					//LogError(_LOCAL_,
					//		 "SOCKET_RECV WSAENETRESET: trying to recover");
					sock->_RecoverReceiveRequest(&lpo);
					break;
				}
				if ((sock->m_eType == bc_sockettype_udp) &&
				    (errstatus == WSAECONNRESET))
				{
					//LogError(_LOCAL_,
					//		 "SOCKET_RECV WSAECONNRESET: trying to recover");
					sock->_RecoverReceiveRequest(&lpo);
					break;
				}
				sock->_SendRecvDoneAbort(bc_result);
				if ((bc_result == BC_R_UNEXPECTED) ||
				    ((bc_result == BC_R_CONNECTIONRESET) &&
				     (errstatus != ERROR_OPERATION_ABORTED)) ||
				    (bc_result == BC_R_HOSTUNREACH))
				{
					//LogError(_LOCAL_,
					//	"SOCKET_RECV: Windows error code: %d, returning BC error %d",
					//	errstatus, bc_result);
				}
				break;

			case SOCKET_SEND:
				ASSERT(sock->m_nPendingIocp > 0);
				sock->m_nPendingIocp--;
				ASSERT(sock->m_nPendingSend > 0);
				sock->m_nPendingSend--;
				if (sock->_IsSendDoneActive(lpo->dev))
				{
					lpo->dev->result = bc_result;
					//LogError(_LOCAL_, "canceled_send");
					sock->_SendSendDoneEvent(&lpo->dev);
				}
				break;

			case SOCKET_ACCEPT:
				ASSERT(sock->m_nPendingIocp > 0);
				sock->m_nPendingIocp--;
				ASSERT(sock->m_nPendingAccept > 0);
				sock->m_nPendingAccept--;
				if (sock->_IsAcceptDoneActive(lpo->adev))
				{
					closesocket(lpo->adev->newsocket->m_nFD);
					lpo->adev->newsocket->m_nFD = INVALID_SOCKET;
					lpo->adev->newsocket->m_nRef--;
					BCSocket::_Free(&lpo->adev->newsocket, __LINE__);
					lpo->adev->result = bc_result;
					//LogError(_LOCAL_, "canceled_accept");
					sock->_SendAcceptDoneEvent(&lpo->adev);
				}
				break;

			case SOCKET_CONNECT:
				ASSERT(sock->m_nPendingIocp > 0);
				sock->m_nPendingIocp--;
				ASSERT(sock->m_bPendingConnect == 1);
				sock->m_bPendingConnect = 0;
				if (sock->_IsConnectDoneActive(lpo->cdev))
				{
					lpo->cdev->result = bc_result;
					//LogError(_LOCAL_, "canceled_connect");
					sock->_SendConnectDoneEvent(&lpo->cdev);
				}
				break;
			}
			BCSocket::_MaybeFree(&sock, __LINE__);

			if (lpo != NULL)
				delete lpo;
			continue;
		}

		messagehdr = &lpo->messagehdr;

		switch (request)
		{
		case SOCKET_RECV:
			sock->_InternalRecv(nbytes);
			break;
		case SOCKET_SEND:
			sock->_InternalSend(lpo->dev, messagehdr, nbytes, errstatus, lpo);
			break;
		case SOCKET_ACCEPT:
			sock->_InternalAccept(lpo, errstatus);
			break;
		case SOCKET_CONNECT:
			sock->_InternalConnect(lpo, errstatus);
			break;
		}

		if (lpo != NULL)
			delete lpo;
	}

	return (NULL);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
