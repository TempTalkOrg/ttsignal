
#include <BC/Utils.h>
#include <BC/BCPArray.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class : BCByteArray
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCByteArray, 10);

/////////////////////////////////////////////////////////////////////////////

BCByteArray::BCByteArray()
	: m_sPool1(NULL, G_p1KBMemPoolAllocator)
	, m_sPool2(NULL, G_p1KBMemPoolAllocator)
	, m_bUsePool1(TRUE)
{
	m_pData = NULL;
	m_nSize = m_nMaxSize = 0;
	m_nGrowBy = 0;
}

BCByteArray::~BCByteArray()
{
	//
}

void BCByteArray::SetSize(INT_PTR nNewSize, INT_PTR nGrowBy)
{
	ASSERT(this);
	ASSERT(nNewSize >= 0);

	if(nNewSize < 0 )
		throw -1;

	if (nGrowBy >= 0)
		m_nGrowBy = nGrowBy;  // set new size

	if (nNewSize == 0)
	{
		// shrink to nothing
		m_sPool1.Clear();
		m_sPool2.Clear();
		m_pData = NULL;
		m_nSize = m_nMaxSize = 0;
		m_bUsePool1 = TRUE;
	}
	else if (m_pData == NULL)
	{
		// create one with exact size
#ifdef SIZE_T_MAX
		ASSERT(nNewSize <= SIZE_T_MAX/sizeof(BYTE));    // no overflow
#endif
		ASSERT(m_bUsePool1);
		m_pData = (BYTE*)m_sPool1.Alloc(nNewSize * sizeof(BYTE));

		memset(m_pData, 0, nNewSize * sizeof(BYTE));  // zero fill

		m_nSize = m_nMaxSize = nNewSize;
	}
	else if (nNewSize <= m_nMaxSize)
	{
		// it fits
		if (nNewSize > m_nSize)
		{
			// initialize the new elements

			memset(&m_pData[m_nSize], 0, (nNewSize-m_nSize) * sizeof(BYTE));

		}

		m_nSize = nNewSize;
	}
	else
	{
		// otherwise, grow array
		nGrowBy = m_nGrowBy;
		if (nGrowBy == 0)
		{
			// heuristically determine growth when nGrowBy == 0
			//  (this avoids heap fragmentation in many situations)
			nGrowBy = BCMIN(1024, BCMAX(4, m_nSize / 8));
		}
		INT_PTR nNewMax;
		if (nNewSize < m_nMaxSize + nGrowBy)
			nNewMax = m_nMaxSize + nGrowBy;  // granularity
		else
			nNewMax = nNewSize;  // no slush

		ASSERT(nNewMax >= m_nMaxSize);  // no wrap around

		if(nNewMax  < m_nMaxSize)
			throw -1;

#ifdef SIZE_T_MAX
		ASSERT(nNewMax <= SIZE_T_MAX/sizeof(BYTE)); // no overflow
#endif
		BYTE * pNewData = NULL;
		if (m_bUsePool1)
		{
			pNewData = (BYTE *) m_sPool2.Alloc(nNewMax * sizeof(BYTE));

			// copy new data from old
			memcpy2(pNewData, m_pData, m_nSize * sizeof(BYTE));

			// get rid of old stuff (note: no destructors called)
			m_sPool1.Clear();
			m_bUsePool1 = FALSE;
		}
		else
		{
			pNewData = (BYTE *) m_sPool1.Alloc(nNewMax * sizeof(BYTE));

			// copy new data from old
			memcpy2(pNewData, m_pData, m_nSize * sizeof(BYTE));

			// get rid of old stuff (note: no destructors called)
			m_sPool2.Clear();
			m_bUsePool1 = TRUE;
		}

		// construct remaining elements
		ASSERT(nNewSize > m_nSize);

		memset(&pNewData[m_nSize], 0, (nNewSize-m_nSize) * sizeof(BYTE));

		m_pData = pNewData;
		m_nSize = nNewSize;
		m_nMaxSize = nNewMax;
	}
}

INT_PTR BCByteArray::Append(LPCVOID lpData, size_t nSize)
{
	ASSERT(this);

	INT_PTR nOldSize = m_nSize;
	SetSize(m_nSize + nSize);

	memcpy2(m_pData + nOldSize, lpData, nSize);

	return nOldSize;
}

INT_PTR BCByteArray::Append(const BCByteArray& src)
{
	ASSERT(this);
	ASSERT(this != &src);   // cannot append to itself

	if(this == &src)
		throw -1;

	INT_PTR nOldSize = m_nSize;
	SetSize(m_nSize + src.m_nSize);

	memcpy2(m_pData + nOldSize, src.m_pData, src.m_nSize * sizeof(BYTE));

	return nOldSize;
}

