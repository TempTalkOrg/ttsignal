
#ifndef BCSOCKET_INCLUDED__
#define BCSOCKET_INCLUDED__

#include "BC/Exports.h"
#include "BC/Utils.h"
#include "BC/BCFixedAlloc.h"
#include "BC/BCNodeList.h"
#include "BC/BCMagic.h"
#include "BC/BCTaskEvent.h"
#include "BC/BCNet.h"
#include "BC/BCSockAddr.h"
#include "BC/BCStats.h"
#include "BC/BCHashTable.h"
#include <unordered_map>


#ifdef BC_PLATFORM_HAVESYSUNH
#include <sys/un.h>
#endif
#ifdef BC_PLATFORM_HAVEKQUEUE
#include <sys/event.h>
#endif
#ifdef BC_PLATFORM_HAVEEPOLL
#include <sys/epoll.h>
#endif
#ifdef BC_PLATFORM_HAVEDEVPOLL
#if defined(HAVE_SYS_DEVPOLL_H)
#include <sys/devpoll.h>
#elif defined(HAVE_DEVPOLL_H)
#include <devpoll.h>
#endif
#endif

#define BCLOUD

/* See task.c about the following definition: */
#ifdef BCLOUD
#ifdef BC_PLATFORM_USETHREADS
#define USE_WATCHER_THREAD
#else
#define USE_SHARED_MANAGER
#endif	/* BC_PLATFORM_USETHREADS */
#endif	/* BCLOUD */

#if defined(SO_BSDCOMPAT) && defined(__linux__)
#include <sys/utsname.h>
#endif

/*%
 * Choose the most preferable multiplex method.
 */
#ifdef BC_PLATFORM_HAVEKQUEUE
#define USE_KQUEUE
#elif defined (BC_PLATFORM_HAVEEPOLL)
#define USE_EPOLL
#elif defined (BC_PLATFORM_HAVEDEVPOLL)
#define USE_DEVPOLL
typedef struct {
	unsigned int want_read : 1,
		want_write : 1;
} pollinfo_t;
#else
#define USE_SELECT
#endif	/* BC_PLATFORM_HAVEKQUEUE */

#ifndef USE_WATCHER_THREAD
#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_DEVPOLL)
struct bc_socketwait {
	int nevents;
};
#elif defined (USE_SELECT)
struct bc_socketwait {
	fd_set *readset;
	fd_set *writeset;
	int nfds;
	int maxfd;
};
#endif	/* USE_KQUEUE */
#endif /* !USE_WATCHER_THREAD */

#ifndef USE_WATCHER_THREAD

#ifdef BC_PLATFORM_NEEDSYSSELECTH
#include <sys/select.h>
#endif

typedef struct bc_socketwait BCSocketWaitType;

#endif /* USE_WATCHER_THREAD */

#ifdef _DEBUG
#define BC_SOCKET_CONSISTENCY_CHECKS
#endif

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

/***
 *** Constants
 ***/

/*%
 * Maximum number of buffers in a scatter/gather read/write.  The operating
 * system in use must support at least this number (plus one on some.)
 */
#define BC_SOCKET_MAXSCATTERGATHER	1024

/*%
 * In bc_socket_bind() set socket option SO_REUSEADDR prior to calling
 * bind() if a non zero port is specified (AF_INET and AF_INET6).
 */
#define BC_SOCKET_REUSEADDRESS		0x01U

/*%
 * Some systems define the socket length argument as an int, some as size_t,
 * some as socklen_t.  This is here so it can be easily changed if needed.
 */
#ifndef BC_SOCKADDR_LEN_T
#define BC_SOCKADDR_LEN_T unsigned int
#endif

/*%
 * Size of per-FD lock buckets.
 */
#ifdef BC_PLATFORM_USETHREADS
#define FDLOCK_COUNT		1024
#define FDLOCK_ID(fd)		((fd) % FDLOCK_COUNT)
#else
#define FDLOCK_COUNT		1
#define FDLOCK_ID(fd)		0
#endif	/* BC_PLATFORM_USETHREADS */


/*%
 * Statistics counters.  Used as BCStatsCounterType values.
 */
enum {
	bc_sockstatscounter_udp4open = 0,
	bc_sockstatscounter_udp6open = 1,
	bc_sockstatscounter_tcp4open = 2,
	bc_sockstatscounter_tcp6open = 3,
	bc_sockstatscounter_unixopen = 4,

