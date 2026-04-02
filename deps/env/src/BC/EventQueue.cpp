///////////////////////////////////////////////////////////////////////////////
// file : EventQueue.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////

#include <BC/BCLog.h>
#include "EventQueue.h"



///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////



///////////////////////////////////////////////////////////////////////////////
// class : BCEventItemS
///////////////////////////////////////////////////////////////////////////////

BCEventItemS::BCEventItemS()
	: eType(BCM_IDLE)
	, wParam(0)
	, lParam(0)
	, priv(0)
	, cbDestroy(NULL)
	, cbCopy(NULL)
{
	memzero(vParams, sizeof(vParams));
}

BCEventItemS::BCEventItemS(const BCEventItemS &other)
{
	operator=(other);
}

BCEventItemS::BCEventItemS(
	uint32_t _eType,
	void *_wParam,
	void *_lParam,
	LPFN_BCEventProcPtr _cbDestroy,
	LPFN_BCEventCopyPtr _cbCopy)
	: eType(_eType)
	, wParam((uint64_t)_wParam)
	, lParam((uint64_t)_lParam)
	, priv(0)
	, cbDestroy(_cbDestroy)
	, cbCopy(_cbCopy)
{
	memzero(vParams, sizeof(vParams));
}

BCEventItemS::BCEventItemS(
	uint32_t _eType,
	uint64_t _wParam,
	uint64_t _lParam,
	LPFN_BCEventProcPtr _cbDestroy,
	LPFN_BCEventCopyPtr _cbCopy)
	: eType(_eType)
	, wParam(_wParam)
	, lParam(_lParam)
	, priv(0)
	, cbDestroy(_cbDestroy)
	, cbCopy(_cbCopy)
{
	memzero(vParams, sizeof(vParams));
}

BCEventItemS::~BCEventItemS()
{
	//
}

LPVOID BCEventItemS::AllocBuffer(size_t nSize)
{
	if (!pool)
	{
		pool.reset(new KBPool);
	}
	if (pool)
	{
		return pool->Calloc(nSize);
	}
	return NULL;
}

LPSTR BCEventItemS::CopyString(LPCSTR lpszStr, int len)
{
	ASSERT(lpszStr);
	if (len <= 0)
	{
		len = strlen(lpszStr);
	}
	LPSTR lpBuf = (LPSTR)AllocBuffer(len + 1);
	memcpy(lpBuf, lpszStr, len);
	lpBuf[len] = '\0';
	return lpBuf;
}

LPVOID BCEventItemS::CopyBuffer(LPCVOID lpData, size_t len)
{
	ASSERT(lpData);
	if (len <= 0)
	{
		return NULL;
	}
	LPSTR lpBuf = (LPSTR)AllocBuffer(len);
	memcpy(lpBuf, lpData, len);
	return lpBuf;
}

BCEventItemS & BCEventItemS::operator=(const BCEventItemS &other)
{
	if (other.cbCopy)
	{
		(other.cbCopy)(other, *this);
	}
	else
	{
		eType = other.eType;
		wParam = other.wParam;
		lParam = other.lParam;
		memcpy2(vParams, other.vParams, sizeof(vParams));
		priv = other.priv;
		cbDestroy = other.cbDestroy;
		cbCopy = other.cbCopy;
		pool = other.pool;
	}
	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCEventDispatcher
///////////////////////////////////////////////////////////////////////////////

bool BCEventDispatcher::PostEvent(
	uint32_t eCtrlType,
	uint64_t dwParam1,
	uint64_t dwParam2,
	LPFN_BCEventProcPtr cbDestroy /*= NULL*/)
{
	BCEventItemS sEvent(eCtrlType, dwParam1, dwParam2, cbDestroy);
	return QueueEvent(sEvent);
}

bool BCEventDispatcher::PostEvent(
	uint32_t eCtrlType,
	void *wParam,
	void *lParam,
	LPFN_BCEventProcPtr cbDestroy /*= NULL*/)
{
	BCEventItemS sEvent(eCtrlType, wParam, lParam, cbDestroy);
	return QueueEvent(sEvent);
}

bool BCEventDispatcher::PostEvent(const BCEventItemS &sEvent)
{
	return QueueEvent(sEvent);
}

///////////////////////////////////////////////////////////////////////////////
// class : BCEventFactory
///////////////////////////////////////////////////////////////////////////////

BCEventFactory::BCEventFactory()
	: m_nCtrls(0)
	, m_pTask(NULL)
	, m_nEventState(EVENT_STATE_IDLE)
{
	m_sTaskEvent.ev_sender = this;
	m_sTaskEvent.ev_type = BC_EVENTCLASS_EVQUEUE;
	m_sTaskEvent.ev_action = _EventCallback;
	m_sTaskEvent.ev_arg = this;
}

BCEventFactory::~BCEventFactory()
{
	_FlushEvents();
	Detach();
}

BCRESULT BCEventFactory::Create(BCTaskMgr *pTaskMgr, const char *name, void *tag)
{
	BCSpinMutex::Owner lock(m_sEventLock);
	if (m_pTask)
	{
		return BC_R_EXISTS;
	}
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

BCTask *BCEventFactory::GetTask() const
{
	return m_pTask;
}

void BCEventFactory::Detach(bool bRemoveOnShutdown)
{
	BCSpinMutex::Owner lock(m_sEventLock);
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

void BCEventFactory::FlushEvents(uint32_t eType)
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

bool BCEventFactory::QueueEvent(const BCEventItemS &refEvent)
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

bool BCEventFactory::OnEventProcess(BCEventItemS &refEvent)
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

void BCEventFactory::OnEventProcShutdown()
{

}

void BCEventFactory::_FlushEvents()
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

bool BCEventFactory::_PopEvent(BCEventItemS &refEvent)
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

void BCEventFactory::_EventCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCEventItemS sEvent(0);
	BCEventFactory *_this = (BCEventFactory *)pEvent->ev_sender;

	ASSERT(_this != NULL);
	_this->m_sEventLock.Lock();
	ASSERT(_this->m_nCtrls == 1);
	_this->m_nCtrls--;
	_this->m_nEventState = EVENT_STATE_RUNNING;
	while(_this->_PopEvent(sEvent))
	{
		_this->m_sEventLock.Unlock();
		if (_this->OnEventProcess(sEvent))
		{
			BCDefEventProc(sEvent);
		}
		_this->m_sEventLock.Lock();
	}
	_this->m_nEventState = EVENT_STATE_IDLE;
	_this->m_sEventLock.Unlock();
}

void BCEventFactory::_ShutdownCallback(BCTask *pTask, BCTaskEvent *pEvent)
{
	BCEventFactory *_this = (BCEventFactory *)pEvent->ev_arg;

	UNUSED(pTask);
	ASSERT(_this != NULL);

#ifndef _DEBUG
	try
	{
#endif
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
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file : EventQueue.cpp
///////////////////////////////////////////////////////////////////////////////
