
#include "BC/BCTime.h"
#include "BC/BCTask.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros :
///////////////////////////////////////////////////////////////////////////////

#define DEFAULT_TASKMGR_QUANTUM			10
#define DEFAULT_DEFAULT_QUANTUM			5
#define FINISHED(m)			((m)->m_bExiting && (m)->m_lstTasks.IsEmpty())

#define TASK_F_SHUTTINGDOWN		0x01

#define TASK_SHUTTINGDOWN(t)	(((t)->m_nFlags & TASK_F_SHUTTINGDOWN) != 0)

#define TASK_MAGIC				BC_MAGIC('T', 'A', 'S', 'K')
#define VALID_TASK(t)			BC_MAGIC_VALID(t, TASK_MAGIC)

#define TASK_MANAGER_MAGIC		BC_MAGIC('T', 'S', 'K', 'M')
#define VALID_MANAGER(m)		BC_MAGIC_VALID(m, TASK_MANAGER_MAGIC)

#define LOCK(x)		((x)->m_sLock.Lock())
#define UNLOCK(x)	((x)->m_sLock.Unlock())

///////////////////////////////////////////////////////////////////////////////
// class : BCTask
///////////////////////////////////////////////////////////////////////////////

BCTask::BCTask()
	: BCMagic(TASK_MAGIC)
	, m_pMgr(NULL)
	, m_eState(task_state_idle)
	, m_nRef(0)
	, m_nQuantum(DEFAULT_DEFAULT_QUANTUM)
	, m_nFlags(0)
	, m_nNowTime(0)
	, m_pTag(NULL)
{
	memset(m_szName, 0, sizeof(m_szName));
}

BCTask::~BCTask()
{
	//
}

BCRESULT BCTask::Create(BCTaskMgr *pMgr, uint32_t nQuantam)
{
	BOOL bExiting;

	ASSERT(VALID_MANAGER(pMgr));

	m_pMgr = pMgr;
	m_eState = task_state_idle;
	m_nRef = 1;
	m_nQuantum = nQuantam;
	m_nFlags = 0;
	m_nNowTime = 0;
	m_pTag = NULL;

	bExiting = FALSE;
	LOCK(pMgr);
	if (!pMgr->m_bExiting)
	{
		if (m_nQuantum == 0)
			m_nQuantum = pMgr->m_nDefQuantum;
		pMgr->m_lstTasks.PushBack(this);
	}
	else
	{
		bExiting = TRUE;
	}
	UNLOCK(pMgr);

	if (bExiting)
	{
		return (BC_R_SHUTTINGDOWN);
	}

	return (BC_R_SUCCESS);
}

void BCTask::Attach(BCTask **ppTarget)
{
	/*
	 * Attach *pTarget to this.
	 */

	ASSERT(VALID_TASK(this));
	ASSERT(ppTarget != NULL && *ppTarget == NULL);

	m_sLock.Lock();
	m_nRef++;
	m_sLock.Unlock();

	*ppTarget = this;
}

inline BOOL BCTask::_Shutdown()
{
	BOOL was_idle = FALSE;
	BCTaskEvent *pEvent;

	/*
	 * Caller must be holding the task's lock.
	 */
	if (! TASK_SHUTTINGDOWN(this))
	{
		m_nFlags |= TASK_F_SHUTTINGDOWN;
		if (m_eState == task_state_idle)
		{
			ASSERT(m_lstEvents.IsEmpty());
			m_eState = task_state_ready;
			was_idle = TRUE;
		}
		ASSERT(m_eState == task_state_ready ||
		       m_eState == task_state_running);
		/*
		 * Note that we post shutdown events LIFO.
		 */
		for (pEvent = m_lstOnShutdown.PopBack();
		     pEvent != NULL;
		     pEvent = m_lstOnShutdown.PopBack())
		{
			m_lstEvents.PushBack(pEvent);
		}
	}

	return (was_idle);
}