	bc_sockstatscounter_udp4openfail = 5,
	bc_sockstatscounter_udp6openfail = 6,
	bc_sockstatscounter_tcp4openfail = 7,
	bc_sockstatscounter_tcp6openfail = 8,
	bc_sockstatscounter_unixopenfail = 9,

	bc_sockstatscounter_udp4close = 10,
	bc_sockstatscounter_udp6close = 11,
	bc_sockstatscounter_tcp4close = 12,
	bc_sockstatscounter_tcp6close = 13,
	bc_sockstatscounter_unixclose = 14,
	bc_sockstatscounter_fdwatchclose = 15,

	bc_sockstatscounter_udp4bindfail = 16,
	bc_sockstatscounter_udp6bindfail = 17,
	bc_sockstatscounter_tcp4bindfail = 18,
	bc_sockstatscounter_tcp6bindfail = 19,
	bc_sockstatscounter_unixbindfail = 20,
	bc_sockstatscounter_fdwatchbindfail = 21,

	bc_sockstatscounter_udp4connect = 22,
	bc_sockstatscounter_udp6connect = 23,
	bc_sockstatscounter_tcp4connect = 24,
	bc_sockstatscounter_tcp6connect = 25,
	bc_sockstatscounter_unixconnect = 26,
	bc_sockstatscounter_fdwatchconnect = 27,

	bc_sockstatscounter_udp4connectfail = 28,
	bc_sockstatscounter_udp6connectfail = 29,
	bc_sockstatscounter_tcp4connectfail = 30,
	bc_sockstatscounter_tcp6connectfail = 31,
	bc_sockstatscounter_unixconnectfail = 32,
	bc_sockstatscounter_fdwatchconnectfail = 33,

	bc_sockstatscounter_tcp4accept = 34,
	bc_sockstatscounter_tcp6accept = 35,
	bc_sockstatscounter_unixaccept = 36,

	bc_sockstatscounter_tcp4acceptfail = 37,
	bc_sockstatscounter_tcp6acceptfail = 38,
	bc_sockstatscounter_unixacceptfail = 39,

	bc_sockstatscounter_udp4sendfail = 40,
	bc_sockstatscounter_udp6sendfail = 41,
	bc_sockstatscounter_tcp4sendfail = 42,
	bc_sockstatscounter_tcp6sendfail = 43,
	bc_sockstatscounter_unixsendfail = 44,
	bc_sockstatscounter_fdwatchsendfail = 45,

	bc_sockstatscounter_udp4recvfail = 46,
	bc_sockstatscounter_udp6recvfail = 47,
	bc_sockstatscounter_tcp4recvfail = 48,
	bc_sockstatscounter_tcp6recvfail = 49,
	bc_sockstatscounter_unixrecvfail = 50,
	bc_sockstatscounter_fdwatchrecvfail = 51,

	bc_sockstatscounter_max = 52
};

/*@{*/
/*!
 * _ATTACHED:	Internal use only.
 * _TRUNC:	Packet was truncated on receive.
 * _CTRUNC:	Packet control information was truncated.  This can
 *		indicate that the packet is not complete, even though
 *		all the data is valid.
 * _TIMESTAMP:	The timestamp member is valid.
 * _PKTINFO:	The pktinfo member is valid.
 * _MULTICAST:	The UDP packet was received via a multicast transmission.
 */
#define BC_SOCKEVENTATTR_ATTACHED		0x80000000U /* internal */
#define BC_SOCKEVENTATTR_TRUNC			0x00800000U /* public */
#define BC_SOCKEVENTATTR_CTRUNC			0x00400000U /* public */
#define BC_SOCKEVENTATTR_TIMESTAMP		0x00200000U /* public */
#define BC_SOCKEVENTATTR_PKTINFO		0x00100000U /* public */
#define BC_SOCKEVENTATTR_MULTICAST		0x00080000U /* public */
/*@}*/

#define BC_SOCKEVENT_ANYEVENT	(0)
#define BC_SOCKEVENT_RECVDONE	(BC_EVENTCLASS_SOCKET + 1)
#define BC_SOCKEVENT_SENDDONE	(BC_EVENTCLASS_SOCKET + 2)
#define BC_SOCKEVENT_NEWCONN	(BC_EVENTCLASS_SOCKET + 3)
#define BC_SOCKEVENT_CONNECT	(BC_EVENTCLASS_SOCKET + 4)

/*
 * Internal events.
 */
