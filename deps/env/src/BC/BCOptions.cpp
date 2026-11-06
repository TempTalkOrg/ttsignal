
///////////////////////////////////////////////////////////////////////////////
// File : BCOptions.cpp
///////////////////////////////////////////////////////////////////////////////

#include <BC/BCOptions.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Class : BCOptionItem
///////////////////////////////////////////////////////////////////////////////

BCOptionItem::BCOptionItem()
	: m_lpszKey(NULL)
	, m_pOption(NULL)
{
	//
}

BCOptionItem::~BCOptionItem()
{
	BC_SAFE_DELETE_PTR(m_pOption);
}

BCRESULT BCOptionItem::Create(LPCSTR lpszKey)
{
	if (!lpszKey || strlen(lpszKey) == 0)
	{
		return BC_R_INVALIDARG;
	}
	BCSpinMutex::Owner lock(m_sOptionLock);
	m_lpszKey = lpszKey;
	return BC_R_SUCCESS;
}

BCRESULT BCOptionItem::SetOption(BCFVar *pOption)
{
	if (!pOption)
	{
		return BC_R_INVALIDARG;
	}
	ListenerList lstListeners;
	ListenerList::iterator iter, iterEnd;
	BCFVar *pOptCopy = NULL;
	BCPString strKey;
	m_sOptionLock.Lock();
	BC_SAFE_DELETE_PTR(m_pOption);
	m_pOption = pOption;
	// Copy listeners to new list
	iter = m_lstListeners.begin();
	iterEnd = m_lstListeners.end();
	for (;iter != iterEnd;iter++)
	{
		lstListeners.push_back(*iter);
	}
	// Clone key value
	strKey = m_lpszKey;
	// Clone option value
	pOptCopy = m_pOption->Clone();
	m_sOptionLock.Unlock();
	// Notify listeners
	iter = lstListeners.begin();
	iterEnd = lstListeners.end();
	for (;iter != iterEnd;iter++)
	{
		(*iter)->OnOptionChanged(strKey, pOptCopy->Clone());
	}
	BC_SAFE_DELETE_PTR(pOptCopy);
	return BC_R_SUCCESS;
}

BCRESULT BCOptionItem::AddListener( BCOptionListener *pListener )
{
	if (!pListener)
	{
		return BC_R_INVALIDARG;
	}
	BCSpinMutex::Owner lock(m_sOptionLock);
	m_lstListeners.remove(pListener);
	m_lstListeners.push_back(pListener);
	return BC_R_SUCCESS;
}

