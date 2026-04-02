
// -=- BCTimer.cpp

#include <BC/BCException.h>
#include <BC/BCTask.h>
#include <BC/BCLog.h>
#include <BC/BCTimer.h>



///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// timer
///////////////////////////////////////////////////////////////////////////////

#define TIMER_MAGIC				BC_MAGIC('T', 'I', 'M', 'R')
#define VALID_TIMER(t)			BC_MAGIC_VALID(t, TIMER_MAGIC)

#define TIMER_MANAGER_MAGIC		BC_MAGIC('T', 'I', 'M', 'M')
#define VALID_MANAGER(m)		BC_MAGIC_VALID(m, TIMER_MANAGER_MAGIC)

/*%
 * Time
 */
#define TIME_NOW(tp) 	BC_RUNTIME_CHECK(bc_time_now((tp)) == BC_R_SUCCESS)

///////////////////////////////////////////////////////////////////////////////
// class : BCTimerEvent
///////////////////////////////////////////////////////////////////////////////

BCTimerEvent::BCTimerEvent()
	: BCTaskEvent()
{
	memzero(&due, sizeof(due));
}

BCTimerEvent::BCTimerEvent(
	void *sender,
	BCEventType type,
	LPFN_BCTaskAction action,
	const void *arg)
		: BCTaskEvent(sender, type, action, arg)
{
	memzero(&due, sizeof(due));
}

BCTimerEvent::~BCTimerEvent()
{
	//
}

