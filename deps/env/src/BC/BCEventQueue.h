///////////////////////////////////////////////////////////////////////////////
// file : BCEventQueue.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#ifndef BCEVENTQUEUE_H_INCLUDED__
#define BCEVENTQUEUE_H_INCLUDED__

#include <memory>
#include <map>
#include <functional>
#include <BC/BCTimer.h>
#include <BC/EventQueue.h>
#include <BC/BCFixedAlloc.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

class BCDelayTask;
typedef std::function<void()>			AsyncTaskFunc;
typedef std::function<BCRESULT()>		AsyncTaskWithResultFunc;
typedef std::function<void(int32_t)>	TimerFuncWithId;
typedef void(*LPFN_TimerFunc)(void *arg, int32_t);
typedef LPFN_TimerFunc					TimerFuncPtr;

///////////////////////////////////////////////////////////////////////////////
// class : TaskFuncWrap
///////////////////////////////////////////////////////////////////////////////

class TaskFuncWrap
{
	DECLARE_FIXED_ALLOC(TaskFuncWrap);
public:
	TaskFuncWrap(AsyncTaskFunc fTask, bool bWaitResult);
	TaskFuncWrap(AsyncTaskWithResultFunc fTask);
	virtual ~TaskFuncWrap();

	void							RunTask();
	std::shared_ptr<BCMutex>		GetLock();
	std::shared_ptr<BCCondition>	GetCondition();
	std::shared_ptr<BCRESULT>		GetResult();

	static void						TaskDetor(BCEventItemS &refEvent);
	static TaskFuncWrap			*	WrapTask(
										BCEventItemS &refEvent, 
										AsyncTaskFunc fTask,
										bool bWaitResult);
	static TaskFuncWrap			*	WrapTask(
										BCEventItemS &refEvent, 
										AsyncTaskWithResultFunc fTask);

private:
	DECLARE_NO_COPY_CLASS(TaskFuncWrap);
	std::shared_ptr<BCMutex>		m_pLock;
	std::shared_ptr<BCCondition>	m_pCond;
	std::shared_ptr<BCRESULT>		m_pResult;
	AsyncTaskFunc 					m_fTask;
	AsyncTaskWithResultFunc			m_fTaskWithResult;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCDelayTask
///////////////////////////////////////////////////////////////////////////////

class BCDelayTask
{
	DECLARE_FIXED_ALLOC(BCDelayTask);
public:
	BCDelayTask();
	~BCDelayTask();
	
	BCRESULT		Create(
						BCTask *pTask,
						BCTimerMgr *pTimerMgr, 
						int32_t nTimerId);
	BCRESULT		Schedule(
						TimerFuncWithId fTask,
						uint64_t nIntervalUsec,
						bool bTicker = false);
	BCRESULT		Schedule(
						TimerFuncPtr fTask,
						void * pUserData,
						uint64_t nIntervalUsec,
						bool bTicker = false);
	void			Cancel();
	void			Destroy();
	inline int32_t 	GetTimerId() const {
		return m_nTimerId;
	}
protected:
	void			OnTimeout();
	static void		_TimeoutCB(BCTask *, BCTaskEvent *);
private:
	DECLARE_NO_COPY_CLASS(BCDelayTask);

	BCTimer				*	m_pTimer;
	TimerFuncWithId			m_fTask;
    int32_t					m_nTimerId;
	TimerFuncPtr			m_pTaskFunc;
	void				*	m_pUserData;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCEventWrap
///////////////////////////////////////////////////////////////////////////////

class BCEventWrap
{
	DECLARE_FIXED_ALLOC(BCEventWrap);
public:
	BCEventWrap(const BCEventItemS &refEvent)
		: m_sEvent(std::move(refEvent)){}
	virtual ~BCEventWrap() {
		BCDefEventProc(m_sEvent);
	}

