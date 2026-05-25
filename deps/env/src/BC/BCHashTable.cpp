
#include <BC/BCHashTable.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

#define INITIAL_MAX 15 /* tunable == 2^n - 1 */

static inline uint32_t HashFuncDefault(
	const void *char_key,
	int32_t *klen);
static uint32_t HashFuncCrc(
	const void *char_key,
	int32_t *klen);
static uint32_t HashFuncCrcFast(
	const void *char_key,
	int32_t *klen);


//////////////////////////////////////////////////////////////////////////
// BCHashTable
//////////////////////////////////////////////////////////////////////////

BCHashTable::BCHashTable(
	HashFuncE hashFunc /*= HF_DEFAULT*/)
	: m_pEntryArray(NULL)
	, m_nCount(0)
	, m_nMax(INITIAL_MAX)
	, m_pHashFunc(NULL)
	, m_pFreeList(NULL)
{
	switch(hashFunc)
	{
	case HF_CRC:
		m_pHashFunc = HashFuncCrc;
		break;
	case HF_CRC_FAST:
		m_pHashFunc = HashFuncCrcFast;
		break;
	default:
		m_pHashFunc = HashFuncDefault;
		break;
	}
	this->m_pEntryArray = AllocEntryArray(m_nMax);
}

BCHashTable::~BCHashTable()
{
	//
}

/*
* Hash creation functions.
*/

BCHashTable::HashEntryType **BCHashTable::AllocEntryArray(
	uint32_t max)
{
	return (HashEntryType **)m_sMemoryPool.Calloc(sizeof(*m_pEntryArray) * (max + 1));
}

/*
* Hash iteration functions.
*/

BCHashTable::HashIndexType * BCHashTable::Next(HashIndexType *pHashIndex)
{
	pHashIndex->pSelf = pHashIndex->pNext;
	while (!pHashIndex->pSelf)
	{
		if (pHashIndex->nIndex > pHashIndex->pHashTable->m_nMax)
		{
			return NULL;
		}
		pHashIndex->pSelf = pHashIndex->pHashTable->m_pEntryArray[pHashIndex->nIndex++];
	}
	pHashIndex->pNext = pHashIndex->pSelf->pNext;
	return pHashIndex;
}

BCHashTable::HashIndexType * BCHashTable::First(
	const StackMemPool &sMemoryPool)
{
	HashIndexType *pHashIndex;
	if (sMemoryPool)
	{
		StackMemPool sPool = sMemoryPool;
		pHashIndex = (HashIndexType *)sPool.Calloc(sizeof(*pHashIndex));
	}
	else
	{
		pHashIndex = &m_iterator;
	}

	pHashIndex->pHashTable = this;
	pHashIndex->nIndex = 0;
	pHashIndex->pSelf = NULL;
	pHashIndex->pNext = NULL;
	return Next(pHashIndex);
}

void BCHashTable::Get(
	HashIndexType *pHashIndex,
	const void **key,
	int32_t *klen,
	const void **val)
{
	if (key)
	{
		*key  = pHashIndex->pSelf->pKey;
	}
	if (klen)
	{
		*klen = pHashIndex->pSelf->nKeyLen;
	}
	if (val)
	{
		*val  = (void *)pHashIndex->pSelf->pValue;
	}
}


/*
* Expanding a hash table
*/

void BCHashTable::ExpandEntryArray()
{
	HashIndexType *pHashIndex;
	HashEntryType **new_array;
	uint32_t new_max;

	new_max = m_nMax * 2 + 1;
	new_array = AllocEntryArray(new_max);
	for (pHashIndex = First(NULLPOOL); pHashIndex; pHashIndex = Next(pHashIndex))
	{
		uint32_t i = pHashIndex->pSelf->nHashValue & new_max;
		pHashIndex->pSelf->pNext = new_array[i];
		new_array[i] = pHashIndex->pSelf;
	}
	m_pEntryArray = new_array;
	m_nMax = new_max;
}

