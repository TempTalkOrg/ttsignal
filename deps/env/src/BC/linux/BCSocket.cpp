
#include <sys/stat.h>
#include "BC/BCTask.h"
#include "BC/BCBuffer.h"
#include "BC/BCLog.h"
#include "BC/BCSocket.h"


#ifdef __GNUC__
#define BC_FORMAT_PRINTF(fmt, args) __attribute__((__format__(__printf__, fmt, args)))
#else
#define BC_FORMAT_PRINTF(fmt, args)
#endif

#ifndef PORT_NONBLOCK
#define PORT_NONBLOCK			O_NONBLOCK
#endif // PORT_NONBLOCK

#ifndef INVALID_SOCKET
#define INVALID_SOCKET			(int)(~0)
#endif // INVALID_SOCKET

#define LOCK(x)			(x)->Lock()
#define UNLOCK(x)		(x)->Unlock()

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////



/*%
 * Maximum number of allowable open sockets.  This is also the maximum
 * allowable socket file descriptor.
 *
 * Care should be taken before modifying this value for select():
 * The API standard doesn't ensure select() accept more than (the system default
 * of) FD_SETSIZE descriptors, and the default size should in fact be fine in
 * the vast majority of cases.  This constant should therefore be increased only
 * when absolutely necessary and possible, i.e., the server is exhausting all
 * available file descriptors (up to FD_SETSIZE) and the select() function
 * and FD_xxx macros support larger values than FD_SETSIZE (which may not
 * always by true, but we keep using some of them to ensure as much
 * portability as possible).  Note also that overall server performance
 * may be rather worsened with a larger value of this constant due to
 * inherent scalability problems of select().
 *
 * As a special note, this value shouldn't have to be touched if
 * this is a build for an authoritative only DNS server.
 */
#ifndef BC_SOCKET_MAXSOCKETS
#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_DEVPOLL)
#define BC_SOCKET_MAXSOCKETS (512*1024)//4096
#elif defined(USE_SELECT)
#define BC_SOCKET_MAXSOCKETS FD_SETSIZE
#endif	/* USE_KQUEUE... */
#endif	/* BC_SOCKET_MAXSOCKETS */

#ifdef USE_SELECT
/*%
 * Mac OS X needs a special definition to support larger values in select().
 * We always define this because a larger value can be specified run-time.
 */
#ifdef __APPLE__
#define _DARWIN_UNLIMITED_SELECT
#endif	/* __APPLE__ */
#endif	/* USE_SELECT */

#ifdef BC_SOCKET_USE_POLLWATCH
/*%
 * If this macro is defined, enable workaround for a Solaris /dev/poll kernel
 * bug: DP_POLL ioctl could keep sleeping even if socket I/O is possible for
 * some of the specified FD.  The idea is based on the observation that it's
 * likely for a busy server to keep receiving packets.  It specifically works
 * as follows: the socket watcher is first initialized with the state of
 * "poll_idle".  While it's in the idle state it keeps sleeping until a socket
 * event occurs.  When it wakes up for a socket I/O event, it moves to the
 * poll_active state, and sets the poll timeout to a short period
 * (BC_SOCKET_POLLWATCH_TIMEOUT msec).  If timeout occurs in this state, the
 * watcher goes to the poll_checking state with the same timeout period.
 * In this state, the watcher tries to detect whether this is a break
 * during intermittent events or the kernel bug is triggered.  If the next
 * polling reports an event within the short period, the previous timeout is
 * likely to be a kernel bug, and so the watcher goes back to the active state.
 * Otherwise, it moves to the idle state again.
 *
 * It's not clear whether this is a thread-related bug, but since we've only
 * seen this with threads, this workaround is used only when enabling threads.
 */

typedef enum { poll_idle, poll_active, poll_checking } pollstate_t;

#ifndef BC_SOCKET_POLLWATCH_TIMEOUT
#define BC_SOCKET_POLLWATCH_TIMEOUT 10
#endif	/* BC_SOCKET_POLLWATCH_TIMEOUT */
#endif	/* BC_SOCKET_USE_POLLWATCH */

/*%
 * Maximum number of events communicated with the kernel.  There should normally
 * be no need for having a large number.
 */
#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_DEVPOLL)
#ifndef BC_SOCKET_MAXEVENTS
#define BC_SOCKET_MAXEVENTS	10240//1024//64
#endif
#endif

/*%
 * Some systems define the socket length argument as an int, some as size_t,
 * some as socklen_t.  This is here so it can be easily changed if needed.
 */
#ifndef BC_SOCKADDR_LEN_T
#define BC_SOCKADDR_LEN_T unsigned int
#endif

/*%
 * Define what the possible "soft" errors can be.  These are non-fatal returns
 * of various network related functions, like recv() and so on.
 *
 * For some reason, BSDI (and perhaps others) will sometimes return <0
 * from recv() but will have errno==0.  This is broken, but we have to
 * work around it here.
 */
#define SOFT_ERROR(e)	((e) == EAGAIN || \
			 (e) == EWOULDBLOCK || \
			 (e) == EINTR || \
			 (e) == 0)

#define DLVL(x) BC_LOGCATEGORY_GENERAL, BC_LOGMODULE_SOCKET, BC_LOG_DEBUG(x)

/*!<
 * DLVL(90)  --  Function entry/exit and other tracing.
 * DLVL(70)  --  Socket "correctness" -- including returning of events, etc.
 * DLVL(60)  --  Socket data send/receive
 * DLVL(50)  --  Event tracing, including receiving/sending completion events.
 * DLVL(20)  --  Socket creation/destruction.
 */
#define TRACE_LEVEL		90
#define CORRECTNESS_LEVEL	70
#define IOEVENT_LEVEL		60
#define EVENT_LEVEL		50
#define CREATION_LEVEL		20

#define TRACE		DLVL(TRACE_LEVEL)
#define CORRECTNESS	DLVL(CORRECTNESS_LEVEL)
#define IOEVENT		DLVL(IOEVENT_LEVEL)
#define EVENT		DLVL(EVENT_LEVEL)
#define CREATION	DLVL(CREATION_LEVEL)

typedef BCTaskEvent intev_t;

#define SOCKET_MAGIC		BC_MAGIC('I', 'O', 'i', 'o')
#define VALID_SOCKET(s)		BC_MAGIC_VALID(s, SOCKET_MAGIC)

/*!
 * IPv6 control information.  If the socket is an IPv6 socket we want
 * to collect the destination address and interface so the client can
 * set them on outgoing packets.
 */
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
#ifndef USE_CMSG
#define USE_CMSG	1
#endif
#endif

/*%
 * NetBSD and FreeBSD can timestamp packets.  XXXMLG Should we have
 * a setsockopt() like interface to request timestamps, and if the OS
 * doesn't do it for us, call gettimeofday() on every UDP receive?
 */
#ifdef SO_TIMESTAMP
#ifndef USE_CMSG
#define USE_CMSG	1
#endif
#endif

/*%
 * The size to raise the receive buffer to (Be carefully to set recv 
 * buffer according this value).
 */
#define RCVBUFSIZE (32*1024)

/*%
 * The number of times a send operation is repeated if the result is EINTR.
 */
#define NRETRIES 10


#define NEWCONNSOCK(ev) ((BCSocket *)(ev)->newsocket)


#define _set_state(sock, _state) (sock)->_SetState(_state, __LINE__)

#define SOCKET_MANAGER_MAGIC	BC_MAGIC('I', 'O', 'm', 'g')
#define VALID_MANAGER(m)		BC_MAGIC_VALID(m, SOCKET_MANAGER_MAGIC)


#ifdef USE_SHARED_MANAGER
static BCSocketMgr *socketmgr = NULL;
#endif /* USE_SHARED_MANAGER */

#define CLOSED			0	/* this one must be zero */
#define MANAGED			1
#define CLOSE_PENDING	2

/*
 * send() and recv() iovec counts
 */
#define MAXSCATTERGATHER_SEND	(BC_SOCKET_MAXSCATTERGATHER)
#ifdef BC_NET_RECVOVERFLOW
# define MAXSCATTERGATHER_RECV	(BC_SOCKET_MAXSCATTERGATHER + 1)
#else
# define MAXSCATTERGATHER_RECV	(BC_SOCKET_MAXSCATTERGATHER)
#endif

/*%
 * The following can be either static or public, depending on build environment.
 */

#ifdef BCLOUD
#define BC_SOCKETFUNC_SCOPE
#else
#define BC_SOCKETFUNC_SCOPE static
#endif

#define SELECT_POKE_SHUTDOWN		(-1)
#define SELECT_POKE_NOTHING		(-2)
#define SELECT_POKE_READ		(-3)
#define SELECT_POKE_ACCEPT		(-3) /*%< Same as _READ */
#define SELECT_POKE_WRITE		(-4)
#define SELECT_POKE_CONNECT		(-4) /*%< Same as _WRITE */
#define SELECT_POKE_CLOSE		(-5)

#define SOCK_DEAD(s)			((s)->m_nRef == 0)

/*%
 * Shortcut index arrays to get access to statistics counters.
 */
enum {
	STATID_OPEN = 0,
	STATID_OPENFAIL = 1,
	STATID_CLOSE = 2,
	STATID_BINDFAIL = 3,
	STATID_CONNECTFAIL = 4,
	STATID_CONNECT = 5,
	STATID_ACCEPTFAIL = 6,
	STATID_ACCEPT = 7,
	STATID_SENDFAIL = 8,
	STATID_RECVFAIL = 9
};
static const BCStatsCounterType upd4statsindex[] = {
	bc_sockstatscounter_udp4open,
	bc_sockstatscounter_udp4openfail,
	bc_sockstatscounter_udp4close,
	bc_sockstatscounter_udp4bindfail,
	bc_sockstatscounter_udp4connectfail,
	bc_sockstatscounter_udp4connect,
	-1,
	-1,
	bc_sockstatscounter_udp4sendfail,
	bc_sockstatscounter_udp4recvfail
};
static const BCStatsCounterType upd6statsindex[] = {
	bc_sockstatscounter_udp6open,
	bc_sockstatscounter_udp6openfail,
	bc_sockstatscounter_udp6close,
	bc_sockstatscounter_udp6bindfail,
	bc_sockstatscounter_udp6connectfail,
	bc_sockstatscounter_udp6connect,
	-1,
	-1,
	bc_sockstatscounter_udp6sendfail,
	bc_sockstatscounter_udp6recvfail
};
static const BCStatsCounterType tcp4statsindex[] = {
	bc_sockstatscounter_tcp4open,
	bc_sockstatscounter_tcp4openfail,
	bc_sockstatscounter_tcp4close,
	bc_sockstatscounter_tcp4bindfail,
	bc_sockstatscounter_tcp4connectfail,
	bc_sockstatscounter_tcp4connect,
	bc_sockstatscounter_tcp4acceptfail,
	bc_sockstatscounter_tcp4accept,
	bc_sockstatscounter_tcp4sendfail,
	bc_sockstatscounter_tcp4recvfail
};
static const BCStatsCounterType tcp6statsindex[] = {
	bc_sockstatscounter_tcp6open,
	bc_sockstatscounter_tcp6openfail,
	bc_sockstatscounter_tcp6close,
	bc_sockstatscounter_tcp6bindfail,
	bc_sockstatscounter_tcp6connectfail,
	bc_sockstatscounter_tcp6connect,
	bc_sockstatscounter_tcp6acceptfail,
	bc_sockstatscounter_tcp6accept,
	bc_sockstatscounter_tcp6sendfail,
	bc_sockstatscounter_tcp6recvfail
};
static const BCStatsCounterType unixstatsindex[] = {
	bc_sockstatscounter_unixopen,
	bc_sockstatscounter_unixopenfail,
	bc_sockstatscounter_unixclose,
	bc_sockstatscounter_unixbindfail,
	bc_sockstatscounter_unixconnectfail,
	bc_sockstatscounter_unixconnect,
	bc_sockstatscounter_unixacceptfail,
	bc_sockstatscounter_unixaccept,
	bc_sockstatscounter_unixsendfail,
	bc_sockstatscounter_unixrecvfail
};
static const BCStatsCounterType fdwatchstatsindex[] = {
	-1,
	-1,
	bc_sockstatscounter_fdwatchclose,
	bc_sockstatscounter_fdwatchbindfail,
	bc_sockstatscounter_fdwatchconnectfail,
	bc_sockstatscounter_fdwatchconnect,
	-1,
	-1,
	bc_sockstatscounter_fdwatchsendfail,
	bc_sockstatscounter_fdwatchrecvfail
};

#if defined(_AIX) && defined(BC_NET_BSD44MSGHDR) && \
    defined(USE_CMSG) && defined(IPV6_RECVPKTINFO)
/*
 * AIX has a kernel bug where IPV6_RECVPKTINFO gets cleared by
 * setting IPV6_V6ONLY.
 */
void
BCSocket::FIX_IPV6_RECVPKTINFO()
{
	char strbuf[BC_STRERRORSIZE];
	int on = 1;

	if (this->m_nPF != AF_INET6 || this->m_eType != bc_sockettype_udp)
		return;

	if (setsockopt(this->m_nFD, IPPROTO_IPV6, IPV6_RECVPKTINFO,
		       (void *)&on, sizeof(on)) < 0)
	{
		LogError(_LOCAL_,
				 "setsockopt(%d, IPV6_RECVPKTINFO) "
				 "%s: %s", this->m_nFD, "failed", strbuf);
	}
}
#else
#define FIX_IPV6_RECVPKTINFO(sock) (void)0
#endif

/*%
 * Increment socket-related statistics counters.
 */
static inline void
inc_stats(BCStats *stats, BCStatsCounterType counterid)
{
	REQUIRE(counterid != -1);

	if (stats != NULL)
		stats->Increment(counterid);
}


/*
 * Make a fd non-blocking.
 */
static BCRESULT
make_nonblock(int fd) {
	int ret;
	int flags;
	char strbuf[BC_STRERRORSIZE];
#ifdef USE_FIONBIO_IOCTL
	int on = 1;

	ret = ioctl(fd, FIONBIO, (char *)&on);
#else
	flags = fcntl(fd, F_GETFL, 0);
	flags |= PORT_NONBLOCK;
	ret = fcntl(fd, F_SETFL, flags);
#endif

	if (ret == -1) {
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
#ifdef USE_FIONBIO_IOCTL
				 "ioctl(%d, FIONBIO, &on): %s", fd,
#else
				 "fcntl(%d, F_SETFL, %d): %s", fd, flags,
#endif
				 strbuf);

		return (BC_R_UNEXPECTED);
	}

	return (BC_R_SUCCESS);
}

#ifdef USE_CMSG
/*
 * Not all OSes support advanced CMSG macros: CMSG_LEN and CMSG_SPACE.
 * In order to ensure as much portability as possible, we provide wrapper
 * functions of these macros.
 * Note that cmsg_space() could run slow on OSes that do not have
 * CMSG_SPACE.
 */
static inline BC_SOCKADDR_LEN_T
cmsg_len(BC_SOCKADDR_LEN_T len) {
#ifdef CMSG_LEN
	return (CMSG_LEN(len));
#else
	BC_SOCKADDR_LEN_T hdrlen;

	/*
	 * Cast NULL so that any pointer arithmetic performed by CMSG_DATA
	 * is correct.
	 */
	hdrlen = (BC_SOCKADDR_LEN_T)CMSG_DATA(((struct cmsghdr *)NULL));
	return (hdrlen + len);
#endif
}