void BCByteArray::Copy(const BCByteArray& src)
{
	ASSERT(this);
	ASSERT(this != &src);   // cannot append to itself

	if(this != &src)
	{
		SetSize(src.m_nSize);

		memcpy2(m_pData, src.m_pData, src.m_nSize * sizeof(BYTE));
	}
}

void BCByteArray::FreeExtra()
{
	ASSERT(this);

	if (m_nSize != m_nMaxSize)
	{
		// shrink to desired size
#ifdef SIZE_T_MAX
		ASSERT(m_nSize <= SIZE_T_MAX/sizeof(BYTE)); // no overflow
#endif
		BYTE * pNewData = NULL;
		if (m_bUsePool1)
		{
			if (m_nSize != 0)
			{
				pNewData = (BYTE *) m_sPool2.Alloc(m_nSize * sizeof(BYTE));
				// copy new data from old
				memcpy2(pNewData, m_pData, m_nSize * sizeof(BYTE));

				// get rid of old stuff (note: no destructors called)
				m_sPool1.Clear();
				m_bUsePool1 = FALSE;
			}
		}
		else
		{
			if (m_nSize != 0)
			{
				pNewData = (BYTE *) m_sPool1.Alloc(m_nSize * sizeof(BYTE));
				// copy new data from old
				memcpy2(pNewData, m_pData, m_nSize * sizeof(BYTE));

				// get rid of old stuff (note: no destructors called)
				m_sPool2.Clear();
				m_bUsePool1 = TRUE;
			}
		}
		m_pData = pNewData;
		m_nMaxSize = m_nSize;
	}
}

/////////////////////////////////////////////////////////////////////////////

void BCByteArray::SetAtGrow(INT_PTR nIndex, BYTE newElement)
{
	ASSERT(this);
	ASSERT(nIndex >= 0);

	if(nIndex < 0)
		throw -1;

	if (nIndex >= m_nSize)
		SetSize(nIndex+1);
	m_pData[nIndex] = newElement;
}

void BCByteArray::InsertAt(INT_PTR nIndex, BYTE newElement, INT_PTR nCount)
{
	ASSERT(this);
	ASSERT(nIndex >= 0);    // will expand to meet need
	ASSERT(nCount > 0);     // zero or negative size not allowed

	if(nIndex < 0 || nCount <= 0)
		throw -1;

	if (nIndex >= m_nSize)
	{
		// adding after the end of the array
		SetSize(nIndex + nCount);  // grow so nIndex is valid
	}
	else
	{
		// inserting in the middle of the array
		INT_PTR nOldSize = m_nSize;
		SetSize(m_nSize + nCount);  // grow it to new size
		// shift old data up to fill gap
		memmove(&m_pData[nIndex+nCount], &m_pData[nIndex],
			(nOldSize-nIndex) * sizeof(BYTE));

		// re-init slots we copied from

		memset(&m_pData[nIndex], 0, nCount * sizeof(BYTE));

	}

	// insert new value in the gap
	ASSERT(nIndex + nCount <= m_nSize);

	// copy elements into the empty space
	while (nCount--)
		m_pData[nIndex++] = newElement;
}

void BCByteArray::RemoveAt(INT_PTR nIndex, INT_PTR nCount)
{
	ASSERT(this);
	ASSERT(nIndex >= 0);
	ASSERT(nCount >= 0);
	ASSERT(nIndex + nCount <= m_nSize);

	if(nIndex < 0 || nCount < 0 || (nIndex + nCount > m_nSize))
		throw -1;

	// just remove a range
	INT_PTR nMoveCount = m_nSize - (nIndex + nCount);

	if (nMoveCount)
		memmove(&m_pData[nIndex], &m_pData[nIndex + nCount],
		(size_t)nMoveCount * sizeof(BYTE));
	m_nSize -= nCount;
}

void BCByteArray::InsertAt(INT_PTR nStartIndex, BCByteArray* pNewArray)
{
	ASSERT(this);
	ASSERT(pNewArray != NULL);
	ASSERT(nStartIndex >= 0);

	if(pNewArray == NULL || nStartIndex < 0)
		throw -1;

	if (pNewArray->GetSize() > 0)
	{
		InsertAt(nStartIndex, pNewArray->GetAt(0), pNewArray->GetSize());
		for (INT_PTR i = 0; i < pNewArray->GetSize(); i++)
			SetAt(nStartIndex + i, pNewArray->GetAt(i));
	}
}

/////////////////////////////////////////////////////////////////////////////
// Diagnostics

#ifdef _DEBUG
void BCByteArray::AssertValid() const
{
	if (m_pData == NULL)
	{
		ASSERT(m_nSize == 0);
		ASSERT(m_nMaxSize == 0);
	}
	else
	{
		ASSERT(m_nSize >= 0);
		ASSERT(m_nMaxSize >= 0);
		ASSERT(m_nSize <= m_nMaxSize);
		//ASSERT(BCIsValidAddress(m_pData, m_nMaxSize * sizeof(BYTE)));
	}
}
#endif //_DEBUG