#define BC_SOCKEVENT_INTR	(BC_EVENTCLASS_SOCKET + 256)
#define BC_SOCKEVENT_INTW	(BC_EVENTCLASS_SOCKET + 257)

typedef enum BCSocketTypeE
{
	bc_sockettype_unknown	= 0,
	bc_sockettype_udp		= 1,
	bc_sockettype_tcp		= 2,
	bc_sockettype_unix		= 3,
	bc_sockettype_fdwatch	= 4
} BCSocketTypeE;

/*@{*/
/*!
 * How a socket should be shutdown in bc_socket_shutdown() calls.
 */
#define BC_SOCKSHUT_RECV	0x00000001	/*%< close read side */
#define BC_SOCKSHUT_SEND	0x00000002	/*%< close write side */
#define BC_SOCKSHUT_ALL		0x00000003	/*%< close them all */
/*@}*/

/*@{*/
/*!
 * What I/O events to cancel in bc_socket_cancel() calls.
 */
#define BC_SOCKCANCEL_RECV		0x00000001	/*%< cancel recv */
#define BC_SOCKCANCEL_SEND		0x00000002	/*%< cancel send */
#define BC_SOCKCANCEL_ACCEPT	0x00000004	/*%< cancel accept */
#define BC_SOCKCANCEL_CONNECT	0x00000008	/*%< cancel connect */
#define BC_SOCKCANCEL_ALL		0x0000000f	/*%< cancel everything */
/*@}*/

/*@{*/
/*!
 * Flags for bc_socket_send() and bc_socket_recv() calls.
 */
#define BC_SOCKFLAG_IMMEDIATE	0x00000001	/*%< send event only if needed */
#define BC_SOCKFLAG_NORETRY		0x00000002	/*%< drop failed UDP sends */
/*@}*/

/*@{*/
/*!
 * Flags for fdwatchcreate.
 */
#define BC_SOCKFDWATCH_READ		0x00000001	/*%< watch for readable */
#define BC_SOCKFDWATCH_WRITE	0x00000002	/*%< watch for writable */
/*@}*/


#define BCAPI_SOCKETMGR_MAGIC		BC_MAGIC('A','s','m','g')
#define BCAPI_SOCKETMGR_VALID(m)	((m) != NULL && \
					 (m)->magic == BCAPI_SOCKETMGR_MAGIC)


#define BCAPI_SOCKET_MAGIC	BC_MAGIC('A','s','c','t')
#define BCAPI_SOCKET_VALID(s)	((s) != NULL && \
				 (s)->magic == BCAPI_SOCKET_MAGIC)

/*
 * Define a maximum number of I/O Completion Port worker threads
 * to handle the load on the Completion Port. The actual number
 * used is the number of CPU's + 1.
 */
#define MAX_IOCPTHREADS 20

#define BCSOCKET_RECVBUF_SIZE			65536


class BCSocket;
class BCThread;
class BCBuffer;
class BCSocketMgr;

typedef int				BCStatsCounterType;
typedef int (*BCSockFDWatchPtr)(BCTask *, BCSocket *, void *, int);
typedef BCSockFDWatchPtr		LPFN_BCSockFDWatch;

///////////////////////////////////////////////////////////////////////////////
// class : BCSockEvent - Note : Create instance with 'new' operator
///////////////////////////////////////////////////////////////////////////////

class BC_API BCSockEvent : public BCTaskEvent
{
	DECLARE_FIXED_ALLOC(BCSockEvent);

public:
	BCSockEvent();
	BCSockEvent(
		void *sender,
		BCEventType type,
		LPFN_BCTaskAction action,
		const void *arg);
	virtual ~BCSockEvent();

	void	SetAddress(BCSocket *sock, BCSockAddrS *address);
	void	Destroy();

#ifdef _DEBUG
	static ULONGLONG	GetAllocCount();
#endif
public:
	BCRESULT				result;		/*%< OK, EOF, whatever else */
	uint32_t				minimum;	/*%< minimum i/o for event */
	uint32_t				n;			/*%< bytes read or written */
	uint32_t				offset;		/*%< offset into buffer list */
	BCRegionS				region[BC_SOCKET_MAXSCATTERGATHER];		/*%< for buffers i/o can't use bufferlist*/
	uint32_t				region_size;/*%< size of region buffers */
	BCBuffer			*	bufferlist;	/*%< list of buffers */
	uint32_t				bufferlist_size; /*%< list of buffers size*/
	BCSockAddrS				address;	/*%< source address */
	BCTimeS				    timestamp;	/*%< timestamp of packet recv */
	struct in6_pktinfo		pktinfo;	/*%< ipv6 pktinfo */
	uint32_t				attributes;	/*%< see below */
protected:
private:
	DECLARE_NO_COPY_CLASS(BCSockEvent);
};