	BCEventItemS		m_sEvent;
private:
	DECLARE_NO_COPY_CLASS(BCEventWrap);
};

///////////////////////////////////////////////////////////////////////////////
// class : BCEventQueue
///////////////////////////////////////////////////////////////////////////////

class BC_API BCEventQueue 
	: public BCEventDispatcher
{
	enum
	{
		EVENT_STATE_IDLE		= 0,
		EVENT_STATE_RUNNING		= 1
	};

public:
	BCRESULT			Create(
							BCTimerMgr *pTimerMgr, 
							BCTaskMgr *pTaskMgr, 
							const char *name, 
							void *tag);
	BCTask			*	GetTask() const;
	void				PostTask(AsyncTaskFunc fTask);
	void				SendTask(AsyncTaskFunc fTask);
	BCRESULT			SendTaskWithResult(AsyncTaskWithResultFunc fTask);
	BCRESULT			ScheduleTask(
							int32_t &taskId,
							TimerFuncWithId fTask, 
							uint64_t nIntervalUsec,
							bool bTicker = false);
	BCRESULT			ScheduleTask(
							int32_t &taskId,
							TimerFuncPtr fTask, 
							void * pUserData,
							uint64_t nIntervalUsec,
							bool bTicker = false);
	BCRESULT			UnscheduleTask(int32_t &taskId, bool onlyCancel = false);
	void				UnscheduleAllTasks();
	void				Detach(bool bRemoveOnShutdown = false);
	void				FlushEvents(uint32_t eType = 0);
protected:
	BCEventQueue();
	virtual ~BCEventQueue();

	bool				QueueEvent(const BCEventItemS &refEvent) override;
	virtual bool		OnEventProcess(BCEventItemS &refEvent) override;
	virtual void		OnEventProcShutdown();
	void				_FlushEvents();
	void				_UnscheduleTasks();

	uint32_t			m_nCtrls;
private:
	DECLARE_NO_COPY_CLASS(BCEventQueue);

	bool				_PopEvent(BCEventItemS &);
	static void			_EventCallback(BCTask *, BCTaskEvent *);
	static void			_ShutdownCallback(BCTask *, BCTaskEvent *);

	typedef BCList<BCEventItemS>				EventList;
	typedef std::map<int32_t, BCDelayTask *>	DelayTaskMap;
	BCSpinMutex			m_sEventLock;
	EventList			m_lstEvents;
	BCTaskEvent			m_sTaskEvent;
	BCTimerMgr		*	m_pTimerMgr;
	BCTask			*	m_pTask;
	uint32_t			m_nEventState;
	DelayTaskMap		m_mapDelayTasks;
	int32_t				m_nNextTaskId;
};

///////////////////////////////////////////////////////////////////////////////
// class : NDKEventFactory
///////////////////////////////////////////////////////////////////////////////

class NDKEventFactory : public BCEventDispatcher
{
	enum 
	{ 
		EVENT_STATE_IDLE		= 0,
		EVENT_STATE_RUNNING		= 1
	};
public:
	BCRESULT			Create(
							LPFN_EventNotifyProc lpfnProc,
							void *pUserData);
	void				PostTask(AsyncTaskFunc fTask);
	void				SendTask(AsyncTaskFunc fTask);
	BCRESULT			SendTaskWithResult(AsyncTaskWithResultFunc fTask);
	void				Detach();
	void				FlushEvents(uint32_t eType = 0);
protected:
	NDKEventFactory();
	virtual ~NDKEventFactory();

	bool				QueueEvent(const BCEventItemS &refEvent);
	virtual bool		OnEventProcess(BCEventItemS &refEvent);
	void				_FlushEvents();

	uint32_t			m_nCtrls;

	bool				_PopEvent(BCEventItemS &);
	static void			ProcessEvent(void *data);

	typedef BCList<BCEventItemS>	EventList;
	BCSpinMutex				m_sEventLock;
	LPFN_EventNotifyProc	m_lpfnNotifyProc;
	void				*	m_pNotifyData;
	EventList				m_lstEvents;
	bool					m_bAvailable;
	uint32_t				m_nEventState;
private:
	DECLARE_NO_COPY_CLASS(NDKEventFactory);
};


///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BCEVENTQUEUE_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : BCEventQueue.h
///////////////////////////////////////////////////////////////////////////////
