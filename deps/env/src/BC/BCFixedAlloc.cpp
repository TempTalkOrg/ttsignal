
// fixalloc.cpp - implementation of fixed block allocator

#include <BC/Utils.h>
#include <BC/BCFCodec.h>
#include <BC/BCException.h>
#include <BC/BCFixedAlloc.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Class : BCFixedAllocList
///////////////////////////////////////////////////////////////////////////////

class BCFixedAllocList
{
public:
	BCFixedAllocList(){}
	~BCFixedAllocList(){}

	void			PushBack(const BCFixedAlloc *pItem)
	{
		BCSpinMutex::Owner lock(m_sLock);
		m_lstItems.PushBack((BCNodeList::Node *)pItem);
	}
	void			RemoveFromList(BCFixedAlloc *pItem)
	{
		BCSpinMutex::Owner lock(m_sLock);
		pItem->RemoveFromList();
	}
	BCFObject	*	Stat(uint32_t nFilter)
	{
		BCFObject *pStat = new BCFObject();

		BCSpinMutex::Owner lock(m_sLock);
		uint32_t nCount = m_lstItems.Count();
		if (nCount > 0)
		{
			BCFixedAlloc *pIter, *pEnd, /**pBCRef,*/ *pBCFDouble, 
				*pBCPString, *pBCFTableEntry;
			uint64_t totalSize = 0, totalUsage = 0;
			//pBCRef = Get("BCRef");
			pBCPString = Get("BCPString");
			pBCFDouble = Get("BCFDouble");
			pBCFTableEntry = Get("BCFTableEntry");
			// Pre-insert stat usage alloc
			//pStat->PutDouble("BCRef", pBCRef->GetUsageCount());
			pStat->PutDouble("BCFDouble", pBCFDouble->GetUsageCount());
			pStat->PutDouble("BCFTableEntry", pBCFTableEntry->GetUsageCount());
			pStat->PutDouble("BCPString", pBCPString->GetUsageCount());
			pStat->PutDouble("totalUsage", 0);
			pStat->PutDouble("totalAlloc", 0);
			pIter = m_lstItems.Begin();
			pEnd = m_lstItems.End();
			for (;pIter != pEnd;pIter = m_lstItems.Next(pIter))
			{
				if (pIter->GetUsageCount() >= nFilter)
				{
					pStat->PutDouble(pIter->GetClsName(), pIter->GetUsageCount());
				}
				totalUsage += pIter->GetAllocSize()*pIter->GetUsageCount();
				totalSize += pIter->GetAllocSize()*pIter->GetAllocCount();
			}
			// update stat usage alloc
			//pStat->PutDouble("BCRef", pBCRef->GetUsageCount());
			pStat->PutDouble("BCFDouble", pBCFDouble->GetUsageCount());
			pStat->PutDouble("BCFTableEntry", pBCFTableEntry->GetUsageCount());
			pStat->PutDouble("BCPString", pBCPString->GetUsageCount());
			pStat->Qsort(_QSortFunc);
			pStat->PutDouble("totalUsage", totalUsage);
			pStat->PutDouble("totalAlloc", totalSize);
		}
		
		return pStat;
	}
	uint64_t		FreeAll(uint32_t nRemaining)
	{
		uint64_t totalSize = 0;
		BCFixedAlloc *pIter, *pEnd;
		BCSpinMutex::Owner lock(m_sLock);
		pIter = m_lstItems.Begin();
		pEnd = m_lstItems.End();
		for (; pIter != pEnd; pIter = m_lstItems.Next(pIter))
		{
			totalSize += pIter->FreeAll(nRemaining);
		}
		return totalSize;
	}
protected:
	static int _QSortFunc(LPCVOID lpParam1, LPCVOID lpParam2)
	{
		BCFTableEntry *pEntry1 = *(BCFTableEntry **)lpParam1;
		BCFTableEntry *pEntry2 = *(BCFTableEntry **)lpParam2;
		uint64_t nCount1 = (uint64_t)((BCFDouble *)pEntry1->GetValue())->GetValue();
		uint64_t nCount2 = (uint64_t)((BCFDouble *)pEntry2->GetValue())->GetValue();

		if (nCount1 > nCount2)
		{
			return -1;
		}
		else if (nCount1 == nCount2)
		{
			return 0;
		}
		else
		{
			return 1;
		}
	}
	BCFixedAlloc	*	Get(LPCSTR lpClsName)
	{
		BCFixedAlloc *pIter, *pEnd;
		pIter = m_lstItems.Begin();
		pEnd = m_lstItems.End();
		for (;pIter != pEnd;pIter = m_lstItems.Next(pIter))
		{
			if (!strcmp(pIter->GetClsName(), lpClsName))
			{
				return pIter;
			}
		}
		return NULL;
	}
private:
	DECLARE_NO_COPY_CLASS(BCFixedAllocList);
	BCSpinMutex					m_sLock;
	TNodeList<BCFixedAlloc>		m_lstItems;
};