typedef TNodeList<BCSockEvent>			BCSockEventList;

///////////////////////////////////////////////////////////////////////////////
// class : BCSockNCEvent - Note : Create instance with 'new' operator
///////////////////////////////////////////////////////////////////////////////

class BC_API BCSockICEvent : public BCTaskEvent
{
	DECLARE_FIXED_ALLOC(BCSockICEvent);

public:
	BCSockICEvent();
	BCSockICEvent(
		void *sender,
		BCEventType type,
		LPFN_BCTaskAction action,
		const void *arg);
	virtual ~BCSockICEvent();

	void	Destroy();

#ifdef _DEBUG
	static ULONGLONG	GetAllocCount();
#endif
public:
	BCSocket		*	newsocket;
	BCRESULT			result;		/*%< OK, EOF, whatever else */
	BCSockAddrS			address;	/*%< source address */
protected:
private:
	DECLARE_NO_COPY_CLASS(BCSockICEvent);
};

typedef TNodeList<BCSockICEvent>		BCSockICEventList;

///////////////////////////////////////////////////////////////////////////////
// class : BCSockOCEvent - Note : Create instance with 'new' operator
///////////////////////////////////////////////////////////////////////////////

class BC_API BCSockOCEvent : public BCTaskEvent
{
	DECLARE_FIXED_ALLOC(BCSockOCEvent);

public:
	BCSockOCEvent();
	BCSockOCEvent(
		void *sender,
		BCEventType type,
		LPFN_BCTaskAction action,
		const void *arg);
	virtual ~BCSockOCEvent();

	void	Destroy();

#ifdef _DEBUG
	static ULONGLONG	GetAllocCount();
#endif
public:
	BCRESULT			result;		/*%< OK, EOF, whatever else */
protected:
private:
	DECLARE_NO_COPY_CLASS(BCSockOCEvent);
};

typedef TNodeList<BCSockOCEvent>		BCSockOCEventList;

///////////////////////////////////////////////////////////////////////////////
// class : BCSocket
///////////////////////////////////////////////////////////////////////////////