/*
* Get an entry with key buffer big enough from free entry list
*
*/

BCHashTable::HashEntryType *BCHashTable::GetFreeEntry(uint32_t nKeyLen)
{
	HashEntryType *pPrev = m_pFreeList, *pCurrent = m_pFreeList;
	if (m_pFreeList)
	{
		if (m_pFreeList->nKeyBufLen >= nKeyLen)
		{
			pCurrent = m_pFreeList;
			m_pFreeList = pCurrent->pNext;
			return pCurrent;
		}
		else
		{
			pPrev = m_pFreeList;
			pCurrent = pPrev->pNext;
		}

		while(pCurrent)
		{
			if (pCurrent->nKeyBufLen >= nKeyLen)
			{
				pPrev->pNext = pCurrent->pNext;
				return pCurrent;
			}
			pPrev = pCurrent;
			pCurrent = pCurrent->pNext;
		}
	}
	return NULL;
}

/*
* This is where we keep the details of the hash function and control
* the maximum collision rate.
*
* If pValue is non-NULL it creates and initializes a new hash entry if
* there isn't already one there; it returns an updatable pointer so
* that hash entries can be removed.
*/

BCHashTable::HashEntryType **BCHashTable::FindEntry(
	const void *pKey,
	int32_t nKeyLen,
	const void *pValue)
{
	HashEntryType **ppHashEntry, *pHashEntry;
	uint32_t nHashValue;

	nHashValue = m_pHashFunc(pKey, &nKeyLen);

	/* scan linked list */
	for (ppHashEntry = &m_pEntryArray[nHashValue & m_nMax], pHashEntry = *ppHashEntry;
		pHashEntry; ppHashEntry = &pHashEntry->pNext, pHashEntry = *ppHashEntry)
	{
		if (pHashEntry->nHashValue == nHashValue && pHashEntry->nKeyLen == nKeyLen
			&& memcmp(pHashEntry->pKey, pKey, nKeyLen) == 0)
			break;
	}
	if (pHashEntry || !pValue)
	{
		return ppHashEntry;
	}

	/* add a new entry for non-NULL values */
	if ((pHashEntry = GetFreeEntry(nKeyLen)) != NULL)
	{
		memcpy2(pHashEntry->pKey, pKey, nKeyLen);
	}
	else
	{
		pHashEntry = (HashEntryType *)m_sMemoryPool.Calloc(sizeof(*pHashEntry));
		pHashEntry->pKey  = m_sMemoryPool.memdup(pKey, nKeyLen);
		pHashEntry->nKeyBufLen = nKeyLen;
	}
	pHashEntry->pNext = NULL;
	pHashEntry->nHashValue = nHashValue;
	pHashEntry->nKeyLen = nKeyLen;
	pHashEntry->pValue  = NULL;
	*ppHashEntry = pHashEntry;
	m_nCount++;
	return ppHashEntry;
}

void * BCHashTable::Get(
	const void *pKey,
	int32_t nKeyLen)
{
	HashEntryType *pHashEntry;
	pHashEntry = *FindEntry(pKey, nKeyLen, NULL);
	if (pHashEntry)
	{
		return (void *)pHashEntry->pValue;
	}
	else
	{
		return NULL;
	}
}

void *BCHashTable::Get(int32_t nKey)
{
	return Get(&nKey, sizeof(int32_t));
}

void BCHashTable::Set(
	const void *pKey,
	int32_t nKeyLen,
	const void *pValue)
{
	HashEntryType **ppHashEntry = FindEntry(pKey, nKeyLen, pValue);
	if (*ppHashEntry)
	{
		if (!pValue)
		{
			/* delete entry */
			HashEntryType *pOldEntry = *ppHashEntry;
			*ppHashEntry = (*ppHashEntry)->pNext;
			pOldEntry->pNext = m_pFreeList;
			m_pFreeList = pOldEntry;
			--m_nCount;
		}
		else
		{
			/* replace entry */
			(*ppHashEntry)->pValue = pValue;
			/* check that the collision rate isn't too high */
			if (m_nCount > m_nMax)
			{
				ExpandEntryArray();
			}
		}
	}
	/* else pKey not present and pValue==NULL */
}