inline void BCTask::_Ready()
{
	ASSERT(VALID_MANAGER(m_pMgr));
	ASSERT(m_eState == task_state_ready);

	LOCK(m_pMgr);

	m_pMgr->m_lstReadyTasks.PushBack(this);
	m_pMgr->m_sCondWorkAvailable.Signal();

	UNLOCK(m_pMgr);
}

inline BOOL BCTask::_Detach()
{
	/*
	 * Caller must be holding the task lock.
	 */

	ASSERT(m_nRef > 0);

	m_nRef--;
	if (m_nRef == 0 && m_eState == task_state_idle)
	{
		ASSERT(m_lstEvents.IsEmpty());
		/*
		 * There are no references to this task, and no
		 * pending events.  We could try to optimize and
		 * either initiate shutdown or clean up the task,
		 * depending on its state, but it's easier to just
		 * make the task ready and allow run() or the event
		 * loop to deal with shutting down and termination.
		 */
		m_eState = task_state_ready;
		return (TRUE);
	}

	return (FALSE);
}

void BCTask::Detach(BCTask **taskp)
{
	BCTask *task;
	BOOL was_idle;

	/*
	 * Detach *taskp from its task.
	 */
	ASSERT(taskp != NULL);
	task = (BCTask *)*taskp;
	ASSERT(VALID_TASK(task));
	ASSERT(task == this);

	LOCK(this);
	was_idle = _Detach();
	UNLOCK(this);

	if (was_idle)
		_Ready();

	*taskp = NULL;
}

inline BOOL BCTask::_Send(BCTaskEvent **ppEvent)
{
	BOOL was_idle = FALSE;
	BCTaskEvent *pEvent;

	/*
	 * Caller must be holding the task lock.
	 */

	ASSERT(ppEvent != NULL);
	pEvent = *ppEvent;
	ASSERT(pEvent != NULL);
	ASSERT(pEvent->ev_type > 0);
	ASSERT(m_eState != task_state_done);

	if (m_eState == task_state_idle)
	{
		was_idle = TRUE;
		ASSERT(m_lstEvents.IsEmpty());
		m_eState = task_state_ready;
	}
	ASSERT(m_eState == task_state_ready ||
	       m_eState == task_state_running);
	m_lstEvents.PushBack(pEvent);
	*ppEvent = NULL;

	return (was_idle);
}

void BCTask::Send(BCTaskEvent **ppEvent)
{
	BOOL was_idle;

	/*
	 * Send '*event' to 'task'.
	 */

	ASSERT(VALID_TASK(this));

	/*
	 * We're trying hard to hold locks for as short a time as possible.
	 * We're also trying to hold as few locks as possible.  This is why
	 * some processing is deferred until after the lock is released.
	 */
	LOCK(this);
	was_idle = _Send(ppEvent);
	UNLOCK(this);

	if (was_idle)
	{
		/*
		 * We need to add this task to the ready queue.
		 *
		 * We've waited until now to do it because making a task
		 * ready requires locking the manager.  If we tried to do
		 * this while holding the task lock, we could deadlock.
		 *
		 * We've changed the state to ready, so no one else will
		 * be trying to add this task to the ready queue.  The
		 * only way to leave the ready state is by executing the
		 * task.  It thus doesn't matter if events are added,
		 * removed, or a shutdown is started in the interval
		 * between the time we released the task lock, and the time
		 * we add the task to the ready queue.
		 */
		_Ready();
	}
}

void BCTask::SendAndDetach(BCTaskEvent **ppEvent)
{
	BOOL idle1, idle2;

	/*
	 * Send '*event' to '*taskp' and then detach '*taskp' from its
	 * task.
	 */

	ASSERT(VALID_TASK(this));

	LOCK(this);
	idle1 = _Send(ppEvent);
	idle2 = _Detach();
	UNLOCK(this);

	/*
	 * If idle1, then idle2 shouldn't be true as well since we're holding
	 * the task lock, and thus the task cannot switch from ready back to
	 * idle.
	 */
	ASSERT(!(idle1 && idle2));

	if (idle1 || idle2)
		_Ready();
}

#define PURGE_OK(pEvent)	(((pEvent)->ev_attributes & BC_EVENTATTR_NOPURGE) == 0)