static BCFixedAllocList &GetFixedAllocList()
{
	static BCFixedAllocList sList;
	return sList;
}

/////////////////////////////////////////////////////////////////////////////
// CPlex

CPlex* PASCAL CPlex::Create(CPlex*& pHead, UINT_PTR nMax, UINT_PTR cbElement)
{
	ASSERT(nMax > 0 && cbElement > 0);
	if (nMax == 0 || cbElement == 0)
	{
		throw BCException(__FUNCTION__, "Invalid arguments");
	}

	CPlex* p = (CPlex*) new BYTE[sizeof(CPlex) + nMax * cbElement];
			// may throw exception
	p->pNext = pHead;
	pHead = p;  // change head (adds in reverse order for simplicity)
	return p;
}

void CPlex::FreeDataChain()     // free this one and links
{
	CPlex* p = this;
	while (p != NULL)
	{
		BYTE* bytes = (BYTE*) p;
		CPlex* pNextBlock = p->pNext;
		delete[] bytes;
		p = pNextBlock;
	}
}

/////////////////////////////////////////////////////////////////////////////
// BCFixedAllocNoSync

BCFixedAllocNoSync::BCFixedAllocNoSync(UINT nAllocSize, UINT nBlockSize)
	: m_nAllocCount(0)
	, m_nUsageCount(0)
{
	//ASSERT(nAllocSize >= sizeof(CNode));
	ASSERT(nBlockSize > 1);

	if (nAllocSize < sizeof(CNode))
		nAllocSize = sizeof(CNode);
	if (nBlockSize <= 1)
		nBlockSize = 64;

	m_nAllocSize = nAllocSize;
	m_nBlockSize = nBlockSize;
	m_pNodeFree = NULL;
	m_pBlocks = NULL;
}

BCFixedAllocNoSync::~BCFixedAllocNoSync()
{
	FreeAll();
}

uint32_t BCFixedAllocNoSync::FreeAll(uint32_t nRemaining)
{
	if (m_pBlocks)
	{
		m_pBlocks->FreeDataChain();
		m_pBlocks = NULL;
		return m_nAllocCount * m_nAllocSize;
	} 
	else
	{
		size_t nCount = 0;
		CNode* p = m_pNodeFree;
		for (uint32_t i = 0;p != NULL;i++)
		{
			CNode* pNext = p->pNext;
			if (i + 1 == nRemaining)
			{
				p->pNext = NULL;
			} 
			else if (i >= nRemaining)
			{
				BYTE* bytes = (BYTE*)p;
				delete[] bytes;
				nCount++;
			}
			p = pNext;
		}
		m_nAllocCount -= nCount;
		return nCount * m_nAllocSize;
	}
}

