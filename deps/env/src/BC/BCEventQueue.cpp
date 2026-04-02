///////////////////////////////////////////////////////////////////////////////
// file : BCEventQueue.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include <BC/BCLog.h>
#include <BC/BCEventQueue.h>



///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// class : TaskFuncWrap
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(TaskFuncWrap, 8);

TaskFuncWrap::TaskFuncWrap(AsyncTaskFunc fTask, bool bWaitResult)
	:m_fTask(std::move(fTask))
{
	if (bWaitResult)
	{
		m_pLock.reset(new BCMutex());
		m_pCond.reset(new BCCondition(m_pLock.get()));
	}
}

TaskFuncWrap::TaskFuncWrap(AsyncTaskWithResultFunc fTask)
	:m_fTaskWithResult(std::move(fTask))
{
	m_pLock.reset(new BCMutex());
	m_pCond.reset(new BCCondition(m_pLock.get()));
	m_pResult = std::make_shared<BCRESULT>(BC_R_SUCCESS);
}

TaskFuncWrap::~TaskFuncWrap()
{
	if (m_pLock && m_pCond)
	{
		BCMutex::Owner lock(*m_pLock);
		m_pCond->Signal();
	}
}

void TaskFuncWrap::RunTask()
{
	if (m_pLock && m_pCond)
	{
		if (m_pResult)
		{
			*m_pResult = m_fTaskWithResult();
		}
		else
		{
			m_fTask();
		}
	}
	else
	{
		m_fTask();
	}
}

std::shared_ptr<BCMutex> TaskFuncWrap::GetLock()
{
	return m_pLock;
}

std::shared_ptr<BCCondition> TaskFuncWrap::GetCondition()
{
	return m_pCond;
}

std::shared_ptr<BCRESULT> TaskFuncWrap::GetResult()
{
	return m_pResult;
}

void TaskFuncWrap::TaskDetor(BCEventItemS &refEvent)
{
	TaskFuncWrap *pEvent = (TaskFuncWrap *)refEvent.wParam;
	if (pEvent)
	{
		delete pEvent;
		refEvent.wParam = 0;
	}
}

TaskFuncWrap *TaskFuncWrap::WrapTask(
	BCEventItemS &refEvent, 
	AsyncTaskFunc fTask,
	bool bWaitResult)
{
	TaskFuncWrap *pWrap = new TaskFuncWrap(std::move(fTask), bWaitResult);
	refEvent.wParam = (uint64_t)pWrap;
	refEvent.cbDestroy = TaskDetor;
	return pWrap;
}

TaskFuncWrap *TaskFuncWrap::WrapTask(
	BCEventItemS &refEvent, 
	AsyncTaskWithResultFunc fTask)
{
	TaskFuncWrap *pWrap = new TaskFuncWrap(std::move(fTask));
	refEvent.wParam = (uint64_t)pWrap;
	refEvent.cbDestroy = TaskDetor;
	return pWrap;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCEventWrap
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCEventWrap, 8);

///////////////////////////////////////////////////////////////////////////////
// class : BCDelayTask
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCDelayTask, 8);

BCDelayTask::BCDelayTask()
	: m_pTimer(NULL)
	, m_nTimerId(0)
	, m_pTaskFunc(NULL)
	, m_pUserData(NULL)
{
	//
}

BCDelayTask::~BCDelayTask()
{
	Destroy();
}

BCRESULT BCDelayTask::Create(
	BCTask *pTask,
	BCTimerMgr *pTimerMgr, 
	int32_t nTimerId)
{
	BCRESULT result = BC_R_FAILURE;
	if (!pTask || !pTimerMgr)
	{
		return BC_R_INVALIDARG;
	}
	if (!m_pTimer)
	{
		m_pTimer = new BCTimer();
		if (m_pTimer)
		{
			result = m_pTimer->Create(pTimerMgr, bc_timertype_inactive,
				NULL, NULL, pTask, _TimeoutCB, this);
		}
		m_nTimerId = nTimerId;
	}
	return result;
}