static inline BC_SOCKADDR_LEN_T
cmsg_space(BC_SOCKADDR_LEN_T len) {
#ifdef CMSG_SPACE
	return (CMSG_SPACE(len));
#else
	struct msghdr msg;
	struct cmsghdr *cmsgp;
	/*
	 * XXX: The buffer length is an ad-hoc value, but should be enough
	 * in a practical sense.
	 */
	char dummybuf[sizeof(struct cmsghdr) + 1024];

	memset(&msg, 0, sizeof(msg));
	msg.msg_control = dummybuf;
	msg.msg_controllen = sizeof(dummybuf);

	cmsgp = (struct cmsghdr *)dummybuf;
	cmsgp->cmsg_len = cmsg_len(len);

	cmsgp = CMSG_NXTHDR(&msg, cmsgp);
	if (cmsgp != NULL)
		return ((char *)cmsgp - (char *)msg.msg_control);
	else
		return (0);
#endif
}
#endif /* USE_CMSG */

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
		, attributes(0)
{
	memset(&region, 0, sizeof(region));
	memset(&address, 0, sizeof(address));
	memset(&pktinfo, 0, sizeof(pktinfo));
	memset(&timestamp, 0, sizeof(timestamp));
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
		, attributes(0)
{
	memset(&region, 0, sizeof(region));
	memset(&address, 0, sizeof(address));
	memset(&pktinfo, 0, sizeof(pktinfo));
	memset(&timestamp, 0, sizeof(timestamp));
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
	memset(&address, 0, sizeof(address));
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
	memset(&address, 0, sizeof(address));
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
		, m_pStatsIndex(NULL)
		, m_nRef(0)
		, m_nFD(INVALID_SOCKET)
		, m_nPF(0)
		, m_pTag(NULL)
		, m_pConnectEvent(NULL)
		, m_nPendingRecv(0)
		, m_nPendingSend(0)
		, m_nPendingAccept(0)
		, m_bListener(0)
		, m_bConnected(0)
		, m_bConnecting(0)
		, m_bBound(0)
#ifdef BC_NET_RECVOVERFLOW
		, m_cOverflow(0)
#endif // BC_NET_RECVOVERFLOW
		, m_pRecvCmsgBuf(NULL)
		, m_nRecvCmsgBufLen(0)
		, m_pSendCmsgBuf(NULL)
		, m_nSendCmsgBufLen(0)
		, m_pFdWatchArg(NULL)
		, m_pFdWatchCB(NULL)
		, m_nFdWatchFlags(0)
		, m_pFdWatchTask(NULL)
		, m_eState(0)
		, m_nStateLineNO(0)
{
	memset(m_szName, 0, sizeof(m_szName));
	memset(&m_sAddress, 0, sizeof(m_sAddress));
}

BCSocket::~BCSocket()
{
	//
}

void BCSocket::_Destroy()
{
	delete this;
}

BCRESULT BCSocket::_InternalCreate(BCSocketMgr *pMgr, BCSocketTypeE eType)
{
	BCRESULT result;
	BC_SOCKADDR_LEN_T cmsgbuflen;

	result = BC_R_UNEXPECTED;

	this->m_nMagic = 0;
	this->m_nRef = 0;

	this->m_pMgr = pMgr;
	this->m_eType = eType;
	this->m_nFD = -1;
	this->m_pStatsIndex = NULL;

	this->m_pRecvCmsgBuf = NULL;
	this->m_pSendCmsgBuf = NULL;

	/*
	 * set up cmsg buffers
	 */
	cmsgbuflen = 0;
#if defined(USE_CMSG) && defined(BC_PLATFORM_HAVEIN6PKTINFO)
	cmsgbuflen = cmsg_space(sizeof(struct in6_pktinfo));
#endif
#if defined(USE_CMSG) && defined(SO_TIMESTAMP)
	cmsgbuflen += cmsg_space(sizeof(struct timeval));
#endif
	this->m_nRecvCmsgBufLen = cmsgbuflen;
	if (this->m_nRecvCmsgBufLen != 0U)
	{
		this->m_pRecvCmsgBuf = (char *)m_sPool.Calloc(cmsgbuflen);
		if (this->m_pRecvCmsgBuf == NULL)
			goto error;
	}

	cmsgbuflen = 0;
#if defined(USE_CMSG) && defined(BC_PLATFORM_HAVEIN6PKTINFO)
	cmsgbuflen = cmsg_space(sizeof(struct in6_pktinfo));
#endif
	this->m_nSendCmsgBufLen = cmsgbuflen;
	if (this->m_nSendCmsgBufLen != 0U)
	{
		this->m_pSendCmsgBuf = (char *)m_sPool.Calloc(cmsgbuflen);
		if (this->m_pSendCmsgBuf == NULL)
			goto error;
	}

	memset(this->m_szName, 0, sizeof(this->m_szName));
	this->m_pTag = NULL;

	/*
	 * set up list of readers and writers to be initially empty
	 */
	this->m_pConnectEvent = NULL;
	this->m_nPendingRecv = 0;
	this->m_nPendingSend = 0;
	this->m_nPendingAccept = 0;
	this->m_bListener = 0;
	this->m_bConnected = 0;
	this->m_bConnecting = 0;
	this->m_bBound = 0;

	/*
	 * Initialize readable and writable events
	 */
	this->m_sReadableEvent.ev_attributes = BC_EVENTATTR_NOPURGE;
	this->m_sReadableEvent.ev_type = BC_SOCKEVENT_INTR;
	this->m_sReadableEvent.ev_arg = this;
	this->m_sReadableEvent.ev_sender = this;
	this->m_sWritableEvent.ev_attributes = BC_EVENTATTR_NOPURGE;
	this->m_sWritableEvent.ev_type = BC_SOCKEVENT_INTW;
	this->m_sWritableEvent.ev_arg = this;
	this->m_sWritableEvent.ev_sender = this;

	this->m_nMagic = SOCKET_MAGIC;

	return (BC_R_SUCCESS);

error:
	return (result);
}

/*
 * Kill.
 *
 * Caller must ensure that the socket is not locked and no external
 * references exist.
 */
void BCSocket::_Close(BCSocketMgr *manager, BCSocket *sock, int fd)
{
	BCSocketTypeE type = sock->m_eType;
	int lockid = FDLOCK_ID(fd);

	/*
	 * No one has this socket open, so the watcher doesn't have to be
	 * poked, and the socket doesn't have to be locked.
	 */
	LOCK(&manager->m_sFdCtx[lockid].lock);
	manager->m_sFdCtx[lockid].mapFdSocket.erase(fd);
	if (type == bc_sockettype_fdwatch)
	{
		// manager->m_sFdCtx[lockid].mapFdState[fd] = CLOSED;
		manager->m_sFdCtx[lockid].mapFdState.erase(fd);
	}
	else
	{
		manager->m_sFdCtx[lockid].mapFdState[fd] = CLOSE_PENDING;
	}
	UNLOCK(&manager->m_sFdCtx[lockid].lock);
	if (type == bc_sockettype_fdwatch)
	{
		/*
		 * The caller may close the socket once this function returns,
		 * and `fd' may be reassigned for a new socket.  So we do
		 * _UnwatchFD() here, rather than defer it via select_poke().
		 * Note: this may complicate data protection among threads and
		 * may reduce performance due to additional locks.  One way to
		 * solve this would be to dup() the watched descriptor, but we
		 * take a simpler approach at this moment.
		 */
		(void)manager->_UnwatchFD(fd, SELECT_POKE_READ);
		(void)manager->_UnwatchFD(fd, SELECT_POKE_WRITE);
	}
	else
	{
		manager->_SelectPoke(fd, SELECT_POKE_CLOSE);
	}

	inc_stats(manager->m_pStats, sock->m_pStatsIndex[STATID_CLOSE]);

	/*
	 * update manager->m_nMaxFd here (XXX: this should be implemented more
	 * efficiently)
	 */
#ifdef USE_SELECT
	LOCK(&manager->m_sLock);
	if (manager->m_nMaxFd == fd) {
		int i;

		manager->m_nMaxFd = 0;
		for (i = fd - 1; i >= 0; i--) {
			lockid = FDLOCK_ID(i);

			LOCK(&manager->m_sFdCtx[lockid].lock);
			if (manager->m_sFdCtx[lockid].mapFdState[i] == MANAGED) {
				manager->m_nMaxFd = i;
				UNLOCK(&manager->m_sFdCtx[lockid].lock);
				break;
			}
			UNLOCK(&manager->m_sFdCtx[lockid].lock);
		}
#ifdef BC_PLATFORM_USETHREADS
		if (manager->m_nMaxFd < manager->m_sPipeFds[0])
			manager->m_nMaxFd = manager->m_sPipeFds[0];
#endif
	}
	UNLOCK(&manager->m_sLock);
#endif	/* USE_SELECT */
}

/*
 * This event requires that the various lists be empty, that the reference
 * count be 1, and that the magic number is valid.  The other socket bits,
 * like the lock, must be initialized as well.  The fd associated must be
 * marked as closed, by setting it to -1 on close, or this routine will
 * also close the socket.
 */
void BCSocket::_Free(BCSocket **socketp) {
	BCSocket *sock = *socketp;

	INSIST(sock->m_nRef == 0);
	INSIST(VALID_SOCKET(sock));
	INSIST(!sock->m_bConnecting);
	INSIST(!sock->m_nPendingRecv);
	INSIST(!sock->m_nPendingSend);
	INSIST(!sock->m_nPendingAccept);
	INSIST(sock->m_lstRecvEvents.IsEmpty());
	INSIST(sock->m_lstSendEvents.IsEmpty());
	INSIST(sock->m_lstAcceptEvents.IsEmpty());
	INSIST(!sock->IsLinked());

	sock->m_nMagic = 0;

	sock->_Destroy();

	*socketp = NULL;
}

#ifdef SO_BSDCOMPAT
/*
 * This really should not be necessary to do.  Having to workout
 * which kernel version we are on at run time so that we don't cause
 * the kernel to issue a warning about us using a deprecated socket option.
 * Such warnings should *never* be on by default in production kernels.
 *
 * We can't do this a build time because executables are moved between
 * machines and hence kernels.
 *
 * We can't just not set SO_BSDCOMAT because some kernels require it.
 */

static BCOnceS         bsdcompat_once = BC_ONCE_INIT;
BOOL bsdcompat = TRUE;

static void
clear_bsdcompat(void*) {
#ifdef __linux__
	 struct utsname buf;
	 char *endp;
	 long int major;
	 long int minor;

	 uname(&buf);    /* Can only fail if buf is bad in Linux. */

	 /* Paranoia in parsing can be increased, but we trust uname(). */
	 major = strtol(buf.release, &endp, 10);
	 if (*endp == '.')
	 {
		minor = strtol(endp+1, &endp, 10);
		if ((major > 2) || ((major == 2) && (minor >= 4)))
		{
			bsdcompat = FALSE;
		}
	 }
#endif /* __linux __ */
}
#endif //SO_BSDCOMPAT

BCRESULT BCSocket::_Open()
{
	BCSocket *sock;
	BCSocketMgr *manager;
	char strbuf[BC_STRERRORSIZE];
	const char *err = "socket";
	int tries = 0;
#if defined(USE_CMSG) || defined(SO_BSDCOMPAT)
	int on = 1;
#endif
#if defined(SO_RCVBUF)
	BC_SOCKADDR_LEN_T optlen;
	int size;
#endif

	sock = this;
	manager = m_pMgr;

 again:
	switch (sock->m_eType)
	{
	case bc_sockettype_udp:
		sock->m_nFD = socket(sock->m_nPF, SOCK_DGRAM, IPPROTO_UDP);
		break;
	case bc_sockettype_tcp:
		sock->m_nFD = socket(sock->m_nPF, SOCK_STREAM, IPPROTO_TCP);
		break;
	case bc_sockettype_unix:
		sock->m_nFD = socket(sock->m_nPF, SOCK_STREAM, 0);
		break;
	case bc_sockettype_fdwatch:
		/*
		 * We should not be called for bc_sockettype_fdwatch sockets.
		 */
		INSIST(0);
		break;
	default:
		INSIST(0);
		break;
	}
	if (sock->m_nFD == -1 && errno == EINTR && tries++ < 42)
		goto again;

#ifdef F_DUPFD
	/*
	 * Leave a space for stdio and TCP to work in.
	 */
	if (manager->m_nReserved != 0 && sock->m_eType == bc_sockettype_udp &&
	    sock->m_nFD >= 0 && sock->m_nFD < manager->m_nReserved)
	{
		int fd, tmp;
		fd = fcntl(sock->m_nFD, F_DUPFD, manager->m_nReserved);
		tmp = errno;
		(void)close(sock->m_nFD);
		errno = tmp;
		sock->m_nFD = fd;
		err = "bc_socket_create: fcntl/reserved";
	}
	else if (sock->m_nFD >= 0 && sock->m_nFD < 20)
	{
		int fd, tmp;
		fd = fcntl(sock->m_nFD, F_DUPFD, 20);
		tmp = errno;
		(void)close(sock->m_nFD);
		errno = tmp;
		sock->m_nFD = fd;
		err = "bc_socket_create: fcntl";
	}
#endif

	if (sock->m_nFD < 0)
	{
		switch (errno)
		{
		case EMFILE:
		case ENFILE:
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_, "%s: %s", err, strbuf);
			/* fallthrough */
		case ENOBUFS:
			return (BC_R_NORESOURCES);

		case EPROTONOSUPPORT:
		case EPFNOSUPPORT:
		case EAFNOSUPPORT:
		/*
		 * Linux 2.2 (and maybe others) return EINVAL instead of
		 * EAFNOSUPPORT.
		 */
		case EINVAL:
			return (BC_R_FAMILYNOSUPPORT);

		default:
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_, "%s: %s",  "failed", strbuf);
			return (BC_R_UNEXPECTED);
		}
	}

	if (make_nonblock(sock->m_nFD) != BC_R_SUCCESS)
	{
		(void)close(sock->m_nFD);
		return (BC_R_UNEXPECTED);
	}

#ifdef SO_BSDCOMPAT
	RUNTIME_CHECK(bc_once_do(&bsdcompat_once,
				  clear_bsdcompat, NULL) == BC_R_SUCCESS);
	if (sock->m_eType != bc_sockettype_unix && bsdcompat &&
	    setsockopt(sock->m_nFD, SOL_SOCKET, SO_BSDCOMPAT,
		       (void *)&on, sizeof(on)) < 0) {
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
				 "setsockopt(%d, SO_BSDCOMPAT) %s: %s",
				 sock->m_nFD,  "failed", strbuf);
		/* Press on... */
	}
#endif //SO_BSDCOMPAT

#ifdef SO_NOSIGPIPE
	if (setsockopt(sock->m_nFD, SOL_SOCKET, SO_NOSIGPIPE,
		       (void *)&on, sizeof(on)) < 0)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
				 "setsockopt(%d, SO_NOSIGPIPE) %s: %s",
				 sock->m_nFD,  "failed", strbuf);
		/* Press on... */
	}
#endif // SO_BSDCOMPAT

#if defined(USE_CMSG) || defined(SO_RCVBUF)
	if (sock->m_eType == bc_sockettype_udp)
	{
#if defined(USE_CMSG)
#if defined(SO_TIMESTAMP)
		if (setsockopt(sock->m_nFD, SOL_SOCKET, SO_TIMESTAMP,
			       (void *)&on, sizeof(on)) < 0
		    && errno != ENOPROTOOPT)
		{
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
					 "setsockopt(%d, SO_TIMESTAMP) %s: %s",
					 sock->m_nFD, "failed", strbuf);
			/* Press on... */
		}
#endif /* SO_TIMESTAMP */

#if defined(BC_PLATFORM_HAVEIPV6)
		if (sock->m_nPF == AF_INET6 && sock->m_nRecvCmsgBufLen == 0U)
		{
			/*
			 * Warn explicitly because this anomaly can be hidden
			 * in usual operation (and unexpectedly appear later).
			 */
			LogError(_LOCAL_, "No buffer available to receive IPv6 destination");
		}
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
#ifdef IPV6_RECVPKTINFO
		/* RFC 3542 */
		if ((sock->m_nPF == AF_INET6)
		    && (setsockopt(sock->m_nFD, IPPROTO_IPV6, IPV6_RECVPKTINFO,
				   (void *)&on, sizeof(on)) < 0))
		{
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
					 "setsockopt(%d, IPV6_RECVPKTINFO) "
					 "%s: %s", sock->m_nFD, "failed", strbuf);
		}
#else
		/* RFC 2292 */
		if ((sock->m_nPF == AF_INET6)
		    && (setsockopt(sock->m_nFD, IPPROTO_IPV6, IPV6_PKTINFO,
				   (void *)&on, sizeof(on)) < 0))
		{
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
					 "setsockopt(%d, IPV6_PKTINFO) %s: %s",
					 sock->m_nFD, "failed", strbuf);
		}
#endif /* IPV6_RECVPKTINFO */
#endif /* BC_PLATFORM_HAVEIN6PKTINFO */
#ifdef IPV6_USE_MIN_MTU        /* RFC 3542, not too common yet*/
		/* use minimum MTU */
		if (sock->m_nPF == AF_INET6)
		{
			(void)setsockopt(sock->m_nFD, IPPROTO_IPV6,
					 IPV6_USE_MIN_MTU,
					 (void *)&on, sizeof(on));
		}
#endif
#if defined(IPV6_MTU)
		/*
		 * Use minimum MTU on IPv6 sockets.
		 */
		if (sock->m_nPF == AF_INET6)
		{
			int mtu = 1280;
			(void)setsockopt(sock->m_nFD, IPPROTO_IPV6, IPV6_MTU,
					 &mtu, sizeof(mtu));
		}
#endif
#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_DONT)
		/*
		 * Turn off Path MTU discovery on IPv6/UDP sockets.
		 */
		if (sock->m_nPF == AF_INET6)
		{
			int action = IPV6_PMTUDISC_DONT;
			(void)setsockopt(sock->m_nFD, IPPROTO_IPV6, IPV6_MTU_DISCOVER,
					 &action, sizeof(action));
		}
#endif
#endif /* BC_PLATFORM_HAVEIPV6 */
#endif /* defined(USE_CMSG) */

#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DONT)
		/*
		 * Turn off Path MTU discovery on IPv4/UDP sockets.
		 */
		if (sock->m_nPF == AF_INET)
		{
			int action = IP_PMTUDISC_DONT;
			(void)setsockopt(sock->m_nFD, IPPROTO_IP, IP_MTU_DISCOVER,
					 &action, sizeof(action));
		}
#endif
#if defined(IP_DONTFRAG)
		/*
		 * Turn off Path MTU discovery on IPv4/UDP sockets.
		 */
		if (sock->m_nPF == AF_INET)
		{
			int off = 0;
			(void)setsockopt(sock->m_nFD, IPPROTO_IP, IP_DONTFRAG,
					 &off, sizeof(off));
		}
#endif

#if defined(SO_RCVBUF)
		optlen = sizeof(size);
		if (getsockopt(sock->m_nFD, SOL_SOCKET, SO_RCVBUF,
			       (void *)&size, &optlen) >= 0 &&
		     size < RCVBUFSIZE)
		{
			size = RCVBUFSIZE;
			if (setsockopt(sock->m_nFD, SOL_SOCKET, SO_RCVBUF,
				       (void *)&size, sizeof(size)) == -1)
			{
				bc_strerror(errno, strbuf, sizeof(strbuf));
				LogError(_LOCAL_,
					"setsockopt(%d, SO_RCVBUF, %d) %s: %s",
					sock->m_nFD, size, "failed", strbuf);
			}
		}
#endif
	}
#endif /* defined(USE_CMSG) || defined(SO_RCVBUF) */

	inc_stats(manager->m_pStats, sock->m_pStatsIndex[STATID_OPEN]);

	return (BC_R_SUCCESS);
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
	BCSocket *sock = (BCSocket *)this;
	BCSocketMgr *manager = (BCSocketMgr *)pMgr;
	BCRESULT result;
	int lockid;

	ASSERT(VALID_MANAGER(manager));
	ASSERT(eType != bc_sockettype_fdwatch);

	result = _InternalCreate(manager, eType);
	if (result != BC_R_SUCCESS)
		return (result);

	switch (sock->m_eType)
	{
	case bc_sockettype_udp:
		sock->m_pStatsIndex =
			(nPf == AF_INET) ? upd4statsindex : upd6statsindex;
		break;
	case bc_sockettype_tcp:
		sock->m_pStatsIndex =
			(nPf == AF_INET) ? tcp4statsindex : tcp6statsindex;
		break;
	case bc_sockettype_unix:
		sock->m_pStatsIndex = unixstatsindex;
		break;
	default:
		INSIST(0);
	}

	sock->m_nPF = nPf;
	result = _Open();
	if (result != BC_R_SUCCESS)
	{
		inc_stats(manager->m_pStats, sock->m_pStatsIndex[STATID_OPENFAIL]);
		_Free(&sock);
		return (result);
	}

	sock->m_nRef = 1;

	/*
	 * Note we don't have to lock the socket like we normally would because
	 * there are no external references to it yet.
	 */

	lockid = FDLOCK_ID(sock->m_nFD);
	LOCK(&manager->m_sFdCtx[lockid].lock);
	manager->m_sFdCtx[lockid].mapFdSocket[sock->m_nFD] = sock;
	manager->m_sFdCtx[lockid].mapFdState[sock->m_nFD] = MANAGED;
#ifdef USE_DEVPOLL
	INSIST(sock->m_pMgr->m_sFdCtx[lockid].mapFdPollInfo[sock->m_nFD].want_read == 0 &&
	       sock->m_pMgr->m_sFdCtx[lockid].mapFdPollInfo[sock->m_nFD].want_write == 0);
#endif
	UNLOCK(&manager->m_sFdCtx[lockid].lock);

	LOCK(&manager->m_sLock);
	manager->m_lstSockets.PushBack(sock);
#ifdef USE_SELECT
	if (manager->m_nMaxFd < sock->m_nFD)
		manager->m_nMaxFd = sock->m_nFD;
#endif
	UNLOCK(&manager->m_sLock);

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
	m_nRef++;
	m_sLock.Unlock();

	*ppSocket = this;
}

void BCSocket::_Destroy(BCSocket **sockp)
{
	int fd;
	BCSocket *sock = *sockp;
	BCSocketMgr *manager = sock->m_pMgr;

	INSIST(sock->m_lstAcceptEvents.IsEmpty());
	INSIST(sock->m_lstRecvEvents.IsEmpty());
	INSIST(sock->m_lstSendEvents.IsEmpty());
	INSIST(sock->m_pConnectEvent == NULL);

	if (sock->m_nFD >= 0)
	{
		fd = sock->m_nFD;
		sock->m_nFD = -1;
		_Close(manager, sock, fd);
	}

	LOCK(&manager->m_sLock);

	sock->RemoveFromList();

#ifdef USE_WATCHER_THREAD
	if (manager->m_lstSockets.IsEmpty())
		manager->m_sCondShutdownOk.Signal();
#endif /* USE_WATCHER_THREAD */

	UNLOCK(&manager->m_sLock);

	_Free(sockp);
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

	m_sLock.Lock();
	ASSERT(m_nRef > 0);
	m_nRef--;

	if (m_nRef == 0 )
	{
		kill_socket = TRUE;
	}
	m_sLock.Unlock();

	if(kill_socket)
		_Destroy(&pSocket);

	*ppSocket = NULL;
}

/*
 * Process control messages received on a socket.
 */