uint32_t BCTask::_DequeueEvents(
	void *pSender,
	BCEventType first,
	BCEventType last,
	void *pTag,
	BCTaskEventList &refLstEvents,
	BOOL bPurging)
{
	BCTaskEvent *pEvent, *next_event, *end_event;
	uint32_t count = 0;

	ASSERT(VALID_TASK(this));
	ASSERT(last >= first);

	/*
	 * Events matching 'sender', whose type is >= first and <= last, and
	 * whose tag is 'tag' will be dequeued.  If 'purging', matching events
	 * which are marked as unpurgable will not be dequeued.
	 *
	 * sender == NULL means "any sender", and tag == NULL means "any tag".
	 */

	LOCK(this);

	end_event = m_lstEvents.End();
	for (pEvent = m_lstEvents.Begin();
		 pEvent != end_event;
		 pEvent = next_event)
	{
		next_event = m_lstEvents.Next(pEvent);
		if (pEvent->ev_type >= first && pEvent->ev_type <= last &&
		    (pSender == NULL || pEvent->ev_sender == pSender) &&
		    (pTag == NULL || pEvent->ev_tag == pTag) &&
		    (!bPurging || PURGE_OK(pEvent)))
		{
			pEvent->RemoveFromList();
			refLstEvents.PushBack(pEvent);
			count++;
		}
	}

	UNLOCK(this);

	return (count);
}

uint32_t BCTask::PurgeRange(
	void *pSender,
	BCEventType first,
	BCEventType last,
	void *pTag)
{
	uint32_t count;
	BCTaskEventList lstEvents;
	BCTaskEvent *pEvent, *pNextEvent, *pEnd;

	/*
	 * Purge events from a task's event queue.
	 */

	count = _DequeueEvents(pSender, first, last, pTag, lstEvents, TRUE);

	pEnd = lstEvents.End();
	for (pEvent = lstEvents.Begin();pEvent != pEnd;pEvent = pNextEvent)
	{
		pNextEvent = lstEvents.Next(pEvent);
		delete pEvent;
	}

	/*
	 * Note that purging never changes the state of the task.
	 */

	return (count);
}

uint32_t BCTask::Purge(
	void *pSender,
	BCEventType type,
	void *pTag)
{
	/*
	 * Purge events from a task's event queue.
	 */
	return (PurgeRange(pSender, type, type, pTag));
}

BOOL BCTask::PurgeEvent(BCTaskEvent *pEvent)
{
	BCTaskEvent *curr_event, *next_event, *end_event;

	/*
	 * Purge 'event' from a task's event queue.
	 *
	 * XXXRTH:  WARNING:  This method may be removed before beta.
	 */

	ASSERT(VALID_TASK(this));

	/*
	 * If 'event' is on the task's event queue, it will be purged,
	 * unless it is marked as unpurgeable.  'event' does not have to be
	 * on the task's event queue; in fact, it can even be an invalid
	 * pointer.  Purging only occurs if the event is actually on the task's
	 * event queue.
	 *
	 * Purging never changes the state of the task.
	 */

	LOCK(this);
	end_event = m_lstEvents.End();
	for (curr_event = m_lstEvents.Begin();
	     curr_event != end_event;
	     curr_event = next_event)
	{
		next_event = m_lstEvents.Next(curr_event);
		if (curr_event == pEvent && PURGE_OK(pEvent))
		{
			curr_event->RemoveFromList();
			break;
		}
	}
	UNLOCK(this);

	if (curr_event == NULL)
		return (FALSE);

	delete curr_event;

	return (TRUE);
}

uint32_t BCTask::UnsendRange(
	void *sender,
	BCEventType first,
	BCEventType last,
	void *tag,
	BCTaskEventList &refLstEvents)
{
	/*
	 * Remove events from a task's event queue.
	 */
	return (_DequeueEvents(sender, first, last, tag, refLstEvents, FALSE));
}

