///////////////////////////////////////////////////////////////////////////////
// file : UVExchanger.cpp
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "UVExchanger.h"



///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{


///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

enum
{
	EX_INTERNAL_STATE_WORKING	= 1,
	EX_INTERNAL_STATE_CLOSING	= 2,
	EX_INTERNAL_STATE_CLOSED	= 3,
};

enum
{
	EXM_SHUTDOWN		= 1,
};

///////////////////////////////////////////////////////////////////////////////
// class : UVEventFactory
///////////////////////////////////////////////////////////////////////////////

UVEventFactory::UVEventFactory()
	: m_nCtrls(0)
	, m_pLoop(NULL)
	, m_bAvailable(false)
	, m_nEventState(EVENT_STATE_IDLE)
{
	memzero(&m_sTaskEvent, sizeof(m_sTaskEvent));
}

UVEventFactory::~UVEventFactory()
{
	FlushEvents();
	uv_unref((uv_handle_t *)&m_sTaskEvent);
}

BCRESULT UVEventFactory::Create(uv_loop_t *loop)
{
	BCSpinMutex::Owner lock(m_sEventLock);
	m_pLoop = loop;
	uv_async_init(loop, &m_sTaskEvent, _EventCallback);
	m_sTaskEvent.data = this;
	uv_ref((uv_handle_t *)&m_sTaskEvent);
	m_bAvailable = true;

	return BC_R_SUCCESS;
}

uv_loop_t	* UVEventFactory::GetLoop() const
{
	return m_pLoop;
}

void UVEventFactory::Detach()
{
	BCSpinMutex::Owner lock(m_sEventLock);
	if (m_bAvailable)
	{
		_FlushEvents();
		m_bAvailable = false;
	}
	//LogDebug(_LOCAL_, "uv_close...");
	uv_close((uv_handle_t *)&m_sTaskEvent, _Close);
}

uint32_t UVEventFactory::FlushEvents(IExchangeHandler *pHandler)
{
	BCSpinMutex::Owner lock(m_sEventLock);
	return _FlushEvents(pHandler);
}

bool UVEventFactory::QueueEvent(const BCEventItemS &refEvent)
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
		uv_async_send(&m_sTaskEvent);
	}
	return true;
}

bool UVEventFactory::OnEventProcess(BCEventItemS &refEvent)
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

bool UVEventFactory::_PopEvent(BCEventItemS &refEvent)
{
	if (m_lstEvents.size() > 0)
	{
		refEvent = m_lstEvents.front();
		m_lstEvents.pop_front();
		return true;
	}
	return false;
}

uint32_t UVEventFactory::_FlushEvents(IExchangeHandler *pHandler /*= NULL*/)
{
	BCEventItemS *pEvent;
	EventList::iterator iter, iterEnd;
	uint32_t nSize = 0;

	if (pHandler)
	{
		EventList lstEvents;
		iter = m_lstEvents.begin();
		iterEnd = m_lstEvents.end();
		for (; iter != iterEnd; iter++)
		{
			pEvent = &(*iter);
			if (pEvent->priv == (uint64_t)pHandler)
			{
				if (pEvent->cbDestroy != NULL)
				{
					(pEvent->cbDestroy)(*iter);
				}
				nSize++;
			}
			else
			{
				lstEvents.push_back(*pEvent);
			}
		}
		m_lstEvents.clear();
		if (lstEvents.size() > 0)
		{
			iter = m_lstEvents.begin();
			iterEnd = m_lstEvents.end();
			for (; iter != iterEnd; iter++)
			{
				m_lstEvents.push_back(*iter);
			}
		}
	} 
	else
	{
		nSize = m_lstEvents.size();
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
	return nSize;
}

void UVEventFactory::_EventCallback(uv_async_t* handle)
{
	BCEventItemS sEvent(0);
	UVEventFactory *_this = static_cast<UVEventFactory *>(handle->data);

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

void UVEventFactory::_Close(uv_handle_t *handle)
{
	UVEventFactory *_this = (UVEventFactory *)handle->data;
	_this->OnUVClose();
}

///////////////////////////////////////////////////////////////////////////////
// Class : UVExchanger
///////////////////////////////////////////////////////////////////////////////

BCSpinMutex		UVExchanger::m_sLock;
UVExchanger	*	UVExchanger::m_pInstance = NULL;

UVExchanger::UVExchanger()
	: m_pThread(NULL)
	, m_startSem(0)
	, m_exitLock()
	, m_exitCond(&m_exitLock)
{
	//
}

UVExchanger::~UVExchanger()
{
	//
}

BCRESULT UVExchanger::CreateInstance(uv_loop_t *loop)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_pInstance)
	{
		return BC_R_ALREADYRUNNING;
	}
	m_pInstance = new UVExchanger();
	if (!m_pInstance)
	{
		return BC_R_NOMEMORY;
	}
	if (loop)
	{
		BCRESULT result = m_pInstance->Create(loop);
		if (result != BC_R_SUCCESS)
		{
			BC_SAFE_DELETE_PTR(m_pInstance);
			return result;
		}
	} 
	else
	{
		m_pInstance->_Initialize();
	}
	return BC_R_SUCCESS;
}