void BCHashTable::Set(int32_t nKey, const void *pValue)
{
	Set(&nKey, sizeof(int32_t), pValue);
}

uint32_t BCHashTable::Count() const
{
	return m_nCount;
}

void BCHashTable::Clear()
{
	HashIndexType *pHashIndex;
	for (pHashIndex = First(NULLPOOL); pHashIndex; pHashIndex = Next(pHashIndex))
	{
		Set(pHashIndex->pSelf->pKey, pHashIndex->pSelf->nKeyLen, NULL);
	}
}

//////////////////////////////////////////////////////////////////////////
// BCOrderedHashTable
//////////////////////////////////////////////////////////////////////////

BCOrderedHashTable::BCOrderedHashTable(
	HashFuncE hashFunc /*= HF_DEFAULT*/)
		: m_sMemoryPool(true, true)
		, m_pEntryArray(NULL)
		, m_nCount(0)
		, m_nMax(INITIAL_MAX)
		, m_pHashFunc(NULL)
		, m_pFreeList(NULL)
		, m_pBegin(NULL)
		, m_pEnd(NULL)
{
	switch(hashFunc)
	{
	case HF_CRC:
		m_pHashFunc = HashFuncCrc;
		break;
	case HF_CRC_FAST:
		m_pHashFunc = HashFuncCrcFast;
		break;
	default:
		m_pHashFunc = HashFuncDefault;
		break;
	}
	this->m_pEntryArray = AllocEntryArray(m_nMax);
}

BCOrderedHashTable::~BCOrderedHashTable()
{
	//
}

/*
* Hash creation functions.
*/

BCOrderedHashTable::HashEntryType **BCOrderedHashTable::AllocEntryArray(
	uint32_t max)
{
	return (HashEntryType **)m_sMemoryPool.Calloc(sizeof(*m_pEntryArray) * (max + 1));
}

/*
* Hash iteration functions.
*/

BCOrderedHashTable::HashIndexType * BCOrderedHashTable::Next(
	HashIndexType *pHashIndex)
{
	pHashIndex->pSelf = pHashIndex->pNext;
	while (!pHashIndex->pSelf)
	{
		if (pHashIndex->nIndex > pHashIndex->pHashTable->m_nMax)
		{
			return NULL;
		}
		pHashIndex->pSelf = pHashIndex->pHashTable->m_pEntryArray[pHashIndex->nIndex++];
	}
	pHashIndex->pNext = pHashIndex->pSelf->pNext;
	return pHashIndex;
}

BCOrderedHashTable::HashIndexType * BCOrderedHashTable::First(
	const StackMemPool &sMemoryPool)
{
	HashIndexType *pHashIndex;
	if (sMemoryPool)
	{
		StackMemPool sPool = sMemoryPool;
		pHashIndex = (HashIndexType *)sPool.Calloc(sizeof(*pHashIndex));
	}
	else
	{
		pHashIndex = &m_iterator;
	}

	pHashIndex->pHashTable = this;
	pHashIndex->nIndex = 0;
	pHashIndex->pSelf = NULL;
	pHashIndex->pNext = NULL;
	return Next(pHashIndex);
}

void BCOrderedHashTable::Get(
	HashIndexType *pHashIndex,
	const void **ppKey,
	int32_t *pnKeyLen,
	const void **ppValue)
{
	if (ppKey)
	{
		*ppKey  = pHashIndex->pSelf->pKey;
	}
	if (pnKeyLen)
	{
		*pnKeyLen = pHashIndex->pSelf->nKeyLen;
	}
	if (ppValue)
	{
		*ppValue  = (void *)pHashIndex->pSelf->pValue;
	}
}


/*
* Expanding a hash table
*/