BCRESULT BCOptionItem::RemoveListener( BCOptionListener *pListener )
{
	if (!pListener)
	{
		return BC_R_INVALIDARG;
	}
	BCSpinMutex::Owner lock(m_sOptionLock);
	m_lstListeners.remove(pListener);
	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// Class : BCOptions
///////////////////////////////////////////////////////////////////////////////

BCOptions::BCOptions() 
{
	//
}

BCOptions::~BCOptions()
{
	Clear();
}

BCRESULT BCOptions::Create()
{
	return BC_R_SUCCESS;
}

BCRESULT BCOptions::SetIntOption( LPCSTR lpszKey, uint32_t nValue )
{
	BCOptionItem *pItem;

	if (!lpszKey || strlen(lpszKey) == 0)
	{
		return BC_R_INVALIDARG;
	}

	BCSpinMutex::Owner lock(m_sOptionLock);
	pItem = _EnsureOptionExists(lpszKey);
	if (!pItem)
	{
		return BC_R_NOMEMORY;
	}
	BCFInt *pInt = new BCFInt();
	if (!pInt)
	{
		return BC_R_NOMEMORY;
	}
	pInt->SetValue(nValue);
	return pItem->SetOption(pInt);
}

BCRESULT BCOptions::SetBooleanOption( LPCSTR lpszKey, bool bValue )
{
	BCOptionItem *pItem;

	if (!lpszKey || strlen(lpszKey) == 0)
	{
		return BC_R_INVALIDARG;
	}

	BCSpinMutex::Owner lock(m_sOptionLock);
	pItem = _EnsureOptionExists(lpszKey);
	if (!pItem)
	{
		return BC_R_NOMEMORY;
	}
	BCFVar *pBool = NULL;
	if (bValue)
	{
		pBool = new BCFTrue();
		if (!pBool)
		{
			return BC_R_NOMEMORY;
		}
	}
	else
	{
		pBool = new BCFFalse();
		if (!pBool)
		{
			return BC_R_NOMEMORY;
		}
	}
	return pItem->SetOption(pBool);
}

BCRESULT BCOptions::SetDoubleOption( LPCSTR lpszKey, float64_t dblValue )
{
	BCOptionItem *pItem;

	if (!lpszKey || strlen(lpszKey) == 0)
	{
		return BC_R_INVALIDARG;
	}

	BCSpinMutex::Owner lock(m_sOptionLock);
	pItem = _EnsureOptionExists(lpszKey);
	if (!pItem)
	{
		return BC_R_NOMEMORY;
	}
	BCFDouble *pDbl = new BCFDouble();
	if (!pDbl)
	{
		return BC_R_NOMEMORY;
	}
	pDbl->SetValue(dblValue);
	return pItem->SetOption(pDbl);
}

BCRESULT BCOptions::SetStringOption( LPCSTR lpszKey, LPCSTR lpszValue )
{
	BCOptionItem *pItem;

	if (!lpszKey || strlen(lpszKey) == 0)
	{
		return BC_R_INVALIDARG;
	}

	BCSpinMutex::Owner lock(m_sOptionLock);
	pItem = _EnsureOptionExists(lpszKey);
	if (!pItem)
	{
		return BC_R_NOMEMORY;
	}
	BCFString *pString = new BCFString();
	if (!pString)
	{
		return BC_R_NOMEMORY;
	}
	pString->SetValue(lpszValue);
	return pItem->SetOption(pString);
}

BCRESULT BCOptions::SetOption( LPCSTR lpszKey, BCFVar *pValue )
{
	BCOptionItem *pItem;

	if (!lpszKey || strlen(lpszKey) == 0 || !pValue)
	{
		return BC_R_INVALIDARG;
	}

	BCSpinMutex::Owner lock(m_sOptionLock);
	pItem = _EnsureOptionExists(lpszKey);
	if (!pItem)
	{
		return BC_R_NOMEMORY;
	}
	return pItem->SetOption(pValue);
}

BCFVar *BCOptions::GetOption( LPCSTR lpszKey )
{
	void *pItem;

	BCSpinMutex::Owner lock(m_sOptionLock);
	if (!m_htOptions.Find(&pItem, lpszKey) || !pItem)
	{
		return NULL;
	}
	BCFVar *pValue = ((BCOptionItem *)pItem)->GetOption();
	if (!pValue)
	{
		return NULL;
	}
	return pValue->Clone();
}

BCRESULT BCOptions::AddListener(
	LPCSTR lpszKey, 
	BCOptionListener *pListener, 
	BOOL bCreateIfNotExists)
{
	void *pItem;

	if (!lpszKey || strlen(lpszKey) == 0 || !pListener)
	{
		return BC_R_INVALIDARG;
	}

	BCSpinMutex::Owner lock(m_sOptionLock);
	if (!m_htOptions.Find(&pItem, lpszKey) || !pItem)
	{
		if (bCreateIfNotExists)
		{
			BCOptionItem *pEntry = _EnsureOptionExists(lpszKey);
			if (!pEntry)
			{
				return BC_R_NOMEMORY;
			}
			pEntry->SetOption(new BCFNull());
			pItem = pEntry;
		}
		else
		{
			return BC_R_NOTBOUND;
		}
	}
	return ((BCOptionItem *)pItem)->AddListener(pListener);
}

BCRESULT BCOptions::RemoveListener( LPCSTR lpszKey, BCOptionListener *pListener )
{
	void *pItem;

	BCSpinMutex::Owner lock(m_sOptionLock);
	if (!m_htOptions.Find(&pItem, lpszKey) || !pItem)
	{
		return BC_R_NOTBOUND;
	}
	return ((BCOptionItem *)pItem)->RemoveListener(pListener);
}

void BCOptions::Clear()
{
	BCSpinMutex::Owner lock(m_sOptionLock);
	BCOptionItem *pItem;
	BCStrHashTable::iterator iter, iterEnd;
	iter = m_htOptions.First();
	iterEnd = m_htOptions.End();
	for (;iter != iterEnd;iter = m_htOptions.Next(iter))
	{
		pItem = (BCOptionItem *)iter.Second();
		BC_SAFE_DELETE_PTR(pItem);
	}
	m_htOptions.Clear();
	m_sPool.Clear();
}

BCOptionItem *BCOptions::_EnsureOptionExists( LPCSTR lpszKey )
{
	void *pVoidItem;

	if (!m_htOptions.Find(&pVoidItem, lpszKey) || !pVoidItem)
	{
		BCOptionItem *pItem = new BCOptionItem();
		if (!pItem)
		{
			return NULL;
		}
		BCRESULT result = ((BCOptionItem *)pItem)->Create(m_sPool.Strdup(lpszKey));
		if (result != BC_R_SUCCESS)
		{
			BC_SAFE_DELETE_PTR(pItem);
			return NULL;
		}
		m_htOptions[lpszKey] = pItem;
		return pItem;
	}
	return (BCOptionItem *)pVoidItem;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

///////////////////////////////////////////////////////////////////////////////
// End of file : BCOptions.cpp
///////////////////////////////////////////////////////////////////////////////