BCRESULT BCDelayTask::Schedule(
	TimerFuncWithId fTask,
	uint64_t nIntervalUsec,
	bool bTicker)
{
	if (m_pTimer)
	{
		BCIntervalS sInterval;
		bc_interval_set(&sInterval, nIntervalUsec / 1000000,
			(nIntervalUsec % 1000000) * 1000);
		m_fTask = fTask;
		m_pTaskFunc = NULL;
		m_pUserData = NULL;
		BCTimerTypeE type = bTicker ? bc_timertype_ticker : bc_timertype_once;
		return m_pTimer->Reset(type, NULL, &sInterval, TRUE);
	}
	return BC_R_INVALIDPTR;
}

BCRESULT BCDelayTask::Schedule(
	TimerFuncPtr pTaskFunc,
	void * pUserData,
	uint64_t nIntervalUsec,
	bool bTicker)
{
	if (m_pTimer)
	{
		BCIntervalS sInterval;
		bc_interval_set(&sInterval, nIntervalUsec / 1000000,
			(nIntervalUsec % 1000000) * 1000);
		m_pTaskFunc = pTaskFunc;
		m_pUserData = pUserData;
		m_fTask = NULL;
		BCTimerTypeE type = bTicker ? bc_timertype_ticker : bc_timertype_once;
		return m_pTimer->Reset(type, NULL, &sInterval, TRUE);
	}
	return BC_R_INVALIDPTR;
}

void BCDelayTask::Cancel()
{
	if (m_pTimer)
	{
		m_pTimer->Reset(bc_timertype_inactive, NULL, NULL, TRUE);
		m_fTask = NULL;
	}
}

void BCDelayTask::Destroy()
{
	if (m_pTimer)
	{
		m_pTimer->Detach(&m_pTimer);
	}
}

void BCDelayTask::OnTimeout()
{
	auto fTask = m_fTask;
	if (fTask)
	{
		fTask(m_nTimerId);
	}
	else if (m_pTaskFunc)
	{
		(*m_pTaskFunc)(m_pUserData, m_nTimerId);
	}
}

void BCDelayTask::_TimeoutCB(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCDelayTask *_this = (BCDelayTask *)pEvent->ev_arg;
	if (_this)
	{
		_this->OnTimeout();
	}
	pEvent->Destroy();
}

///////////////////////////////////////////////////////////////////////////////
// class : BCEventQueue
///////////////////////////////////////////////////////////////////////////////

BCEventQueue::BCEventQueue()
	: m_nCtrls(0)
	, m_pTimerMgr(NULL)
	, m_pTask(NULL)
	, m_nEventState(EVENT_STATE_IDLE)
	, m_nNextTaskId(0)
{
	m_sTaskEvent.ev_sender = this;
	m_sTaskEvent.ev_type = BC_EVENTCLASS_EVQUEUE;
	m_sTaskEvent.ev_action = _EventCallback;
	m_sTaskEvent.ev_arg = this;
}

BCEventQueue::~BCEventQueue()
{
	_FlushEvents();
	Detach();
}

BCRESULT BCEventQueue::Create(
	BCTimerMgr *pTimerMgr, 
	BCTaskMgr *pTaskMgr, 
	const char *name, 
	void *tag)
{
	if (!pTimerMgr || !pTaskMgr || !name)
	{
		return BC_R_INVALIDARG;
	}
	BCSpinMutex::Owner lock(m_sEventLock);
	if (m_pTask)
	{
		return BC_R_EXISTS;
	}
	m_pTimerMgr = pTimerMgr;
	m_pTask = new BCTask();
	if (!m_pTask)
	{
		return BC_R_NOMEMORY;
	}
	BCRESULT result = m_pTask->Create(pTaskMgr, 0);
	if (result != BC_R_SUCCESS)
	{
		BC_SAFE_DELETE_PTR(m_pTask);
		return result;
	}
	m_pTask->SetName(name, tag);
	result = m_pTask->OnShutdown(_ShutdownCallback, this);
	return result;
}

BCTask *BCEventQueue::GetTask() const
{
	return m_pTask;
}

void BCEventQueue::PostTask(AsyncTaskFunc fTask)
{
	BCEventItemS sEvent;

	TaskFuncWrap::WrapTask(sEvent, fTask, false);
	PostEvent(sEvent);
}