class BC_API BCSocket
	: public BCNodeList::Node
	, public BCMagic
{
	DECLARE_FIXED_ALLOC(BCSocket);

	friend class BCSockEvent;
	friend class BCSockICEvent;
	friend class BCSockOCEvent;
	friend class BCSocketMgr;
public:
	BCSocket();
	virtual ~BCSocket();

	BCRESULT				Create(
                                BCSocketMgr *pMgr,
                                int nPf,
                                BCSocketTypeE eType);
	void					Attach(BCSocket **ppSocket);
	void					Detach(BCSocket **ppSocket);
	BCRESULT				Recv(
                                BCRegionS *pRegion,
                                uint32_t minimum,
                                BCTask *task,
                                LPFN_BCTaskAction action,
                                const void *arg);
	BCRESULT				Recv2(
                                BCRegionS *pRegion,
                                uint32_t minimum,
                                BCTask *task,
                                BCSockEvent *pEvent,
                                uint32_t nFlags);
	BCRESULT				RecvV(
                                BCBuffer *buflist,
                                uint32_t minimum,
                                BCTask *task,
                                LPFN_BCTaskAction action,
                                const void *arg);
	BCRESULT				RecvV2(
                                BCBuffer *buflist,
                                uint32_t minimum,
                                BCTask *task,
                                BCSockEvent *pEvent,
                                uint32_t nFlags);
	BCRESULT				Send(
                                BCRegionS *region,
								uint32_t region_size,
                                BCTask *task,
                                LPFN_BCTaskAction action,
                                const void *arg);
	BCRESULT				SendTo(
                                BCRegionS *region,
								uint32_t region_size,
                                BCTask *task,
                                LPFN_BCTaskAction action,
                                const void *arg,
                                BCSockAddrS *pAddress,
                                struct in6_pktinfo *pktinfo);
	BCRESULT				SendTo2(
                                BCRegionS *region,
								uint32_t region_size,
                                BCTask *task,
                                BCSockAddrS *address,
                                struct in6_pktinfo *pktinfo,
                                BCSockEvent *pEvent,
                                uint32_t flags);
	BCRESULT				SendV(
                                BCBuffer *buflist,
                                BCTask *task,
                                LPFN_BCTaskAction action,
                                const void *arg);
	BCRESULT				SendToV(
                                BCBuffer *buflist,
                                BCTask *task,
                                LPFN_BCTaskAction action,
                                const void *arg,
                                BCSockAddrS *address,
                                struct in6_pktinfo *pktinfo);
	BCRESULT				SendToV2(
                                BCBuffer *buflist,
                                BCTask *task,
                                BCSockAddrS *address,
                                struct in6_pktinfo *pktinfo,
                                BCSockEvent *pEvent,
                                uint32_t flags);
	BCRESULT				Bind(
                                BCSockAddrS *sockaddr,
                                uint32_t options);
	BCRESULT				Filter(const char *szFilter);
	BCRESULT				Listen(uint32_t backlog);
	BCRESULT				Accept(
                                BCTask *task,
                                LPFN_BCTaskAction action,
                                const void *arg);
	BCRESULT				Connect(
                                BCSockAddrS *addr,
                                BCTask *task,
                                LPFN_BCTaskAction action,
                                const void *arg);
	BCRESULT				GetPeerName(BCSockAddrS *addressp);
	BCRESULT				GetSockName(BCSockAddrS *addressp);
	void					Cancel(BCTask *task, uint32_t how);
	BCSocketTypeE		    GetType();
#ifdef OS_ANDROID
	int						GetFd() const { return m_nFD; }
#endif
	BOOL					IsBound();
	void					IPv6only(BOOL yes);
	void					SetName(const char *szName, void *pTag);
	const char			*	GetName();
	void				*	GetTag();
	BCSocketMgr			*	GetManager() const;
	uint32_t				GetRef() const
	{
		return m_nRef;
	}

public:
#ifdef _DEBUG
	static ULONGLONG		GetAllocCount();
	uint32_t				GetSendEventCount() const
	{
		return m_lstSendEvents.Count();
	}
#endif
	BCRESULT 				Open() ;
	BCRESULT				Close() ;
	BCRESULT				FDWatchPoke(int flags);
protected:
	inline void 			_SetState(uint32_t eState, uint32_t nLineNumber)
	{
		m_eState			= eState;
		m_nStateLineNO	= nLineNumber;
	}
	BCRESULT				_InternalCreate(BCSocketMgr *pMgr, BCSocketTypeE eType);
	BCRESULT				_Open();
	BOOL					_IsSendDoneActive(BCSockEvent *dev);
	BOOL					_IsAcceptDoneActive(BCSockICEvent *dev);
	BOOL					_IsConnectDoneActive(BCSockOCEvent *dev);
	void					_SendRecvDoneEvent(BCSockEvent **dev);
	void					_SendSendDoneEvent(BCSockEvent **dev);
	void					_SendAcceptDoneEvent(BCSockICEvent **adev);
	void					_SendConnectDoneEvent(BCSockOCEvent **cdev);
	BCRESULT				_Recv(
                                BCSockEvent *dev,
                                BCTask *task,
                                uint32_t flags);
	BCRESULT				_Send(
                                BCSockEvent *dev,
                                BCTask *task,
                                BCSockAddrS *pAddress,
                                struct in6_pktinfo *pktinfo,
                                uint32_t flags);

	virtual void			_Destroy();
protected:
	static void				_Close(BCSocketMgr *pMgr, BCSocket *pSocket, int fd);
	static void				_Free(BCSocket **ppSock);
	static void				_Destroy(BCSocket **ppSocket);
	static void 			_ProcessCMsg(BCSocket *sock, struct msghdr *msg, BCSockEvent *dev) ;
	static void				_BuildMsgHdrSend(
                                BCSocket *sock,
                                BCSockEvent *dev,
                                struct msghdr *msg,
                                struct iovec *iov,
                                size_t *write_countp);
	static void				_BuildMsgHdrRecv(
                                BCSocket *sock,
                                BCSockEvent *dev,
                                struct msghdr *msg,
                                struct iovec *iov,
                                size_t *read_countp);
	static void				_InternalAccept(BCTask *me, BCTaskEvent *ev) ;
	static void				_InternalConnect(BCTask *me, BCTaskEvent *ev);
	static void				_SetDevAddress(
                                BCSockAddrS *address,
                                BCSocket *sock,
                                BCSockEvent *dev);
	static int				_DoIoRecv(BCSocket *sock, BCSockEvent *dev) ;
	static int				_DoIoSend(BCSocket *sock, BCSockEvent *dev) ;
	void					_DispatchRecv();
	void 					_DispatchSend();
	void 					_DispatchAccept() ;
	void 					_DispatchConnect() ;
	static void 			_InternalRecv(BCTask *me, BCTaskEvent *ev) ;
	static void 			_InternalSend(BCTask *me, BCTaskEvent *ev) ;
	static void 			_InteranlFDWatchWrite(BCTask *me, BCTaskEvent *ev) ;
	static void 			_InteranlFDWatchRead(BCTask *me, BCTaskEvent *ev);
private:
	DECLARE_NO_COPY_CLASS(BCSocket);
	/* Not locked. */
	BCSocketMgr					*	m_pMgr;
	BCSpinMutex						m_sLock;
	BCSocketTypeE					m_eType;
	const BCStatsCounterType	*	m_pStatsIndex;

	/* Locked by socket lock. */
	uint32_t						m_nRef; /* EXTERNAL references */
	int								m_nFD;	/* file handle */
	int								m_nPF;	/* protocol family */
	char							m_szName[16];
	void						*	m_pTag;

	BCSockEventList					m_lstSendEvents;
	BCSockEventList					m_lstRecvEvents;
	BCSockICEventList				m_lstAcceptEvents;
	BCSockOCEvent				*	m_pConnectEvent;

	/*
	 * Internal events.  Posted when a descriptor is readable or
	 * writable.  These are statically allocated and never freed.
	 * They will be set to non-purgable before use.
	 */
	BCTaskEvent						m_sReadableEvent;
	BCTaskEvent						m_sWritableEvent;

	BCSockAddrS						m_sAddress;  /* remote address */

    uint32_t						m_nPendingRecv : 1,
                                    m_nPendingSend : 1,
                                    m_nPendingAccept : 1,
                                    m_bListener : 1, /* listener socket */
                                    m_bConnected : 1,
                                    m_bConnecting : 1, /* connect pending */
                                    m_bBound : 1; /* bound to local addr */
#ifdef BC_NET_RECVOVERFLOW
	unsigned char					m_cOverflow; /* used for MSG_TRUNC fake */
#endif

	char						*	m_pRecvCmsgBuf;
	BC_SOCKADDR_LEN_T				m_nRecvCmsgBufLen;
	char						*	m_pSendCmsgBuf;
	BC_SOCKADDR_LEN_T				m_nSendCmsgBufLen;

	void						*	m_pFdWatchArg;
	LPFN_BCSockFDWatch				m_pFdWatchCB;
	int							    m_nFdWatchFlags;
	BCTask						*	m_pFdWatchTask;

	// extra
	uint32_t					    m_eState; /* Socket state. Debugging and consistency checking. */
	int32_t					        m_nStateLineNO;	/* line which last touched state */
	KBPool		            		m_sPool;
};