uint32_t BCTask::Unsend(
	void *sender,
	BCEventType type,
	void *tag,
	BCTaskEventList &refLstEvents)
{
	/*
	 * Remove events from a task's event queue.
	 */

	return (_DequeueEvents(sender, type, type, tag, refLstEvents, FALSE));
}

BCRESULT BCTask::OnShutdown(
	LPFN_BCTaskAction action,
	const void *arg)
{
	BOOL disallowed = FALSE;
	BCRESULT result = BC_R_SUCCESS;
	BCTaskEvent *pEvent;

	/*
	 * Send a shutdown event with action 'action' and argument 'arg' when
	 * 'task' is shutdown.
	 */

	ASSERT(VALID_TASK(this));
	ASSERT(action != NULL);

	pEvent = new BCTaskEvent(NULL, BC_TASKEVENT_SHUTDOWN, action, arg);
	if (pEvent == NULL)
	{
		return (BC_R_NOMEMORY);
	}

	LOCK(this);
	if (TASK_SHUTTINGDOWN(this))
	{
		disallowed = TRUE;
		result = BC_R_SHUTTINGDOWN;
	}
	else
	{
		m_lstOnShutdown.PushBack(pEvent);
	}
	UNLOCK(this);

	if (disallowed)
	{
		delete pEvent;
	}

	return (result);
}

void BCTask::RemoveAllOnShutdown()
{
	BOOL disallowed = FALSE;
	BCRESULT result = BC_R_SUCCESS;
	BCTaskEvent *pEvent;

	/*
	 * Send a shutdown event with action 'action' and argument 'arg' when
	 * 'task' is shutdown.
	 */

	ASSERT(VALID_TASK(this));

	LOCK(this);
	while ((pEvent = m_lstOnShutdown.PopFront()))
	{
		delete pEvent;
	}
	UNLOCK(this);
}

void BCTask::Shutdown()
{
	BOOL was_idle;

	/*
	 * Shutdown 'task'.
	 */

	ASSERT(VALID_TASK(this));

	LOCK(this);
	was_idle = _Shutdown();
	UNLOCK(this);

	if (was_idle)
		_Ready();
}

void BCTask::Destroy(BCTask **taskp)
{
	/*
	 * Destroy '*taskp'.
	 */
	Shutdown();
	Detach(taskp);
}

void BCTask::SetName(const char *name, void *tag)
{
	/*
	 * Name 'task'.
	 */

	ASSERT(VALID_TASK(this));

	LOCK(this);
	memset(m_szName, 0, sizeof(m_szName));
	strncpy(m_szName, name, sizeof(m_szName) - 1);
	m_pTag = tag;
	UNLOCK(this);
}

const char *BCTask::GetName() const
{
	ASSERT(VALID_TASK(this));

	return (m_szName);
}

void *BCTask::GetTag() const
{
	ASSERT(VALID_TASK(this));

	return (m_pTag);
}

void BCTask::GetCurTime(uint32_t *pTime)
{
	ASSERT(VALID_TASK(this));
	ASSERT(pTime != NULL);

	LOCK(this);

	*pTime = m_nNowTime;

	UNLOCK(this);
}

BCRESULT BCTask::BeginExclusive()
{
	ASSERT(m_eState == task_state_running);
	LOCK(m_pMgr);
	if (m_pMgr->m_bExclusiveRequested)
	{
		UNLOCK(m_pMgr);
		return (BC_R_LOCKBUSY);
	}
	m_pMgr->m_bExclusiveRequested = TRUE;
	while (m_pMgr->m_nRunningTasks > 1)
	{
		m_pMgr->m_sCondExclusiveGranted.Wait();
	}
	UNLOCK(m_pMgr);

	return (BC_R_SUCCESS);
}

void BCTask::EndExclusive()
{
	ASSERT(m_eState == task_state_running);
	LOCK(m_pMgr);
	ASSERT(m_pMgr->m_bExclusiveRequested);
	m_pMgr->m_bExclusiveRequested = FALSE;
	m_pMgr->m_sCondWorkAvailable.Broadcast();
	UNLOCK(m_pMgr);
}