void BCEventQueue::SendTask(AsyncTaskFunc fTask)
{
	BCEventItemS sEvent;
	TaskFuncWrap *pTask(TaskFuncWrap::WrapTask(sEvent, fTask, true));
	std::shared_ptr<BCMutex> pLock(pTask->GetLock());
	std::shared_ptr<BCCondition> pCond(pTask->GetCondition());

	if (pLock)
	{
		pLock->Lock();
	}
	PostEvent(sEvent);
	if (pCond)
	{
		pCond->Wait();
	}
	if (pLock)
	{
		pLock->Unlock();
	}
}

BCRESULT BCEventQueue::SendTaskWithResult(AsyncTaskWithResultFunc fTask)
{
	BCRESULT result = BC_R_SUCCESS;
	BCEventItemS sEvent;
	TaskFuncWrap *pTask(TaskFuncWrap::WrapTask(sEvent, fTask));
	std::shared_ptr<BCMutex> pLock(pTask->GetLock());
	std::shared_ptr<BCCondition> pCond(pTask->GetCondition());
	std::shared_ptr<BCRESULT> pResult(pTask->GetResult());

	if (pLock)
	{
		pLock->Lock();
	}
	PostEvent(sEvent);
	if (pCond)
	{
		pCond->Wait();
		result = *pResult;
	}
	if (pLock)
	{
		pLock->Unlock();
	}
	return result;
}

BCRESULT BCEventQueue::ScheduleTask(
	int32_t &taskId,
	TimerFuncWithId fTask, 
	uint64_t nIntervalUsec,
	bool bTicker)
{
	BCRESULT result;

	BCSpinMutex::Owner lock(m_sEventLock);
	DelayTaskMap::iterator iter = m_mapDelayTasks.find(taskId);
	if (iter == m_mapDelayTasks.end())
	{
		BCDelayTask *pDelayTask = new BCDelayTask();
		if (!pDelayTask)
		{
			goto return_error;
		}
		result = pDelayTask->Create(m_pTask, m_pTimerMgr, ++m_nNextTaskId);
		if (result != BC_R_SUCCESS)
		{
			goto delete_delayTask;
		}
		result = pDelayTask->Schedule(std::move(fTask), 
			nIntervalUsec, bTicker);
		if (result != BC_R_SUCCESS)
		{
			goto delete_delayTask;
		}
		taskId = pDelayTask->GetTimerId();
		m_mapDelayTasks.insert(std::make_pair(taskId, pDelayTask));

		return BC_R_SUCCESS;

delete_delayTask:
		BC_SAFE_DELETE_PTR(pDelayTask);
return_error:
		return -1;
	}
	else
	{
		BCDelayTask *pDelayTask = iter->second;
		if (pDelayTask)
		{
			return pDelayTask->Schedule(std::move(fTask), nIntervalUsec, bTicker);
		}
	}

	return BC_R_UNEXPECTED;
}

BCRESULT BCEventQueue::ScheduleTask(
	int32_t &taskId,
	TimerFuncPtr pTaskFunc,
	void *pUserData,
	uint64_t nIntervalUsec,
	bool bTicker)
{
	BCRESULT result;

	BCSpinMutex::Owner lock(m_sEventLock);
	DelayTaskMap::iterator iter = m_mapDelayTasks.find(taskId);
	if (iter == m_mapDelayTasks.end())
	{
		BCDelayTask *pDelayTask = new BCDelayTask();
		if (!pDelayTask)
		{
			goto return_error;
		}
		result = pDelayTask->Create(m_pTask, m_pTimerMgr, ++m_nNextTaskId);
		if (result != BC_R_SUCCESS)
		{
			goto delete_delayTask;
		}
		result = pDelayTask->Schedule(pTaskFunc, pUserData,	nIntervalUsec, bTicker);
		if (result != BC_R_SUCCESS)
		{
			goto delete_delayTask;
		}
		taskId = pDelayTask->GetTimerId();
		m_mapDelayTasks.insert(std::make_pair(taskId, pDelayTask));

		return BC_R_SUCCESS;

delete_delayTask:
		BC_SAFE_DELETE_PTR(pDelayTask);
return_error:
		return -1;
	}
	else
	{
		BCDelayTask *pDelayTask = iter->second;
		if (pDelayTask)
		{
			return pDelayTask->Schedule(pTaskFunc, pUserData, nIntervalUsec, bTicker);
		}
	}

	return BC_R_UNEXPECTED;
}