///////////////////////////////////////////////////////////////////////////////
// class : BCPtrArray
//
// Implementation of parameterized Array
//
/////////////////////////////////////////////////////////////////////////////
// NOTE: we allocate an array of 'm_nMaxSize' elements, but only
//  the current size 'm_nSize' contains properly constructed
//  objects.
/////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCPtrArray, 100);

BCPtrArray::BCPtrArray()
	: m_sPool1(NULL, G_p1KBMemPoolAllocator)
	, m_sPool2(NULL, G_p1KBMemPoolAllocator)
	, m_bUsePool1(TRUE)
{
	m_pData = NULL;
	m_nSize = m_nMaxSize = 0;
	m_nGrowBy = 1024;
}

BCPtrArray::~BCPtrArray()
{
	//
}

void BCPtrArray::SetSize(INT_PTR nNewSize, INT_PTR nGrowBy)
{
	ASSERT(nNewSize >= 0);

	if(nNewSize < 0 )
		throw -1;

	if (nGrowBy >= 0)
		m_nGrowBy = nGrowBy;  // set new size

	if (nNewSize == 0)
	{
		// shrink to nothing
		m_sPool1.Clear();
		m_sPool2.Clear();
		m_pData = NULL;
		m_nSize = m_nMaxSize = 0;
		m_bUsePool1 = TRUE;
	}
	else if (m_pData == NULL)
	{
		// create one with exact size
#ifdef SIZE_T_MAX
		ASSERT(nNewSize <= SIZE_T_MAX/sizeof(void*));    // no overflow
#endif
		ASSERT(m_bUsePool1);
		m_pData = (void**)m_sPool1.Alloc(nNewSize * sizeof(void*));

		memset(m_pData, 0, nNewSize * sizeof(void*));  // zero fill

		m_nSize = m_nMaxSize = nNewSize;
	}
	else if (nNewSize <= m_nMaxSize)
	{
		// it fits
		if (nNewSize > m_nSize)
		{
			// initialize the new elements

			memset(&m_pData[m_nSize], 0, (nNewSize-m_nSize) * sizeof(void*));
		}

		m_nSize = nNewSize;
	}
	else
	{
		// otherwise, grow array
		nGrowBy = m_nGrowBy;
		if (nGrowBy == 0)
		{
			// heuristically determine growth when nGrowBy == 0
			//  (this avoids heap fragmentation in many situations)
			nGrowBy = BCMIN(1024, BCMAX(4, m_nSize / 8));
		}
		INT_PTR nNewMax;
		if (nNewSize < m_nMaxSize + nGrowBy)
			nNewMax = m_nMaxSize + nGrowBy;  // granularity
		else
			nNewMax = nNewSize;  // no slush

		ASSERT(nNewMax >= m_nMaxSize);  // no wrap around

		if(nNewMax  < m_nMaxSize)
			throw -1;

#ifdef SIZE_T_MAX
		ASSERT(nNewMax <= SIZE_T_MAX/sizeof(void*)); // no overflow
#endif
		void** pNewData = NULL;
		if (m_bUsePool1)
		{
			pNewData = (void**) m_sPool2.Alloc(nNewMax * sizeof(void*));

			// copy new data from old
			memcpy2(pNewData, m_pData, m_nSize * sizeof(void*));

			// get rid of old stuff (note: no destructors called)
			m_sPool1.Clear();
			m_bUsePool1 = FALSE;
		}
		else
		{
			pNewData = (void**) m_sPool1.Alloc(nNewMax * sizeof(void*));

			// copy new data from old
			memcpy2(pNewData, m_pData, m_nSize * sizeof(void*));

			// get rid of old stuff (note: no destructors called)
			m_sPool2.Clear();
			m_bUsePool1 = TRUE;
		}

		// construct remaining elements
		ASSERT(nNewSize > m_nSize);

		memset(&pNewData[m_nSize], 0, (nNewSize-m_nSize) * sizeof(void*));

		m_pData = pNewData;
		m_nSize = nNewSize;
		m_nMaxSize = nNewMax;
	}
}

INT_PTR BCPtrArray::Append(const BCPtrArray& src)
{
	ASSERT(this != &src);   // cannot append to itself

	if(this == &src)
		throw -1;

	INT_PTR nOldSize = m_nSize;
	SetSize(m_nSize + src.m_nSize);

	memcpy2(m_pData + nOldSize, src.m_pData, src.m_nSize * sizeof(void*));

	return nOldSize;
}