void BCOrderedHashTable::ExpandEntryArray()
{
	HashIndexType *pHashIndex;
	HashEntryType **new_array;
	uint32_t new_max;

	new_max = m_nMax * 2 + 1;
	new_array = AllocEntryArray(new_max);
	for (pHashIndex = First(NULLPOOL); pHashIndex; pHashIndex = Next(pHashIndex))
	{
		uint32_t i = pHashIndex->pSelf->nHashValue & new_max;
		pHashIndex->pSelf->pNext = new_array[i];
		new_array[i] = pHashIndex->pSelf;
	}
	m_pEntryArray = new_array;
	m_nMax = new_max;
}

/*
* Get an entry with key buffer big enough from free entry list
*
*/

BCOrderedHashTable::HashEntryType *BCOrderedHashTable::GetFreeEntry(
	uint32_t nKeyLen)
{
	HashEntryType *pPrev = m_pFreeList, *pCurrent = m_pFreeList;
	if (m_pFreeList)
	{
		if (m_pFreeList->nKeyBufLen >= nKeyLen)
		{
			pCurrent = m_pFreeList;
			m_pFreeList = pCurrent->pNext;
			return pCurrent;
		}
		else
		{
			pPrev = m_pFreeList;
			pCurrent = pPrev->pNext;
		}

		while(pCurrent)
		{
			if (pCurrent->nKeyBufLen >= nKeyLen)
			{
				pPrev->pNext = pCurrent->pNext;
				return pCurrent;
			}
			pPrev = pCurrent;
			pCurrent = pCurrent->pNext;
		}
	}
	return NULL;
}

/*
* This is where we keep the details of the hash function and control
* the maximum collision rate.
*
* If pValue is non-NULL it creates and initializes a new hash entry if
* there isn't already one there; it returns an updatable pointer so
* that hash entries can be removed.
*/

BCOrderedHashTable::HashEntryType **BCOrderedHashTable::FindEntry(
	const void *pKey,
	int32_t nKeyLen,
	const void *pValue)
{
	HashEntryType **ppHashEntry, *pHashEntry;
	uint32_t nHashValue;

	nHashValue = m_pHashFunc(pKey, &nKeyLen);

	/* scan linked list */
	for (ppHashEntry = &m_pEntryArray[nHashValue & m_nMax], pHashEntry = *ppHashEntry;
		pHashEntry; ppHashEntry = &pHashEntry->pNext, pHashEntry = *ppHashEntry)
	{
		if (pHashEntry->nHashValue == nHashValue && pHashEntry->nKeyLen == nKeyLen
			&& memcmp(pHashEntry->pKey, pKey, nKeyLen) == 0)
			break;
	}
	if (pHashEntry || !pValue)
	{
		return ppHashEntry;
	}

	/* add a new entry for non-NULL values */
	if ((pHashEntry = GetFreeEntry(nKeyLen)) != NULL)
	{
		memcpy2(pHashEntry->pKey, pKey, nKeyLen);
	}
	else
	{
		pHashEntry = (HashEntryType *)m_sMemoryPool.Calloc(sizeof(*pHashEntry));
		pHashEntry->pKey  = m_sMemoryPool.memdup(pKey, nKeyLen);
		pHashEntry->nKeyBufLen = nKeyLen;
	}
	pHashEntry->pNext = NULL;
	pHashEntry->nHashValue = nHashValue;
	pHashEntry->nKeyLen = nKeyLen;
	pHashEntry->pValue  = NULL;
	pHashEntry->pLeft = NULL;
	pHashEntry->pRight = NULL;
	*ppHashEntry = pHashEntry;
	m_nCount++;
	return ppHashEntry;
}

void * BCOrderedHashTable::Get(
	const void *pKey,
	int32_t nKeyLen)
{
	HashEntryType *pHashEntry;
	pHashEntry = *FindEntry(pKey, nKeyLen, NULL);
	if (pHashEntry)
	{
		return (void *)pHashEntry->pValue;
	}
	else
	{
		return NULL;
	}
}