BCRESULT BCEventQueue::UnscheduleTask(int32_t &nTaskId, bool onlyCancel)
{
	BCSpinMutex::Owner lock(m_sEventLock);
	DelayTaskMap::iterator iter = m_mapDelayTasks.find(nTaskId);
	if (iter == m_mapDelayTasks.end())
	{
		return BC_R_NOTBOUND;
	}
	BCDelayTask *pDelayTask = iter->second;
	if (pDelayTask)
	{
		pDelayTask->Cancel();
		if (!onlyCancel)
		{
			delete pDelayTask;
			m_mapDelayTasks.erase(iter);
			nTaskId = 0;
		}
	}
	return BC_R_SUCCESS;
}

void BCEventQueue::UnscheduleAllTasks()
{
	BCSpinMutex::Owner lock(m_sEventLock);
	_UnscheduleTasks();
}

void BCEventQueue::Detach(bool bRemoveOnShutdown)
{
	BCSpinMutex::Owner lock(m_sEventLock);
	_UnscheduleTasks();
	if (m_pTask != NULL)
	{
		if (bRemoveOnShutdown)
		{
			m_pTask->RemoveAllOnShutdown();
		}
		m_pTask->Shutdown();
		m_pTask->Detach(&m_pTask);
	}
}

void BCEventQueue::FlushEvents(uint32_t eType)
{
	BCEventItemS *pEvent;
	EventList::iterator iter, iterEnd;
	EventList newList;

	BCSpinMutex::Owner lock(m_sEventLock);
	iter = m_lstEvents.begin();
	iterEnd = m_lstEvents.end();
	for (; iter != iterEnd; iter++)
	{
		pEvent = &(*iter);
		if (pEvent->eType == eType || !eType)
		{
			if (pEvent->cbDestroy != NULL)
			{
				(pEvent->cbDestroy)(*iter);
			}
		}
		else
		{
			newList.push_back(*pEvent);
		}
	}
	m_lstEvents.clear();
	if (newList.size() > 0)
	{
		iter = newList.begin();
		iterEnd = newList.end();
		for (; iter != iterEnd; iter++)
		{
			m_lstEvents.push_back(*iter);
		}
	}
}

bool BCEventQueue::QueueEvent(const BCEventItemS &refEvent)
{
	BCTaskEvent *pEvent;

	BCSpinMutex::Owner lock(m_sEventLock);
	m_lstEvents.push_back(refEvent);
	if (m_pTask != NULL
		&& m_lstEvents.size() > 0
		&& m_nCtrls == 0
		&& m_nEventState == EVENT_STATE_IDLE)
	{
		pEvent = &m_sTaskEvent;
		ASSERT(m_nCtrls == 0);
		m_nCtrls++;
		m_pTask->Send(&pEvent);
	}
	return true;
}

bool BCEventQueue::OnEventProcess(BCEventItemS &refEvent)
{
	bool bResult = false;

#ifndef _DEBUG
	try
	{
#endif
		bResult = BCDefEventProc(refEvent);
#ifndef _DEBUG
	}
	catch(...)
	{
		LogError(_LOCAL_, "Unexpected error occured!");
	}
#endif

	return bResult;
}

void BCEventQueue::OnEventProcShutdown()
{

}

void BCEventQueue::_FlushEvents()
{
	BCEventItemS *pEvent;
	EventList::iterator iter, iterEnd;

	BCSpinMutex::Owner lock(m_sEventLock);
	iter = m_lstEvents.begin();
	iterEnd = m_lstEvents.end();
	for (;iter != iterEnd;iter++)
	{
		pEvent = &(*iter);
		if (pEvent->cbDestroy != NULL)
		{
			(pEvent->cbDestroy)(*iter);
		}
	}
	m_lstEvents.clear();
}

void BCEventQueue::_UnscheduleTasks()
{
	BCDelayTask *pDelayTask;
	DelayTaskMap::iterator iter, iterEnd;

	iter = m_mapDelayTasks.begin();
	iterEnd = m_mapDelayTasks.end();
	for (;iter != iterEnd;iter++)
	{
		pDelayTask = iter->second;
		if (pDelayTask)
		{
			pDelayTask->Cancel();
			delete pDelayTask;
		}
	}
	m_mapDelayTasks.clear();
}