typedef TNodeList<BCSocket>			BCSocketList;

///////////////////////////////////////////////////////////////////////////////
// class : BCSocketMgr
///////////////////////////////////////////////////////////////////////////////

class BC_API BCSocketMgr : public BCMagic
{
	DECLARE_FIXED_ALLOC(BCSocketMgr);

	friend class BCSocket;

	typedef struct FDCtrlCtx{
		BCSpinMutex				lock;
		/* Locked by fdlock. */
		typedef std::unordered_map<int32_t, BCSocket*>	FdSocketMap;
		FdSocketMap		   		mapFdSocket;
		typedef std::unordered_map<int32_t, int32_t>	FdStateMap;
		FdStateMap				mapFdState;

#ifdef USE_DEVPOLL
		typedef std::unordered_map<int32_t, pollinfo_t>	FdPollInfoMap;
		FdPollInfoMap			mapFdPollInfo;
#endif
	}FDCtrlCtx;
public:
	BCSocketMgr();
	virtual ~BCSocketMgr();

	BCRESULT				Create(uint32_t nMaxSocks = 0);

	static BCRESULT			Create(
                                BCSocketMgr **managerp,
                                unsigned int maxsocks = 0);
	static void				Destroy(BCSocketMgr **ppMgr);

	BCRESULT				FDWatchCreate(
                                int fd,
                                int flags,
                                LPFN_BCSockFDWatch callback,
                                void *cbarg,
                                BCTask *task,
                                BCSocket **socketp);
	void					SetReserved(uint32_t reserved);
	void					Maxudp(int maxudp);
	BCRESULT				GetMaxSockets(unsigned int *nsockp);
	void					SetStats(BCStats *stats);
	uint32_t				GetSocketCount() const;
protected:
	virtual void			_Destroy();

