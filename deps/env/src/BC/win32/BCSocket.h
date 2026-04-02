
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
#include "BC/BCFixedAlloc.h"

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

/*
 * Define a maximum number of I/O Completion Port worker threads
 * to handle the load on the Completion Port. The actual number
 * used is the number of CPU's + 1.
 */
#define MAX_IOCPTHREADS 20

#define BCSOCKET_RECVBUF_SIZE			65536


class BCSocket;
class BCSocketMgr;
class SockIoCompletionInfo;
class BCThread;
class BCBuffer;

struct msghdr;

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
	uint64_t				timestamp;	/*%< timestamp of packet recv */
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

	BCRESULT		Create(
						BCSocketMgr *pMgr,
						int nPf,
						BCSocketTypeE eType);
	void			Attach(BCSocket **ppSocket);
	void			Detach(BCSocket **ppSocket);
	BCRESULT		Recv(
						BCRegionS *pRegion,
						uint32_t minimum,
						BCTask *task,
						LPFN_BCTaskAction action,
						const void *arg);
	BCRESULT		Recv2(
						BCRegionS *pRegion,
						uint32_t minimum,
						BCTask *task,
						BCSockEvent *pEvent,
						uint32_t nFlags);
	BCRESULT		RecvV(
						BCBuffer *buflist,
						uint32_t minimum,
						BCTask *task,
						LPFN_BCTaskAction action,
						const void *arg);
	BCRESULT		RecvV2(
						BCBuffer *buflist,
						uint32_t minimum,
						BCTask *task,
						BCSockEvent *pEvent,
						uint32_t nFlags);
	BCRESULT		Send(
						BCRegionS *region,
						uint32_t region_size,
						BCTask *task,
						LPFN_BCTaskAction action,
						const void *arg);
	BCRESULT		SendTo(
						BCRegionS *region,
						uint32_t region_size,
						BCTask *task,
						LPFN_BCTaskAction action,
						const void *arg,
						BCSockAddrS *pAddress,
						struct in6_pktinfo *pktinfo);
	BCRESULT		SendTo2(
						BCRegionS *region,
						uint32_t region_size,
						BCTask *task,
						BCSockAddrS *address,
						struct in6_pktinfo *pktinfo,
						BCSockEvent *pEvent,
						uint32_t flags);
	BCRESULT		SendV(
						BCBuffer *buflist,
						BCTask *task,
						LPFN_BCTaskAction action,
						const void *arg);
	BCRESULT		SendToV(
						BCBuffer *buflist,
						BCTask *task,
						LPFN_BCTaskAction action,
						const void *arg,
						BCSockAddrS *address,
						struct in6_pktinfo *pktinfo);
	BCRESULT		SendToV2(
						BCBuffer *buflist,
						BCTask *task,
						BCSockAddrS *address,
						struct in6_pktinfo *pktinfo,
						BCSockEvent *pEvent,
						uint32_t flags);
	BCRESULT		Bind(
						BCSockAddrS *sockaddr,
						uint32_t options);
	BCRESULT		Filter(const char *szFilter);
	BCRESULT		Listen(uint32_t backlog);
	BCRESULT		Accept(
						BCTask *task,
						LPFN_BCTaskAction action,
						const void *arg);
	BCRESULT		Connect(
						BCSockAddrS *addr,
						BCTask *task,
						LPFN_BCTaskAction action,
						const void *arg);
	BCRESULT		GetPeerName(BCSockAddrS *addressp);
	BCRESULT		GetSockName(BCSockAddrS *addressp);
	void			Cancel(BCTask *task, uint32_t how);
	BCSocketTypeE	GetType();
	BOOL			IsBound();
	void			IPv6only(BOOL yes);
	void			SetName(const char *szName, void *pTag);
	const char	*	GetName();
	void		*	GetTag();
	BCSocketMgr	*	GetManager() const;
	uint32_t		GetRef() const
	{
		return m_nRef;
	}
public:
#ifdef _DEBUG
	static ULONGLONG	GetAllocCount();
	uint32_t			GetSendEventCount() const
	{
		return m_lstSendEvents.Count();
	}
#endif

protected:
	inline void _SetState(uint32_t eState, uint32_t nLineNumber)
	{
		m_eState			= eState;
		m_nStateLineNO	= nLineNumber;
	}
	void			_IocpUpdate();
	void			_Close();
	int32_t			_InternalSendMsg(
						SockIoCompletionInfo *lpo,
						struct msghdr *messagehdr,
						int flags,
						int *Error);
	BOOL			_IsSendDoneActive(BCSockEvent *dev);
	BOOL			_IsAcceptDoneActive(BCSockICEvent *dev);
	BOOL			_IsConnectDoneActive(BCSockOCEvent *dev);
	void			_SendRecvDoneEvent(BCSockEvent **dev);
	void			_SendSendDoneEvent(BCSockEvent **dev);
	void			_SendAcceptDoneEvent(BCSockICEvent **adev);
	void			_SendConnectDoneEvent(BCSockOCEvent **cdev);
	void			_SendRecvDoneAbort(BCRESULT result);
	void			_QueueReceiveEvent(BCTask *task, BCSockEvent *dev);
	void			_QueueReceiveRequest();
	void			_HardRecoverReceiveRequest();
	void			_RecoverReceiveRequest(SockIoCompletionInfo **lplpo);
	void			_BuildMsghdrSend(
						BCSockEvent *dev,
						struct msghdr *msg,
						char *cmsg,
						WSABUF *iov,
						SockIoCompletionInfo  *lpo);
	int32_t			_MapError(
						int32_t windows_errno,
						int32_t *bc_errno,
						char *errorstring,
						size_t bufsize);
	void			_FillRecv(BCSockEvent *dev);
	void			_CompleteIoRecv();
	int32_t			_CompleteIoSend(
						BCSockEvent *dev,
						struct msghdr *messagehdr,
						int32_t cc,
						int32_t send_errno);
	int32_t			_StartIoSend(
						BCSockEvent *dev,
						int32_t *nbytes,
						int32_t *send_errno);
	void			_InternalAccept(
						SockIoCompletionInfo *lpo,
						int32_t accept_errno);
	void			_InternalConnect(
						SockIoCompletionInfo *lpo,
						int32_t connect_errno);
	void			_InternalRecv(int32_t nbytes);
	void			_InternalSend(
						BCSockEvent *dev,
						struct msghdr *messagehdr,
						int32_t nbytes,
						int32_t send_errno,
						SockIoCompletionInfo *lpo);
	BCRESULT		_Recv(
						BCSockEvent *dev,
						BCTask *task,
						uint32_t flags);
	BCRESULT		_Send(
						BCSockEvent *dev,
						BCTask *task,
						BCSockAddrS *pAddress,
						struct in6_pktinfo *pktinfo,
						uint32_t flags);

	virtual void	_Destroy();
