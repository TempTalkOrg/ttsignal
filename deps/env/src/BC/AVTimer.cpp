///////////////////////////////////////////////////////////////////////////////
// file : AVTimer.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include "BC/Exports.h"
#include "BC/BCLog.h"
#include "BC/AVTimer.h"



///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

#define MIN_INTERVAL		1000

///////////////////////////////////////////////////////////////////////////////
// TimerQueue
///////////////////////////////////////////////////////////////////////////////

AVTimerStubQueue::AVTimerStubQueue()
	: m_nNextTimerId(1)
{
	//
}

AVTimerStubQueue::~AVTimerStubQueue()
{
	//
}

int AVTimerStubQueue::CheckTimeouts()
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_lstTimers.IsEmpty())
	{
		return MIN_INTERVAL;
	}
	int64_t now = bc_time_now();
	AVTimerStub *pFrontTimer = NULL;
	bool bContinue = false;
	while ((pFrontTimer = m_lstTimers.Begin()) && pFrontTimer->IsBefore(now))
	{
		m_lstTimers.PopFront();
		//m_sLock.Unlock();
#ifndef _DEBUG
		try
		{
#endif
			bContinue = pFrontTimer->Trigger(now);
#ifndef _DEBUG
		}
		catch(...)
		{
			LogInfo(_LOCAL_, "Exception raised:Invalid timer - maybe timer holder"
				" was destroyed!");
		}
#endif
		//m_sLock.Lock();
		if (bContinue)
		{
			pFrontTimer->due += pFrontTimer->interval;
			if (pFrontTimer->IsBefore(now))
			{
				// Time has jumped forwards!
				//LogInfo(_LOCAL_, "time has moved forwards!");
				pFrontTimer->due = now + pFrontTimer->interval;
			}
			_InsertTimer(pFrontTimer);
		}
		else if (m_lstTimers.IsEmpty())
		{
			return MIN_INTERVAL;
		}
	}
	return _GetNextTimeout();
}

void AVTimerStubQueue::ClearTimers()
{
	BCSpinMutex::Owner lock(m_sLock);
	m_lstTimers.Clear();
}

BCRESULT AVTimerStubQueue::AddTimer(
	IAVTimerHandler *pHandler, 
	int32_t interval,
	uint32_t &id,
	uint64_t userData)
{
	BCSpinMutex::Owner lock(m_sLock);
	AVTimerStub *pStub = new AVTimerStub(interval, pHandler, 
		m_nNextTimerId, userData);
	if (!pStub)
	{
		return BC_R_NOMEMORY;
	}
	id = m_nNextTimerId++;
	_InsertTimer(pStub);
	return BC_R_SUCCESS;
}

BCRESULT AVTimerStubQueue::RemoveTimer(
	IAVTimerHandler *pHandler,
	uint32_t id)
{
	BCSpinMutex::Owner lock(m_sLock);
	AVTimerStub *iter, *iterEnd;
	iterEnd = m_lstTimers.End();
	for (iter = m_lstTimers.Begin(); iter != iterEnd; iter = m_lstTimers.Next(iter))
	{
		if (iter->handler == pHandler && iter->id == id)
		{
			iter->RemoveFromList();
			delete iter;
			return BC_R_EXISTS;
		}
	}
	return BC_R_NOTFOUND;
}

void AVTimerStubQueue::Stop(AVTimerStub *pTimer)
{
	BCSpinMutex::Owner lock(m_sLock);
	pTimer->RemoveFromList();
}

bool AVTimerStubQueue::IsStarted(AVTimerStub *pTimer)
{
	BCSpinMutex::Owner lock(m_sLock);
	AVTimerStub *it, *pEnd = m_lstTimers.End();
	for (it = m_lstTimers.Begin(); it != pEnd; it = m_lstTimers.Next(it))
	{
		if (it == pTimer)
			return true;
	}
	return false;
}

void AVTimerStubQueue::_InsertTimer(AVTimerStub * pTimer)
{
	AVTimerStub *it, *pEnd = m_lstTimers.End();
	for (it = m_lstTimers.Begin(); it != pEnd; it = m_lstTimers.Next(it))
	{
		if (pTimer->IsBefore(it->due))
		{
			m_lstTimers.Insert(it, pTimer);
			return;
		}
	}
	m_lstTimers.PushBack(pTimer);
}

int AVTimerStubQueue::_GetNextTimeout()
{
	int64_t now = bc_time_now();
	if (m_lstTimers.IsEmpty())
	{
		return MIN_INTERVAL;
	}
	AVTimerStub *pFrontTimer = m_lstTimers.Begin();
	ASSERT(pFrontTimer);
	int64_t diff = pFrontTimer->due - now;
	int toWait = BCMAX(MIN_INTERVAL, diff);
	if (toWait > pFrontTimer->interval)
	{
		// Time has jumped backwards!
		//vlog.info("time has moved backwards!");
		pFrontTimer->due = now;
		toWait = MIN_INTERVAL;
	}
	return toWait;
}