void BCPtrArray::Copy(const BCPtrArray& src)
{
	ASSERT(this != &src);   // cannot append to itself

	if(this != &src)
	{
		SetSize(src.m_nSize);

		memcpy2(m_pData, src.m_pData, src.m_nSize * sizeof(void*));
	}

}

void BCPtrArray::FreeExtra()
{
	if (m_nSize != m_nMaxSize)
	{
		// shrink to desired size
#ifdef SIZE_T_MAX
		ASSERT(m_nSize <= SIZE_T_MAX/sizeof(void*)); // no overflow
#endif
		void** pNewData = NULL;
		if (m_bUsePool1)
		{
			if (m_nSize != 0)
			{
				pNewData = (void**) m_sPool2.Alloc(m_nSize * sizeof(void*));
				// copy new data from old
				memcpy2(pNewData, m_pData, m_nSize * sizeof(void*));

				// get rid of old stuff (note: no destructors called)
				m_sPool1.Clear();
				m_bUsePool1 = FALSE;
			}
		}
		else
		{
			if (m_nSize != 0)
			{
				pNewData = (void**) m_sPool1.Alloc(m_nSize * sizeof(void*));
				// copy new data from old
				memcpy2(pNewData, m_pData, m_nSize * sizeof(void*));

				// get rid of old stuff (note: no destructors called)
				m_sPool2.Clear();
				m_bUsePool1 = TRUE;
			}
		}
		m_pData = pNewData;
		m_nMaxSize = m_nSize;
	}
}

/////////////////////////////////////////////////////////////////////////////

void BCPtrArray::SetAtGrow(INT_PTR nIndex, void* newElement)
{
	ASSERT(nIndex >= 0);

	if(nIndex < 0)
		throw -1;

	if (nIndex >= m_nSize)
		SetSize(nIndex+1);
	m_pData[nIndex] = newElement;
}

void BCPtrArray::InsertAt(INT_PTR nIndex, void* newElement, INT_PTR nCount)
{
	ASSERT(nIndex >= 0);    // will expand to meet need
	ASSERT(nCount > 0);     // zero or negative size not allowed

	if(nIndex < 0 || nCount <= 0)
		throw -1;

	if (nIndex >= m_nSize)
	{
		// adding after the end of the array
		SetSize(nIndex + nCount);  // grow so nIndex is valid
	}
	else
	{
		// inserting in the middle of the array
		INT_PTR nOldSize = m_nSize;
		SetSize(m_nSize + nCount);  // grow it to new size
		// shift old data up to fill gap
		memmove(&m_pData[nIndex+nCount], &m_pData[nIndex],
			(nOldSize-nIndex) * sizeof(void*));

		// re-init slots we copied from

		memset(&m_pData[nIndex], 0, nCount * sizeof(void*));

	}

	// insert new value in the gap
	ASSERT(nIndex + nCount <= m_nSize);

	// copy elements into the empty space
	while (nCount--)
		m_pData[nIndex++] = newElement;
}

void BCPtrArray::RemoveAt(INT_PTR nIndex, INT_PTR nCount)
{
	ASSERT(nIndex >= 0);
	ASSERT(nCount >= 0);
	ASSERT(nIndex + nCount <= m_nSize);

	if(nIndex < 0 || nCount < 0 || (nIndex + nCount > m_nSize))
		throw -1;

	// just remove a range
	INT_PTR nMoveCount = m_nSize - (nIndex + nCount);

	if (nMoveCount)
		memmove(&m_pData[nIndex], &m_pData[nIndex + nCount],
		nMoveCount * sizeof(void*));
	m_nSize -= nCount;
}

void BCPtrArray::InsertAt(INT_PTR nStartIndex, BCPtrArray* pNewArray)
{
	ASSERT(pNewArray != NULL);
	ASSERT(nStartIndex >= 0);

	if(pNewArray == NULL || nStartIndex < 0)
		throw -1;

	if (pNewArray->GetSize() > 0)
	{
		InsertAt(nStartIndex, pNewArray->GetAt(0), pNewArray->GetSize());
		for (INT_PTR i = 0; i < pNewArray->GetSize(); i++)
			SetAt(nStartIndex + i, pNewArray->GetAt(i));
	}
}

/////////////////////////////////////////////////////////////////////////////
// Diagnostics

#ifdef _DEBUG
void BCPtrArray::AssertValid() const
{
	if (m_pData == NULL)
	{
		ASSERT(m_nSize == 0);
		ASSERT(m_nMaxSize == 0);
	}
	else
	{
		ASSERT(m_nSize >= 0);
		ASSERT(m_nMaxSize >= 0);
		ASSERT(m_nSize <= m_nMaxSize);
		//ASSERT(BCIsValidAddress(m_pData, m_nMaxSize * sizeof(void*)));
	}
}
#endif //_DEBUG


///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
