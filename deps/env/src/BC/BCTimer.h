#ifndef BC_TIMER_H__
#define BC_TIMER_H__

#include <BC/Exports.h>
#include <BC/BCThread.h>
#include <BC/BCNodeList.h>
#include <BC/Utils.h>
#include <BC/BCTaskEvent.h>
#include <BC/BCHeap.h>
#include <BC/BCTime.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

// Some platforms (e.g. Windows) include max() and min() macros in their
// standard headers, but they are also standard C++ template functions, so some
// C++ headers will undefine them.  So we steer clear of the names min and max
// and define __bcmin and __bcmax instead.

#ifndef __bcmax
#define __bcmax(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef __bcmin
#define __bcmin(a,b) (((a) < (b)) ? (a) : (b))
#endif


class BCTimerMgr;

///////////////////////////////////////////////////////////////////////////////
// timer
///////////////////////////////////////////////////////////////////////////////

/***
 *** Types
 ***/

/*% Timer Type */
typedef enum BCTimerTypeE
{
	bc_timertype_ticker		= 0, 	/*%< Ticker */
	bc_timertype_once		= 1, 	/*%< Once */
	bc_timertype_limited	= 2, 	/*%< Limited */
	bc_timertype_inactive	= 3 	/*%< Inactive */
} BCTimerTypeE;

#define BC_TIMEREVENT_FIRSTEVENT	(BC_EVENTCLASS_TIMER + 0)
#define BC_TIMEREVENT_TICK			(BC_EVENTCLASS_TIMER + 1)
#define BC_TIMEREVENT_IDLE			(BC_EVENTCLASS_TIMER + 2)
#define BC_TIMEREVENT_LIFE			(BC_EVENTCLASS_TIMER + 3)
#define BC_TIMEREVENT_LASTEVENT		(BC_EVENTCLASS_TIMER + 65535)

#define BCAPI_TIMERMGR_MAGIC		BC_MAGIC('A','t','m','g')
#define BCAPI_TIMERMGR_VALID(m)	((m) != NULL && \
			(m)->m_nMagic == BCAPI_TIMERMGR_MAGIC)


///////////////////////////////////////////////////////////////////////////////
// class : BCTimerEvent - Note : Create instance with 'new' operator
///////////////////////////////////////////////////////////////////////////////

class BC_API BCTimerEvent : public BCTaskEvent
{
	friend class BCTimer;
	friend class BCTimerMgr;
public:
	BCTimerEvent();
	BCTimerEvent(
		void *sender,
		BCEventType type,
		LPFN_BCTaskAction action,
		const void *arg);
	virtual ~BCTimerEvent();

	void	Destroy();
protected:
private:
	DECLARE_NO_COPY_CLASS(BCTimerEvent);

public:
	BCTimeS				due;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCTimer2
//
//         Note : 1. You MUST create instance with 'new' operator;
//                2. Tick timer interval with longer than 30 milliseconds
//                   will has better performance. Average error(+ -10millisec)
///////////////////////////////////////////////////////////////////////////////

class BC_API BCTimer
	: public BCNodeList::Node
	, public BCMagic
{
	friend class BCTimerMgr;
public:
	BCTimer();
	virtual ~BCTimer();

	BCRESULT		Create(
						BCTimerMgr *pMgr,
						BCTimerTypeE eType,
						BCTimeS *pExpires,
						BCIntervalS *pInterval,
						BCTask *pTask,
						LPFN_BCTaskAction lpfnAction,
						const void *pArg);
	BCRESULT		Reset(
						BCTimerTypeE type,
						BCTimeS *expires,
						BCIntervalS *interval,
						BOOL purge);
	BCTimerTypeE	GetType();
	BCRESULT		Touch();
	void			Attach(BCTimer **timerp);
	void			Detach(BCTimer **timerp);

protected:
	BCRESULT		_Schedule(BCTimeS *now, BOOL signal_ok);
	void			_Deschedule();
	void			_Destroy();
private:
	DECLARE_NO_COPY_CLASS(BCTimer);
	/*! Not locked. */
	BCTimerMgr		*	m_pMgr;
	BCSpinMutex			m_sLock;
	/*! Locked by timer lock. */
	uint32_t			m_nRef;
	BCTimeS				m_sIdle;
	/*! Locked by manager lock. */
	BCTimerTypeE		m_eType;
	BCTimeS				m_sExpires;
	BCIntervalS			m_sInterval;
	BCTask			*	m_pTask;
	LPFN_BCTaskAction	m_lpfnAction;
	void			*	m_pArg;
	uint32_t			m_nIndex;
	BCTimeS				m_sDue;
};

typedef TNodeList<BCTimer>		BCTimerList;

///////////////////////////////////////////////////////////////////////////////
// class : BCTimerMgr
///////////////////////////////////////////////////////////////////////////////

class BC_API BCTimerMgr : public BCMagic
{
	friend class BCTimer;
public:
	BCTimerMgr();
	virtual ~BCTimerMgr();

	static BCRESULT	Create(BCTimerMgr **ppMgr);
	void			Poke();
	static void		Destroy(BCTimerMgr **ppMgr);
protected:
	void			_Dispatch(BCTimeS *now);
	static void	*	_Run(LPVOID pArg);
	static BOOL		_Sooner(void *t1, void *t2);
	static void		_SetIndex(void *what, uint32_t index);
private:
	DECLARE_NO_COPY_CLASS(BCTimerMgr);
	/* Not locked. */
	BCMutex					m_sLock;
	/* Locked by manager lock. */
	BOOL					m_bDone;
	BCTimerList				m_lstTimers;
	uint32_t				m_nScheduled;
	BCTimeS					m_sDue;
	BCCondition				m_sCondWakeup;
	BCThread			*	m_pThread;
	BCHeap					m_sHeap;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

};//End of namespace BC

#endif //BC_TIMER_H__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