BCRESULT UVExchanger::ExchangeEvent(
	BCEventItemS &refEvent,
	IExchangeHandler *pHandler)
{
	if (!refEvent.priv && pHandler && pHandler->OnBeforeExchangeEvent(refEvent))
	{
		BCSpinMutex::Owner lock(m_sLock);
		if (m_pInstance)
		{
			bool bIgnore = (pHandler->m_internal_events_state > EX_INTERNAL_STATE_WORKING);
			m_sLock.Unlock();
			if (bIgnore)
			{
				BCDefEventProc(refEvent);
				m_sLock.Lock();
				return BC_R_IGNORE;
			}
			else
			{
				m_sLock.Lock();
				refEvent.priv = (uint64_t)pHandler;
				pHandler->m_internal_events_count++;
				bool result = m_pInstance->PostEvent(refEvent);
				return result ? BC_R_SUCCESS : BC_R_FAILURE;
			}
		}
	}
	else
	{
		BCDefEventProc(refEvent);
		return BC_R_IGNORE;
	}
	return BC_R_FAILURE;
}

BCRESULT UVExchanger::ExchangeShutdown(IExchangeHandler *pHandler)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_pInstance && pHandler)
	{
		if (pHandler->m_internal_events_state > EX_INTERNAL_STATE_WORKING)
		{
			return BC_R_ALREADYRUNNING;
		} 
		else
		{
			BCEventItemS sEvent(MAKEEVENT(EXM_SHUTDOWN, 0, 0), pHandler);
			sEvent.priv = (uint64_t)m_pInstance;
			pHandler->m_internal_events_state = EX_INTERNAL_STATE_CLOSING;
			bool result = m_pInstance->PostEvent(sEvent);
			return result ? BC_R_SUCCESS : BC_R_FAILURE;
		}
	}
	return BC_R_FAILURE;
}

uint32_t UVExchanger::RemoveEventByHandler(IExchangeHandler *pHandler)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_pInstance)
	{
		return m_pInstance->FlushEvents(pHandler);
	}
	return 0;
}

uv_loop_t * UVExchanger::GetLoop()
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_pInstance)
	{
		return ((UVEventFactory *)m_pInstance)->GetLoop();
	}
	return NULL;
}

bool UVExchanger::OnEventProcess(BCEventItemS &refEvent)
{
	if (refEvent.priv)
	{
		if (refEvent.priv)
		{
			if (refEvent.priv == (uint64_t)m_pInstance)
			{
				IExchangeHandler *pHandler = (IExchangeHandler *)refEvent.wParam;
				switch (EVENTMAJOR(refEvent.eType))
				{
				case EXM_SHUTDOWN:
					{
						m_sLock.Lock();
						pHandler->m_internal_events_state = EX_INTERNAL_STATE_CLOSED;
						ASSERT(pHandler->m_internal_events_count == 0);
						m_sLock.Unlock();
						pHandler->OnExchangeShutdown();
					}
					break;
				default:
					break;
				}
			}
			else
			{
				IExchangeHandler *pHandler = (IExchangeHandler *)refEvent.priv;
				m_sLock.Lock();
				pHandler->m_internal_events_count--;
				m_sLock.Unlock();
				return pHandler->OnExchangeEvent(refEvent);
			}
		}
		else
		{
			BCDefEventProc(refEvent);
		}
	}
	return true;
}

void UVExchanger::OnUVClose()
{
	//
}

void UVExchanger::Destroy()
{
	if (m_pInstance)
	{
		BCMutex::Owner lck(m_pInstance->m_exitLock);
		uv_loop_close(GetLoop());
		m_pInstance->m_exitCond.Wait();
	}
	BC_SAFE_DELETE_PTR(m_pInstance);
}

void UVExchanger::_ThreadProc(void *arg)
{
	UVExchanger *_this = (UVExchanger *)arg;

	uv_loop_t *loop = new uv_loop_t;
	uv_loop_init(loop);
	_this->m_pInstance->Create(loop);
	//Signal caller thread to continue
	_this->m_startSem.Post();
	// All subsequent activity takes place within the event loop:
	if (_this->m_pInstance)
	{
		uv_run(GetLoop(), UV_RUN_DEFAULT);
	}
	// Destroy loop instance
	BC_SAFE_DELETE_PTR(_this->m_pInstance->m_pLoop);
	// This function call does not return, unless, at some point in time, 
	// "s_exitCode" gets set to something non-zero.
	{
		BCMutex::Owner lck(_this->m_exitLock);
		_this->m_pThread = NULL;
		_this->m_exitCond.Signal();
	}
}

BCRESULT UVExchanger::_Initialize()
{
	BCRESULT result;

	m_pThread = new BCThread(_ThreadProc, this, BCThread::PRIORITY_HIGH);
	if (!m_pThread)
	{
		result = BC_R_NOMEMORY;
		goto return_error;
	}
	m_pThread->Start();
	m_startSem.Wait();

	return BC_R_SUCCESS;

return_error:

	return result;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
