
#include "StdAfx.h"
#include <stdio.h>
#include <stdlib.h>
#include "BC/Exchanger.h"
#include "Runtime.h"



///////////////////////////////////////////////////////////////////////////////
// Class : Runtime
///////////////////////////////////////////////////////////////////////////////

BCSpinMutex					Runtime::s_sLock;
Runtime					*	Runtime::s_pInstance = NULL;

Runtime::Runtime()
	: m_exitLock()
	, m_exitCond(&m_exitLock)
	, m_pTaskMgr(NULL)
	, m_pTimerMgr(NULL)
	, m_pSocketMgr(NULL)
	, m_bIPv6(false)
	, m_taskMgrs(NULL)
	, m_nTaskMgrSize(0)
	, m_nNextTaskMgrIndex(0)
	, m_timerMgrs(NULL)
	, m_nTimerMgrSize(0)
	, m_nNextTimerMgrIndex(0)
{
}

Runtime::~Runtime()
{
}

BCRESULT Runtime::Initialize(BCFObject *pConfig)
{
	BCRESULT result;

	BCSpinMutex::Owner lock(s_sLock);
	if (s_pInstance)
	{
		return BC_R_SUCCESS;
	}
	s_pInstance = new Runtime();
	if (!s_pInstance)
	{
		result = BC_R_NOMEMORY;
		goto return_error;
	}
	result = s_pInstance->Create(pConfig);
	if (result != BC_R_SUCCESS)
	{
		goto delete_instance;
	}
	return BC_R_SUCCESS;

delete_instance:
	BC_SAFE_DELETE_PTR(s_pInstance);
return_error:
	return result;
}

void Runtime::Destroy()
{
	BCSpinMutex::Owner lck(s_sLock);
	if (s_pInstance)
	{
		s_pInstance->Shutdown();
		BC_SAFE_DELETE_PTR(s_pInstance);
	}
}

BCTask *Runtime::Task()
{
	if (s_pInstance)
	{
		return s_pInstance->GetTask();
	}
	return NULL;
}

BCTaskMgr *Runtime::TaskMgr()
{
	if (s_pInstance)
	{
		return s_pInstance->m_pTaskMgr;
	}
	return NULL;
}

BCTaskMgr *Runtime::RandomTaskMgr()
{
	if (s_pInstance)
	{
		return s_pInstance->GetRandomTaskMgr();
	}
	return NULL;
}

BCTimerMgr *Runtime::TimerMgr()
{
	if (s_pInstance)
	{
		return s_pInstance->m_pTimerMgr;
	}
	return NULL;
}

BCTimerMgr *Runtime::RandomTimerMgr()
{
	if (s_pInstance)
	{
		return s_pInstance->GetRandomTimerMgr();
	}
	return NULL;
}

BCSocketMgr *Runtime::SocketMgr()
{
	if (s_pInstance)
	{
		return s_pInstance->m_pSocketMgr;
	}
	return NULL;
}

void Runtime::PostTask(AsyncTaskFunc fTask)
{
	if (s_pInstance)
	{
		((BCEventQueue *)s_pInstance)->PostTask(fTask);
	}
}

void Runtime::SendTask(AsyncTaskFunc fTask)
{
	if (s_pInstance)
	{
		((BCEventQueue *)s_pInstance)->SendTask(fTask);
	}
}

BCRESULT Runtime::SendTaskWithResult(AsyncTaskWithResultFunc fTask)
{
	if (s_pInstance)
	{
		return ((BCEventQueue *)s_pInstance)->SendTaskWithResult(fTask);
	}
	return BC_R_FAILURE;
}

BCRESULT Runtime::ScheduleTask(
	int32_t &taskId, 
	TimerFuncWithId fTask, 
	uint64_t nIntervalUsec,
	bool bTicker /*= false*/)
{
	if (s_pInstance)
	{
		return ((BCEventQueue *)s_pInstance)->ScheduleTask(taskId, 
			std::move(fTask), nIntervalUsec, bTicker);
	}
	return BC_R_FAILURE;
}

BCRESULT Runtime::UnscheduleTask(int32_t &taskId, bool onlyCancel)
{
	if (s_pInstance)
	{
		return ((BCEventQueue *)s_pInstance)->UnscheduleTask(taskId, onlyCancel);
	}
	return BC_R_FAILURE;
}

bool Runtime::OnEventProcess(BCEventItemS &refEvent)
{
	BCDefEventProc(refEvent);
	return true;
}

void Runtime::OnEventProcShutdown()
{
	BCMutex::Owner lck(m_exitLock);
	m_exitCond.Signal();
}

BCRESULT Runtime::Create(BCFObject *pConfig)
{
	BCRESULT result;

	char szHost[64] = { 0 };
	int32_t iNetType = AF_UNSPEC;

	m_sConfig.Init(pConfig);
	memzero(szHost, sizeof(szHost));
	m_bIPv6 = iNetType == PF_INET6 ? true : false;

	m_pTaskMgr = new BCTaskMgr();
	if (!m_pTaskMgr)
	{
		result = BC_R_NOMEMORY;
		goto return_error;
	}
	result = m_pTaskMgr->Create(m_sConfig.workerThreads, 0, 
		BCThread::PRIORITY_NORMAL, "RuntimeThread");
	if (result != BC_R_SUCCESS)
	{
		goto destroy_taskmgr;
	}
	result = BCTimerMgr::Create(&m_pTimerMgr);
	if (result != BC_R_SUCCESS)
	{
		goto destroy_taskmgr;
	}
	m_pSocketMgr = new BCSocketMgr();
	if (!m_pSocketMgr)
	{
		result = BC_R_NOMEMORY;
		goto destroy_timermgr;
	}
	result = m_pSocketMgr->Create();
	if (result != BC_R_SUCCESS)
	{
		goto destroy_sockmgr;
	}
	result = CreateTaskMgrs();
	if (result != BC_R_SUCCESS)
	{
		goto destroy_taskmgrs;
	}
	result = CreateTimerMgrs();
	if (result != BC_R_SUCCESS)
	{
		goto destroy_timermgrs;
	}
	result = BCEventQueue::Create(m_pTimerMgr, m_pTaskMgr, "Runtime", this);
	if (result != BC_R_SUCCESS)
	{
		goto destroy_timermgrs;
	}
	return BC_R_SUCCESS;

destroy_timermgrs:
	DestroyTimerMgrs();
destroy_taskmgrs:
	DestroyTaskMgrs();
destroy_sockmgr:
	BCSocketMgr::Destroy(&m_pSocketMgr);
destroy_timermgr:
	BCTimerMgr::Destroy(&m_pTimerMgr);
destroy_taskmgr:
	BCTaskMgr::Destroy(&m_pTaskMgr);
return_error:
	return result;
}

