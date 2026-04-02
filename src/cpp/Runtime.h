#ifndef __RUNTIME_H_INCLUDED__
#define __RUNTIME_H_INCLUDED__

#include <BC/BCThread.h>
#include <BC/BCTask.h>
#include <BC/BCTimer.h>
#include <BC/BCSocket.h>
#include <BC/BCEventQueue.h>
#include <BC/BCFCodec.h>

using namespace BC;


///////////////////////////////////////////////////////////////////////////////
// Class : Runtime
///////////////////////////////////////////////////////////////////////////////

class Runtime : public BCEventQueue
{
	///////////////////////////////////////////////////////////////////////////////
	// class : Config
	///////////////////////////////////////////////////////////////////////////////
	class Config
	{
	public:
		Config() : workerThreads(1), taskThreads(16), timerThreads(2)
		{
			//
		}
		Config(const Config &other)
		{
			operator=(other);
		}
		~Config() {}

		int32_t			workerThreads;
		int32_t			taskThreads;
		int32_t			timerThreads;

		Config &operator = (const Config &other)
		{
			workerThreads = other.workerThreads;
			taskThreads = other.taskThreads;
			timerThreads = other.timerThreads;
			return *this;
		}

		BCRESULT		Init(BCFObject *pConfig)
		{
			BCFVar *pVar;

			pVar = pConfig->Get("workerThreads");
			if (IS_BCF_NUMBER(pVar))
			{
				workerThreads = GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("taskThreads");
			if (IS_BCF_NUMBER(pVar))
			{
				taskThreads = GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("timerThreads");
			if (IS_BCF_NUMBER(pVar))
			{
				timerThreads = GET_BCF_INT(pVar);
			}
			return BC_R_SUCCESS;
		}
	private:
		KBPool			sPool;
	};
public:
	Runtime();
	~Runtime();

	static BCRESULT				Initialize(BCFObject *pConfig);
	static void					Destroy();
	static BCTask			*   Task();
	static BCTaskMgr		*	TaskMgr();
	static BCTaskMgr		*	RandomTaskMgr();
	static BCTimerMgr		*	TimerMgr();
	static BCTimerMgr		*	RandomTimerMgr();
	static BCSocketMgr		*	SocketMgr();
	static void					PostTask(AsyncTaskFunc fTask);
	static void					SendTask(AsyncTaskFunc fTask);
	static BCRESULT				SendTaskWithResult(AsyncTaskWithResultFunc fTask);
	static BCRESULT				ScheduleTask(
									int32_t &taskId,
									TimerFuncWithId fTask, 
									uint64_t nIntervalUsec,
									bool bTicker = false);
	static BCRESULT				UnscheduleTask(int32_t &taskId, bool onlyCancel = false);
protected:
	// Override BCEventQueue interface
	bool				OnEventProcess(BCEventItemS &refEvent) override;
	void				OnEventProcShutdown() override;

	BCRESULT			Create(BCFObject *pConfig);
	BCRESULT			CreateTaskMgrs();
	BCRESULT			CreateTimerMgrs();
	void				DestroyTaskMgrs();
	void				DestroyTimerMgrs();
	BCTaskMgr		*	GetRandomTaskMgr();
	BCTimerMgr		*	GetRandomTimerMgr();
	void				Shutdown();
private:
	DECLARE_NO_COPY_CLASS(Runtime);
	static BCSpinMutex			s_sLock;
	static Runtime			*	s_pInstance;

	BCMutex						m_exitLock;
	BCCondition					m_exitCond;
	Config						m_sConfig;
	BCTaskMgr				*	m_pTaskMgr;
	BCTimerMgr				*	m_pTimerMgr;
	BCSocketMgr				*	m_pSocketMgr;
	bool						m_bIPv6;
	BCTaskMgr				**	m_taskMgrs;
	int32_t						m_nTaskMgrSize;
	int32_t						m_nNextTaskMgrIndex;
	BCTimerMgr				**	m_timerMgrs;
	int32_t						m_nTimerMgrSize;
	int32_t						m_nNextTimerMgrIndex;
};

#endif // __RUNTIME_H_INCLUDED__