void BCTimerEvent::Destroy()
{
	delete this;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCTimer2
///////////////////////////////////////////////////////////////////////////////

BCTimer::BCTimer()
	: BCMagic(TIMER_MAGIC)
	, m_pMgr(NULL)
	, m_nRef(0)
	, m_eType(bc_timertype_inactive)
	, m_pTask(NULL)
	, m_lpfnAction(NULL)
	, m_pArg(NULL)
	, m_nIndex(0)
{
	bc_time_settoepoch(&m_sIdle);
	bc_time_settoepoch(&m_sExpires);
	bc_interval_set(&m_sInterval, 0, 0);
	bc_time_settoepoch(&m_sDue);
}

BCTimer::~BCTimer()
{
	//
}

BCRESULT BCTimer::_Schedule(BCTimeS *now, BOOL signal_ok)
{
	BCRESULT result;
	BCTimeS due;
	int cmp;
	BOOL timedwait;

	/*!
	 * Note: the caller must ensure locking.
	 */

	ASSERT(m_eType != bc_timertype_inactive);

	/*!
	 * If the manager was timed wait, we may need to signal the
	 * manager to force a wakeup.
	 */
	timedwait = (m_pMgr->m_nScheduled > 0 &&
			     bc_time_seconds(&m_pMgr->m_sDue) != 0);

	/*
	 * Compute the new due time.
	 */
	if (m_eType != bc_timertype_once)
	{
		result = bc_time_add(now, &m_sInterval, &due);
		if (result != BC_R_SUCCESS)
			return (result);
		if (m_eType == bc_timertype_limited &&
		    bc_time_compare(&m_sExpires, &due) < 0)
			due = m_sExpires;
	}
	else
	{
		if (bc_time_isepoch(&m_sIdle))
			due = m_sExpires;
		else if (bc_time_isepoch(&m_sExpires))
			due = m_sIdle;
		else if (bc_time_compare(&m_sIdle, &m_sExpires) < 0)
			due = m_sIdle;
		else
			due = m_sExpires;
	}

	/*
	 * Schedule the timer.
	 */

	if (m_nIndex > 0)
	{
		/*
		 * Already scheduled.
		 */
		cmp = bc_time_compare(&due, &m_sDue);
		m_sDue = due;
		switch (cmp)
		{
		case -1:
			m_pMgr->m_sHeap.Increased(m_nIndex);
			break;
		case 1:
			m_pMgr->m_sHeap.Decreased(m_nIndex);
			break;
		case 0:
			/* Nothing to do. */
			break;
		}
	}
	else
	{
		m_sDue = due;
		result = m_pMgr->m_sHeap.Insert(this);
		if (result != BC_R_SUCCESS)
		{
			ASSERT(result == BC_R_NOMEMORY);
			return (BC_R_NOMEMORY);
		}
		m_pMgr->m_nScheduled++;
	}

	/*
	 * If this timer is at the head of the queue, we need to ensure
	 * that we won't miss it if it has a more recent due time than
	 * the current "next" timer.  We do this either by waking up the
	 * run thread, or explicitly setting the value in the manager.
	 */

	/*
	 * This is a temporary (probably) hack to fix a bug on tru64 5.1
	 * and 5.1a.  Sometimes, pthread_cond_timedwait() doesn't actually
	 * return when the time expires, so here, we check to see if
	 * we're 15 seconds or more behind, and if we are, we signal
	 * the dispatcher.  This isn't such a bad idea as a general purpose
	 * watchdog, so perhaps we should just leave it in here.
	 */
	if (signal_ok && timedwait)
	{
		BCIntervalS fifteen;
		BCTimeS then;

		bc_interval_set(&fifteen, 15, 0);
		result = bc_time_add(&m_pMgr->m_sDue, &fifteen, &then);

		if (result == BC_R_SUCCESS &&
		    bc_time_compare(&then, now) < 0)
		{
			m_pMgr->m_sCondWakeup.Signal();
			signal_ok = FALSE;
		}
	}

	if (m_nIndex == 1 && signal_ok)
	{
		m_pMgr->m_sCondWakeup.Signal();
	}

	return (BC_R_SUCCESS);
}

void BCTimer::_Deschedule()
{
	BOOL need_wakeup = FALSE;

	/*
	 * The caller must ensure locking.
	 */

	if (m_nIndex > 0)
	{
		if (m_nIndex == 1)
			need_wakeup = TRUE;
		m_pMgr->m_sHeap.Delete(m_nIndex);
		m_nIndex = 0;
		ASSERT(m_pMgr->m_nScheduled > 0);
		m_pMgr->m_nScheduled--;

		if (need_wakeup)
		{
			m_pMgr->m_sCondWakeup.Signal();
		}
	}
}

BCRESULT BCTimer::Create(
	BCTimerMgr *pMgr,
	BCTimerTypeE eType,
	BCTimeS *pExpires,
	BCIntervalS *pInterval,
	BCTask *pTask,
	LPFN_BCTaskAction lpfnAction,
	const void *pArg)
{
	BCRESULT result;
	BCTimeS now;

	/*
	 * Create a new 'type' timer managed by 'manager'.  The timers
	 * parameters are specified by 'expires' and 'interval'.  Events
	 * will be posted to 'task' and when dispatched 'action' will be
	 * called with 'arg' as the arg value.  The new timer is returned
	 * in 'timerp'.
	 */

	ASSERT(VALID_MANAGER(pMgr));
	ASSERT(pTask != NULL);
	ASSERT(lpfnAction != NULL);
	if (pExpires == NULL)
		pExpires = bc_time_epoch;
	if (pInterval == NULL)
		pInterval = bc_interval_zero;
	ASSERT(eType == bc_timertype_inactive ||
		!(bc_time_isepoch(pExpires) && bc_interval_iszero(pInterval)));
	ASSERT(eType != bc_timertype_limited ||
		!(bc_time_isepoch(pExpires) || bc_interval_iszero(pInterval)));

	/*
	 * Get current time.
	 */
	if (eType != bc_timertype_inactive)
	{
		TIME_NOW(&now);
	}
	else
	{
		/*
		 * We don't have to do this, but it keeps the compiler from
		 * complaining about "now" possibly being used without being
		 * set, even though it will never actually happen.
		 */
		bc_time_settoepoch(&now);
	}

	m_pMgr = pMgr;
	m_nRef = 1;

	if (eType == bc_timertype_once && !bc_interval_iszero(pInterval))
	{
		result = bc_time_add(&now, pInterval, &m_sIdle);
		if (result != BC_R_SUCCESS)
		{
			return (result);
		}
	} else
		bc_time_settoepoch(&m_sIdle);

	m_eType = eType;
	m_sExpires = *pExpires;
	m_sInterval = *pInterval;
	m_pTask = NULL;
	pTask->Attach(&m_pTask);
	m_lpfnAction = lpfnAction;
	/*
	 * Removing the const attribute from "arg" is the best of two
	 * evils here.  If the timer->arg member is made const, then
	 * it affects a great many recipients of the timer event
	 * which did not pass in an "arg" that was truly const.
	 * Changing BCTimer::Create() to not have "arg" prototyped as const,
	 * though, can cause compilers warnings for calls that *do*
	 * have a truly const arg.  The caller will have to carefully
	 * keep track of whether arg started as a true const.
	 */
	DE_CONST(pArg, m_pArg);
	m_nIndex = 0;


	pMgr->m_sLock.Lock();

	/*
	 * Note we don't have to lock the timer like we normally would because
	 * there are no external references to it yet.
	 */

	if (eType != bc_timertype_inactive)
		result = _Schedule(&now, TRUE);
	else
		result = BC_R_SUCCESS;
	if (result == BC_R_SUCCESS)
		pMgr->m_lstTimers.PushBack(this);

	pMgr->m_sLock.Unlock();

	if (result != BC_R_SUCCESS)
	{
		pTask->Detach(&pTask);
		return (result);
	}

	return (BC_R_SUCCESS);
}

void BCTimer::_Destroy()
{
	/*
	 * The caller must ensure it is safe to destroy the timer.
	 */

	m_pMgr->m_sLock.Lock();

	(void)m_pTask->PurgeRange(
				  this,
				  BC_TIMEREVENT_FIRSTEVENT,
				  BC_TIMEREVENT_LASTEVENT,
				  NULL);
	_Deschedule();
	RemoveFromList();

	m_pMgr->m_sLock.Unlock();

	m_pTask->Detach(&m_pTask);

	delete this;
}

BCRESULT BCTimer::Reset(
	BCTimerTypeE type,
	BCTimeS *expires,
	BCIntervalS *interval,
	BOOL purge)
{
	BCTimeS now;
	BCRESULT result;

	/*
	 * Change the timer's type, expires, and interval values to the given
	 * values.  If 'purge' is BC_TRUE, any pending events from this timer
	 * are purged from its task's event queue.
	 */

	ASSERT(VALID_TIMER(this));
	ASSERT(VALID_MANAGER(m_pMgr));
	if (expires == NULL)
		expires = bc_time_epoch;
	if (interval == NULL)
		interval = bc_interval_zero;
	ASSERT(type == bc_timertype_inactive ||
		!(bc_time_isepoch(expires) && bc_interval_iszero(interval)));
	ASSERT(type != bc_timertype_limited ||
		!(bc_time_isepoch(expires) || bc_interval_iszero(interval)));

	/*
	 * Get current time.
	 */
	if (type != bc_timertype_inactive)
	{
		TIME_NOW(&now);
	}
	else
	{
		/*
		 * We don't have to do this, but it keeps the compiler from
		 * complaining about "now" possibly being used without being
		 * set, even though it will never actually happen.
		 */
		bc_time_settoepoch(&now);
	}

	m_pMgr->m_sLock.Lock();
	m_sLock.Lock();

	if (purge)
		(void)m_pTask->PurgeRange(
					  this,
					  BC_TIMEREVENT_FIRSTEVENT,
					  BC_TIMEREVENT_LASTEVENT,
					  NULL);
	m_eType = type;
	m_sExpires = *expires;
	m_sInterval = *interval;
	if (type == bc_timertype_once && !bc_interval_iszero(interval))
	{
		result = bc_time_add(&now, interval, &m_sIdle);
	}
	else
	{
		bc_time_settoepoch(&m_sIdle);
		result = BC_R_SUCCESS;
	}

	if (result == BC_R_SUCCESS)
	{
		if (type == bc_timertype_inactive)
		{
			_Deschedule();
			result = BC_R_SUCCESS;
		}
		else
		{
			result = _Schedule(&now, TRUE);
		}
	}

	m_sLock.Unlock();
	m_pMgr->m_sLock.Unlock();

	return (result);
}

BCTimerTypeE BCTimer::GetType()
{
	BCTimerTypeE t;

	ASSERT(VALID_TIMER(this));

	m_sLock.Lock();
	t = m_eType;
	m_sLock.Unlock();

	return (t);
}

BCRESULT BCTimer::Touch()
{
	BCRESULT result;
	BCTimeS now;

	/*
	 * Set the last-touched time of 'timer' to the current time.
	 */

	ASSERT(VALID_TIMER(this));

	m_sLock.Lock();

	/*
	 * We'd like to
	 *
	 *	REQUIRE(timer->type == bc_timertype_once);
	 *
	 * but we cannot without locking the manager lock too, which we
	 * don't want to do.
	 */

	TIME_NOW(&now);
	result = bc_time_add(&now, &m_sInterval, &m_sIdle);

	m_sLock.Unlock();

	return (result);
}

void BCTimer::Attach(BCTimer **timerp)
{
	/*
	 * Attach *timerp to timer.
	 */

	ASSERT(VALID_TIMER(this));
	ASSERT(timerp != NULL && *timerp == NULL);

	m_sLock.Lock();
	m_nRef++;
	m_sLock.Unlock();

	*timerp = this;
}

void BCTimer::Detach(BCTimer **timerp)
{
	BCTimer *timer;
	BOOL free_timer = FALSE;

	/*
	 * Detach *timerp from its timer.
	 */

	ASSERT(timerp != NULL);
	timer = (BCTimer *)*timerp;
	ASSERT(VALID_TIMER(timer));
	ASSERT(timer == this);

	m_sLock.Lock();
	ASSERT(m_nRef > 0);
	m_nRef--;
	if (m_nRef == 0)
		free_timer = TRUE;
	m_sLock.Unlock();

	if (free_timer)
		_Destroy();

	*timerp = NULL;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCTimerMgr
///////////////////////////////////////////////////////////////////////////////

BCTimerMgr::BCTimerMgr()
	: BCMagic(TIMER_MANAGER_MAGIC)
	, m_bDone(FALSE)
	, m_nScheduled(0)
	, m_sCondWakeup(&m_sLock)
	, m_pThread(NULL)
{
	bc_time_settoepoch(&m_sDue);
}

BCTimerMgr::~BCTimerMgr()
{
	//
}

void BCTimerMgr::_Dispatch(BCTimeS *now)
{
	BOOL done = FALSE, post_event, need_schedule;
	BCTimerEvent *pEvent;
	BCEventType type = 0;
	BCTimer *timer;
	BCRESULT result;
	BOOL idle;

	/*!
	 * The caller must be holding the manager lock.
	 */

	while (m_nScheduled > 0 && !done)
	{
		timer = (BCTimer *)m_sHeap.Element(1);
		ASSERT(timer->m_eType != bc_timertype_inactive);
		if (bc_time_compare(now, &timer->m_sDue) >= 0)
		{
			if (timer->m_eType == bc_timertype_ticker)
			{
				type = BC_TIMEREVENT_TICK;
				post_event = TRUE;
				need_schedule = TRUE;
			}
			else if (timer->m_eType == bc_timertype_limited)
			{
				int cmp;
				cmp = bc_time_compare(now, &timer->m_sExpires);
				if (cmp >= 0)
				{
					type = BC_TIMEREVENT_LIFE;
					post_event = TRUE;
					need_schedule = FALSE;
				}
				else
				{
					type = BC_TIMEREVENT_TICK;
					post_event = TRUE;
					need_schedule = TRUE;
				}
			}
			else if (!bc_time_isepoch(&timer->m_sExpires) &&
				     bc_time_compare(now, &timer->m_sExpires) >= 0)
			{
				type = BC_TIMEREVENT_LIFE;
				post_event = TRUE;
				need_schedule = FALSE;
			}
			else
			{
				idle = FALSE;

				timer->m_sLock.Lock();
				if (!bc_time_isepoch(&timer->m_sIdle) &&
				    bc_time_compare(now, &timer->m_sIdle) >= 0)
				{
					idle = TRUE;
				}
				timer->m_sLock.Unlock();
				if (idle)
				{
					type = BC_TIMEREVENT_IDLE;
					post_event = TRUE;
					need_schedule = FALSE;
				}
				else
				{
					/*
					 * Idle timer has been touched;
					 * reschedule.
					 */
					post_event = FALSE;
					need_schedule = TRUE;
				}
			}

			if (post_event)
			{
				/*
				 * XXX We could preallocate this event.
				 */
				pEvent = new BCTimerEvent(
							   timer,
							   type,
							   timer->m_lpfnAction,
							   timer->m_pArg);

				if (pEvent != NULL)
				{
					pEvent->due = timer->m_sDue;
					timer->m_pTask->Send((BCTaskEvent **)&pEvent);
				}
				else
				{
					LogError(_LOCAL_, "couldn't allocate event");
				}
			}

			timer->m_nIndex = 0;
			m_sHeap.Delete(1);
			m_nScheduled--;

			if (need_schedule)
			{
				result = timer->_Schedule(now, FALSE);
				if (result != BC_R_SUCCESS)
				{
					LogError(_LOCAL_, "%s: %u", "couldn't schedule "
							"timer", result);
				}
			}
		}
		else
		{
			m_sDue = timer->m_sDue;
			done = TRUE;
		}
	}
}

void *BCTimerMgr::_Run(LPVOID pArg)
{
	BCTimerMgr *manager = (BCTimerMgr *)pArg;
	BCTimeS now;
	BCRESULT result;

	manager->m_sLock.Lock();
	while (!manager->m_bDone)
	{
		TIME_NOW(&now);

		manager->_Dispatch(&now);

		if (manager->m_nScheduled > 0)
		{
			result = manager->m_sCondWakeup.TimedWait(&manager->m_sDue);
			ASSERT(result == BC_R_SUCCESS || result == BC_R_TIMEDOUT);
		}
		else
		{
			manager->m_sCondWakeup.Wait();
		}
	}
	manager->m_sLock.Unlock();

	return NULL;
}

BOOL BCTimerMgr::_Sooner(void *t1, void *t2)
{
	BCTimer *pTimer1, *pTimer2;

	pTimer1 = (BCTimer *)t1;
	pTimer2 = (BCTimer *)t2;
	ASSERT(VALID_TIMER(pTimer1));
	ASSERT(VALID_TIMER(pTimer2));

	if (bc_time_compare(&pTimer1->m_sDue, &pTimer2->m_sDue) < 0)
		return (TRUE);
	return (FALSE);
}

void BCTimerMgr::_SetIndex(void *what, uint32_t index)
{
	BCTimer *timer;

	timer = (BCTimer *)what;
	ASSERT(VALID_TIMER(timer));

	timer->m_nIndex = index;
}

BCRESULT BCTimerMgr::Create(BCTimerMgr **managerp)
{
	BCTimerMgr *manager;
	BCRESULT result;

	/*
	 * Create a timer manager.
	 */

	ASSERT(managerp != NULL && *managerp == NULL);

	manager = new BCTimerMgr();
	if (manager == NULL)
		return (BC_R_NOMEMORY);

	result = manager->m_sHeap.Create(_Sooner, _SetIndex, 0);
	if (result != BC_R_SUCCESS)
	{
		ASSERT(result == BC_R_NOMEMORY);
		delete manager;
		return (BC_R_NOMEMORY);
	}

	manager->m_pThread = new BCThread(_Run, manager,
		BCThread::PRIORITY_NORMAL, "BCTimerMgrThread");
	if (manager->m_pThread == NULL)
	{
		delete manager;
		LogError(_LOCAL_, "BCTimerMgr::Create() %s", "failed");
		return (BC_R_UNEXPECTED);
	}
	manager->m_pThread->Start();

	*managerp = (BCTimerMgr *)manager;

	return (BC_R_SUCCESS);
}

void BCTimerMgr::Poke()
{
	ASSERT(VALID_MANAGER(this));

	m_sCondWakeup.Signal();
}

void BCTimerMgr::Destroy(BCTimerMgr **managerp)
{
	BCTimerMgr *manager;

	/*
	 * Destroy a timer manager.
	 */

	ASSERT(managerp != NULL);
	manager = (BCTimerMgr *)*managerp;
	ASSERT(VALID_MANAGER(manager));

	manager->m_sLock.Lock();

	ASSERT(manager->m_lstTimers.IsEmpty());
	manager->m_bDone = TRUE;

	manager->m_sCondWakeup.Signal();

	manager->m_sLock.Unlock();

	/*
	 * Wait for thread to exit.
	 */
	manager->m_pThread->Join(NULL);

	/*
	 * Clean up.
	 */
	delete manager;

	*managerp = NULL;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}//End of namespace BC

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////