void BCSocket::_ProcessCMsg(BCSocket *sock, struct msghdr *msg, BCSockEvent *dev)
{
#ifdef USE_CMSG
	struct cmsghdr *cmsgp;
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
	struct in6_pktinfo *pktinfop;
#endif // BC_PLATFORM_HAVEIN6PKTINFO
#ifdef SO_TIMESTAMP
	struct timeval *timevalp;
#endif // SO_TIMESTAMP
#endif // USE_CMSG

	/*
	 * sock is used only when BC_NET_BSD44MSGHDR and USE_CMSG are defined.
	 * msg and dev are used only when BC_NET_BSD44MSGHDR is defined.
	 * They are all here, outside of the CPP tests, because it is
	 * more consistent with the usual BC coding style.
	 */
	UNUSED(sock);
	UNUSED(msg);
	UNUSED(dev);

#ifdef BC_NET_BSD44MSGHDR

#ifdef MSG_TRUNC
	if ((msg->msg_flags & MSG_TRUNC) == MSG_TRUNC)
		dev->attributes |= BC_SOCKEVENTATTR_TRUNC;
#endif // MSG_TRUNC

#ifdef MSG_CTRUNC
	if ((msg->msg_flags & MSG_CTRUNC) == MSG_CTRUNC)
		dev->attributes |= BC_SOCKEVENTATTR_CTRUNC;
#endif // MSG_CTRUNC

#ifndef USE_CMSG
	return;
#else // USE_CMSG
	if (msg->msg_controllen == 0U || msg->msg_control == NULL)
		return;

#ifdef SO_TIMESTAMP
	timevalp = NULL;
#endif // SO_TIMESTAMP
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
	pktinfop = NULL;
#endif // BC_PLATFORM_HAVEIN6PKTINFO

	cmsgp = CMSG_FIRSTHDR(msg);
	while (cmsgp != NULL)
	{
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
		if (cmsgp->cmsg_level == IPPROTO_IPV6
		    && cmsgp->cmsg_type == IPV6_PKTINFO)
		{

			pktinfop = (struct in6_pktinfo *)CMSG_DATA(cmsgp);
			memcpy(&dev->pktinfo, pktinfop, sizeof(struct in6_pktinfo));
			dev->attributes |= BC_SOCKEVENTATTR_PKTINFO;

			if (IN6_IS_ADDR_MULTICAST(&pktinfop->ipi6_addr))
				dev->attributes |= BC_SOCKEVENTATTR_MULTICAST;
			goto next;
		}
#endif // BC_PLATFORM_HAVEIN6PKTINFO

#ifdef SO_TIMESTAMP
		if (cmsgp->cmsg_level == SOL_SOCKET
		    && cmsgp->cmsg_type == SCM_TIMESTAMP)
		{
			timevalp = (struct timeval *)CMSG_DATA(cmsgp);
			dev->timestamp.seconds = timevalp->tv_sec;
			dev->timestamp.nanoseconds = timevalp->tv_usec * 1000;
			dev->attributes |= BC_SOCKEVENTATTR_TIMESTAMP;
			goto next;
		}
#endif // SO_TIMESTAMP

	next:
		cmsgp = CMSG_NXTHDR(msg, cmsgp);
	}
#endif /* USE_CMSG */

#else // !BC_NET_BSD44MSGHDR

#ifdef USE_CMSG
	UNUSED(cmsgp);
#ifdef BC_PLATFORM_HAVEIN6PKTINFO
	UNUSED(pktinfop);
#endif // BC_PLATFORM_HAVEIN6PKTINFO
#ifdef SO_TIMESTAMP
	UNUSED(timevalp);
#endif // SO_TIMESTAMP
#endif // USE_CMSG

#endif /* BC_NET_BSD44MSGHDR */
}

/*
 * Construct an iov array and attach it to the msghdr passed in.  This is
 * the SEND constructor, which will use the used region of the buffer
 * (if using a buffer list) or will use the internal region (if a single
 * buffer I/O is requested).
 *
 * Nothing can be NULL, and the done event must list at least one buffer
 * on the buffer linked list for this function to be meaningful.
 *
 * If write_countp != NULL, *write_countp will hold the number of bytes
 * this transaction can send.
 */
void BCSocket::_BuildMsgHdrSend(
	BCSocket *sock,
	BCSockEvent *dev,
	struct msghdr *msg,
	struct iovec *iov,
	size_t *write_countp)
{
	unsigned int iovcount, nBlockSize;
	size_t write_count;
	size_t skip_count;
	void *pBuffer;

	memset(msg, 0, sizeof(*msg));

	if (!sock->m_bConnected)
	{
		msg->msg_name = (void *)&dev->address.type.sa;
		msg->msg_namelen = dev->address.length;
	}
	else
	{
		msg->msg_name = NULL;
		msg->msg_namelen = 0;
	}

	write_count = 0;
	iovcount = 0;

	/*
	 * Single buffer I/O?  Skip what we've done so far in this region.
	 */
	if (dev->bufferlist == NULL)
	{
		skip_count = dev->n;
		for (uint32_t i = 0; i < dev->region_size; i++)
		{
			if (skip_count >= dev->region[i].length)
			{
				skip_count -= dev->region[i].length;
			}
			else if (skip_count > 0)
			{
				write_count += dev->region[i].length - skip_count;
				iov[iovcount].iov_base = (char *)(dev->region[i].base + skip_count);
				iov[iovcount].iov_len = dev->region[i].length - skip_count;
				skip_count = 0;
				iovcount++;
			}
			else
			{
				write_count += dev->region[i].length;
				iov[iovcount].iov_base = (char *)(dev->region[i].base);
				iov[iovcount].iov_len = dev->region[i].length;
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
		skip_count = dev->n;
		dev->bufferlist->Rewind();
		dev->bufferlist->Forward(skip_count);
		skip_count = 0;
		pBuffer = dev->bufferlist->ReadBlock(INFINITE, nBlockSize);
		while(pBuffer != NULL && nBlockSize > 0)
		{
			INSIST(iovcount < MAXSCATTERGATHER_SEND);

			iov[iovcount].iov_base = (void *)pBuffer;
			iov[iovcount].iov_len = nBlockSize;
			write_count += nBlockSize;
			iovcount++;

			pBuffer = dev->bufferlist->ReadBlock(INFINITE, nBlockSize);
		}
	}

	INSIST(skip_count == 0U);

	msg->msg_iov = iov;
	msg->msg_iovlen = iovcount;

#ifdef BC_NET_BSD44MSGHDR
	msg->msg_control = NULL;
	msg->msg_controllen = 0;
	msg->msg_flags = 0;
#if defined(USE_CMSG) && defined(BC_PLATFORM_HAVEIN6PKTINFO)
	if ((sock->m_eType == bc_sockettype_udp)
	    && ((dev->attributes & BC_SOCKEVENTATTR_PKTINFO) != 0))
	{
		struct cmsghdr *cmsgp;
		struct in6_pktinfo *pktinfop;

		msg->msg_controllen = cmsg_space(sizeof(struct in6_pktinfo));
		INSIST(msg->msg_controllen <= sock->m_nSendCmsgBufLen);
		msg->msg_control = (void *)sock->m_pSendCmsgBuf;

		cmsgp = (struct cmsghdr *)sock->m_pSendCmsgBuf;
		cmsgp->cmsg_level = IPPROTO_IPV6;
		cmsgp->cmsg_type = IPV6_PKTINFO;
		cmsgp->cmsg_len = cmsg_len(sizeof(struct in6_pktinfo));
		pktinfop = (struct in6_pktinfo *)CMSG_DATA(cmsgp);
		memcpy(pktinfop, &dev->pktinfo, sizeof(struct in6_pktinfo));
	}
#endif /* USE_CMSG && BC_PLATFORM_HAVEIPV6 */
#else /* BC_NET_BSD44MSGHDR */
	msg->msg_accrights = NULL;
	msg->msg_accrightslen = 0;
#endif /* BC_NET_BSD44MSGHDR */

	if (write_countp != NULL)
		*write_countp = write_count;
}

/*
 * Construct an iov array and attach it to the msghdr passed in.  This is
 * the RECV constructor, which will use the available region of the buffer
 * (if using a buffer list) or will use the internal region (if a single
 * buffer I/O is requested).
 *
 * Nothing can be NULL, and the done event must list at least one buffer
 * on the buffer linked list for this function to be meaningful.
 *
 * If read_countp != NULL, *read_countp will hold the number of bytes
 * this transaction can receive.
 */
void BCSocket::_BuildMsgHdrRecv(
	BCSocket *sock,
	BCSockEvent *dev,
	struct msghdr *msg,
	struct iovec *iov,
	size_t *read_countp)
{
	unsigned int iovcount, nBlockSize, bufferlist_size;
	size_t read_count;
	void *pBuffer;

	memset(msg, 0, sizeof(struct msghdr));

	if (sock->m_eType == bc_sockettype_udp)
	{
		memset(&dev->address, 0, sizeof(dev->address));
#ifdef BROKEN_RECVMSG
		if (sock->m_nPF == AF_INET)
		{
			msg->msg_name = (void *)&dev->address.type.sin;
			msg->msg_namelen = sizeof(dev->address.type.sin6);
		}
		else if (sock->m_nPF == AF_INET6)
		{
			msg->msg_name = (void *)&dev->address.type.sin6;
			msg->msg_namelen = sizeof(dev->address.type.sin6);
#ifdef BC_PLATFORM_HAVESYSUNH
		}
		else if (sock->m_nPF == AF_UNIX)
		{
			msg->msg_name = (void *)&dev->address.type.sunix;
			msg->msg_namelen = sizeof(dev->address.type.sunix);
#endif
		}
		else
		{
			msg->msg_name = (void *)&dev->address.type.sa;
			msg->msg_namelen = sizeof(dev->address.type);
		}
#else
		msg->msg_name = (void *)&dev->address.type.sa;
		msg->msg_namelen = sizeof(dev->address.type);
#endif
#ifdef BC_NET_RECVOVERFLOW
		/* If needed, steal one iovec for overflow detection. */
		maxiov--;
#endif
	}
	else
	{ /* TCP */
		msg->msg_name = NULL;
		msg->msg_namelen = 0;
		dev->address = sock->m_sAddress;
	}

	read_count = 0;

	/*
	 * Single buffer I/O?  Skip what we've done so far in this region.
	 */
	if (dev->bufferlist == NULL)
	{
		read_count = dev->region[0].length - dev->n;
		iov[0].iov_base = (void *)(dev->region[0].base + dev->n);
		iov[0].iov_len = read_count;
		iovcount = 1;

		goto config;
	}

	/*
	 * Multibuffer I/O.
	 * Skip empty buffers.
	 */
	dev->bufferlist->RemoveConsumed();

	iovcount = 0;
	bufferlist_size = dev->bufferlist_size;
	if (bufferlist_size == 0)
	{
		bufferlist_size = MAXSCATTERGATHER_RECV;
	}
	else
	{
		bufferlist_size = BCMIN(bufferlist_size, MAXSCATTERGATHER_RECV);
	}
	/*
	 * Multibuffer I/O.
	 * WARNING: below buffers should not exceed the 'RCVBUFSIZE' macro value.
	 */
	for (uint32_t i = 0;i < bufferlist_size;i++)
	{
		INSIST(iovcount < bufferlist_size);

		nBlockSize = INFINITE;
		pBuffer = dev->bufferlist->GetWritableBlock(nBlockSize);
		if (pBuffer != NULL && nBlockSize > 0)
		{
			iov[iovcount].iov_base = (void *)pBuffer;
			iov[iovcount].iov_len = nBlockSize;
			read_count += nBlockSize;
			dev->bufferlist->UngetWritableBlock(nBlockSize);
			iovcount++;
		}
		else
		{
			break;
		}
	}
	dev->bufferlist->Subtract(read_count);

 config:

	/*
	 * If needed, set up to receive that one extra byte.  Note that
	 * we know there is at least one iov left, since we stole it
	 * at the top of this function.
	 */
#ifdef BC_NET_RECVOVERFLOW
	if (sock->m_eType == bc_sockettype_udp)
	{
		iov[iovcount].iov_base = (void *)(&sock->m_cOverflow);
		iov[iovcount].iov_len = 1;
		iovcount++;
	}
#endif

	msg->msg_iov = iov;
	msg->msg_iovlen = iovcount;

#ifdef BC_NET_BSD44MSGHDR
	msg->msg_control = NULL;
	msg->msg_controllen = 0;
	msg->msg_flags = 0;
#if defined(USE_CMSG)
	if (sock->m_eType == bc_sockettype_udp)
	{
		msg->msg_control = sock->m_pRecvCmsgBuf;
		msg->msg_controllen = sock->m_nRecvCmsgBufLen;
	}
#endif /* USE_CMSG */
#else /* BC_NET_BSD44MSGHDR */
	msg->msg_accrights = NULL;
	msg->msg_accrightslen = 0;
#endif /* BC_NET_BSD44MSGHDR */

	if (read_countp != NULL)
		*read_countp = read_count;
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
		pTask->SendAndDetach((BCTaskEvent **)dev);
	else
		pTask->Send((BCTaskEvent **)dev);

	*dev = NULL;
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
		pTask->SendAndDetach((BCTaskEvent **)dev);
	else
		pTask->Send((BCTaskEvent **)dev);

	*dev = NULL;
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
}

/*
 * Call accept() on a socket, to get the new file descriptor.  The listen
 * socket is used as a prototype to create a new BCSocket.  The new
 * socket has one outstanding reference.  The task receiving the event
 * will be detached from just after the event is delivered.
 *
 * On entry to this function, the event delivered is the internal
 * readable event, and the first item on the accept_list should be
 * the done event we want to send.  If the list is empty, this is a no-op,
 * so just unlock and return.
 */
void BCSocket::_InternalAccept(BCTask *me, BCTaskEvent *ev)
{
	BCSocket *sock;
	BCSocketMgr *manager;
	BCSockICEvent *dev;
	BCTask *task;
	BC_SOCKADDR_LEN_T addrlen;
	int fd;
	BCRESULT result = BC_R_SUCCESS;
	char strbuf[BC_STRERRORSIZE];
	const char *err = "accept";

	UNUSED(me);

	sock = (BCSocket *)ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->m_sLock);

	manager = sock->m_pMgr;
	INSIST(VALID_MANAGER(manager));

	INSIST(sock->m_bListener);
	INSIST(sock->m_nPendingAccept == 1);
	sock->m_nPendingAccept = 0;

	INSIST(sock->m_nRef > 0);
	sock->m_nRef--;  /* the internal event is done with this socket */
	if (sock->m_nRef == 0)
	{
		UNLOCK(&sock->m_sLock);
		_Destroy(&sock);
		return;
	}

	/*
	 * Get the first item off the accept list.
	 * If it is empty, unlock the socket and return.
	 */
	dev = sock->m_lstAcceptEvents.Begin();
	if (dev == sock->m_lstAcceptEvents.End())
	{
		UNLOCK(&sock->m_sLock);
		return;
	}

	/*
	 * Try to accept the new connection.  If the accept fails with
	 * EAGAIN or EINTR, simply poke the watcher to watch this socket
	 * again.  Also ignore ECONNRESET, which has been reported to
	 * be spuriously returned on Linux 2.2.19 although it is not
	 * a documented error for accept().  ECONNABORTED has been
	 * reported for Solaris 8.  The rest are thrown in not because
	 * we have seen them but because they are ignored by other
	 * daemons such as BIND 8 and Apache.
	 */

	addrlen = sizeof(NEWCONNSOCK(dev)->m_sAddress.type);
	memset(&NEWCONNSOCK(dev)->m_sAddress.type, 0, addrlen);
	fd = accept(sock->m_nFD, &NEWCONNSOCK(dev)->m_sAddress.type.sa,
		   &addrlen);

#ifdef F_DUPFD
	/*
	 * Leave a space for stdio to work in.
	 */
	if (fd >= 0 && fd < 20)
	{
		int newFD, tmp;
		newFD = fcntl(fd, F_DUPFD, 20);
		tmp = errno;
		(void)close(fd);
		errno = tmp;
		fd = newFD;
		err = "accept/fcntl";
	}
#endif

	if (fd < 0)
	{
		if (SOFT_ERROR(errno))
			goto soft_error;
		switch (errno)
		{
		case ENFILE:
		case EMFILE:
			LogError(_LOCAL_, "%s: too many open file descriptors", err);
			goto soft_error;

		case ENOBUFS:
		case ENOMEM:
		case ECONNRESET:
		case ECONNABORTED:
		case EHOSTUNREACH:
		case EHOSTDOWN:
		case ENETUNREACH:
		case ENETDOWN:
		case ECONNREFUSED:
#ifdef EPROTO
		case EPROTO:
#endif
#ifdef ENONET
		case ENONET:
#endif
			goto soft_error;
		default:
			break;
		}
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "_InternalAccept: %s() %s: %s", err, "failed", strbuf);
		fd = -1;
		result = BC_R_UNEXPECTED;
	}
	else
	{
		if (addrlen == 0U)
		{
			LogError(_LOCAL_, "_InternalAccept(): accept() failed to return remote address");

			(void)close(fd);
			goto soft_error;
		}
		else if (NEWCONNSOCK(dev)->m_sAddress.type.sa.sa_family != sock->m_nPF)
		{
			LogError(_LOCAL_,
					 "_InternalAccept(): "
					 "accept() returned peer address "
					 "family %u (expected %u)",
					 NEWCONNSOCK(dev)->m_sAddress.
					 type.sa.sa_family,
					 sock->m_nPF);
			(void)close(fd);
			goto soft_error;
		}
	}

	if (fd != -1)
	{
		NEWCONNSOCK(dev)->m_sAddress.length = addrlen;
		NEWCONNSOCK(dev)->m_nPF = sock->m_nPF;
	}

	/*
	 * Pull off the done event.
	 */
	dev->RemoveFromList();

	/*
	 * Poke watcher if there are more pending accepts.
	 */
	if (!sock->m_lstAcceptEvents.IsEmpty())
		sock->m_pMgr->_SelectPoke(sock->m_nFD, SELECT_POKE_ACCEPT);

	UNLOCK(&sock->m_sLock);

	if (fd != -1 && (make_nonblock(fd) != BC_R_SUCCESS))
	{
		(void)close(fd);
		fd = -1;
		result = BC_R_UNEXPECTED;
	}

	/*
	 * -1 means the new socket didn't happen.
	 */
	if (fd != -1)
	{
		int lockid = FDLOCK_ID(fd);

		LOCK(&manager->m_sFdCtx[lockid].lock);
		manager->m_sFdCtx[lockid].mapFdSocket[fd] = NEWCONNSOCK(dev);
		manager->m_sFdCtx[lockid].mapFdState[fd] = MANAGED;
		UNLOCK(&manager->m_sFdCtx[lockid].lock);

		LOCK(&manager->m_sLock);
		manager->m_lstSockets.PushBack(NEWCONNSOCK(dev));

		NEWCONNSOCK(dev)->m_nFD = fd;
		NEWCONNSOCK(dev)->m_bBound = 1;
		NEWCONNSOCK(dev)->m_bConnected = 1;

		/*
		 * Save away the remote address
		 */
		dev->address = NEWCONNSOCK(dev)->m_sAddress;

#ifdef USE_SELECT
		if (manager->m_nMaxFd < fd)
			manager->m_nMaxFd = fd;
#endif

		//LogInfo(_LOCAL_,  "accepted connection, new socket %p",
		//	   dev->newsocket);

		UNLOCK(&manager->m_sLock);

		inc_stats(manager->m_pStats, sock->m_pStatsIndex[STATID_ACCEPT]);
	}
	else
	{
		inc_stats(manager->m_pStats, sock->m_pStatsIndex[STATID_ACCEPTFAIL]);
		NEWCONNSOCK(dev)->m_nRef--;
		_Free((BCSocket **)&dev->newsocket);
	}

	/*
	 * Fill in the done event details and send it off.
	 */
	dev->result = result;
	task = (BCTask *)dev->ev_sender;
	dev->ev_sender = sock;

	task->SendAndDetach((BCTaskEvent **)&dev);
	return;

 soft_error:
	sock->m_pMgr->_SelectPoke(sock->m_nFD, SELECT_POKE_ACCEPT);
	UNLOCK(&sock->m_sLock);

	inc_stats(manager->m_pStats, sock->m_pStatsIndex[STATID_ACCEPTFAIL]);
	return;
}