void *BCOrderedHashTable::Get(int32_t nKey)
{
	return Get(&nKey, sizeof(int32_t));
}

void BCOrderedHashTable::Set(
	const void *pKey,
	int32_t nKeyLen,
	const void *pValue)
{
	HashEntryType **ppHashEntry = FindEntry(pKey, nKeyLen, pValue);
	if (*ppHashEntry)
	{
		if (!pValue)
		{
			/* delete entry */
			HashEntryType *pOldEntry = *ppHashEntry;
			*ppHashEntry = (*ppHashEntry)->pNext;
			pOldEntry->pNext = m_pFreeList;
			m_pFreeList = pOldEntry;
			--m_nCount;
			/* reroder entry list */
			if (pOldEntry->pLeft)
			{
				pOldEntry->pLeft->pRight = pOldEntry->pRight;
			}
			if (pOldEntry->pRight)
			{
				pOldEntry->pRight->pLeft = pOldEntry->pLeft;
			}
			if (pOldEntry == m_pBegin)
			{
				m_pBegin = pOldEntry->pRight;
			}
			if (pOldEntry == m_pEnd)
			{
				m_pEnd = pOldEntry->pLeft;
			}
		}
		else
		{
			/* Update entry list when insert newly entry */
			if (!(*ppHashEntry)->pValue)
			{
				if (!m_pBegin)
				{
					m_pBegin = *ppHashEntry;
				}
				if (m_pEnd)
				{
					m_pEnd->pRight = *ppHashEntry;
					(*ppHashEntry)->pLeft = m_pEnd;
				}
				m_pEnd = *ppHashEntry;
			}
			/* replace entry */
			(*ppHashEntry)->pValue = pValue;
			/* check that the collision rate isn't too high */
			if (m_nCount > m_nMax)
			{
				ExpandEntryArray();
			}
		}
	}
	/* else pKey not present and pValue==NULL */
}

void BCOrderedHashTable::Set(int32_t nKey, const void *pValue)
{
	Set(&nKey, sizeof(nKey), pValue);
}

uint32_t BCOrderedHashTable::Count() const
{
	return m_nCount;
}

void BCOrderedHashTable::Clear()
{
	HashIndexType *pHashIndex;
	for (pHashIndex = First(NULLPOOL); pHashIndex; pHashIndex = Next(pHashIndex))
	{
		Set(pHashIndex->pSelf->pKey, pHashIndex->pSelf->nKeyLen, NULL);
	}
}

/*
*  Ordered entry list operations
*/

BCOrderedHashTable::HashEntryType * BCOrderedHashTable::Begin() const
{
	return m_pBegin;
}

BCOrderedHashTable::HashEntryType *BCOrderedHashTable::End() const
{
	return m_pEnd;
}

BCOrderedHashTable::HashEntryType * BCOrderedHashTable::Next(
	HashEntryType *pHashEntry)
{
	if (pHashEntry)
	{
		return pHashEntry->pRight;
	}
	else
	{
		return NULL;
	}
}

BCOrderedHashTable::HashEntryType * BCOrderedHashTable::Prev(
	HashEntryType *pHashEntry)
{
	if (pHashEntry)
	{
		return pHashEntry->pLeft;
	}
	else
	{
		return NULL;
	}
}