	BCRESULT 				_WatchFD(int fd, int msg) ;
	BCRESULT 				_UnwatchFD(int fd, int msg);
	void					_SelectPoke(int fd, int msg);
	void					_SelectReadMsg(int *fd, int *msg) ;
	void					_ProcessFD(
                                int fd,
                                BOOL readable,
                                BOOL writeable);
#ifdef USE_KQUEUE
	BOOL					_ProcessFDs(struct kevent *events, int nevents) ;
#elif defined(USE_EPOLL)
	BOOL					_ProcessFDs(struct epoll_event *events, int nevents);
#elif defined(USE_DEVPOLL)
	BOOL					_ProcessFDs(struct pollfd *events, int nevents) ;
#elif defined(USE_SELECT)
	void					_ProcessFDs(int maxfd, fd_set *readfds, fd_set *writefds);
#endif // USE_KQUEUE
	void					_WakeupSocket(int fd, int msg);
#ifdef USE_WATCHER_THREAD
	BOOL					_ProcessCtlfd();
	BCRESULT				_SetupWatcher();
	void					_CleanupWatcher();
	static void 		*	_WatcherThreadFunc(void *uap);
#endif // USE_WATCHER_THREAD
#ifndef USE_WATCHER_THREAD
	static int				_WaitEvents(
								BCSocketMgr *pMgr,
								struct timeval *tvp,
								BCSocketWaitType **swaitp);
	static BCRESULT			_Dispatch(
                                BCSocketMgr *pMgr,
                                BCSocketWaitType *swait) ;
#endif // USE_WATCHER_THREAD
private:
	DECLARE_NO_COPY_CLASS(BCSocketMgr);
	/* Not locked. */
	BCMutex					m_sLock;
	FDCtrlCtx				m_sFdCtx[FDLOCK_COUNT];
	BCStats				*	m_pStats;

#ifdef USE_KQUEUE
	int						m_nKqueueFd;
	int						m_nEvents;
	struct kevent		*	m_pEvents;
#endif	/* USE_KQUEUE */
#ifdef USE_EPOLL
	int						m_nEpollFd;
	int						m_nEvents;
	struct epoll_event	*	m_pEvents;
#endif	/* USE_EPOLL */
#ifdef USE_DEVPOLL
	int						m_nDevpollFd;
	int						m_nEvents;
	struct pollfd		*	m_pEvents;
#endif	/* USE_DEVPOLL */
#ifdef USE_SELECT
	int						m_nFdBufSize;
#endif	/* USE_SELECT */
	unsigned int			m_nMaxSocks;
#ifdef BC_PLATFORM_USETHREADS
	int						m_sPipeFds[2];
#endif

	/* Locked by manager lock. */
	BCSocketList			m_lstSockets;
#ifdef USE_SELECT
	fd_set				*	m_pReadFds;
	fd_set				*	m_pReadFdsCopy;
	fd_set				*	m_pWriteFds;
	fd_set				*	m_pWriteFdsCopy;
	int						m_nMaxFd;
#endif	/* USE_SELECT */
	int						m_nReserved;	/* unlocked */
#ifdef USE_WATCHER_THREAD
	BCThread			*	m_pWatcherThrd;
	BCCondition				m_sCondShutdownOk;
#else /* USE_WATCHER_THREAD */
	unsigned int			m_nRefs;
#endif /* USE_WATCHER_THREAD */
	int						m_nMaxUdp;

	StackMemPool			m_sPool;

	BCHashTable				m_htSockEvents;

	/*
	 * Debugging.
	 * Modified by InterlockedIncrement() and InterlockedDecrement()
	 */
	uint32_t				m_nTotalSockets;
};

///////////////////////////////////////////////////////////////////////////////
// socket unitilities :
///////////////////////////////////////////////////////////////////////////////

BC_API
void
bc_socket_cleanunix(BCSockAddrS *sockaddr, BOOL active);

BC_API
BCRESULT
bc_socket_permunix();

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