bool BCEventQueue::_PopEvent(BCEventItemS &refEvent)
{
	if (m_lstEvents.size() == 0)
	{
		return false;
	}
	else
	{
		refEvent = m_lstEvents.front();
		m_lstEvents.pop_front();
		return true;
	}
}

void BCEventQueue::_EventCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCEventItemS sEvent(0);
	BCEventQueue *_this = (BCEventQueue *)pEvent->ev_sender;

	ASSERT(_this != NULL);
	_this->m_sEventLock.Lock();
	ASSERT(_this->m_nCtrls == 1);
	_this->m_nCtrls--;
	_this->m_nEventState = EVENT_STATE_RUNNING;
	while(_this->_PopEvent(sEvent))
	{
		_this->m_sEventLock.Unlock();
		if (sEvent.cbDestroy == TaskFuncWrap::TaskDetor)
		{
			TaskFuncWrap *pFuncEvent = (TaskFuncWrap *)sEvent.wParam;
			if (pFuncEvent)
			{
				pFuncEvent->RunTask();
			}
			BCDefEventProc(sEvent);
		}
		else if (_this->OnEventProcess(sEvent))
		{
			BCDefEventProc(sEvent);
		}
		_this->m_sEventLock.Lock();
	}
	_this->m_nEventState = EVENT_STATE_IDLE;
	_this->m_sEventLock.Unlock();
}

void BCEventQueue::_ShutdownCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCEventQueue *_this = (BCEventQueue *)pEvent->ev_arg;

	UNUSED(pTask);
	ASSERT(_this != NULL);

#ifndef _DEBUG
	try
	{
#endif
		//_this->UnscheduleAllTasks();
		_this->OnEventProcShutdown();
#ifndef _DEBUG
	}
	catch (...)
	{
		LogError(_LOCAL_, "Unexpected error occurred!");
	}
#endif

	// Free event
	pEvent->Destroy();
}

///////////////////////////////////////////////////////////////////////////////
// class : NDKEventFactory
///////////////////////////////////////////////////////////////////////////////

NDKEventFactory::NDKEventFactory()
	: m_nCtrls(0)
	, m_lpfnNotifyProc(NULL)
	, m_pNotifyData(NULL)
	, m_bAvailable(false)
	, m_nEventState(EVENT_STATE_IDLE)
{
	//
}

NDKEventFactory::~NDKEventFactory()
{
	_FlushEvents();
}

BCRESULT NDKEventFactory::Create(
	LPFN_EventNotifyProc lpfnProc,
	void *pUserData)
{
	BCSpinMutex::Owner lock(m_sEventLock);
	m_lpfnNotifyProc = lpfnProc;
	m_pNotifyData = pUserData;
	m_bAvailable = true;

	return BC_R_SUCCESS;
}

void NDKEventFactory::PostTask(AsyncTaskFunc fTask)
{
	BCEventItemS sEvent;

	TaskFuncWrap::WrapTask(sEvent, fTask, false);
	PostEvent(sEvent);
}

void NDKEventFactory::SendTask(AsyncTaskFunc fTask)
{
	BCEventItemS sEvent;
	TaskFuncWrap *pTask(TaskFuncWrap::WrapTask(sEvent, fTask, true));
	std::shared_ptr<BCMutex> pLock(pTask->GetLock());
	std::shared_ptr<BCCondition> pCond(pTask->GetCondition());
	if (pLock)
	{
		pLock->Lock();
	}

	PostEvent(sEvent);
	if (pCond)
	{
		pCond->Wait();
	}
	if (pLock)
	{
		pLock->Unlock();
	}
}

BCRESULT NDKEventFactory::SendTaskWithResult(AsyncTaskWithResultFunc fTask)
{
	BCRESULT result = BC_R_SUCCESS;
	BCEventItemS sEvent;
	TaskFuncWrap *pTask(TaskFuncWrap::WrapTask(sEvent, fTask));
	std::shared_ptr<BCMutex> pLock(pTask->GetLock());
	std::shared_ptr<BCCondition> pCond(pTask->GetCondition());
	std::shared_ptr<BCRESULT> pResult(pTask->GetResult());

	if (pLock)
	{
		pLock->Lock();
	}
	PostEvent(sEvent);
	if (pCond)
	{
		pCond->Wait();
		result = *pResult;
	}
	if (pLock)
	{
		pLock->Unlock();
	}
	return result;
}