BOOL BCTask::IsExiting() const
{
	ASSERT(VALID_TASK(this));
	return (TASK_SHUTTINGDOWN(this));
}

BCTaskMgr *BCTask::GetManager() const
{
	ASSERT(VALID_TASK(this));

	return m_pMgr;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCTaskMgr
///////////////////////////////////////////////////////////////////////////////

BCTaskMgr::BCTaskMgr()
	: BCMagic(TASK_MANAGER_MAGIC)
	, m_nWorkers(0)
	, m_ppThreads(NULL)
	, m_nDefQuantum(DEFAULT_TASKMGR_QUANTUM)
	, m_sCondWorkAvailable(&m_sLock)
	, m_sCondExclusiveGranted(&m_sLock)
	, m_nRunningTasks(0)
	, m_bExclusiveRequested(FALSE)
	, m_bExiting(FALSE)
{
	//
}

BCTaskMgr::~BCTaskMgr()
{
	//
}

BCRESULT BCTaskMgr::Create(
	uint32_t workers,
	uint32_t default_quantum,
	BCThread::priority_t ePri,
	LPCSTR lpszThreadName)
{
	BCRESULT result;
	unsigned int i, started = 0;
	BCThread *pThread = NULL;

	/*
	 * Create a new task m_pManager.
	 */

	ASSERT(workers > 0);

	m_nWorkers = 0;
	m_ppThreads = (BCThread **)m_sPool.Calloc(workers * sizeof(BCThread *));
	if (m_ppThreads == NULL)
	{
		result = BC_R_NOMEMORY;
		goto failed;
	}

	if (default_quantum == 0)
		default_quantum = DEFAULT_DEFAULT_QUANTUM;
	m_nDefQuantum = default_quantum;
	m_nRunningTasks = 0;
	m_bExclusiveRequested = FALSE;
	m_bExiting = FALSE;

	m_sLock.Lock();
	/*
	 * Start workers.
	 */
	for (i = 0; i < workers; i++)
	{
		pThread = new BCThread(_Run, this, ePri, lpszThreadName);
		if (pThread)
		{
			m_ppThreads[m_nWorkers] = pThread;
			pThread->Start();
			m_nWorkers++;
			started++;
		}
	}
	m_sLock.Unlock();

	if (started == 0)
	{
		_Free();
		return (BC_R_NOTHREADS);
	}

	return (BC_R_SUCCESS);

failed:
	return (result);
}

void BCTaskMgr::Destroy(BCTaskMgr **managerp)
{
	BCTaskMgr *manager;
	BCTask *pTask;
	uint32_t i;

	ASSERT(managerp != NULL && *managerp != NULL);
	manager = *managerp;
	ASSERT(VALID_MANAGER(manager));

	/*
	 * Only one non-worker thread may ever call this routine.
	 * If a worker thread wants to initiate shutdown of the
	 * task manager, it should ask some non-worker thread to call
	 * BCTaskmgr::Destroy(), e.g. by signalling a condition variable
	 * that the startup thread is sleeping on.
	 */

	/*
	 * Unlike elsewhere, we're going to hold this lock a long time.
	 * We need to do so, because otherwise the list of tasks could
	 * change while we were traversing it.
	 *
	 * This is also the only function where we will hold both the
	 * task manager lock and a task lock at the same time.
	 */

	manager->m_sLock.Lock();

	/*
	 * Make sure we only get called once.
	 */
	ASSERT(!manager->m_bExiting);
	manager->m_bExiting = TRUE;

	/*
	 * Post shutdown event(s) to every task (if they haven't already been
	 * posted).
	 */
	while ((pTask = manager->m_lstTasks.PopFront()) != NULL)
	{
		LOCK(pTask);
		if (pTask->_Shutdown())
			manager->m_lstReadyTasks.PushBack(pTask);
		UNLOCK(pTask);
	}

	/*
		* Wake up any sleeping workers.  This ensures we get work done if
		* there's work left to do, and if there are already no tasks left
		* it will cause the workers to see manager->exiting.
		*/
	manager->m_sCondWorkAvailable.Broadcast();
	manager->m_sLock.Unlock();

	/*
	 * Wait for all the worker threads to exit.
	 */
	for (i = 0; i < manager->m_nWorkers; i++)
	{
		manager->m_ppThreads[i]->Join(NULL);
	}

	/*
	 * Free all undispatched events
	 */
	while ((pTask = manager->m_lstReadyTasks.PopFront()) != NULL)
	{
		BCTaskEvent *pEvent;
		while ((pEvent = pTask->m_lstEvents.PopFront()) != NULL)
		{
			pEvent->Destroy();
		}
		while ((pEvent = pTask->m_lstOnShutdown.PopFront()) != NULL)
		{
			pEvent->Destroy();
		}
		delete pTask;
	}

	delete manager;

	*managerp = NULL;
}

uint32_t BCTaskMgr::GetTaskCount() const
{
	return m_lstTasks.Count() + m_lstReadyTasks.Count();
}

void BCTaskMgr::_Free()
{
	if (m_ppThreads)
	{
		m_sPool.Clear();
		m_ppThreads = NULL;
	}
}

void BCTaskMgr::_Dispatch()
{
	BCTask *pTask;

	ASSERT(VALID_MANAGER(this));

	/*
	 * Again we're trying to hold the lock for as short a time as possible
	 * and to do as little locking and unlocking as possible.
	 *
	 * In both while loops, the appropriate lock must be held before the
	 * while body starts.  Code which acquired the lock at the top of
	 * the loop would be more readable, but would result in a lot of
	 * extra locking.  Compare:
	 *
	 * Straightforward:
	 *
	 *	LOCK();
	 *	...
	 *	UNLOCK();
	 *	while (expression) {
	 *		LOCK();
	 *		...
	 *		UNLOCK();
	 *
	 *	       	Unlocked part here...
	 *
	 *		LOCK();
	 *		...
	 *		UNLOCK();
	 *	}
	 *
	 * Note how if the loop continues we unlock and then immediately lock.
	 * For N iterations of the loop, this code does 2N+1 locks and 2N+1
	 * unlocks.  Also note that the lock is not held when the while
	 * condition is tested, which may or may not be important, depending
	 * on the expression.
	 *
	 * As written:
	 *
	 *	LOCK();
	 *	while (expression) {
	 *		...
	 *		UNLOCK();
	 *
	 *	       	Unlocked part here...
	 *
	 *		LOCK();
	 *		...
	 *	}
	 *	UNLOCK();
	 *
	 * For N iterations of the loop, this code does N+1 locks and N+1
	 * unlocks.  The while expression is always protected by the lock.
	 */

	m_sLock.Lock();
	while (!FINISHED(this)) {
		/*
		 * For reasons similar to those given in the comment in
		 * BCTask::Send() above, it is safe for us to dequeue
		 * the task while only holding the manager lock, and then
		 * change the task to running state while only holding the
		 * task lock.
		 */
		while ((m_lstReadyTasks.IsEmpty() ||
			m_bExclusiveRequested) && !FINISHED(this))
		{
			m_sCondWorkAvailable.Wait();
		}

		pTask = m_lstReadyTasks.PopFront();
		if (pTask != NULL)
		{
			uint32_t dispatch_count = 0;
			BOOL done = FALSE;
			BOOL requeue = FALSE;
			BOOL finished = FALSE;
			BCTaskEvent *pEvent;

			ASSERT(VALID_TASK(pTask));

			/*
			 * Note we only unlock the manager lock if we actually
			 * have a task to do.  We must reacquire the manager
			 * lock before exiting the 'if (task != NULL)' block.
			 */
			m_nRunningTasks++;
			m_sLock.Unlock();

			LOCK(pTask);
			ASSERT(pTask->m_eState == task_state_ready);
			pTask->m_eState = task_state_running;
			bc_stdtime_get(&pTask->m_nNowTime);
			do
			{
				if (!pTask->m_lstEvents.IsEmpty())
				{
					pEvent = pTask->m_lstEvents.PopFront();

					/*
					 * Execute the event action.
					 */
					if (pEvent->ev_action != NULL)
					{
						UNLOCK(pTask);
						(pEvent->ev_action)(pTask, pEvent);
						LOCK(pTask);
					}
					dispatch_count++;
				}

				if (pTask->m_nRef == 0 &&
				    pTask->m_lstEvents.IsEmpty() &&
				    !TASK_SHUTTINGDOWN(pTask))
				{
					BOOL was_idle;

					/*
					 * There are no references and no
					 * pending events for this task,
					 * which means it will not become
					 * runnable again via an external
					 * action (such as sending an event
					 * or detaching).
					 *
					 * We initiate shutdown to prevent
					 * it from becoming a zombie.
					 *
					 * We do this here instead of in
					 * the "if EMPTY(task->events)" block
					 * below because:
					 *
					 *	If we post no shutdown events,
					 *	we want the task to finish.
					 *
					 *	If we did post shutdown events,
					 *	will still want the task's
					 *	quantum to be applied.
					 */
					was_idle = pTask->_Shutdown();
					ASSERT(!was_idle);
				}

				if (pTask->m_lstEvents.IsEmpty())
				{
					/*
					 * Nothing else to do for this task
					 * right now.
					 */
					if (pTask->m_nRef == 0 &&
						TASK_SHUTTINGDOWN(pTask))
					{
						/*
						 * The task is done.
						 */
						finished = TRUE;
						pTask->m_eState = task_state_done;
					}
					else
					{
						pTask->m_eState = task_state_idle;
					}
					done = TRUE;
				}
				else if (dispatch_count >= pTask->m_nQuantum)
				{
					/*
					 * Our quantum has expired, but
					 * there is more work to be done.
					 * We'll requeue it to the ready
					 * queue later.
					 *
					 * We don't check quantum until
					 * dispatching at least one event,
					 * so the minimum quantum is one.
					 */
					pTask->m_eState = task_state_ready;
					requeue = TRUE;
					done = TRUE;
				}
			} while (!done);
			UNLOCK(pTask);

			if (finished)
				_Finished(pTask);

			m_sLock.Lock();
			m_nRunningTasks--;

			if (m_bExclusiveRequested && m_nRunningTasks == 1)
			{
				m_sCondExclusiveGranted.Signal();
			}

			if (requeue)
			{
				/*
				 * We know we're awake, so we don't have
				 * to wakeup any sleeping threads if the
				 * ready queue is empty before we requeue.
				 *
				 * A possible optimization if the queue is
				 * empty is to 'goto' the 'if (task != NULL)'
				 * block, avoiding the ENQUEUE of the task
				 * and the subsequent immediate DEQUEUE
				 * (since it is the only executable task).
				 * We don't do this because then we'd be
				 * skipping the exit_requested check.  The
				 * cost of ENQUEUE is low anyway, especially
				 * when you consider that we'd have to do
				 * an extra EMPTY check to see if we could
				 * do the optimization.  If the ready queue
				 * were usually nonempty, the 'optimization'
				 * might even hurt rather than help.
				 */
				m_lstReadyTasks.PushBack(pTask);
			}
		}
	}

	m_sLock.Unlock();
}

void BCTaskMgr::_Finished(BCTask *pTask)
{
	ASSERT(pTask->m_lstEvents.IsEmpty());
	ASSERT(pTask->m_lstOnShutdown.IsEmpty());
	ASSERT(pTask->m_nRef == 0);
	ASSERT(pTask->m_eState == task_state_done);

	m_sLock.Lock();
	pTask->RemoveFromList();
	if (FINISHED(this))
	{
		/*
		 * All tasks have completed and the
		 * task manager is exiting.  Wake up
		 * any idle worker threads so they
		 * can exit.
		 */
		m_sCondWorkAvailable.Broadcast();
	}
	m_sLock.Unlock();

	delete pTask;
}

void *BCTaskMgr::_Run(void *pArg)
{
	BCTaskMgr *pMgr = (BCTaskMgr *)pArg;

	ASSERT(pMgr);

	pMgr->_Dispatch();

	return (NULL);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

};

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