/*
 * Called when a socket with a pending connect() finishes.
 */
void BCSocket::_InternalConnect(BCTask *me, BCTaskEvent *ev)
{
	BCSocket *sock;
	BCSockOCEvent *dev;
	BCTask *task;
	int cc;
	BC_SOCKADDR_LEN_T optlen;
	char strbuf[BC_STRERRORSIZE];
	char peerbuf[BC_SOCKADDR_FORMATSIZE];

	UNUSED(me);
	INSIST(ev->ev_type == BC_SOCKEVENT_INTW);

	sock = (BCSocket *)ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->m_sLock);

	/*
	 * When the internal event was sent the reference count was bumped
	 * to keep the socket around for us.  Decrement the count here.
	 */
	INSIST(sock->m_nRef > 0);
	sock->m_nRef--;
	if (sock->m_nRef == 0)
	{
		UNLOCK(&sock->m_sLock);
		_Destroy(&sock);
		return;
	}

	/*
	 * Has this event been canceled?
	 */
	dev = sock->m_pConnectEvent;
	if (dev == NULL)
	{
		INSIST(!sock->m_bConnecting);
		UNLOCK(&sock->m_sLock);
		return;
	}

	INSIST(sock->m_bConnecting);
	sock->m_bConnecting = 0;

	/*
	 * Get any possible error status here.
	 */
	optlen = sizeof(cc);
	if (getsockopt(sock->m_nFD, SOL_SOCKET, SO_ERROR,
		       (void *)&cc, &optlen) < 0)
		cc = errno;
	else
		errno = cc;

	if (errno != 0)
	{
		/*
		 * If the error is EAGAIN, just re-select on this
		 * fd and pretend nothing strange happened.
		 */
		if (SOFT_ERROR(errno) || errno == EINPROGRESS)
		{
			sock->m_bConnecting = 1;
			sock->m_pMgr->_SelectPoke(sock->m_nFD,
				    SELECT_POKE_CONNECT);
			UNLOCK(&sock->m_sLock);

			return;
		}

		inc_stats(sock->m_pMgr->m_pStats,
			  sock->m_pStatsIndex[STATID_CONNECTFAIL]);

		/*
		 * Translate other errors into BC_R_* flavors.
		 */
		switch (errno)
		{
#define ERROR_MATCH(a, b) case a: dev->result = b; break;
			ERROR_MATCH(EACCES, BC_R_NOPERM);
			ERROR_MATCH(EADDRNOTAVAIL, BC_R_ADDRNOTAVAIL);
			ERROR_MATCH(EAFNOSUPPORT, BC_R_ADDRNOTAVAIL);
			ERROR_MATCH(ECONNREFUSED, BC_R_CONNREFUSED);
			ERROR_MATCH(EHOSTUNREACH, BC_R_HOSTUNREACH);
#ifdef EHOSTDOWN
			ERROR_MATCH(EHOSTDOWN, BC_R_HOSTUNREACH);
#endif
			ERROR_MATCH(ENETUNREACH, BC_R_NETUNREACH);
			ERROR_MATCH(ENOBUFS, BC_R_NORESOURCES);
			ERROR_MATCH(EPERM, BC_R_HOSTUNREACH);
			ERROR_MATCH(EPIPE, BC_R_NOTCONNECTED);
			ERROR_MATCH(ETIMEDOUT, BC_R_TIMEDOUT);
			ERROR_MATCH(ECONNRESET, BC_R_CONNECTIONRESET);
#undef ERROR_MATCH
		default:
			dev->result = BC_R_UNEXPECTED;
			bc_sockaddr_format(&sock->m_sAddress, peerbuf, sizeof(peerbuf));
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
					 "_InternalConnect: connect(%s) %s",
					 peerbuf, strbuf);
		}
	}
	else
	{
		inc_stats(sock->m_pMgr->m_pStats,
			  sock->m_pStatsIndex[STATID_CONNECT]);
		dev->result = BC_R_SUCCESS;
		sock->m_bConnected = 1;
		sock->m_bBound = 1;
	}

	sock->m_pConnectEvent = NULL;

	UNLOCK(&sock->m_sLock);

	task = (BCTask *)dev->ev_sender;
	dev->ev_sender = sock;
	task->SendAndDetach(((BCTaskEvent **)(&dev)));
}

void BCSocket::_SetDevAddress(
		BCSockAddrS *address,
		BCSocket *sock,
		BCSockEvent *dev)
{
	if (sock->m_eType == bc_sockettype_udp)
	{
		if (address != NULL)
			dev->address = *address;
		else
			dev->address = sock->m_sAddress;
	}
	else if (sock->m_eType == bc_sockettype_tcp)
	{
		INSIST(address == NULL);
		dev->address = sock->m_sAddress;
	}
}

#if defined(BC_SOCKET_DEBUG)
static void
dump_msg(struct msghdr *msg) {
	unsigned int i;

	printf("MSGHDR %p\n", msg);
	printf("\tname %p, namelen %ld\n", msg->msg_name,
	       (long) msg->msg_namelen);
	printf("\tiov %p, iovlen %ld\n", msg->msg_iov,
	       (long) msg->msg_iovlen);
	for (i = 0; i < (unsigned int)msg->msg_iovlen; i++)
		printf("\t\t%d\tbase %p, len %ld\n", i,
		       msg->msg_iov[i].iov_base,
		       (long) msg->msg_iov[i].iov_len);
#ifdef BC_NET_BSD44MSGHDR
	printf("\tcontrol %p, controllen %ld\n", msg->msg_control,
	       (long) msg->msg_controllen);
#endif
}
#endif

#define DOIO_SUCCESS	0	/* i/o ok, event sent */
#define DOIO_SOFT		1	/* i/o ok, soft error, no event sent */
#define DOIO_HARD		2	/* i/o error, event sent */
#define DOIO_EOF		3	/* EOF, no event sent */

int BCSocket::_DoIoRecv(BCSocket *sock, BCSockEvent *dev)
{
	int cc;
	struct iovec iov[MAXSCATTERGATHER_RECV];
	size_t read_count;
	size_t actual_count;
	struct msghdr msghdr;
	int recv_errno;

	_BuildMsgHdrRecv(sock, dev, &msghdr, iov, &read_count);

#if defined(BC_SOCKET_DEBUG)
	dump_msg(&msghdr);
#endif

	cc = recvmsg(sock->m_nFD, &msghdr, 0);
	recv_errno = errno;

#if defined(BC_SOCKET_DEBUG)
	dump_msg(&msghdr);
#endif

	if (cc < 0)
	{
		if (SOFT_ERROR(recv_errno))
			return (DOIO_SOFT);

#define SOFT_OR_HARD(_system, _bc) \
	if (recv_errno == _system) { \
		if (sock->m_bConnected) { \
			dev->result = _bc; \
			inc_stats(sock->m_pMgr->m_pStats, \
				  sock->m_pStatsIndex[STATID_RECVFAIL]); \
			return (DOIO_HARD); \
		} \
		return (DOIO_SOFT); \
	}
#define ALWAYS_HARD(_system, _bc) \
	if (recv_errno == _system) { \
		dev->result = _bc; \
		inc_stats(sock->m_pMgr->m_pStats, \
			  sock->m_pStatsIndex[STATID_RECVFAIL]); \
		return (DOIO_HARD); \
	}

		SOFT_OR_HARD(ECONNREFUSED, BC_R_CONNREFUSED);
		SOFT_OR_HARD(ENETUNREACH, BC_R_NETUNREACH);
		SOFT_OR_HARD(EHOSTUNREACH, BC_R_HOSTUNREACH);
		SOFT_OR_HARD(EHOSTDOWN, BC_R_HOSTDOWN);
		/* HPUX 11.11 can return EADDRNOTAVAIL. */
		SOFT_OR_HARD(EADDRNOTAVAIL, BC_R_ADDRNOTAVAIL);
		ALWAYS_HARD(ENOBUFS, BC_R_NORESOURCES);
		/*
		 * HPUX returns EPROTO and EINVAL on receiving some ICMP/ICMPv6
		 * errors.
		 */
#ifdef EPROTO
		SOFT_OR_HARD(EPROTO, BC_R_HOSTUNREACH);
#endif
		SOFT_OR_HARD(EINVAL, BC_R_HOSTUNREACH);

#undef SOFT_OR_HARD
#undef ALWAYS_HARD

		dev->result = bc_errno2result(recv_errno);
		inc_stats(sock->m_pMgr->m_pStats,
			  sock->m_pStatsIndex[STATID_RECVFAIL]);
		return (DOIO_HARD);
	}

	/*
	 * On TCP and UNIX sockets, zero length reads indicate EOF,
	 * while on UDP sockets, zero length reads are perfectly valid,
	 * although strange.
	 */
	switch (sock->m_eType)
	{
	case bc_sockettype_tcp:
	case bc_sockettype_unix:
		if (cc == 0)
			return (DOIO_EOF);
		break;
	case bc_sockettype_udp:
		break;
	case bc_sockettype_fdwatch:
	default:
		INSIST(0);
	}

	if (sock->m_eType == bc_sockettype_udp)
	{
		dev->address.length = msghdr.msg_namelen;
		if (bc_sockaddr_getport(&dev->address) == 0)
		{
			return (DOIO_SOFT);
		}
		/*
		 * Simulate a firewall blocking UDP responses bigger than
		 * 512 bytes.
		 */
		if (sock->m_pMgr->m_nMaxUdp != 0 && cc > sock->m_pMgr->m_nMaxUdp)
			return (DOIO_SOFT);
	}

	/*
	 * Overflow bit detection.  If we received MORE bytes than we should,
	 * this indicates an overflow situation.  Set the flag in the
	 * dev entry and adjust how much we read by one.
	 */
#ifdef BC_NET_RECVOVERFLOW
	if ((sock->m_eType == bc_sockettype_udp) && ((size_t)cc > read_count))
	{
		dev->attributes |= BC_SOCKEVENTATTR_TRUNC;
		cc--;
	}
#endif

	/*
	 * If there are control messages attached, run through them and pull
	 * out the interesting bits.
	 */
	if (sock->m_eType == bc_sockettype_udp)
		_ProcessCMsg(sock, &msghdr, dev);

	/*
	 * update the buffers (if any) and the i/o count
	 */
	dev->n += cc;
	actual_count = cc;
	if (dev->bufferlist != NULL)
	{
		dev->bufferlist->Add(actual_count);
	}

	/*
	 * If we read less than we expected, update counters,
	 * and let the upper layer poke the descriptor.
	 */
	if (((size_t)cc != read_count) && (dev->n < dev->minimum))
		return (DOIO_SOFT);

	/*
	 * Full reads are posted, or partials if partials are ok.
	 */
	dev->result = BC_R_SUCCESS;
	return (DOIO_SUCCESS);
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
int BCSocket::_DoIoSend(BCSocket *sock, BCSockEvent *dev)
{
	int cc;
	struct iovec iov[MAXSCATTERGATHER_SEND];
	size_t write_count;
	struct msghdr msghdr;
	char addrbuf[BC_SOCKADDR_FORMATSIZE];
	int attempts = 0;
	int send_errno;
	char strbuf[BC_STRERRORSIZE];
	// uint32_t nRemainingLength;

continuesend:
	// Debug usage
	attempts = 0;
	// nRemainingLength = dev->bufferlist->RemainingLength();
	_BuildMsgHdrSend(sock, dev, &msghdr, iov, &write_count);

 resend:
	cc = sendmsg(sock->m_nFD, &msghdr, 0);
	send_errno = errno;
	// LogDebug(_LOCAL_, "BCSocket::_DoIoSend bufSize:%"_U32BITARG_";write_count:%d;cc=%d;errno:%d", 
	// 	nRemainingLength, (int)write_count, cc, errno);

	/*
	 * Check for error or block condition.
	 */
	if (cc < 0)
	{
		if (send_errno == EINTR && ++attempts < NRETRIES)
			goto resend;

		if (SOFT_ERROR(send_errno))
		{
			return (DOIO_SOFT);
		}

#define SOFT_OR_HARD(_system, _bc) \
	if (send_errno == _system) { \
		if (sock->m_bConnected) { \
			dev->result = _bc; \
			inc_stats(sock->m_pMgr->m_pStats, \
				  sock->m_pStatsIndex[STATID_SENDFAIL]); \
			return (DOIO_HARD); \
		} \
		return (DOIO_SOFT); \
	}
#define ALWAYS_HARD(_system, _bc) \
	if (send_errno == _system) { \
		dev->result = _bc; \
		inc_stats(sock->m_pMgr->m_pStats, \
			  sock->m_pStatsIndex[STATID_SENDFAIL]); \
		return (DOIO_HARD); \
	}

		SOFT_OR_HARD(ECONNREFUSED, BC_R_CONNREFUSED);
		ALWAYS_HARD(EACCES, BC_R_NOPERM);
		ALWAYS_HARD(EAFNOSUPPORT, BC_R_ADDRNOTAVAIL);
		ALWAYS_HARD(EADDRNOTAVAIL, BC_R_ADDRNOTAVAIL);
		ALWAYS_HARD(EHOSTUNREACH, BC_R_HOSTUNREACH);
#ifdef EHOSTDOWN
		ALWAYS_HARD(EHOSTDOWN, BC_R_HOSTUNREACH);
#endif
		ALWAYS_HARD(ENETUNREACH, BC_R_NETUNREACH);
		ALWAYS_HARD(ENOBUFS, BC_R_NORESOURCES);
		ALWAYS_HARD(EPERM, BC_R_HOSTUNREACH);
		ALWAYS_HARD(EPIPE, BC_R_NOTCONNECTED);
		ALWAYS_HARD(ECONNRESET, BC_R_CONNECTIONRESET);

#undef SOFT_OR_HARD
#undef ALWAYS_HARD

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
		LogError(_LOCAL_, "_DoIoSend: %s: %s", addrbuf, strbuf);
		dev->result = bc_errno2result(send_errno);
		inc_stats(sock->m_pMgr->m_pStats, sock->m_pStatsIndex[STATID_SENDFAIL]);
		return (DOIO_HARD);
	}

	if (cc == 0)
	{
		inc_stats(sock->m_pMgr->m_pStats, sock->m_pStatsIndex[STATID_SENDFAIL]);
		LogError(_LOCAL_, "_DoIoSend: send() %s 0", "returned");
	}

	/*
	 * If we write less than we expected, update counters, poke.
	 */
	dev->n += cc;
	if ((size_t)cc != write_count)
	{
		if (send_errno != EAGAIN && send_errno != EWOULDBLOCK)
		{
			goto continuesend;
		}
		return (DOIO_SOFT);
	}

	/*
	 * Exactly what we wanted to write.  We're done with this
	 * entry.  Post its completion event.
	 */
	dev->result = BC_R_SUCCESS;
	return (DOIO_SUCCESS);
}

BCRESULT BCSocket::Open()
{
	BCRESULT result;

	REQUIRE(VALID_SOCKET(this));

	LOCK(&m_sLock);
	REQUIRE(m_nRef == 1);
	REQUIRE(m_eType != bc_sockettype_fdwatch);
	UNLOCK(&m_sLock);
	/*
	 * We don't need to retain the lock hereafter, since no one else has
	 * this thiset.
	 */
	REQUIRE(m_nFD == -1);

	result = _Open();
	if (result != BC_R_SUCCESS)
		m_nFD = -1;

	if (result == BC_R_SUCCESS)
	{
		int lockid = FDLOCK_ID(m_nFD);

		LOCK(&m_pMgr->m_sFdCtx[lockid].lock);
		m_pMgr->m_sFdCtx[lockid].mapFdSocket[m_nFD] = this;
		m_pMgr->m_sFdCtx[lockid].mapFdState[m_nFD] = MANAGED;
#ifdef USE_DEVPOLL
		INSIST(m_pMgr->m_sFdCtx[lockid].mapFdPollInfo[m_nFD].want_read == 0 &&
		       m_pMgr->m_sFdCtx[lockid].mapFdPollInfo[m_nFD].want_write == 0);
#endif
		UNLOCK(&m_pMgr->m_sFdCtx[lockid].lock);

#ifdef USE_SELECT
		LOCK(&m_pMgr->m_sLock);
		if (m_pMgr->m_nMaxFd < m_nFD)
			m_pMgr->m_nMaxFd = m_nFD;
		UNLOCK(&m_pMgr->m_sLock);
#endif
	}

	return (result);
}

/*
 * Create a new 'type' socket managed by 'manager'.  Events
 * will be posted to 'task' and when dispatched 'action' will be
 * called with 'arg' as the arg value.  The new socket is returned
 * in 'socketp'.
 */
BCRESULT
BCSocketMgr::FDWatchCreate(
		int fd,
		int flags,
		LPFN_BCSockFDWatch callback,
		void *cbarg,
		BCTask *task,
		BCSocket **socketp)
{
	BCSocket *sock = NULL;
	BCRESULT result;
	int lockid;

	REQUIRE(VALID_MANAGER(this));
	REQUIRE(socketp != NULL && *socketp == NULL);

	sock = new BCSocket();
	if (sock == NULL)
	{
		result = BC_R_NOMEMORY;
		return result;
	}
	result = sock->_InternalCreate(this, bc_sockettype_fdwatch);
	if (result != BC_R_SUCCESS)
	{
		BCSocket::_Destroy(&sock);
		return (result);
	}

	sock->m_nFD = fd;
	sock->m_pFdWatchArg = cbarg;
	sock->m_pFdWatchCB = callback;
	sock->m_nFdWatchFlags = flags;
	sock->m_pFdWatchTask = task;
	sock->m_pStatsIndex = fdwatchstatsindex;

	sock->m_nRef = 1;
	*socketp = (BCSocket *)sock;

	/*
	 * Note we don't have to lock the socket like we normally would because
	 * there are no external references to it yet.
	 */

	lockid = FDLOCK_ID(sock->m_nFD);
	LOCK(&m_sFdCtx[lockid].lock);
	m_sFdCtx[lockid].mapFdSocket[sock->m_nFD] = sock;
	m_sFdCtx[lockid].mapFdState[sock->m_nFD] = MANAGED;
	UNLOCK(&m_sFdCtx[lockid].lock);

	LOCK(&m_sLock);
	m_lstSockets.PushBack(sock);
#ifdef USE_SELECT
	if (m_nMaxFd < sock->m_nFD)
		m_nMaxFd = sock->m_nFD;
#endif
	UNLOCK(&m_sLock);

	if (flags & BC_SOCKFDWATCH_READ)
		sock->m_pMgr->_SelectPoke(sock->m_nFD, SELECT_POKE_READ);
	if (flags & BC_SOCKFDWATCH_WRITE)
		sock->m_pMgr->_SelectPoke(sock->m_nFD, SELECT_POKE_WRITE);

	return (BC_R_SUCCESS);
}

/*
 * Indicate to the manager that it should watch the socket again.
 * This can be used to restart watching if the previous event handler
 * didn't indicate there was more data to be processed.  Primarily
 * it is for writing but could be used for reading if desired
 */

BCRESULT
BCSocket::FDWatchPoke(int flags)
{
	REQUIRE(VALID_SOCKET(this));

	/*
	 * We check both flags first to allow us to get the lock
	 * once but only if we need it.
	 */

	if ((flags & (BC_SOCKFDWATCH_READ | BC_SOCKFDWATCH_WRITE)) != 0)
	{
		LOCK(&m_sLock);
		if (((flags & BC_SOCKFDWATCH_READ) != 0) &&
		    !m_nPendingRecv)
			m_pMgr->_SelectPoke(m_nFD, SELECT_POKE_READ);
		if (((flags & BC_SOCKFDWATCH_WRITE) != 0) &&
		    !m_nPendingSend)
			m_pMgr->_SelectPoke(m_nFD, SELECT_POKE_WRITE);
		UNLOCK(&m_sLock);
	}

	return (BC_R_SUCCESS);
}

BCRESULT BCSocket::Close()
{
	int fd;

	ASSERT(VALID_SOCKET(this));

	LOCK(&m_sLock);

	REQUIRE(m_nRef == 1);
	REQUIRE(m_eType != bc_sockettype_fdwatch);

	INSIST(!m_bConnecting);
	INSIST(!m_nPendingRecv);
	INSIST(!m_nPendingSend);
	INSIST(!m_nPendingAccept);
	INSIST(m_lstRecvEvents.IsEmpty());
	INSIST(m_lstSendEvents.IsEmpty());
	INSIST(m_lstAcceptEvents.IsEmpty());
	INSIST(m_pConnectEvent == NULL);

	fd = m_nFD;
	m_nFD = -1;
	memset(m_szName, 0, sizeof(m_szName));
	m_pTag = NULL;
	m_bListener = 0;
	m_bConnected = 0;
	m_bConnecting = 0;
	m_bBound = 0;
	bc_sockaddr_any(&m_sAddress);

	UNLOCK(&m_sLock);

	_Close(m_pMgr, this, fd);

	return (BC_R_SUCCESS);
}

/*
 * I/O is possible on a given socket.  Schedule an event to this task that
 * will call an internal function to do the I/O.  This will charge the
 * task with the I/O operation and let our select loop handler get back
 * to doing something real as fast as possible.
 *
 * The socket and manager must be locked before calling this function.
 */
void BCSocket::_DispatchRecv()
{
	intev_t *iev;
	BCSockEvent *ev;
	BCTask *sender;

	// INSIST(!m_nPendingRecv);
	if (m_nPendingRecv > 0)
	{
		return;
	}

	if (m_eType != bc_sockettype_fdwatch)
	{
		ev = m_lstRecvEvents.Begin();
		if (ev == m_lstRecvEvents.End())
			return;
		sender = (BCTask *)ev->ev_sender;
	}
	else
	{
		sender = m_pFdWatchTask;
	}

	m_nPendingRecv = 1;
	iev = &m_sReadableEvent;

	m_nRef++;
	iev->ev_sender = this;
	if (m_eType == bc_sockettype_fdwatch)
		iev->ev_action = _InteranlFDWatchRead;
	else
		iev->ev_action = _InternalRecv;
	iev->ev_arg = this;

	sender->Send((BCTaskEvent **)&iev);
}

void BCSocket::_DispatchSend()
{
	intev_t *iev;
	BCSockEvent *ev;
	BCTask *sender;

	// INSIST(!m_nPendingSend);
	if (m_nPendingSend > 0)
	{
		return;
	}

	if (m_eType != bc_sockettype_fdwatch)
	{
		ev = m_lstSendEvents.Begin();
		if (ev == m_lstSendEvents.End())
			return;
		sender = (BCTask *)ev->ev_sender;
	}
	else
	{
		sender = m_pFdWatchTask;
	}

	m_nPendingSend = 1;
	iev = &m_sWritableEvent;

	m_nRef++;
	iev->ev_sender = this;
	if (m_eType == bc_sockettype_fdwatch)
		iev->ev_action = _InteranlFDWatchWrite;
	else
		iev->ev_action = _InternalSend;
	iev->ev_arg = this;

	sender->Send((BCTaskEvent **)&iev);
}

/*
 * Dispatch an internal accept event.
 */
void BCSocket::_DispatchAccept()
{
	intev_t *iev;
	BCSockICEvent *ev;
	BCTask *sender;

	INSIST(!m_nPendingAccept);

	/*
	 * Are there any done events left, or were they all canceled
	 * before the manager got the socket lock?
	 */
	ev = m_lstAcceptEvents.Begin();
	if (ev == m_lstAcceptEvents.End())
		return;

	m_nPendingAccept = 1;
	iev = &m_sReadableEvent;

	m_nRef++;  /* keep socket around for this internal event */
	iev->ev_sender = this;
	iev->ev_action = _InternalAccept;
	iev->ev_arg = this;

	sender = (BCTask *)ev->ev_sender;
	sender->Send((BCTaskEvent **)&iev);
}

void BCSocket::_DispatchConnect()
{
	intev_t *iev;
	BCSockOCEvent *ev;
	BCTask *sender;

	iev = &m_sWritableEvent;

	ev = m_pConnectEvent;
	INSIST(ev != NULL); /* XXX */

	INSIST(m_bConnecting);

	m_nRef++;  /* keep socket around for this internal event */
	iev->ev_sender = this;
	iev->ev_action = _InternalConnect;
	iev->ev_arg = this;

	sender = (BCTask *)ev->ev_sender;
	sender->Send((BCTaskEvent **)&iev);
}

void BCSocket::_InternalRecv(BCTask *me, BCTaskEvent *ev)
{
	BCSockEvent *dev, *pDevEnd;
	BCSocket *sock;

	UNUSED(me);
	INSIST(ev->ev_type == BC_SOCKEVENT_INTR);

	sock = (BCSocket *)ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->m_sLock);

	INSIST(sock->m_nPendingRecv == 1);
	sock->m_nPendingRecv = 0;

	INSIST(sock->m_nRef > 0);
	sock->m_nRef--;  /* the internal event is done with this socket */
	if (sock->m_nRef == 0)
	{
		UNLOCK(&sock->m_sLock);
		_Destroy(&sock);
		return;
	}

	/*
	 * Try to do as much I/O as possible on this socket.  There are no
	 * limits here, currently.
	 */
	dev = sock->m_lstRecvEvents.Begin();
	pDevEnd = sock->m_lstRecvEvents.End();
	for (; dev != pDevEnd; dev = sock->m_lstRecvEvents.Begin())
	{
		switch (_DoIoRecv(sock, dev))
		{
		case DOIO_SOFT:
			goto poke;

		case DOIO_EOF:
			/*
			 * read of 0 means the remote end was closed.
			 * Run through the event queue and dispatch all
			 * the events with an EOF result code.
			 */
			do
			{
				dev->result = BC_R_EOF;
				sock->_SendRecvDoneEvent(&dev);
				dev = sock->m_lstRecvEvents.Begin();
			} while (dev != pDevEnd);
			goto poke;

		case DOIO_SUCCESS:
		case DOIO_HARD:
			sock->_SendRecvDoneEvent(&dev);
			break;
		}
	}

 poke:
	if (!sock->m_lstRecvEvents.IsEmpty())
		sock->m_pMgr->_SelectPoke(sock->m_nFD, SELECT_POKE_READ);

	UNLOCK(&sock->m_sLock);
}