void NDKEventFactory::Detach()
{
	BCSpinMutex::Owner lock(m_sEventLock);
	if (m_bAvailable)
	{
		_FlushEvents();
		m_bAvailable = false;
	}
}

void NDKEventFactory::FlushEvents(uint32_t eType)
{
	BCEventItemS *pEvent;
	EventList::iterator iter, iterEnd;
	EventList newList;

	BCSpinMutex::Owner lock(m_sEventLock);
	iter = m_lstEvents.begin();
	iterEnd = m_lstEvents.end();
	for (; iter != iterEnd; iter++)
	{
		pEvent = &(*iter);
		if (pEvent->eType == eType || !eType)
		{
			if (pEvent->cbDestroy != NULL)
			{
				(pEvent->cbDestroy)(*iter);
			}
		}
		else
		{
			newList.push_back(*pEvent);
		}
	}
	m_lstEvents.clear();
	if (newList.size() > 0)
	{
		iter = newList.begin();
		iterEnd = newList.end();
		for (; iter != iterEnd; iter++)
		{
			m_lstEvents.push_back(*iter);
		}
	}
}

bool NDKEventFactory::QueueEvent(const BCEventItemS &refEvent)
{
	BCSpinMutex::Owner lock(m_sEventLock);
	m_lstEvents.push_back(refEvent);
	if (m_bAvailable
		&& m_lstEvents.size() > 0
		&& m_nCtrls == 0
		&& m_nEventState == EVENT_STATE_IDLE)
	{
		ASSERT(m_nCtrls == 0);
		m_nCtrls++;
		if (m_lpfnNotifyProc)
		{
			(m_lpfnNotifyProc)(m_pNotifyData);
		}
	}
	return true;
}

bool NDKEventFactory::OnEventProcess(BCEventItemS &refEvent)
{
	bool bResult = false;

#ifndef _DEBUG
	try
	{
#endif
		bResult = BCDefEventProc(refEvent);
#ifndef _DEBUG
	}
	catch (...)
	{
		//LogError(_LOCAL_, "Unexpected error occured!");
	}
#endif

	return bResult;
}

void NDKEventFactory::_FlushEvents()
{
	BCEventItemS *pEvent;
	EventList::iterator iter, iterEnd;

	BCSpinMutex::Owner lock(m_sEventLock);
	iter = m_lstEvents.begin();
	iterEnd = m_lstEvents.end();
	for (; iter != iterEnd; iter++)
	{
		pEvent = &(*iter);
		if (pEvent->cbDestroy != NULL)
		{
			(pEvent->cbDestroy)(*iter);
		}
	}
	m_lstEvents.clear();
}

bool NDKEventFactory::_PopEvent(BCEventItemS &refEvent)
{
	if (m_lstEvents.size() > 0)
	{
		refEvent = m_lstEvents.front();
		m_lstEvents.pop_front();
		return true;
	}
	return false;
}

void NDKEventFactory::ProcessEvent(void *data)
{
	BCEventItemS sEvent(0);
	NDKEventFactory *_this = static_cast<NDKEventFactory *>(data);

	ASSERT(_this != NULL);
	_this->m_sEventLock.Lock();
	ASSERT(_this->m_nCtrls == 1);
	_this->m_nCtrls--;
	_this->m_nEventState = EVENT_STATE_RUNNING;
	while (_this->_PopEvent(sEvent))
	{
		_this->m_sEventLock.Unlock();
		if (sEvent.cbDestroy == TaskFuncWrap::TaskDetor)
		{
			TaskFuncWrap *pFuncEvent = (TaskFuncWrap *)sEvent.wParam;
			if (pFuncEvent)
			{
				pFuncEvent->RunTask();
			}
			BCDefEventProc(sEvent);
		}
		else if (_this->OnEventProcess(sEvent))
		{
			BCDefEventProc(sEvent);
		}
		_this->m_sEventLock.Lock();
	}
	_this->m_nEventState = EVENT_STATE_IDLE;
	_this->m_sEventLock.Unlock();
}

bool BCDefEventProc(BCEventItemS &refEvent)
{
	if (refEvent.cbDestroy != NULL)
	{
		(refEvent.cbDestroy)(refEvent);
	}
	return true;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file : BCEventQueue.cpp
///////////////////////////////////////////////////////////////////////////////