BCRESULT Runtime::CreateTaskMgrs()
{
	BCRESULT result = BC_R_SUCCESS;

	if (m_taskMgrs || m_sConfig.taskThreads == 0)
	{
		return BC_R_SUCCESS;
	}
	m_nTaskMgrSize = m_sConfig.taskThreads;
	m_taskMgrs = (BCTaskMgr**)calloc(m_nTaskMgrSize, sizeof(BCTaskMgr *));
	if (!m_taskMgrs)
	{
		return BC_R_NOMEMORY;
	}
	int i = 0;
	for (int i = 0; i < m_nTaskMgrSize; i++)
	{
		m_taskMgrs[i] = new BCTaskMgr();
		if (!m_taskMgrs[i])
		{
			result = BC_R_NOMEMORY;
			goto destroy_taskmgrs;
		}
		result = m_taskMgrs[i]->Create(1, 0,
			BCThread::PRIORITY_NORMAL, "RuntimeSharedThread");
		if (result != BC_R_SUCCESS)
		{
			free(m_taskMgrs[i]);
			m_taskMgrs[i] = NULL;
			goto destroy_taskmgrs;
		}
	}

	return BC_R_SUCCESS;

destroy_taskmgrs:
	for (int j = 0; j < i; j++)
	{
		if (m_taskMgrs[j])
		{
			BCTaskMgr::Destroy(&m_taskMgrs[j]);
		}
	}
	free(m_taskMgrs);
	m_taskMgrs = NULL;
	m_nTaskMgrSize = 0;

	return result;
}

BCRESULT Runtime::CreateTimerMgrs()
{
	BCRESULT result = BC_R_SUCCESS;

	if (m_timerMgrs || m_sConfig.timerThreads == 0)
	{
		return BC_R_SUCCESS;
	}
	m_nTimerMgrSize = m_sConfig.timerThreads;
	m_timerMgrs = (BCTimerMgr**)calloc(m_nTimerMgrSize, sizeof(BCTimerMgr*));
	if (!m_timerMgrs)
	{
		return BC_R_NOMEMORY;
	}
	int i = 0;
	for (int i = 0; i < m_nTimerMgrSize; i++)
	{
		result = BCTimerMgr::Create(&m_timerMgrs[i]);
		if (result != BC_R_SUCCESS)
		{
			goto destroy_timermgrs;
		}
	}

	return BC_R_SUCCESS;

destroy_timermgrs:
	for (int j = 0; j < i; j++)
	{
		if (m_timerMgrs[j])
		{
			BCTimerMgr::Destroy(&m_timerMgrs[j]);
		}
	}
	free(m_timerMgrs);
	m_timerMgrs = NULL;
	m_nTimerMgrSize = 0;

	return result;
}

void Runtime::DestroyTaskMgrs()
{
	if (!m_taskMgrs) 
		return;
	for (int i = 0; i < m_nTaskMgrSize; i++)
	{
		if (m_taskMgrs[i])
		{
			BCTaskMgr::Destroy(&m_taskMgrs[i]);
		}
	}
	free(m_taskMgrs);
	m_taskMgrs = NULL;
	m_nTaskMgrSize = 0;
}

void Runtime::DestroyTimerMgrs()
{
	if (!m_timerMgrs) 
		return;
	for (int i = 0; i < m_nTimerMgrSize; i++)
	{
		if (m_timerMgrs[i])
		{
			BCTimerMgr::Destroy(&m_timerMgrs[i]);
		}
	}
	free(m_timerMgrs);
	m_timerMgrs = NULL;
	m_nTimerMgrSize = 0;
}

BCTaskMgr* Runtime::GetRandomTaskMgr()
{
	BCMutex::Owner lock(m_exitLock);
	if (!m_taskMgrs || m_nTaskMgrSize == 0)
	{
		return NULL;
	}
	return m_taskMgrs[m_nNextTaskMgrIndex++ % m_nTaskMgrSize];
}

BCTimerMgr* Runtime::GetRandomTimerMgr()
{
	BCMutex::Owner lock(m_exitLock);
	if (!m_timerMgrs || m_nTimerMgrSize == 0)
	{
		return NULL;
	}
	return m_timerMgrs[m_nNextTimerMgrIndex++ % m_nTimerMgrSize];
}

void Runtime::Shutdown()
{
	BCMutex::Owner lock(m_exitLock);
	Detach();
	m_exitCond.Wait();
	BCSocketMgr::Destroy(&m_pSocketMgr);
	BCTimerMgr::Destroy(&m_pTimerMgr);
	BCTaskMgr::Destroy(&m_pTaskMgr);
	DestroyTaskMgrs();
	DestroyTimerMgrs();
}