void BCSocket::_InternalSend(BCTask *me, BCTaskEvent *ev)
{
	BCSockEvent *dev, *pDevEnd;
	BCSocket *sock;

	UNUSED(me);
	INSIST(ev->ev_type == BC_SOCKEVENT_INTW);

	/*
	 * Find out what socket this is and lock it.
	 */
	sock = (BCSocket *)ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->m_sLock);

	INSIST(sock->m_nPendingSend == 1);
	sock->m_nPendingSend = 0;

	INSIST(sock->m_nRef > 0);
	sock->m_nRef--;  /* the internal event is done with this socket */
	if (sock->m_nRef == 0)
	{
		UNLOCK(&sock->m_sLock);
		_Destroy(&sock);
		return;
	}

	/*
	 * Try to do as much I/O as possible on this socket.  There are no
	 * limits here, currently.
	 */
	dev = sock->m_lstSendEvents.Begin();
	pDevEnd = sock->m_lstSendEvents.End();
	while (dev != pDevEnd)
	{
		switch (_DoIoSend(sock, dev))
		{
		case DOIO_SOFT:
			goto poke;

		case DOIO_HARD:
		case DOIO_SUCCESS:
			sock->_SendSendDoneEvent(&dev);
			break;
		}

		dev = sock->m_lstSendEvents.Begin();
	}

 poke:
	if (!sock->m_lstSendEvents.IsEmpty())
		sock->m_pMgr->_SelectPoke(sock->m_nFD, SELECT_POKE_WRITE);

	UNLOCK(&sock->m_sLock);
}

void BCSocket::_InteranlFDWatchWrite(BCTask *me, BCTaskEvent *ev)
{
	BCSocket *sock;
	int more_data;

	INSIST(ev->ev_type == BC_SOCKEVENT_INTW);

	/*
	 * Find out what socket this is and lock it.
	 */
	sock = (BCSocket *)ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->m_sLock);

	INSIST(sock->m_nPendingSend == 1);

	UNLOCK(&sock->m_sLock);
	more_data = (sock->m_pFdWatchCB)(me, (BCSocket *)sock,
				      sock->m_pFdWatchArg, BC_SOCKFDWATCH_WRITE);
	LOCK(&sock->m_sLock);

	sock->m_nPendingSend = 0;

	INSIST(sock->m_nRef > 0);
	sock->m_nRef--;  /* the internal event is done with this socket */
	if (sock->m_nRef == 0)
	{
		UNLOCK(&sock->m_sLock);
		_Destroy(&sock);
		return;
	}

	if (more_data)
		sock->m_pMgr->_SelectPoke(sock->m_nFD, SELECT_POKE_WRITE);

	UNLOCK(&sock->m_sLock);
}

void BCSocket::_InteranlFDWatchRead(BCTask *me, BCTaskEvent *ev)
{
	BCSocket *sock;
	int more_data;

	INSIST(ev->ev_type == BC_SOCKEVENT_INTR);

	/*
	 * Find out what socket this is and lock it.
	 */
	sock = (BCSocket *)ev->ev_sender;
	INSIST(VALID_SOCKET(sock));

	LOCK(&sock->m_sLock);

	INSIST(sock->m_nPendingRecv == 1);

	UNLOCK(&sock->m_sLock);
	more_data = (sock->m_pFdWatchCB)(me, (BCSocket *)sock,
				      sock->m_pFdWatchArg, BC_SOCKFDWATCH_READ);
	LOCK(&sock->m_sLock);

	sock->m_nPendingRecv = 0;

	INSIST(sock->m_nRef > 0);
	sock->m_nRef--;  /* the internal event is done with this socket */
	if (sock->m_nRef == 0)
	{
		UNLOCK(&sock->m_sLock);
		_Destroy(&sock);
		return;
	}

	if (more_data)
		sock->m_pMgr->_SelectPoke(sock->m_nFD, SELECT_POKE_READ);

	UNLOCK(&sock->m_sLock);
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
	int io_state;
	BOOL have_lock = FALSE;
	BCTask *ntask = NULL;
	BCRESULT result = BC_R_SUCCESS;

	dev->ev_sender = task;

	if (m_eType == bc_sockettype_udp)
	{
		io_state = _DoIoRecv(this, dev);
	}
	else
	{
		LOCK(&m_sLock);
		have_lock = TRUE;

		if (m_lstRecvEvents.IsEmpty())
			io_state = _DoIoRecv(this, dev);
		else
			io_state = DOIO_SOFT;
	}

	switch (io_state)
	{
	case DOIO_SOFT:
		/*
		 * We couldn't read all or part of the request right now, so
		 * queue it.
		 *
		 * Attach to socket and to task
		 */
		task->Attach(&ntask);
		dev->attributes |= BC_SOCKEVENTATTR_ATTACHED;

		if (!have_lock)
		{
			LOCK(&m_sLock);
			have_lock = TRUE;
		}

		/*
		 * Enqueue the request.  If the socket was previously not being
		 * watched, poke the watcher to start paying attention to it.
		 */
		if (m_lstRecvEvents.IsEmpty() && !m_nPendingRecv)
			m_pMgr->_SelectPoke(m_nFD, SELECT_POKE_READ);
		m_lstRecvEvents.PushBack(dev);

		if ((flags & BC_SOCKFLAG_IMMEDIATE) != 0)
			result = BC_R_INPROGRESS;
		break;

	case DOIO_EOF:
		dev->result = BC_R_EOF;
		/* fallthrough */

	case DOIO_HARD:
	case DOIO_SUCCESS:
		if ((flags & BC_SOCKFLAG_IMMEDIATE) == 0)
			_SendRecvDoneEvent(&dev);
		break;
	}

	if (have_lock)
		UNLOCK(&m_sLock);

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

	ASSERT(VALID_SOCKET(this));
	ASSERT(action != NULL);

	ASSERT(VALID_MANAGER(m_pMgr));

	ASSERT(m_bBound);

	dev = new BCSockEvent(this, BC_SOCKEVENT_RECVDONE, action, arg);
	if (dev == NULL)
	{
		m_sLock.Unlock();
		return (BC_R_NOMEMORY);
	}

	return (Recv2(region, minimum, task, dev, 0));
}

BCRESULT BCSocket::Recv2(
	BCRegionS *pRegion,
	uint32_t minimum,
	BCTask *task,
	BCSockEvent *pEvent,
	uint32_t nFlags)
{
	ASSERT(VALID_SOCKET(this));
	pEvent->result = BC_R_UNEXPECTED;
	pEvent->ev_sender = this;
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
		pEvent->minimum = 1;
	else
	{
		if (minimum == 0)
			pEvent->minimum = pRegion->length;
		else
			pEvent->minimum = minimum;
	}

	return (_Recv(pEvent, task, nFlags));
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

	ASSERT(VALID_SOCKET(this));
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
		dev->minimum = 1;
	else
	{
		if (minimum == 0)
			dev->minimum = iocount;
		else
			dev->minimum = minimum;
	}

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	dev->bufferlist = buflist;

	return (_Recv(dev, task, 0));
}

BCRESULT BCSocket::RecvV2(
	BCBuffer *buflist,
	uint32_t minimum,
	BCTask *task,
	BCSockEvent *pEvent,
	uint32_t nFlags)
{
	uint32_t iocount;

	UNUSED(nFlags);
	ASSERT(VALID_SOCKET(this));
	pEvent->result = BC_R_UNEXPECTED;
	pEvent->ev_sender = this;


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
		pEvent->minimum = 1;
	else
	{
		if (minimum == 0)
			pEvent->minimum = iocount;
		else
			pEvent->minimum = minimum;
	}

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	pEvent->bufferlist = buflist;

	return (_Recv(pEvent, task, 0));
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
	BOOL have_lock = FALSE;
	BCTask *ntask = NULL;
	BCRESULT result = BC_R_SUCCESS;

	dev->ev_sender = task;

	_SetDevAddress(pAddress, this, dev);
	if (pktinfo != NULL)
	{
		dev->attributes |= BC_SOCKEVENTATTR_PKTINFO;
		dev->pktinfo = *pktinfo;

		if (!bc_sockaddr_issitelocal(&dev->address) &&
		    !bc_sockaddr_islinklocal(&dev->address))
		{
			/*
			 * Set the pktinfo index to 0 here, to let the
			 * kernel decide what interface it should send on.
			 */
			dev->pktinfo.ipi6_ifindex = 0;
		}
	}

	if (m_eType == bc_sockettype_udp)
	{
		io_state = _DoIoSend(this, dev);
	}
	else
	{
		LOCK(&m_sLock);
		have_lock = TRUE;

		if (m_lstSendEvents.IsEmpty())
		{
			io_state = _DoIoSend(this, dev);
		}
		else
		{
			io_state = DOIO_SOFT;
		}
	}

	switch (io_state)
	{
	case DOIO_SOFT:
		/*
		 * We couldn't send all or part of the request right now, so
		 * queue it unless BC_SOCKFLAG_NORETRY is set.
		 */
		if ((flags & BC_SOCKFLAG_NORETRY) == 0)
		{
			task->Attach(&ntask);
			dev->attributes |= BC_SOCKEVENTATTR_ATTACHED;

			if (!have_lock)
			{
				LOCK(&m_sLock);
				have_lock = TRUE;
			}

			/*
			 * Enqueue the request.  If the socket was previously
			 * not being watched, poke the watcher to start
			 * paying attention to it.
			 */
			if (m_lstSendEvents.IsEmpty() && !m_nPendingSend)
			{
				m_pMgr->_SelectPoke(m_nFD, SELECT_POKE_WRITE);
			}
			m_lstSendEvents.PushBack(dev);

			if ((flags & BC_SOCKFLAG_IMMEDIATE) != 0)
				result = BC_R_INPROGRESS;
			break;
		}

	case DOIO_HARD:
	case DOIO_SUCCESS:
		if ((flags & BC_SOCKFLAG_IMMEDIATE) == 0)
			_SendSendDoneEvent(&dev);
		break;
	}

	if (have_lock)
		UNLOCK(&m_sLock);

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

	ASSERT(VALID_SOCKET(this));
	ASSERT(m_eType != bc_sockettype_fdwatch);
	ASSERT(region != NULL);
	ASSERT(region_size <= BC_SOCKET_MAXSCATTERGATHER);
	ASSERT(task != NULL);
	ASSERT(action != NULL);

	ASSERT(VALID_MANAGER(m_pMgr));

	ASSERT(m_bBound);

	dev = new BCSockEvent(this, BC_SOCKEVENT_SENDDONE, action, arg);
	if (dev == NULL)
	{
		return (BC_R_NOMEMORY);
	}
	for (uint32_t i = 0; i < region_size; i++)
	{
		dev->region[i] = region[i];
		dev->region_size++;
	}

	return (_Send(dev, task, pAddress, pktinfo, 0));
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
	ASSERT(VALID_SOCKET(this));
	ASSERT((flags & ~(BC_SOCKFLAG_IMMEDIATE|BC_SOCKFLAG_NORETRY)) == 0);
	if ((flags & BC_SOCKFLAG_NORETRY) != 0)
		ASSERT(m_eType == bc_sockettype_udp);

	ASSERT(region != NULL);
	ASSERT(region_size <= BC_SOCKET_MAXSCATTERGATHER);
	ASSERT(task != NULL);
	ASSERT(pEvent != NULL);
	/*
	 * make sure that the socket's not closed
	 */
	if (m_nFD == INVALID_SOCKET)
	{
		return (BC_R_CONNREFUSED);
	}

	for (uint32_t i = 0; i < region_size; i++)
	{
		pEvent->region[i] = region[i];
		pEvent->region_size++;
	}
	pEvent->ev_sender = this;
	pEvent->result = BC_R_UNEXPECTED;
	pEvent->bufferlist = NULL;
	pEvent->n = 0;
	pEvent->offset = 0;
	pEvent->attributes = 0;

	return (_Send(pEvent, task, address, pktinfo, flags));
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

	ASSERT(VALID_SOCKET(this));
	ASSERT(buflist != NULL);
	ASSERT(VALID_BCBUFFER(buflist));
	ASSERT(task != NULL);
	ASSERT(action != NULL);

	ASSERT(VALID_MANAGER(m_pMgr));

	iocount = buflist->RemainingLength();
	ASSERT(iocount > 0);

	dev = new BCSockEvent(this, BC_SOCKEVENT_SENDDONE, action, arg);
	if (dev == NULL)
	{
		return (BC_R_NOMEMORY);
	}

	/*
	 * Move each buffer from the passed in list to our internal one.
	 */
	dev->bufferlist = buflist;

	return (_Send(dev, task, address, pktinfo, 0));
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

	ASSERT(VALID_SOCKET(this));
	ASSERT((flags & ~(BC_SOCKFLAG_IMMEDIATE|BC_SOCKFLAG_NORETRY)) == 0);
	if ((flags & BC_SOCKFLAG_NORETRY) != 0)
		ASSERT(m_eType == bc_sockettype_udp);
	pEvent->ev_sender = this;
	pEvent->result = BC_R_UNEXPECTED;
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

	return (_Send(pEvent, task, address, pktinfo, flags));
}

