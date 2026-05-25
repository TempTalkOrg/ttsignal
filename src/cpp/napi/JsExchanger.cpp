///////////////////////////////////////////////////////////////////////////////
// file : JsExchanger.cpp
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "Utils.h"
#include "JsExchanger.h"


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
// Class : JsExchanger
///////////////////////////////////////////////////////////////////////////////

BCSpinMutex		JsExchanger::m_sLock;
JsExchanger	*	JsExchanger::m_pInstance = NULL;

JsExchanger::JsExchanger() 
{
	//
}

JsExchanger::~JsExchanger()
{
	//
}

BCRESULT JsExchanger::CreateInstance()
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_pInstance)
	{
		return BC_R_ALREADYRUNNING;
	}
	m_pInstance = new JsExchanger();
	if (!m_pInstance)
	{
		return BC_R_NOMEMORY;
	}
	BCRESULT result = m_pInstance->Create(uv_default_loop());
	if (result != BC_R_SUCCESS)
	{
		BC_SAFE_DELETE_PTR(m_pInstance);
		return result;
	}
	return BC_R_SUCCESS;
}

BCRESULT JsExchanger::ExchangeEvent(
	BCEventItemS &refEvent,
	IExchangeHandler *pHandler)
{
	if (!refEvent.priv && pHandler && pHandler->OnBeforeExchangeEvent(refEvent))
	{
		m_sLock.Lock();
		if (m_pInstance)
		{
			bool bIgnore = (pHandler->m_internal_events_state > EX_INTERNAL_STATE_WORKING);
			if (bIgnore)
			{
				m_sLock.Unlock();
				BCDefEventProc(refEvent);
				return BC_R_IGNORE;
			}
			else
			{
				refEvent.priv = (uint64_t)pHandler;
				pHandler->m_internal_events_count++;
				bool result = m_pInstance->PostEvent(refEvent);
				m_sLock.Unlock();
				return result ? BC_R_SUCCESS : BC_R_FAILURE;
			}
		}
		m_sLock.Unlock();
	}
	else
	{
		BCDefEventProc(refEvent);
		return BC_R_IGNORE;
	}
	return BC_R_FAILURE;
}

BCRESULT JsExchanger::ExchangeShutdown(IExchangeHandler *pHandler)
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

bool JsExchanger::OnEventProcess(BCEventItemS &refEvent)
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

uint32_t JsExchanger::RemoveEventByHandler(IExchangeHandler *pHandler)
{
	if (m_pInstance)
	{
		return m_pInstance->_RemoveEventByHandler(pHandler);
	}
	return 0;
}

uint32_t JsExchanger::_RemoveEventByHandler(IExchangeHandler *pHandler)
{
	EventList sList;
	BCEventItemS sEvent(0);
	uint32_t nCount = 0;

	m_sEventLock.Lock();
	while (_PopEvent(sEvent))
	{
		if (sEvent.priv == (uint64_t)pHandler)
		{
			m_sEventLock.Unlock();
			m_sLock.Lock();
			pHandler->m_internal_events_count--;
			m_sLock.Unlock();
			BCDefEventProc(sEvent);
			m_sEventLock.Lock();
			nCount++;
		}
		else
		{
			sList.push_back(sEvent);
		}
	}
	while (sList.size() > 0)
	{
		sEvent = sList.front();
		m_lstEvents.push_back(sEvent);
		sList.pop_front();
	}
	m_sEventLock.Unlock();
	return nCount;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