protected:
	static void		_MaybeFree(BCSocket *pSocket, int32_t lineno);
	static void		_MaybeFree(BCSocket **ppSocket, int32_t lineno);
	static void		_Free(BCSocket *pSocket, int32_t lineno);
	static void		_Free(BCSocket **ppSocket, int32_t lineno);
#ifdef BC_SOCKET_CONSISTENCY_CHECKS
	void			_Consistent();
	void			_Dump();
#endif
private:
	DECLARE_NO_COPY_CLASS(BCSocket);
	/* Not locked. */
	BCSocketMgr		*	m_pMgr;
	BCSpinMutex			m_sLock;
	BCSocketTypeE		m_eType;

	/* Pointers to scatter/gather buffers */
	WSABUF				m_sIoVector[BC_SOCKET_MAXSCATTERGATHER];

	/* Locked by socket lock. */
	uint32_t			m_nRef; /* EXTERNAL references */
	SOCKET				m_nFD;	/* file handle */
	int					m_nPF;	/* protocol family */
	char				m_szName[16];
	void			*	m_pTag;

	/*
	 * Each recv() call uses this buffer.  It is a per-socket receive
	 * buffer that allows us to decouple the system recv() from the
	 * recv_list done events.  This means the items on the recv_list
	 * can be removed without having to cancel pending system recv()
	 * calls.  It also allows us to read-ahead in some cases.
	 */
	struct RecvBufferS{
		SOCKADDR_STORAGE	from_addr;			// UDP send/recv address
		int					from_addr_len;		// length of the address
		char			*	base;				// the base of the buffer
		char			*	consume_position;	// where to start copying data from next
		unsigned int		len;				// the actual size of this buffer
		unsigned int		remaining;			// the number of bytes remaining
	}						m_sRecvBuf;

	BCSockEventList			m_lstSendEvents;
	BCSockEventList			m_lstRecvEvents;
	BCSockICEventList		m_lstAcceptEvents;
	BCSockOCEvent		*	m_pConnectEvent;

	BCSockAddrS				m_sAddress;  /* remote address */

	uint32_t				m_bListener       : 1, /* listener socket */
							m_bConnected      : 1,
							m_bPendingConnect : 1, /* connect pending */
							m_bBound          : 1; /* bound to local addr */
	uint32_t	m_nPendingIocp;	/* Should equal the counters below. Debug. */
	uint32_t	m_nPendingRecv;	/* Number of outstanding recv() calls. */
	uint32_t	m_nPendingSend;	/* Number of outstanding send() calls. */
	uint32_t	m_nPendingAccept; /* Number of outstanding accept() calls. */
	uint32_t	m_eState; /* Socket state. Debugging and consistency checking. */
	int32_t		m_nStateLineNO;	/* line which last touched state */
	int32_t		m_nInRecoveryCount; /* avoid recovery loop. */

	// extra
	char		m_szBuffer[BCSOCKET_RECVBUF_SIZE];
};

typedef TNodeList<BCSocket>			BCSocketList;

///////////////////////////////////////////////////////////////////////////////
// class : BCSocketMgr
///////////////////////////////////////////////////////////////////////////////

class BC_API BCSocketMgr : public BCMagic
{
	DECLARE_FIXED_ALLOC(BCSocketMgr);

	friend class BCSocket;
public:
	BCSocketMgr();
	virtual ~BCSocketMgr();

	BCRESULT		Create(uint32_t nMaxSocks = 0);
	static void		Destroy(BCSocketMgr **ppMgr);

	uint32_t		GetSocketCount() const;
protected:
	virtual void	_Destroy();

	void			_SignalIocpExit();
	void			_IocpCreateThreads(int32_t nThreads);
	void			_IocpInitialize();
	static void *	_SocketIoThread(LPVOID ThreadContext);
private:
	DECLARE_NO_COPY_CLASS(BCSocketMgr);
	/* Not locked. */
	BCMutex				m_sLock;

	/* Locked by manager lock. */
	BCSocketList		m_lstSockets;
	BOOL				m_bShutdown;
	BCCondition			m_sCondShutdownOK;
	HANDLE				m_hIoCompletionPort;
	int32_t				m_nMaxIocpThreads;
	BCThread		*	m_hIocpThreads[MAX_IOCPTHREADS];

	/*
	 * Debugging.
	 * Modified by InterlockedIncrement() and InterlockedDecrement()
	 */
	LONG				m_nTotalSockets;
	LONG				m_nIocpTotal;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////