void BCOrderedHashTable::Get(
	HashEntryType *pHashEntry,
	const void **ppKey,
	int32_t *pnKeyLen,
	const void **ppValue)
{
	if (pHashEntry)
	{
		if (ppKey)
		{
			*ppKey  = pHashEntry->pKey;
		}
		if (pnKeyLen)
		{
			*pnKeyLen = pHashEntry->nKeyLen;
		}
		if (ppValue)
		{
			*ppValue  = (void *)pHashEntry->pValue;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : BCStrHTItem
///////////////////////////////////////////////////////////////////////////////

BCStrHTItem::BCStrHTItem()
	: m_pValue(NULL)
{
	//
}

BCStrHTItem::~BCStrHTItem()
{
	//
}

///////////////////////////////////////////////////////////////////////////////
// class : BCStrHashTable
///////////////////////////////////////////////////////////////////////////////

void						*	BCStrHashTable::m_pWildPtr = NULL;
BCStrHashTable::iterator		BCStrHashTable::m_sIterEnd;

BCStrHashTable::BCStrHashTable()
{
	//
}

BCStrHashTable::~BCStrHashTable()
{
	//
}

void *&BCStrHashTable::operator [](const char *szIndex)
{
	BCStrHTItem *pItem;

	if (szIndex == NULL)
	{
		ASSERT(szIndex);
		return m_pWildPtr;
	}

	pItem = (BCStrHTItem *)m_htMap.Get(szIndex, HASH_KEY_STRING);
	if (pItem == NULL)
	{
		pItem = new BCStrHTItem();
		if (pItem == NULL)
		{
			ASSERT(0);
			return m_pWildPtr;
		}
		m_htMap.Set(szIndex, HASH_KEY_STRING, pItem);
	}
	return pItem->m_pValue;
}

void *BCStrHashTable::Erase(const char *szIndex)
{
	BCStrHTItem *pItem;
	void *pValue;

	if (szIndex == NULL)
	{
		ASSERT(szIndex);
		return NULL;
	}

	pItem = (BCStrHTItem *)m_htMap.Get(szIndex, HASH_KEY_STRING);
	if (pItem)
	{
		pValue = pItem->m_pValue;
		BC_SAFE_DELETE_PTR(pItem);
		m_htMap.Set(szIndex, HASH_KEY_STRING, NULL);
		return pValue;
	}
	return NULL;
}

bool BCStrHashTable::Find(void **ppOutValue, const char *szIndex, int32_t nLen)
{
	BCStrHTItem *pItem;

	if (szIndex == NULL)
	{
		return false;
	}

	pItem = (BCStrHTItem *)m_htMap.Get(szIndex, nLen > 0?nLen:HASH_KEY_STRING);
	if (pItem)
	{
		*ppOutValue = pItem->m_pValue;
		return true;
	}
	return false;
}

BCStrHashTable::iterator BCStrHashTable::First()
{
	BCHashTable::HashIndexType *pHashIndex;

	pHashIndex = m_htMap.First(NULLPOOL);
	if (pHashIndex)
	{
		return iterator(pHashIndex);
	}
	return m_sIterEnd;
}

BCStrHashTable::iterator BCStrHashTable::Next(iterator &iter)
{
	BCHashTable::HashIndexType *pIndex;

	ASSERT(iter.m_pIndex);

	pIndex = m_htMap.Next(iter.m_pIndex);
	if (pIndex)
	{
		return iterator(pIndex);
	}
	return m_sIterEnd;
}

BCStrHashTable::iterator &BCStrHashTable::End()
{
	return m_sIterEnd;
}

void BCStrHashTable::Clear()
{
	BCHashTable::HashIndexType *pHashIndex;
	BCHashTable::HashEntryType *pHashEntry;

	for (pHashIndex = m_htMap.First(NULLPOOL);
		 pHashIndex;
		 pHashIndex = m_htMap.Next(pHashIndex))
	{
		pHashEntry = pHashIndex->pSelf;
		ASSERT(pHashEntry);
		ASSERT(pHashEntry->pValue);
		delete ((BCStrHTItem *)pHashEntry->pValue);
		pHashEntry->pValue = NULL;
	}
	m_htMap.Clear();
}

uint32_t BCStrHashTable::Count() const
{
	return m_htMap.Count();
}

/*
*  CRC Generator
*/

static const unsigned long g_crc32Table[0x100] =
{
	0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
	0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
	0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
	0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
	0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
	0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
	0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
	0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
	0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
	0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
	0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
	0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
	0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
	0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
	0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
	0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
	0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
	0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
	0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
	0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
	0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
	0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
	0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
	0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
	0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
	0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
	0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
	0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
	0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
	0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
	0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
	0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
	0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
	0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
	0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
	0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
	0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
	0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
	0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
	0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
	0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
	0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
	0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
	0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
	0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
	0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
	0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
	0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
	0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
	0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
	0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
	0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
	0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
	0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
	0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
	0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
	0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
	0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
	0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
	0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
	0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
	0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
	0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
	0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

uint32_t GetCrc(const void* buffer, size_t count)
{
	const uint8_t *p = (const uint8_t *)buffer;
	int32_t temp1;
	int32_t temp2;
	int32_t crc = 0xFFFFFFFFL;

	while ( count-- != 0 )
	{
		temp1 = ( crc>> 8 ) & 0x00FFFFFFL;
		temp2 = g_crc32Table[((int)crc ^ *p++) & 0xff];
		crc = temp1 ^ temp2;
	}
	crc ^= 0xFFFFFFFFL;

	return( crc );
}

#if defined(_WIN32) && !defined(_WIN64) && !defined(__clang__)
uint32_t __declspec(naked) GetCrcFast(const void* buffer, size_t count)
{
	//crc(eax), p(edx), count(ecx), temp1(ebp), temp2(ebx)
	__asm
	{
		push		ebp
		mov			ebp, esp
		//	crc = 0xFFFFFFFFh
		mov         eax, 0FFFFFFFFh
		//	p = (uint8_t*) buffer;
		mov         edx,dword ptr [buffer]
		mov         ecx,dword ptr [count]
beginloop:
		//	while ( m_nCount-- != 0 )
		test        ecx, ecx
		je          endloop
		dec         ecx
		//	{
		//		temp1 = ( crc>> 8 ) & 0x00FFFFFFL;
		mov			ebp, eax
		shr         ebp, 8
		and         ebp, 0FFFFFFh
		//		temp2 = g_crc32Table[((int)crc ^ *p++) & 0xff];
		xor			edi, edi
		movzx       edi, byte ptr [edx]
		xor         edi, eax
		and         edi, 0FFh
		mov         ebx, dword ptr [edi*4 + g_crc32Table]
		inc         edx
		//		crc = temp1 ^ temp2;
		xor         ebp, ebx
		mov         eax, ebp
		//	}
		jmp			beginloop
endloop:
		//	crc ^= 0xFFFFFFFFL;
		//	return( crc );
		xor         eax, 0FFFFFFFFh
		pop			ebp
		ret
	}
}
#endif // _WIN32

static inline uint32_t HashFuncDefault(
	const void *char_key,
	int32_t *pnKeyLen)
{
	uint32_t hash = 0;
	const uint8_t *pKey = (const uint8_t *)char_key;
	const uint8_t *p;
	int32_t i;

	if (*pnKeyLen == HASH_KEY_STRING)
	{
		for (p = pKey; *p; p++)
		{
			hash = hash * 33 + *p;
		}
		*pnKeyLen = (int32_t)(p - pKey);
	}
	else
	{
		for (p = pKey, i = *pnKeyLen; i; i--, p++)
		{
			hash = hash * 33 + *p;
		}
	}

	return hash;
}

static uint32_t HashFuncCrc(
	const void *pKey,
	int32_t *pnKenLen)
{
	if (*pnKenLen == HASH_KEY_STRING)
	{
		*pnKenLen = strlen((const char *)pKey);
	}
	return GetCrc(pKey, *pnKenLen);
}

uint32_t HashFuncCrcFast(
	const void *pKey,
	int32_t *pnKenLen)
{
	if(*pnKenLen == HASH_KEY_STRING)
	{
		*pnKenLen = strlen((const char *)pKey);
	}
#if defined(_WIN32) && !defined(_WIN64) && !defined(__clang__)
	return GetCrcFast(pKey, *pnKenLen);
#else // !_WIN32
	return GetCrc(pKey, *pnKenLen);
#endif // _WIN32
}

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file...
///////////////////////////////////////////////////////////////////////////////