BCRESULT BCSocket::Bind(
	BCSockAddrS *sockaddr,
	uint32_t options)
{
	char strbuf[BC_STRERRORSIZE];
	int on = 1;

	REQUIRE(VALID_SOCKET(this));

	LOCK(&m_sLock);

	INSIST(!m_bBound);

	if (m_nPF != sockaddr->type.sa.sa_family)
	{
		UNLOCK(&m_sLock);
		return (BC_R_FAMILYMISMATCH);
	}
	/*
	 * Only set SO_REUSEADDR when we want a specific port.
	 */
#ifdef AF_UNIX
	if (m_nPF == AF_UNIX)
		goto bind_socket;
#endif
	if ((options & BC_SOCKET_REUSEADDRESS) != 0 &&
	    bc_sockaddr_getport(sockaddr) != (in_port_t)0 &&
	    setsockopt(m_nFD, SOL_SOCKET, SO_REUSEADDR, (void *)&on,
		       sizeof(on)) < 0)
	{
		LogError(_LOCAL_, "setsockopt(%d) %s", m_nFD, "failed");
		/* Press on... */
	}
#ifdef AF_UNIX
 bind_socket:
#endif
	if (bind(m_nFD, &sockaddr->type.sa, sockaddr->length) < 0)
	{
		inc_stats(m_pMgr->m_pStats,  m_pStatsIndex[STATID_BINDFAIL]);

		UNLOCK(&m_sLock);
		switch (errno)
		{
		case EACCES:
			return (BC_R_NOPERM);
		case EADDRNOTAVAIL:
			return (BC_R_ADDRNOTAVAIL);
		case EADDRINUSE:
			return (BC_R_ADDRINUSE);
		case EINVAL:
			return (BC_R_BOUND);
		default:
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_, "bind: %s", strbuf);
			return (BC_R_UNEXPECTED);
		}
	}

	m_bBound = 1;

	UNLOCK(&m_sLock);
	return (BC_R_SUCCESS);
}

/*
 * Enable this only for specific OS versions, and only when they have repaired
 * their problems with it.  Until then, this is is broken and needs to be
 * diabled by default.  See RT22589 for details.
 */
#undef ENABLE_ACCEPTFILTER

BCRESULT BCSocket::Filter(const char *filter)
{
#if defined(SO_ACCEPTFILTER) && defined(ENABLE_ACCEPTFILTER)
	char strbuf[BC_STRERRORSIZE];
	struct accept_filter_arg afa;
#else
	UNUSED(filter);
#endif

	REQUIRE(VALID_SOCKET(this));

#if defined(SO_ACCEPTFILTER) && defined(ENABLE_ACCEPTFILTER)
	bzero(&afa, sizeof(afa));
	strncpy(afa.af_name, filter, sizeof(afa.af_name));
	if (setsockopt(m_nFD, SOL_SOCKET, SO_ACCEPTFILTER,
			 &afa, sizeof(afa)) == -1)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "setsockopt(SO_ACCEPTFILTER): %s", strbuf);
		return (BC_R_FAILURE);
	}
	return (BC_R_SUCCESS);
#else
	return (BC_R_NOTIMPLEMENTED);
#endif
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
BCRESULT BCSocket::Listen(unsigned int backlog)
{
	char strbuf[BC_STRERRORSIZE];

	REQUIRE(VALID_SOCKET(this));

	LOCK(&m_sLock);

	REQUIRE(!m_bListener);
	REQUIRE(m_bBound);
	REQUIRE(m_eType == bc_sockettype_tcp ||
		m_eType == bc_sockettype_unix);

	if (backlog == 0)
		backlog = SOMAXCONN;

	if (listen(m_nFD, (int)backlog) < 0)
	{
		UNLOCK(&m_sLock);
		bc_strerror(errno, strbuf, sizeof(strbuf));

		LogError(_LOCAL_, "listen: %s", strbuf);

		return (BC_R_UNEXPECTED);
	}

	m_bListener = 1;

	UNLOCK(&m_sLock);
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
	BCSockICEvent *dev;
	BCTask *ntask = NULL;
	BCSocket *nsock;
	BCRESULT result;
	BOOL do_poke = FALSE;

	REQUIRE(VALID_SOCKET(this));
	REQUIRE(VALID_MANAGER(m_pMgr));

	LOCK(&m_sLock);

	REQUIRE(m_bListener);

	/*
	 * Sender field is overloaded here with the task we will be sending
	 * this event to.  Just before the actual event is delivered the
	 * actual ev_sender will be touched up to be the socket.
	 */
	dev = new BCSockICEvent(task, BC_SOCKEVENT_NEWCONN, action, arg);
	if (dev == NULL)
	{
		UNLOCK(&m_sLock);
		return (BC_R_NOMEMORY);
	}

	nsock = new BCSocket();
	if (nsock == NULL)
	{
		dev->Destroy();
		UNLOCK(&m_sLock);
		return BC_R_NOMEMORY;
	}
	result = nsock->_InternalCreate(m_pMgr, m_eType);
	if (result != BC_R_SUCCESS)
	{
		_Destroy(&nsock);
		dev->Destroy();
		UNLOCK(&m_sLock);
		return (result);
	}

	/*
	 * Attach to socket and to task.
	 */
	task->Attach(&ntask);
	if (ntask->IsExiting())
	{
		ntask->Detach(&ntask);
		_Destroy(&nsock);
		dev->Destroy();
		UNLOCK(&m_sLock);
		return (BC_R_SHUTTINGDOWN);
	}
	nsock->m_nRef++;
	nsock->m_pStatsIndex = m_pStatsIndex;

	dev->ev_sender = ntask;
	dev->newsocket = (BCSocket *)nsock;

	/*
	 * Poke watcher here.  We still have the socket locked, so there
	 * is no race condition.  We will keep the lock for such a short
	 * bit of time waking it up now or later won't matter all that much.
	 */
	if (m_lstAcceptEvents.IsEmpty())
		do_poke = TRUE;

	m_lstAcceptEvents.PushBack(dev);

	if (do_poke)
		m_pMgr->_SelectPoke(m_nFD, SELECT_POKE_ACCEPT);

	UNLOCK(&m_sLock);
	return (BC_R_SUCCESS);
}

BCRESULT BCSocket::Connect(
	BCSockAddrS *addr,
	BCTask *task,
	LPFN_BCTaskAction action,
	const void *arg)
{
	BCSockOCEvent *dev;
	BCTask *ntask = NULL;
	int cc;
	char strbuf[BC_STRERRORSIZE];
	char addrbuf[BC_SOCKADDR_FORMATSIZE];

	REQUIRE(VALID_SOCKET(this));
	REQUIRE(addr != NULL);
	REQUIRE(task != NULL);
	REQUIRE(action != NULL);

	REQUIRE(VALID_MANAGER(m_pMgr));
	REQUIRE(addr != NULL);

	if (bc_sockaddr_ismulticast(addr))
		return (BC_R_MULTICAST);

	LOCK(&m_sLock);

	REQUIRE(!m_bConnecting);

	dev = new BCSockOCEvent(this, BC_SOCKEVENT_CONNECT, action,	arg);
	if (dev == NULL)
	{
		UNLOCK(&m_sLock);
		return (BC_R_NOMEMORY);
	}

	/*
	 * Try to do the connect right away, as there can be only one
	 * outstanding, and it might happen to complete.
	 */
	m_sAddress = *addr;
	cc = connect(m_nFD, &addr->type.sa, addr->length);
	if (cc < 0)
	{
		/*
		 * HP-UX "fails" to connect a UDP socket and sets errno to
		 * EINPROGRESS if it's non-blocking.  We'd rather regard this as
		 * a success and let the user detect it if it's really an error
		 * at the time of sending a packet on the socket.
		 */
		if (m_eType == bc_sockettype_udp && errno == EINPROGRESS)
		{
			cc = 0;
			goto success;
		}
		if (SOFT_ERROR(errno) || errno == EINPROGRESS)
			goto queue;

		switch (errno)
		{
#define ERROR_MATCH(a, b) case a: dev->result = b; goto err_exit;
			ERROR_MATCH(EACCES, BC_R_NOPERM);
			ERROR_MATCH(EADDRNOTAVAIL, BC_R_ADDRNOTAVAIL);
			ERROR_MATCH(EAFNOSUPPORT, BC_R_ADDRNOTAVAIL);
			ERROR_MATCH(ECONNREFUSED, BC_R_CONNREFUSED);
			ERROR_MATCH(EHOSTUNREACH, BC_R_HOSTUNREACH);
#ifdef EHOSTDOWN
			ERROR_MATCH(EHOSTDOWN, BC_R_HOSTUNREACH);
#endif
			ERROR_MATCH(ENETUNREACH, BC_R_NETUNREACH);
			ERROR_MATCH(ENOBUFS, BC_R_NORESOURCES);
			ERROR_MATCH(EPERM, BC_R_HOSTUNREACH);
			ERROR_MATCH(EPIPE, BC_R_NOTCONNECTED);
			ERROR_MATCH(ECONNRESET, BC_R_CONNECTIONRESET);
#undef ERROR_MATCH
		}

		m_bConnected = 0;

		bc_strerror(errno, strbuf, sizeof(strbuf));
		bc_sockaddr_format(addr, addrbuf, sizeof(addrbuf));
		LogError(_LOCAL_, "connect(%s) %d/%s", addrbuf, errno, strbuf);

		UNLOCK(&m_sLock);
		inc_stats(m_pMgr->m_pStats,
			  m_pStatsIndex[STATID_CONNECTFAIL]);
		dev->Destroy();
		return (BC_R_UNEXPECTED);

	err_exit:
		m_bConnected = 0;
		task->Send((BCTaskEvent **)&dev);

		UNLOCK(&m_sLock);
		inc_stats(m_pMgr->m_pStats,
			  m_pStatsIndex[STATID_CONNECTFAIL]);
		return (BC_R_SUCCESS);
	}

	/*
	 * If connect completed, fire off the done event.
	 */
 success:
	if (cc == 0)
	{
		m_bConnected = 1;
		m_bBound = 1;
		dev->result = BC_R_SUCCESS;
		task->Send((BCTaskEvent **)&dev);

		UNLOCK(&m_sLock);

		inc_stats(m_pMgr->m_pStats,
			  m_pStatsIndex[STATID_CONNECT]);

		return (BC_R_SUCCESS);
	}

 queue:

	/*
	 * Attach to task.
	 */
	task->Attach(&ntask);

	m_bConnecting = 1;

	dev->ev_sender = ntask;

	/*
	 * Poke watcher here.  We still have the socket locked, so there
	 * is no race condition.  We will keep the lock for such a short
	 * bit of time waking it up now or later won't matter all that much.
	 */
	if (m_pConnectEvent == NULL)
		m_pMgr->_SelectPoke(m_nFD, SELECT_POKE_CONNECT);

	m_pConnectEvent = dev;

	UNLOCK(&m_sLock);
	return (BC_R_SUCCESS);
}

BCRESULT BCSocket::GetPeerName(BCSockAddrS *addressp)
{
	BCRESULT result;

	ASSERT(VALID_SOCKET(this));
	ASSERT(addressp != NULL);

	m_sLock.Lock();

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

	len = sizeof(addressp->type);
	if (getsockname(m_nFD, &addressp->type.sa, &len) < 0)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
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
				dev->RemoveFromList();
				dev->newsocket->m_nRef--;
				_Free(&dev->newsocket);

				dev->result = BC_R_CANCELED;
				dev->ev_sender = this;
				current_task->SendAndDetach((BCTaskEvent **)&dev);
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

		ASSERT(m_bConnecting);
		this->m_bConnecting = 0;

		dev = m_pConnectEvent;
		current_task = (BCTask *)dev->ev_sender;

		if ((task == NULL) || (task == current_task))
		{
			m_pConnectEvent = NULL;
			dev->result = BC_R_CANCELED;
			dev->ev_sender = this;
			current_task->SendAndDetach((BCTaskEvent **)&dev);
		}
	}

	m_sLock.Unlock();
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
	memset(m_szName, 0, sizeof(m_szName));
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
#endif // _DEBUG





///////////////////////////////////////////////////////////////////////////////
// class : BCSocketMgr
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCSocketMgr, 5);

BCSocketMgr::BCSocketMgr()
		: BCMagic(SOCKET_MANAGER_MAGIC)
		, m_sCondShutdownOk(&m_sLock)
		, m_nTotalSockets(0)
{
	//
}

BCSocketMgr::~BCSocketMgr()
{
	//
}

BCRESULT BCSocketMgr::Create(unsigned int nMaxSocks /*= 0*/)
{
#ifdef USE_WATCHER_THREAD
	char strbuf[BC_STRERRORSIZE];
#endif
	BCRESULT result;

	if (nMaxSocks == 0)
		nMaxSocks = BC_SOCKET_MAXSOCKETS;

	/* zero-clear so that necessary cleanup on failure will be easy */
	m_nMaxSocks = nMaxSocks;
	m_nReserved = 0;
	m_nMaxUdp = 0;
	m_pStats = NULL;

	m_nMagic = SOCKET_MANAGER_MAGIC;

#ifdef USE_WATCHER_THREAD
	/*
	 * Create the special fds that will be used to wake up the
	 * select/poll loop when something internal needs to be done.
	 */
	if (pipe(m_sPipeFds) != 0)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "pipe() %s: %s",  "failed", strbuf);
		result = BC_R_UNEXPECTED;
		goto free_manager;
	}

	RUNTIME_CHECK(make_nonblock(m_sPipeFds[0]) == BC_R_SUCCESS);
#if 0
	RUNTIME_CHECK(make_nonblock(m_sPipeFds[1]) == BC_R_SUCCESS);
#endif
#endif	/* USE_WATCHER_THREAD */

	/*
	 * Set up initial state for the select loop
	 */
	result = _SetupWatcher();
	if (result != BC_R_SUCCESS)
		goto cleanup;
#ifdef USE_WATCHER_THREAD
	/*
	 * Start up the select/poll thread.
	 */
	m_pWatcherThrd = new BCThread(_WatcherThreadFunc,
		this, BCThread::PRIORITY_HIGH, "BCSocketMgrWatcherThread");
	if (m_pWatcherThrd == NULL)
	{
		LogError(_LOCAL_,  "new BCThread() %s", "failed");
		_CleanupWatcher();
		result = BC_R_NOMEMORY;
		goto cleanup;
	}
	m_pWatcherThrd->Start();
#endif /* USE_WATCHER_THREAD */

	return (BC_R_SUCCESS);

cleanup:
#ifdef USE_WATCHER_THREAD
	(void)close(m_sPipeFds[0]);
	(void)close(m_sPipeFds[1]);
#endif	/* USE_WATCHER_THREAD */

free_manager:
	m_sPool.Clear();
	return (result);
}

BCRESULT BCSocketMgr::Create(
	BCSocketMgr **managerp,
	unsigned int maxsocks /*= 0*/)
{
	BCSocketMgr *manager;
#ifdef USE_WATCHER_THREAD
	char strbuf[BC_STRERRORSIZE];
#endif
	BCRESULT result;

	REQUIRE(managerp != NULL && *managerp == NULL);

#ifdef USE_SHARED_MANAGER
	if (socketmgr != NULL)
	{
		/* Don't allow maxsocks to be updated */
		if (maxsocks > 0 && socketmgr->m_nMaxSocks != maxsocks)
			return (BC_R_EXISTS);

		socketmgr->m_nRefs++;
		*managerp = (BCSocketMgr *)socketmgr;
		return (BC_R_SUCCESS);
	}
#endif /* USE_SHARED_MANAGER */

	if (maxsocks == 0)
		maxsocks = BC_SOCKET_MAXSOCKETS;

	manager = new BCSocketMgr();
	if (manager == NULL)
		return (BC_R_NOMEMORY);

	/* zero-clear so that necessary cleanup on failure will be easy */
	manager->m_nMaxSocks = maxsocks;
	manager->m_nReserved = 0;
	manager->m_nMaxUdp = 0;
	manager->m_pStats = NULL;

	manager->m_nMagic = SOCKET_MANAGER_MAGIC;

#ifdef USE_WATCHER_THREAD
	/*
	 * Create the special fds that will be used to wake up the
	 * select/poll loop when something internal needs to be done.
	 */
	if (pipe(manager->m_sPipeFds) != 0)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "pipe() %s: %s",  "failed", strbuf);
		result = BC_R_UNEXPECTED;
		goto free_manager;
	}

	RUNTIME_CHECK(make_nonblock(manager->m_sPipeFds[0]) == BC_R_SUCCESS);
#if 0
	RUNTIME_CHECK(make_nonblock(manager->m_sPipeFds[1]) == BC_R_SUCCESS);
#endif
#endif	/* USE_WATCHER_THREAD */

#ifdef USE_SHARED_MANAGER
	manager->m_nRefs = 1;
#endif /* USE_SHARED_MANAGER */

	/*
	 * Set up initial state for the select loop
	 */
	result = manager->_SetupWatcher();
	if (result != BC_R_SUCCESS)
		goto cleanup;
#ifdef USE_WATCHER_THREAD
	/*
	 * Start up the select/poll thread.
	 */
	manager->m_pWatcherThrd = new BCThread(_WatcherThreadFunc,
		manager, BCThread::PRIORITY_HIGH, "BCSocketMgrWatcherThread");
	if (manager->m_pWatcherThrd == NULL)
	{
		LogError(_LOCAL_,  "new BCThread() %s", "failed");
		manager->_CleanupWatcher();
		result = BC_R_NOMEMORY;
		goto cleanup;
	}
	manager->m_pWatcherThrd->Start();
#endif /* USE_WATCHER_THREAD */

#ifdef USE_SHARED_MANAGER
	socketmgr = manager;
#endif /* USE_SHARED_MANAGER */
	*managerp = (BCSocketMgr *)manager;

	return (BC_R_SUCCESS);

cleanup:
#ifdef USE_WATCHER_THREAD
	(void)close(manager->m_sPipeFds[0]);
	(void)close(manager->m_sPipeFds[1]);
#endif	/* USE_WATCHER_THREAD */

free_manager:
	manager->m_sPool.Clear();
	return (result);
}

void BCSocketMgr::Destroy(BCSocketMgr **managerp)
{
	BCSocketMgr *manager;

	/*
	 * Destroy a socket manager.
	 */

	REQUIRE(managerp != NULL);
	manager = (BCSocketMgr *)*managerp;
	REQUIRE(VALID_MANAGER(manager));

#ifdef USE_SHARED_MANAGER
	manager->m_nRefs--;
	if (manager->m_nRefs > 0)
	{
		*managerp = NULL;
		return;
	}
	socketmgr = NULL;
#endif /* USE_SHARED_MANAGER */

	LOCK(&manager->m_sLock);

	/*
	 * Wait for all sockets to be destroyed.
	 */
	while (!manager->m_lstSockets.IsEmpty())
	 {
#ifdef USE_WATCHER_THREAD
		manager->m_sCondShutdownOk.Wait();
#else /* USE_WATCHER_THREAD */
		UNLOCK(&manager->m_sLock);
		manager->_Dispatch(NULL);
		LOCK(&manager->m_sLock);
#endif /* USE_WATCHER_THREAD */
	}

	UNLOCK(&manager->m_sLock);

	/*
	 * Here, poke our select/poll thread.  Do this by closing the write
	 * half of the pipe, which will send EOF to the read half.
	 * This is currently a no-op in the non-threaded case.
	 */
	manager->_SelectPoke(0, SELECT_POKE_SHUTDOWN);

#ifdef USE_WATCHER_THREAD
	/*
	 * Wait for thread to exit.
	 */
	manager->m_pWatcherThrd->Join(NULL);
#endif /* USE_WATCHER_THREAD */

	/*
	 * Clean up.
	 */
	manager->_CleanupWatcher();

#ifdef USE_WATCHER_THREAD
	(void)close(manager->m_sPipeFds[0]);
	(void)close(manager->m_sPipeFds[1]);
#endif /* USE_WATCHER_THREAD */

	for (int i = 0;i < FDLOCK_COUNT;i++)
	{
		LOCK(&manager->m_sFdCtx[i].lock);
		auto &map = manager->m_sFdCtx[i].mapFdState;
		for (auto it = map.begin(); it != map.end(); it++)
		{
			if (it->second == CLOSE_PENDING) /* need to lock */
			{
				(void)close(it->first);
			}
		}
		UNLOCK(&manager->m_sFdCtx[i].lock);
	}

	manager->m_nMagic = 0;

	delete manager;

	*managerp = NULL;

#ifdef USE_SHARED_MANAGER
	socketmgr = NULL;
#endif
}