///////////////////////////////////////////////////////////////////////////////
// class : AVTimer
///////////////////////////////////////////////////////////////////////////////

BCOnceS					AVTimer::m_sInitOnce(BC_ONCE_INIT_NEEDED, 1);
BCThread			*	AVTimer::m_pThread		= NULL;
AVTimer				*	AVTimer::m_pInstance	= NULL;

AVTimer::AVTimer()
	: m_sCondLock()
	, m_sCond(&m_sCondLock)
	, m_queueTimers()
	, m_sStartCond(&m_sCondLock)
	, m_sExitCond(&m_sCondLock)
	, m_bExit(false)
{
	bc_time_settoepoch(&m_tmDue);
}

AVTimer::~AVTimer()
{
	//
}

BCRESULT AVTimer::Start()
{
	bc_once_do(&m_sInitOnce, _InitOnce, NULL);
	return BC_R_SUCCESS;
}

BCRESULT AVTimer::Stop()
{
	if (m_pInstance)
	{
		{
			BCMutex::Owner lock(m_pInstance->m_sCondLock);
			m_pInstance->m_bExit = true;
			m_pInstance->m_sCond.Signal();
			m_pInstance->m_sExitCond.Wait();
		}
		BC_SAFE_DELETE_PTR(m_pInstance);
		m_pThread = NULL; // Can't delete
		m_sInitOnce.status = BC_ONCE_INIT_NEEDED;
		m_sInitOnce.counter = 1;
	}
	return BC_R_SUCCESS;
}

BCRESULT AVTimer::AddListener(
	IAVTimerHandler *pHandler, 
	uint32_t interval,
	uint32_t &id,
	uint64_t userData)
{
	if (!pHandler)
	{
		return BC_R_INVALIDARG;
	}
	bc_once_do(&m_sInitOnce, _InitOnce, NULL);
	if (m_pInstance)
	{
		return m_pInstance->_AddListener(pHandler, interval, 
			id, userData);
	}
	return BC_R_UNEXPECTED;
}

BCRESULT AVTimer::RemoveListener(
	IAVTimerHandler *pHandler,
	uint32_t id)
{
	if (!pHandler)
	{
		return BC_R_INVALIDARG;
	}
	bc_once_do(&m_sInitOnce, _InitOnce, NULL);
	if (m_pInstance)
	{
		return m_pInstance->_RemoveListener(pHandler, id);
	}
	return BC_R_UNEXPECTED;
}

BCRESULT AVTimer::_AddListener(
	IAVTimerHandler *pHandler, 
	uint32_t interval,
	uint32_t &id,
	uint64_t userData)
{
	m_queueTimers.AddTimer(pHandler, interval, id, userData);
	{
		BCMutex::Owner lock(m_sCondLock);
		m_sCond.Signal();
	}
	return BC_R_SUCCESS;
}

BCRESULT AVTimer::_RemoveListener(
	IAVTimerHandler *pHandler,
	uint32_t id)
{
	return m_queueTimers.RemoveTimer(pHandler, id);
}

void AVTimer::_RunTimer()
{
	BCIntervalS sInterval;
	uint32_t tmSecs = 0, tmNanosecs = 0;
	int result;
	bool bExit = false;

	{
		BCMutex::Owner lock(m_sCondLock);
		m_sStartCond.Signal();
	}
	bc_time_now(&m_tmDue);
	while (true)
	{
		m_sCondLock.Lock();
		while (BC_R_TIMEDOUT == m_sCond.TimedWait(&m_tmDue))
		{
			if (m_bExit)
			{
				break;
			}
			result = m_queueTimers.CheckTimeouts();
			tmSecs = result / 1000000;
			tmNanosecs = (result % 1000000) * 1000;
			bc_interval_set(&sInterval, tmSecs, tmNanosecs);
			bc_time_add(&m_tmDue, &sInterval, &m_tmDue);
		}
		if (m_bExit)
		{
			bExit = true;
		}
		m_sCondLock.Unlock();
		tmSecs = 0;
		tmNanosecs = 0;
		if (bExit)
		{
			break;
		}
	}
	{
		BCMutex::Owner lock(m_sCondLock);
		m_sExitCond.Signal();
	}
}

void AVTimer::_InitOnce(void*)
{
	m_pInstance = new AVTimer();
	m_pThread = new BCThread(_ThreadProc, m_pInstance, 
		BCThread::PRIORITY_HIGH);
	if (m_pThread)
	{
		BCMutex::Owner lock(m_pInstance->m_sCondLock);
		m_pThread->Start();
		m_pInstance->m_sStartCond.Wait();
	}
}

void AVTimer::_ThreadProc(LPVOID lpArg)
{
	AVTimer *pInstance = (AVTimer *)lpArg;
	if (pInstance == m_pInstance)
	{
		m_pInstance->_RunTimer();
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