void* BCFixedAllocNoSync::Alloc()
{
	if (m_pNodeFree == NULL)
	{
		// add another block
		CPlex* pNewBlock = CPlex::Create(m_pBlocks, m_nBlockSize, m_nAllocSize);

		// chain them into free list
		CNode* pNode = (CNode*)pNewBlock->data();
		// free in reverse order to make it easier to debug
		(BYTE*&)pNode += (m_nAllocSize * m_nBlockSize) - m_nAllocSize;
		for (int i = m_nBlockSize-1; i >= 0; i--, (BYTE*&)pNode -= m_nAllocSize)
		{
			pNode->pNext = m_pNodeFree;
			m_pNodeFree = pNode;
		}
		m_nAllocCount += m_nBlockSize;
	}
	ASSERT(m_pNodeFree != NULL);  // we must have something

	// remove the first available node from the free list
	void* pNode = m_pNodeFree;
	m_pNodeFree = m_pNodeFree->pNext;
	m_nUsageCount++;
	return pNode;
}

void BCFixedAllocNoSync::Free(void* p)
{
	if (p != NULL)
	{
		// simply return the node to the free list
		CNode* pNode = (CNode*)p;
		pNode->pNext = m_pNodeFree;
		m_pNodeFree = pNode;
		m_nUsageCount--;
	}
}

void* BCFixedAllocNoSync::AllocNode()
{
	if (m_pNodeFree == NULL)
	{
		// chain them into free list
		CNode* pNode = (CNode*)new BYTE[m_nAllocSize];
		pNode->pNext = m_pNodeFree;
		m_pNodeFree = pNode;
		m_nAllocCount += 1;
	}
	ASSERT(m_pNodeFree != NULL);  // we must have something

	// remove the first available node from the free list
	void* pNode = m_pNodeFree;
	m_pNodeFree = m_pNodeFree->pNext;
	m_nUsageCount++;
	return pNode;
}

void BCFixedAllocNoSync::FreeNode(void* p)
{
	if (p != NULL)
	{
		// simply return the node to the free list
		CNode* pNode = (CNode*)p;
		pNode->pNext = m_pNodeFree;
		m_pNodeFree = pNode;
		m_nUsageCount--;
	}
}

/////////////////////////////////////////////////////////////////////////////
// BCFixedAlloc

BCFixedAlloc::BCFixedAlloc(LPCSTR lpszClsName, UINT nAllocSize, UINT nBlockSize)
	: base(nAllocSize, nBlockSize)
{
	strncpy(m_szClsName, lpszClsName, strlen(lpszClsName));
	GetFixedAllocList().PushBack(this);
}

BCFixedAlloc::~BCFixedAlloc()
{
	GetFixedAllocList().RemoveFromList(this);
}

uint32_t BCFixedAlloc::FreeAll(uint32_t nRemaining)
{
	BCSpinMutex::Owner lock(m_protect);
	try
	{
		return base::FreeAll(nRemaining);
	}
	catch( ... )
	{
		throw;
	}
	return 0;
}

void* BCFixedAlloc::Alloc()
{
	BCSpinMutex::Owner lock(m_protect);
	void* p = NULL;
	try
	{
		p = base::AllocNode();
	}
	catch(...)
	{
		throw;
	}

	return p;
}

void BCFixedAlloc::Free(void* p)
{
	if (p != NULL)
	{
		BCSpinMutex::Owner lock(m_protect);
		try
		{
			base::FreeNode(p);
		}
		catch( ... )
		{
			throw;
		}
	}
}

uint64_t BCFixedAlloc::GetAllocCount() const
{
	return m_nAllocCount;
}

uint64_t BCFixedAlloc::GetUsageCount() const
{
	return m_nUsageCount;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : BC

///////////////////////////////////////////////////////////////////////////////
// Get fixed alloc statistics :
///////////////////////////////////////////////////////////////////////////////

BC_API
BCFObject *BCStatFixedAlloc(uint32_t nFilter)
{
	return GetFixedAllocList().Stat(nFilter);
}


BC_API
void BCFreeAllFixedAlloc(uint32_t eRemaining)
{
	GetFixedAllocList().FreeAll(eRemaining);
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