void BCSocketMgr::SetReserved(uint32_t reserved)
{
	REQUIRE(VALID_MANAGER(this));

	m_nReserved = reserved;
}

void BCSocketMgr::Maxudp(int maxudp)
{
	REQUIRE(VALID_MANAGER(this));

	m_nMaxUdp = maxudp;
}

BCRESULT BCSocketMgr::GetMaxSockets(unsigned int *nsockp)
{
	REQUIRE(VALID_MANAGER(this));
	REQUIRE(nsockp != NULL);

	*nsockp = m_nMaxSocks;

	return (BC_R_SUCCESS);
}

void BCSocketMgr::SetStats(BCStats *stats)
{
	REQUIRE(VALID_MANAGER(this));
	REQUIRE(m_lstSockets.IsEmpty());
	REQUIRE(m_pStats == NULL);
	REQUIRE(stats->GetNumOfCounters() == bc_sockstatscounter_max);

	stats->Attach(&m_pStats);
}

uint32_t BCSocketMgr::GetSocketCount() const
{
	return m_lstSockets.Count();
}

void BCSocketMgr::_Destroy()
{
	delete this;
}

BCRESULT BCSocketMgr::_WatchFD(int fd, int msg)
{
	BCRESULT result = BC_R_SUCCESS;

#ifdef USE_KQUEUE
	struct kevent evchange;

	memset(&evchange, 0, sizeof(evchange));
	if (msg == SELECT_POKE_READ)
		evchange.filter = EVFILT_READ;
	else
		evchange.filter = EVFILT_WRITE;
	evchange.flags = EV_ADD;
	evchange.ident = fd;
	if (kevent(m_nKqueueFd, &evchange, 1, NULL, 0, NULL) != 0)
		result = bc__errno2resultx(errno);

	return (result);
#elif defined(USE_EPOLL)
	struct epoll_event event;
	uint32_t events, newEvents;

	// Get old events value
	events = (uint64_t)m_htSockEvents.Get(fd);
	if (msg == SELECT_POKE_READ)
	{
		newEvents = EPOLLIN;
	}
	else
	{
		newEvents = EPOLLOUT;
	}
	if (newEvents == events)
	{
		// Already exists
		return result;
	}
	newEvents |= events;
	event.events = newEvents;
	memset(&event.data, 0, sizeof(event.data));
	event.data.fd = fd;
	if (events == 0) // Not exists, add new control
	{
		if (epoll_ctl(m_nEpollFd, EPOLL_CTL_ADD, fd, &event) == -1 &&
		    errno != EEXIST)
		{
			result = bc_errno2result(errno);
		} 
	}
	else // Already exists, modify the old one
	{
		if (epoll_ctl(m_nEpollFd, EPOLL_CTL_MOD, fd, &event) == -1 &&
		    errno != EEXIST)
		{
			result = bc_errno2result(errno);
		}
	}
	m_htSockEvents.Set(fd, (void *)(uint64_t)newEvents);
	// LogDebug(_LOCAL_, "BCSocketMgr::_WatchFD fd:%d;msg:%d;exist:%d", fd, msg, errno == EEXIST?1:0);

	return (result);
#elif defined(USE_DEVPOLL)
	struct pollfd pfd;
	int lockid = FDLOCK_ID(fd);

	memset(&pfd, 0, sizeof(pfd));
	if (msg == SELECT_POKE_READ)
		pfd.events = POLLIN;
	else
		pfd.events = POLLOUT;
	pfd.fd = fd;
	pfd.revents = 0;
	LOCK(&m_sFdCtx[lockid].lock);
	if (write(m_nDevpollFd, &pfd, sizeof(pfd)) == -1)
		result = bc_errno2result(errno);
	else
	{
		if (msg == SELECT_POKE_READ)
			m_sFdCtx[lockid].mapFdPollInfo[fd].want_read = 1;
		else
			m_sFdCtx[lockid].mapFdPollInfo[fd].want_write = 1;
	}
	UNLOCK(&m_sFdCtx[lockid].lock);

	return (result);
#elif defined(USE_SELECT)
	LOCK(&m_sLock);
	if (msg == SELECT_POKE_READ)
		FD_SET(fd, m_pReadFds);
	if (msg == SELECT_POKE_WRITE)
		FD_SET(fd, m_pWriteFds);
	UNLOCK(&m_sLock);

	return (result);
#endif
}

BCRESULT BCSocketMgr::_UnwatchFD(int fd, int msg)
{
	BCRESULT result = BC_R_SUCCESS;

#ifdef USE_KQUEUE
	struct kevent evchange;

	memset(&evchange, 0, sizeof(evchange));
	if (msg == SELECT_POKE_READ)
		evchange.filter = EVFILT_READ;
	else
		evchange.filter = EVFILT_WRITE;
	evchange.flags = EV_DELETE;
	evchange.ident = fd;
	if (kevent(m_nKqueueFd, &evchange, 1, NULL, 0, NULL) != 0)
		result = bc__errno2resultx(errno);

	return (result);
#elif defined(USE_EPOLL)
	struct epoll_event event;
	uint32_t events, newEvents;

	// Get old events value
	events = (uint64_t)m_htSockEvents.Get(fd);
	newEvents = events;
	if (msg == SELECT_POKE_READ)
	{
		newEvents &= ~EPOLLIN;
	}
	else
	{
		newEvents &= ~EPOLLOUT;
	}
	if (newEvents == events)
	{
		// Already exists
		return result;
	}
	event.events = newEvents;
	memset(&event.data, 0, sizeof(event.data));
	event.data.fd = fd;
	if (newEvents != 0) // Already exists, modify the old one
	{
		if (epoll_ctl(m_nEpollFd, EPOLL_CTL_MOD, fd, &event) == -1 &&
		    errno != EEXIST)
		{
			result = bc_errno2result(errno);
		}
	}
	else
	{
		if (epoll_ctl(m_nEpollFd, EPOLL_CTL_DEL, fd, &event) == -1 &&
		    errno != ENOENT)
		{
			char strbuf[BC_STRERRORSIZE];
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_, "epoll_ctl(DEL), %d: %s", fd, strbuf);
			result = BC_R_UNEXPECTED;
		}
	}
	m_htSockEvents.Set(fd, (void *)(uint64_t)newEvents);
	
	return (result);
#elif defined(USE_DEVPOLL)
	struct pollfd pfds[2];
	size_t writelen = sizeof(pfds[0]);
	int lockid = FDLOCK_ID(fd);

	memset(pfds, 0, sizeof(pfds));
	pfds[0].events = POLLREMOVE;
	pfds[0].fd = fd;

	/*
	 * Canceling read or write polling via /dev/poll is tricky.  Since it
	 * only provides a way of canceling per FD, we may need to re-poll the
	 * socket for the other operation.
	 */
	LOCK(&m_sFdCtx[lockid].lock);
	if (msg == SELECT_POKE_READ &&
	    m_sFdCtx[lockid].mapFdPollInfo[fd].want_write == 1)
	{
		pfds[1].events = POLLOUT;
		pfds[1].fd = fd;
		writelen += sizeof(pfds[1]);
	}
	if (msg == SELECT_POKE_WRITE &&
	    m_sFdCtx[lockid].mapFdPollInfo[fd].want_read == 1)
	{
		pfds[1].events = POLLIN;
		pfds[1].fd = fd;
		writelen += sizeof(pfds[1]);
	}

	if (write(m_nDevpollFd, pfds, writelen) == -1)
		result = bc__errno2result(errno);
	else
	{
		if (msg == SELECT_POKE_READ)
			m_sFdCtx[lockid].mapFdPollInfo[fd].want_read = 0;
		else
			m_sFdCtx[lockid].mapFdPollInfo[fd].want_write = 0;
	}
	UNLOCK(&m_sFdCtx[lockid].lock);

	return (result);
#elif defined(USE_SELECT)
	LOCK(&m_sLock);
	if (msg == SELECT_POKE_READ)
		FD_CLR(fd, m_pReadFds);
	else if (msg == SELECT_POKE_WRITE)
		FD_CLR(fd, m_pWriteFds);
	UNLOCK(&m_sLock);

	return (result);
#endif
}

void BCSocketMgr::_WakeupSocket(int fd, int msg)
{
	BCRESULT result;
	int lockid = FDLOCK_ID(fd);

	/*
	 * This is a wakeup on a socket.  If the socket is not in the
	 * process of being closed, start watching it for either reads
	 * or writes.
	 */

	if (msg == SELECT_POKE_CLOSE)
	{
		LOCK(&m_sFdCtx[lockid].lock);
		INSIST(m_sFdCtx[lockid].mapFdState[fd] == CLOSE_PENDING);
		// m_sFdCtx[lockid].mapFdState[fd] = CLOSED;
		m_sFdCtx[lockid].mapFdState.erase(fd);
		UNLOCK(&m_sFdCtx[lockid].lock);
		(void)_UnwatchFD(fd, SELECT_POKE_READ);
		(void)_UnwatchFD(fd, SELECT_POKE_WRITE);
		(void)close(fd);
		return;
	}

	LOCK(&m_sFdCtx[lockid].lock);
	if (m_sFdCtx[lockid].mapFdState[fd] == CLOSE_PENDING)
	{
		UNLOCK(&m_sFdCtx[lockid].lock);

		/*
		 * We accept (and ignore) any error from _UnwatchFD() as we are
		 * closing the socket, hoping it doesn't leave dangling state in
		 * the kernel.
		 * Note that _UnwatchFD() must be called after releasing the
		 * m_sFdCtx; otherwise it could cause deadlock due to a lock order
		 * reversal.
		 */
		(void)_UnwatchFD(fd, SELECT_POKE_READ);
		(void)_UnwatchFD(fd, SELECT_POKE_WRITE);
		return;
	}
	if (m_sFdCtx[lockid].mapFdState[fd] != MANAGED)
	{
		UNLOCK(&m_sFdCtx[lockid].lock);
		return;
	}
	UNLOCK(&m_sFdCtx[lockid].lock);

	/*
	 * Set requested bit.
	 */
	result = _WatchFD(fd, msg);
	if (result != BC_R_SUCCESS)
	{
		/*
		 * XXXJT: what should we do?  Ignoring the failure of watching
		 * a socket will make the application dysfunctional, but there
		 * seems to be no reasonable recovery process.
		 */
		LogError(_LOCAL_, "failed to start watching FD (%d): %d", fd, result);
	}
}

#ifdef USE_WATCHER_THREAD
/*
 * Poke the select loop when there is something for us to do.
 * The write is required (by POSIX) to complete.  That is, we
 * will not get partial writes.
 */
void BCSocketMgr::_SelectPoke(int fd, int msg)
{
	int cc;
	int buf[2];
	char strbuf[BC_STRERRORSIZE];

	buf[0] = fd;
	buf[1] = msg;

	do
	{
		cc = write(m_sPipeFds[1], buf, sizeof(buf));
#ifdef ENOSR
		/*
		 * Treat ENOSR as EAGAIN but loop slowly as it is
		 * unlikely to clear fast.
		 */
		if (cc < 0 && errno == ENOSR)
		{
			sleep(1);
			errno = EAGAIN;
		}
#endif
	} while (cc < 0 && SOFT_ERROR(errno));

	if (cc < 0)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogFatal(_LOCAL_,
					   "write() failed "
					   "during watcher poke: %s", strbuf);
	}

	INSIST(cc == sizeof(buf));
}

/*
 * Read a message on the internal fd.
 */
void BCSocketMgr::_SelectReadMsg(int *fd, int *msg)
{
	int buf[2];
	int cc;
	char strbuf[BC_STRERRORSIZE];

	cc = read(m_sPipeFds[0], buf, sizeof(buf));
	if (cc < 0)
	{
		*msg = SELECT_POKE_NOTHING;
		*fd = -1;	/* Silence compiler. */
		if (SOFT_ERROR(errno))
			return;

		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
					   "read() failed "
					   "during watcher poke: %s",  strbuf);

		return;
	}
	INSIST(cc == sizeof(buf));

	*fd = buf[0];
	*msg = buf[1];
}
#else /* USE_WATCHER_THREAD */
/*
 * Update the state of the socketmgr when something changes.
 */
void BCSocketMgr::_SelectPoke(int fd, int msg)
{
	if (msg == SELECT_POKE_SHUTDOWN)
		return;
	else if (fd >= 0)
		_WakeupSocket(fd, msg);
	return;
}
#endif /* USE_WATCHER_THREAD */

/*
 * Process read/writes on each fd here.  Avoid locking
 * and unlocking twice if both reads and writes are possible.
 */
void BCSocketMgr::_ProcessFD(
	int fd,
	BOOL readable,
	BOOL writeable)
{
	BCSocket *sock;
	BOOL unlock_sock;
	BOOL unwatch_read = FALSE, unwatch_write = FALSE;
	int lockid = FDLOCK_ID(fd);

	/*
	 * If the socket is going to be closed, don't do more I/O.
	 */
	LOCK(&m_sFdCtx[lockid].lock);
	if (m_sFdCtx[lockid].mapFdState[fd] == CLOSE_PENDING)
	{
		UNLOCK(&m_sFdCtx[lockid].lock);

		(void)_UnwatchFD(fd, SELECT_POKE_READ);
		(void)_UnwatchFD(fd, SELECT_POKE_WRITE);
		return;
	}

	auto &map = m_sFdCtx[lockid].mapFdSocket;
	if (map.find(fd) != map.end())
	{
		sock = map[fd];
	}
	unlock_sock = FALSE;
	if (readable)
	{
		if (sock == NULL)
		{
			unwatch_read = TRUE;
			goto check_write;
		}
		unlock_sock = TRUE;
		LOCK(&sock->m_sLock);
		if (!SOCK_DEAD(sock))
		{
			if (sock->m_bListener)
				sock->_DispatchAccept();
			else
				sock->_DispatchRecv();
		}
		unwatch_read = TRUE;
	}
check_write:
	if (writeable)
	{
		if (sock == NULL)
		{
			unwatch_write = TRUE;
			goto unlock_fd;
		}
		if (!unlock_sock)
		{
			unlock_sock = TRUE;
			LOCK(&sock->m_sLock);
		}
		if (!SOCK_DEAD(sock))
		{
			if (sock->m_bConnecting)
				sock->_DispatchConnect();
			else
				sock->_DispatchSend();
		}
		unwatch_write = TRUE;
	}
	if (unlock_sock)
		UNLOCK(&sock->m_sLock);

 unlock_fd:
	UNLOCK(&m_sFdCtx[lockid].lock);
	if (unwatch_read)
		(void)_UnwatchFD(fd, SELECT_POKE_READ);
	if (unwatch_write)
		(void)_UnwatchFD(fd, SELECT_POKE_WRITE);
}

#ifdef USE_KQUEUE
BOOL BCSocketMgr::_ProcessFDs(
	struct kevent *events,
	int nevents)
{
	int i;
	BOOL readable, writable;
	BOOL done = FALSE;
#ifdef USE_WATCHER_THREAD
	BOOL have_ctlevent = FALSE;
#endif

	if (nevents == m_nEvents)
	{
		/*
		 * This is not an error, but something unexpected.  If this
		 * happens, it may indicate the need for increasing
		 * BC_SOCKET_MAXEVENTS.
		 */
		LogInfo(_LOCAL_,
			    "maximum number of FD events (%d) received",
			    nevents);
	}

	for (i = 0; i < nevents; i++)
	{
#ifdef USE_WATCHER_THREAD
		if (events[i].ident == (uintptr_t)m_sPipeFds[0])
		{
			have_ctlevent = TRUE;
			continue;
		}
#endif
		readable = (events[i].filter == EVFILT_READ);
		writable = (events[i].filter == EVFILT_WRITE);
		_ProcessFD(events[i].ident, readable, writable);
	}

#ifdef USE_WATCHER_THREAD
	if (have_ctlevent)
		done = _ProcessCtlfd();
#endif

	return (done);
}
#elif defined(USE_EPOLL)
BOOL BCSocketMgr::_ProcessFDs(
	struct epoll_event *events,
	int nevents)
{
	int i;
	BOOL done = FALSE;
#ifdef USE_WATCHER_THREAD
	BOOL have_ctlevent = FALSE;
#endif

	if (nevents == m_nEvents)
	{
		LogInfo(_LOCAL_,
			    "maximum number of FD events (%d) received",
			    nevents);
	}

	for (i = 0; i < nevents; i++)
	{
#ifdef USE_WATCHER_THREAD
		if (events[i].data.fd == m_sPipeFds[0])
		{
			have_ctlevent = TRUE;
			continue;
		}
#endif
		if ((events[i].events & EPOLLERR) != 0 ||
		    (events[i].events & EPOLLHUP) != 0)
		{
			/*
			 * epoll does not set IN/OUT bits on an erroneous
			 * condition, so we need to try both anyway.  This is a
			 * bit inefficient, but should be okay for such rare
			 * events.  Note also that the read or write attempt
			 * won't block because we use non-blocking sockets.
			 */
			events[i].events |= (EPOLLIN | EPOLLOUT);
		}
		_ProcessFD(events[i].data.fd,
			   (events[i].events & EPOLLIN) != 0,
			   (events[i].events & EPOLLOUT) != 0);
	}

#ifdef USE_WATCHER_THREAD
	if (have_ctlevent)
		done = _ProcessCtlfd();
#endif

	return (done);
}
#elif defined(USE_DEVPOLL)
BOOL BCSocketMgr::_ProcessFDs(
	struct pollfd *events,
	int nevents)
{
	int i;
	BOOL done = FALSE;
#ifdef USE_WATCHER_THREAD
	BOOL have_ctlevent = FALSE;
#endif

	if (nevents == m_nEvents)
	{
		LogInfo(_LOCAL_,
			    "maximum number of FD events (%d) received",
			    nevents);
	}

	for (i = 0; i < nevents; i++)
	{
#ifdef USE_WATCHER_THREAD
		if (events[i].fd == m_sPipeFds[0])
		{
			have_ctlevent = TRUE;
			continue;
		}
#endif
		_ProcessFD(events[i].fd,
			   (events[i].events & POLLIN) != 0,
			   (events[i].events & POLLOUT) != 0);
	}

#ifdef USE_WATCHER_THREAD
	if (have_ctlevent)
		done = _ProcessCtlfd();
#endif

	return (done);
}
#elif defined(USE_SELECT)
void BCSocketMgr::_ProcessFDs(
	int maxfd,
	fd_set *readfds,
	fd_set *writefds)
{
	int i;

	for (i = 0; i < maxfd; i++)
	{
#ifdef USE_WATCHER_THREAD
		if (i == m_sPipeFds[0] || i == m_sPipeFds[1])
			continue;
#endif /* USE_WATCHER_THREAD */
		_ProcessFD(i, FD_ISSET(i, readfds), FD_ISSET(i, writefds));
	}
}
#endif

#ifdef USE_WATCHER_THREAD
BOOL BCSocketMgr::_ProcessCtlfd()
{
	int msg, fd;

	for (;;)
	{
		_SelectReadMsg(&fd, &msg);

		/*
		 * Nothing to read?
		 */
		if (msg == SELECT_POKE_NOTHING)
			break;

		/*
		 * Handle shutdown message.  We really should
		 * jump out of this loop right away, but
		 * it doesn't matter if we have to do a little
		 * more work first.
		 */
		if (msg == SELECT_POKE_SHUTDOWN)
			return (TRUE);

		/*
		 * This is a wakeup on a socket.  Look
		 * at the event queue for both read and write,
		 * and decide if we need to watch on it now
		 * or not.
		 */
		_WakeupSocket(fd, msg);
	}

	return (FALSE);
}

/*
 * This is the thread that will loop forever, always in a select or poll
 * call.
 *
 * When select returns something to do, track down what thread gets to do
 * this I/O and post the event to it.
 */
void *BCSocketMgr::_WatcherThreadFunc(void *uap)
{
	BCSocketMgr *manager = (BCSocketMgr *)uap;
	BOOL done;
	int ctlfd;
	int cc;
#ifdef USE_KQUEUE
	const char *fnname = "kevent()";
#elif defined (USE_EPOLL)
	const char *fnname = "epoll_wait()";
#elif defined(USE_DEVPOLL)
	const char *fnname = "ioctl(DP_POLL)";
	struct dvpoll dvp;
#elif defined (USE_SELECT)
	const char *fnname = "select()";
	int maxfd;
#endif
	char strbuf[BC_STRERRORSIZE];
#ifdef BC_SOCKET_USE_POLLWATCH
	pollstate_t pollstate = poll_idle;
#endif

	/*
	 * Get the control fd here.  This will never change.
	 */
	ctlfd = manager->m_sPipeFds[0];
	done = FALSE;
	while (!done)
	{
		do {
#ifdef USE_KQUEUE
			cc = kevent(manager->m_nKqueueFd, NULL, 0,
				    manager->m_pEvents, manager->m_nEvents, NULL);
#elif defined(USE_EPOLL)
			cc = epoll_wait(manager->m_nEpollFd, manager->m_pEvents,
					manager->m_nEvents, -1);
#elif defined(USE_DEVPOLL)
			dvp.dp_fds = manager->m_pEvents;
			dvp.dp_nfds = manager->m_nEvents;
#ifndef BC_SOCKET_USE_POLLWATCH
			dvp.dp_timeout = -1;
#else
			if (pollstate == poll_idle)
				dvp.dp_timeout = -1;
			else
				dvp.dp_timeout = BC_SOCKET_POLLWATCH_TIMEOUT;
#endif	/* BC_SOCKET_USE_POLLWATCH */
			cc = ioctl(manager->m_nDevpollFd, DP_POLL, &dvp);
#elif defined(USE_SELECT)
			LOCK(&manager->m_sLock);
			memcpy(manager->m_pReadFdsCopy, manager->m_pReadFds,
			       manager->m_nFdBufSize);
			memcpy(manager->m_pWriteFdsCopy, manager->m_pWriteFds,
			       manager->m_nFdBufSize);
			maxfd = manager->m_nMaxFd + 1;
			UNLOCK(&manager->m_sLock);

			cc = select(maxfd, manager->m_pReadFdsCopy,
				    manager->m_pWriteFdsCopy, NULL, NULL);
#endif	/* USE_KQUEUE */

			if (cc < 0 && !SOFT_ERROR(errno))
			{
				bc_strerror(errno, strbuf, sizeof(strbuf));
				LogFatal(_LOCAL_, "%s %s: %s", fnname, "failed", strbuf);
			}

#if defined(USE_DEVPOLL) && defined(BC_SOCKET_USE_POLLWATCH)
			if (cc == 0)
			{
				if (pollstate == poll_active)
					pollstate = poll_checking;
				else if (pollstate == poll_checking)
					pollstate = poll_idle;
			}
			else if (cc > 0)
			{
				if (pollstate == poll_checking)
				{
					/*
					 * XXX: We'd like to use a more
					 * verbose log level as it's actually an
					 * unexpected event, but the kernel bug
					 * reportedly happens pretty frequently
					 * (and it can also be a false positive)
					 * so it would be just too noisy.
					 */
					LogError(_LOCAL_, "unexpected POLL timeout");
				}
				pollstate = poll_active;
			}
#endif
		} while (cc < 0);

#if defined(USE_KQUEUE) || defined (USE_EPOLL) || defined (USE_DEVPOLL)
		done = manager->_ProcessFDs(manager->m_pEvents, cc);
#elif defined(USE_SELECT)
		manager->_ProcessFDs(maxfd, manager->m_pReadFdsCopy,
			    manager->m_pWriteFdsCopy);

		/*
		 * Process reads on internal, control fd.
		 */
		if (FD_ISSET(ctlfd, manager->m_pReadFdsCopy))
			done = manager->_ProcessCtlfd();
#endif
	}

	LogInfo(_LOCAL_, "%s", "watcher exiting");

	return ((void *)0);
}
#endif /* USE_WATCHER_THREAD */

/*
 * Create a new socket manager.
 */

BCRESULT BCSocketMgr::_SetupWatcher()
{
	BCRESULT result;
#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_DEVPOLL)
	char strbuf[BC_STRERRORSIZE];
#endif

#ifdef USE_KQUEUE
	m_nEvents = BC_SOCKET_MAXEVENTS;
	m_pEvents = (struct kevent *)m_sPool.Calloc(
					sizeof(struct kevent) * m_nEvents);
	if (m_pEvents == NULL)
		return (BC_R_NOMEMORY);
	m_nKqueueFd = kqueue();
	if (m_nKqueueFd == -1)
	{
		result = bc_errno2result(errno);
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "kqueue %s: %s", "failed", strbuf);
		return (result);
	}

#ifdef USE_WATCHER_THREAD
	result = _WatchFD(m_sPipeFds[0], SELECT_POKE_READ);
	if (result != BC_R_SUCCESS)
	{
		close(m_nKqueueFd);
		return (result);
	}
#endif	/* USE_WATCHER_THREAD */
#elif defined(USE_EPOLL)
	m_nEvents = BC_SOCKET_MAXEVENTS;
	m_pEvents = (struct epoll_event *)m_sPool.Calloc(
		sizeof(struct epoll_event) * m_nEvents);
	if (m_pEvents == NULL)
		return (BC_R_NOMEMORY);
	m_nEpollFd = epoll_create(m_nEvents);
	if (m_nEpollFd == -1)
	{
		result = bc_errno2result(errno);
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "epoll_create %s: %s", "failed", strbuf);
		return (result);
	}
#ifdef USE_WATCHER_THREAD
	result = _WatchFD(m_sPipeFds[0], SELECT_POKE_READ);
	if (result != BC_R_SUCCESS)
	{
		close(m_nEpollFd);
		return (result);
	}
#endif	/* USE_WATCHER_THREAD */
#elif defined(USE_DEVPOLL)
	/*
	 * XXXJT: /dev/poll seems to reject large numbers of events,
	 * so we should be careful about redefining BC_SOCKET_MAXEVENTS.
	 */
	m_nEvents = BC_SOCKET_MAXEVENTS;
	m_pEvents = m_sPool.Calloc(
			sizeof(struct pollfd) * m_nEvents);
	if (m_pEvents == NULL)
		return (BC_R_NOMEMORY);
	m_nDevpollFd = open("/dev/poll", O_RDWR);
	if (m_nDevpollFd == -1)
	{
		result = bc_errno2result(errno);
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_, "open(/dev/poll) %s: %s", "failed", strbuf);
		return (result);
	}
#ifdef USE_WATCHER_THREAD
	result = _WatchFD(m_sPipeFds[0], SELECT_POKE_READ);
	if (result != BC_R_SUCCESS)
	{
		close(m_nDevpollFd);
		return (result);
	}
#endif	/* USE_WATCHER_THREAD */
#elif defined(USE_SELECT)
	UNUSED(result);

#if BC_SOCKET_MAXSOCKETS > FD_SETSIZE
	/*
	 * Note: this code should also cover the case of MAXSOCKETS <=
	 * FD_SETSIZE, but we separate the cases to avoid possible portability
	 * issues regarding howmany() and the actual representation of fd_set.
	 */
	m_nFdBufSize = howmany(m_nMaxSocks, NFDBITS) *
		sizeof(fd_mask);
#else
	m_nFdBufSize = sizeof(fd_set);
#endif // BC_SOCKET_MAXSOCKETS > FD_SETSIZE

	m_pReadFds = NULL;
	m_pReadFdsCopy = NULL;
	m_pWriteFds = NULL;
	m_pWriteFdsCopy = NULL;

	m_pReadFds = (fd_set *)m_sPool.Alloc(m_nFdBufSize);
	if (m_pReadFds != NULL)
		m_pReadFdsCopy = (fd_set *)m_sPool.Alloc(m_nFdBufSize);
	if (m_pReadFdsCopy != NULL)
		m_pWriteFds = (fd_set *)m_sPool.Alloc(m_nFdBufSize);
	if (m_pWriteFds != NULL)
	{
		m_pWriteFdsCopy = (fd_set *)m_sPool.Alloc(m_nFdBufSize);
	}
	if (m_pWriteFdsCopy == NULL)
	{
		if (m_pWriteFds != NULL)
		{
			//
		}
		if (m_pReadFdsCopy != NULL)
		{
			//
		}
		if (m_pReadFds != NULL)
		{
			//
		}
		return (BC_R_NOMEMORY);
	}
	memset(m_pReadFds, 0, m_nFdBufSize);
	memset(m_pWriteFds, 0, m_nFdBufSize);

#ifdef USE_WATCHER_THREAD
	(void)_WatchFD(m_sPipeFds[0], SELECT_POKE_READ);
	m_nMaxFd = m_sPipeFds[0];
#else /* USE_WATCHER_THREAD */
	m_nMaxFd = 0;
#endif /* USE_WATCHER_THREAD */
#endif	/* USE_KQUEUE */

	return (BC_R_SUCCESS);
}

void BCSocketMgr::_CleanupWatcher()
{
#ifdef USE_WATCHER_THREAD
	BCRESULT result;

	result = _UnwatchFD(m_sPipeFds[0], SELECT_POKE_READ);
	if (result != BC_R_SUCCESS)
	{
		LogError(_LOCAL_,  "epoll_ctl(DEL) %s", "failed");
	}
#endif	/* USE_WATCHER_THREAD */

#ifdef USE_KQUEUE
	close(m_nKqueueFd);
#elif defined(USE_EPOLL)
	close(m_nEpollFd);
#elif defined(USE_DEVPOLL)
	close(m_nDevpollFd);
#elif defined(USE_SELECT)
#endif	/* USE_KQUEUE */
	m_sPool.Clear();
}

#ifndef USE_WATCHER_THREAD
/*
 * In our assumed scenario, we can simply use a single static object.
 * XXX: this is not true if the application uses multiple threads with
 *      'multi-context' mode.  Fixing this is a future TODO item.
 */
static BCSocketWaitType swait_private;

int BCSocketMgr::_WaitEvents(
	BCSocketMgr *pMgr,
	struct timeval *tvp,
	BCSocketWaitType **swaitp)
{
	BCSocketMgr *manager = (BCSocketMgr *)pMgr;
	int n;
#ifdef USE_KQUEUE
	struct timespec ts, *tsp;
#endif
#ifdef USE_EPOLL
	int timeout;
#endif
#ifdef USE_DEVPOLL
	struct dvpoll dvp;
#endif

	REQUIRE(swaitp != NULL && *swaitp == NULL);

#ifdef USE_SHARED_MANAGER
	if (manager == NULL)
		manager = socketmgr;
#endif
	if (manager == NULL)
		return (0);

#ifdef USE_KQUEUE
	if (tvp != NULL) {
		ts.tv_sec = tvp->tv_sec;
		ts.tv_nsec = tvp->tv_usec * 1000;
		tsp = &ts;
	} else
		tsp = NULL;
	swait_private.nevents = kevent(manager->m_nKqueueFd, NULL, 0,
				       manager->m_pEvents, manager->m_nEvents, tsp);
	n = swait_private.nevents;
#elif defined(USE_EPOLL)
	if (tvp != NULL)
		timeout = tvp->tv_sec * 1000 + (tvp->tv_usec + 999) / 1000;
	else
		timeout = -1;
	swait_private.nevents = epoll_wait(manager->m_nEpollFd,
					   manager->m_pEvents,
					   manager->m_nEvents, timeout);
	n = swait_private.nevents;
#elif defined(USE_DEVPOLL)
	dvp.dp_fds = manager->m_pEvents;
	dvp.dp_nfds = manager->m_nEvents;
	if (tvp != NULL) {
		dvp.dp_timeout = tvp->tv_sec * 1000 +
			(tvp->tv_usec + 999) / 1000;
	} else
		dvp.dp_timeout = -1;
	swait_private.nevents = ioctl(manager->m_nDevpollFd, DP_POLL, &dvp);
	n = swait_private.nevents;
#elif defined(USE_SELECT)
	memcpy(manager->m_pReadFdsCopy, manager->m_pReadFds,  manager->m_nFdBufSize);
	memcpy(manager->m_pWriteFdsCopy, manager->m_pWriteFds,
	       manager->m_nFdBufSize);

	swait_private.readset = manager->m_pReadFdsCopy;
	swait_private.writeset = manager->m_pWriteFdsCopy;
	swait_private.maxfd = manager->m_nMaxFd + 1;

	n = select(swait_private.maxfd, swait_private.readset,
		   swait_private.writeset, NULL, tvp);
#endif

	*swaitp = &swait_private;
	return (n);
}

BCRESULT BCSocketMgr::_Dispatch(
    BCSocketMgr *pMgr,
    BCSocketWaitType *swait)
{
	BCSocketMgr *manager = (BCSocketMgr *)pMgr;

	REQUIRE(swait == &swait_private);

#ifdef USE_SHARED_MANAGER
	if (manager == NULL)
		manager = socketmgr;
#endif
	if (manager == NULL)
		return (BC_R_NOTFOUND);

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_DEVPOLL)
	(void)manager->_ProcessFDs(manager->m_pEvents, swait->nevents);
	return (BC_R_SUCCESS);
#elif defined(USE_SELECT)
	manager->_ProcessFDs(swait->maxfd, swait->readset, swait->writeset);
	return (BC_R_SUCCESS);
#endif
}
#endif /* USE_WATCHER_THREAD */


///////////////////////////////////////////////////////////////////////////////
// Socket unitilities :
///////////////////////////////////////////////////////////////////////////////

void bc_socket_cleanunix(BCSockAddrS *sockaddr, BOOL active)
{
#ifdef BC_PLATFORM_HAVESYSUNH
	int s;
	struct stat sb;
	char strbuf[BC_STRERRORSIZE];

	if (sockaddr->type.sa.sa_family != AF_UNIX)
		return;

#ifndef S_ISSOCK
#if defined(S_IFMT) && defined(S_IFSOCK)
#define S_ISSOCK(mode) ((mode & S_IFMT)==S_IFSOCK)
#elif defined(_S_IFMT) && defined(S_IFSOCK)
#define S_ISSOCK(mode) ((mode & _S_IFMT)==S_IFSOCK)
#endif
#endif

#ifndef S_ISFIFO
#if defined(S_IFMT) && defined(S_IFIFO)
#define S_ISFIFO(mode) ((mode & S_IFMT)==S_IFIFO)
#elif defined(_S_IFMT) && defined(S_IFIFO)
#define S_ISFIFO(mode) ((mode & _S_IFMT)==S_IFIFO)
#endif
#endif

#if !defined(S_ISFIFO) && !defined(S_ISSOCK)
#error You need to define S_ISFIFO and S_ISSOCK as appropriate for your platform.  See <sys/stat.h>.
#endif

#ifndef S_ISFIFO
#define S_ISFIFO(mode) 0
#endif

#ifndef S_ISSOCK
#define S_ISSOCK(mode) 0
#endif

	if (active)
	{
		if (stat(sockaddr->type.sunix.sun_path, &sb) < 0)
		{
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
				      "bc_socket_cleanunix: stat(%s): %s",
				      sockaddr->type.sunix.sun_path, strbuf);
			return;
		}
		if (!(S_ISSOCK(sb.st_mode) || S_ISFIFO(sb.st_mode)))
		{
			LogError(_LOCAL_,
				      "bc_socket_cleanunix: %s: not a socket",
				      sockaddr->type.sunix.sun_path);
			return;
		}
		if (unlink(sockaddr->type.sunix.sun_path) < 0)
		{
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
				      "bc_socket_cleanunix: unlink(%s): %s",
				      sockaddr->type.sunix.sun_path, strbuf);
		}
		return;
	}

	s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
			      "bc_socket_cleanunix: socket(%s): %s",
			      sockaddr->type.sunix.sun_path, strbuf);
		return;
	}

	if (stat(sockaddr->type.sunix.sun_path, &sb) < 0)
	{
		switch (errno)
		{
		case ENOENT:    /* We exited cleanly last time */
			break;
		default:
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
				      "bc_socket_cleanunix: stat(%s): %s",
				      sockaddr->type.sunix.sun_path, strbuf);
			break;
		}
		goto cleanup;
	}

	if (!(S_ISSOCK(sb.st_mode) || S_ISFIFO(sb.st_mode)))
	{
		LogError(_LOCAL_,
			      "bc_socket_cleanunix: %s: not a socket",
			      sockaddr->type.sunix.sun_path);
		goto cleanup;
	}

	if (connect(s, (struct sockaddr *)&sockaddr->type.sunix,
		    sizeof(sockaddr->type.sunix)) < 0)
	{
		switch (errno)
		{
		case ECONNREFUSED:
		case ECONNRESET:
			if (unlink(sockaddr->type.sunix.sun_path) < 0)
			{
				bc_strerror(errno, strbuf, sizeof(strbuf));
				LogError(_LOCAL_,
					      "bc_socket_cleanunix: "
					      "unlink(%s): %s",
					      sockaddr->type.sunix.sun_path,
					      strbuf);
			}
			break;
		default:
			bc_strerror(errno, strbuf, sizeof(strbuf));
			LogError(_LOCAL_,
				      "bc_socket_cleanunix: connect(%s): %s",
				      sockaddr->type.sunix.sun_path, strbuf);
			break;
		}
	}
 cleanup:
	close(s);
#else
	UNUSED(sockaddr);
	UNUSED(active);
#endif
}

BCRESULT bc_socket_permunix(
	BCSockAddrS *sockaddr,
	uint32_t perm,
	uint32_t owner,
	uint32_t group)
{
#ifdef BC_PLATFORM_HAVESYSUNH
	BCRESULT result = BC_R_SUCCESS;
	char strbuf[BC_STRERRORSIZE];
	char path[sizeof(sockaddr->type.sunix.sun_path)];
#ifdef NEED_SECURE_DIRECTORY
	char *slash;
#endif

	REQUIRE(sockaddr->type.sa.sa_family == AF_UNIX);
	INSIST(strlen(sockaddr->type.sunix.sun_path) < sizeof(path));
	strcpy(path, sockaddr->type.sunix.sun_path);

#ifdef NEED_SECURE_DIRECTORY
	slash = strrchr(path, '/');
	if (slash != NULL)
	{
		if (slash != path)
			*slash = '\0';
		else
			strcpy(path, "/");
	} else
		strcpy(path, ".");
#endif

	if (chmod(path, perm) < 0)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
			      "bc_socket_permunix: chmod(%s, %d): %s",
			      path, perm, strbuf);
		result = BC_R_FAILURE;
	}
	if (chown(path, owner, group) < 0)
	{
		bc_strerror(errno, strbuf, sizeof(strbuf));
		LogError(_LOCAL_,
			      "bc_socket_permunix: chown(%s, %d, %d): %s",
			      path, owner, group,
			      strbuf);
		result = BC_R_FAILURE;
	}
	return (result);
#else
	UNUSED(sockaddr);
	UNUSED(perm);
	UNUSED(owner);
	UNUSED(group);
	return (BC_R_NOTIMPLEMENTED);
#endif
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
