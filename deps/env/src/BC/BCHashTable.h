
#ifndef BC_BCHASHTABLE_INCLUDE__
#define BC_BCHASHTABLE_INCLUDE__

#include <BC/Exports.h>
#include <BC/Utils.h>
#include <BC/BCMemPool.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

/**
* When passing a key to Set or Get, this value can be passed to indicate a
* string-valued key, and have HashFunc compute the length automatically.
*
* @remark strmr_hash will use strlen(key) for the length. The NUL terminator
*         is not included in the hash value (why throw a constant in?).
*         Since the hash table merely references the provided key (rather
*         than copying it), This() will return the NUL-term'd key.
*/
#define HASH_KEY_STRING     (-1)

/**
* Callback functions for calculating hash values.
* @param key The key.
* @param klen The length of the key, or HASH_KEY_STRING to use the string
*             length. If HASH_KEY_STRING then returns the actual key length.
*/
typedef uint32_t (*HashFuncPtr)(const void *pKey, int32_t *pnKeyLen);

typedef enum HashFuncE
{
	HF_DEFAULT	= 0,
	HF_CRC		= 1,
	HF_CRC_FAST	= 2,
}HashFuncE;

///////////////////////////////////////////////////////////////////////////////
// BCHashTable
///////////////////////////////////////////////////////////////////////////////

class BC_API BCHashTable
{
public:

	/*
	* The internal form of a hash table.
	*
	* The table is an m_pEntryArray indexed by the hash of the key; collisions
	* are resolved by hanging a linked list of hash entries off each
	* element of the m_pEntryArray. Although this is a really simple design it
	* isn't too bad given that pools have a low allocation overhead.
	*/

	typedef struct HashEntryType
	{
		HashEntryType		*pNext;
		uint32_t			nHashValue;
		void				*pKey;
		uint32_t			nKeyBufLen;
		int32_t				nKeyLen;
		const void			*pValue;
	}HashEntryType;

	/*
	* Data structure for iterating through a hash table.
	*
	* We keep a pointer to the pNext hash entry here to allow the current
	* hash entry to be freed or otherwise mangled between calls to
	* Next().
	*/
	typedef struct HashIndexType
	{
		BCHashTable			*pHashTable;
		HashEntryType		*pSelf;
		HashEntryType		*pNext;
		uint32_t			nIndex;
	}HashIndexType;
public:
	BCHashTable(
		HashFuncE hashFunc = HF_DEFAULT);
	~BCHashTable();

	HashIndexType * Next(
		HashIndexType *pHashIndex);

	HashIndexType * First(
		const StackMemPool &sMemoryPool);

	static void Get(
		HashIndexType *pHashIndex,
		const void **ppKey,
		int32_t *pnKeyLen,
		const void **ppValue);

	void *Get(
		const void *pKey,
		int32_t nKeyLen);

	void *Get(int32_t nKey);

	void Set(
		const void *pKey,
		int32_t nKeyLen,
		const void *pValue);

	void Set(
		int32_t nKey,
		const void *pValue);

	uint32_t Count() const;

	void Clear();
protected:

	HashEntryType **AllocEntryArray(
		uint32_t max);

	void ExpandEntryArray();

	HashEntryType *GetFreeEntry(
		uint32_t nKeyLen);

	HashEntryType **FindEntry(
		const void *pKey,
		int32_t nKeyLen,
		const void *pValue);

private:
	/*
	* The size of the m_pEntryArray is always a power of two. We use the maximum
	* nIndex rather than the size so that we can use bitwise-AND for
	* modular arithmetic.
	* The m_nCount of hash entries may be greater depending on the chosen
	* collision rate.
	*/
	KBPool					m_sMemoryPool;
	HashEntryType			**m_pEntryArray;
	HashIndexType			m_iterator;  /* For First(NULL, ...) */
	uint32_t				m_nCount;
	uint32_t				m_nMax;
	HashFuncPtr				m_pHashFunc;
	HashEntryType			*m_pFreeList;  /* List of recycled entries */

	DECLARE_NO_COPY_CLASS(BCHashTable)
};

///////////////////////////////////////////////////////////////////////////////
// BCOrderedHashTable
///////////////////////////////////////////////////////////////////////////////

class BC_API BCOrderedHashTable
{
public:

	/*
	* The internal form of a hash table.
	*
	* The table is an m_pEntryArray indexed by the hash of the key; collisions
	* are resolved by hanging a linked list of hash entries off each
	* element of the m_pEntryArray. Although this is a really simple design it
	* isn't too bad given that pools have a low allocation overhead.
	*/

	typedef struct HashEntryType
	{
		HashEntryType		*pNext;
		uint32_t			nHashValue;
		void				*pKey;
		uint32_t			nKeyBufLen;
		int32_t				nKeyLen;
		const void			*pValue;
		/*
		* Pointers to keep entry order
		*/
		HashEntryType		*pLeft;
		HashEntryType		*pRight;
	}HashEntryType;

	/*
	* Data structure for iterating through a hash table.
	*
	* We keep a pointer to the pNext hash entry here to allow the current
	* hash entry to be freed or otherwise mangled between calls to
	* Next().
	*/
	typedef struct HashIndexType
	{
		BCOrderedHashTable	*pHashTable;
		HashEntryType		*pSelf;
		HashEntryType		*pNext;
		uint32_t			nIndex;
	}HashIndexType;
public:
	BCOrderedHashTable(
		HashFuncE hashFunc = HF_DEFAULT);
	~BCOrderedHashTable();

	HashIndexType * Next(
		HashIndexType *pHashIndex);

	HashIndexType * First(
		const StackMemPool &sMemoryPool);

	static void Get(
		HashIndexType *pHashIndex,
		const void **ppKey,
		int32_t *pnKeyLen,
		const void **ppValue);

	/* Ordered list operations */
	HashEntryType * Begin() const;

	HashEntryType *End() const;

	HashEntryType * Next(
		HashEntryType *pHashEntry);

	HashEntryType * Prev(
		HashEntryType *pHashEntry);

	static void Get(
		HashEntryType *pHashEntry,
		const void **ppKey,
		int32_t *pnKeyLen,
		const void **ppValue);

	/* General operations */
	void *Get(
		const void *pKey,
		int32_t nKeyLen);

	void *Get(int32_t nKey);

	void Set(
		const void *pKey,
		int32_t nKeyLen,
		const void *pValue);

	void Set(
		int32_t nKey,
		const void *pValue);

	uint32_t Count() const;

	void Clear();
protected:

	HashEntryType **AllocEntryArray(
		uint32_t max);

	void ExpandEntryArray();

	HashEntryType *GetFreeEntry(
		uint32_t nKeyLen);

	HashEntryType **FindEntry(
		const void *pKey,
		int32_t nKeyLen,
		const void *pValue);

private:
	/*
	* The size of the m_pEntryArray is always a power of two. We use the maximum
	* nIndex rather than the size so that we can use bitwise-AND for
	* modular arithmetic.
	* The m_nCount of hash entries may be greater depending on the chosen
	* collision rate.
	*/
	StackMemPool			m_sMemoryPool;
	HashEntryType			**m_pEntryArray;
	HashIndexType			m_iterator;  /* For First(NULL, ...) */
	uint32_t				m_nCount;
	uint32_t				m_nMax;
	HashFuncPtr				m_pHashFunc;
	HashEntryType			*m_pFreeList;  /* List of recycled entries */
	HashEntryType			*m_pBegin;	/* First inserted entry keeper */
	HashEntryType			*m_pEnd;	/* Latest interted entry keeper */

	DECLARE_NO_COPY_CLASS(BCOrderedHashTable)
};

///////////////////////////////////////////////////////////////////////////////
// class : BCStrHashTable - index by string, only for simple value type
///////////////////////////////////////////////////////////////////////////////

class BCStrHTItem
{
public:
	BCStrHTItem();
	~BCStrHTItem();

	void		*	m_pValue;
};

class BC_API BCStrHashTable
{
public:
	class BC_API iterator
	{
		friend class BCStrHashTable;
	public:
		iterator() : m_pIndex(NULL){}
		~iterator(){}

		const char	*	First()
		{
			return (const char *)m_pIndex->pSelf->pKey;
		}
		void		*&	Second()
		{
			BCStrHTItem *pItem = (BCStrHTItem *)m_pIndex->pSelf->pValue;
			ASSERT(pItem);
			return pItem->m_pValue;
		}

		iterator &		operator = (const iterator &other)
		{
			m_pIndex= other.m_pIndex;
			return *this;
		}

		bool			operator == (const iterator &other)
		{
			return m_pIndex == other.m_pIndex;
		}

		bool			operator != (const iterator &other)
		{
			return m_pIndex != other.m_pIndex;
		}
	protected:
		iterator(BCHashTable::HashIndexType *pIndex) : m_pIndex(pIndex)
		{
			ASSERT(m_pIndex);
		}
	private:
		BCHashTable::HashIndexType		*	m_pIndex;
	};
public:
	BCStrHashTable();
	virtual ~BCStrHashTable();

	void		*&	operator[](const char *szIndex);
	void		*	Erase(const char *szIndex);
	bool			Find(void **ppOutValue, const char *szIndex, int32_t nLen = 0);
	iterator		First();
	iterator		Next(iterator &iter);
	iterator	&	End();
	void			Clear();
	uint32_t		Count() const;
protected:
private:
	DECLARE_NO_COPY_CLASS(BCStrHashTable);
	BCHashTable			m_htMap;
	static void		*	m_pWildPtr;
	static iterator		m_sIterEnd;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCStrHashMap - index by string, only for complex value type
///////////////////////////////////////////////////////////////////////////////

template<typename _ValueType>
class BCStrHashMap
{
public:
	class iterator
	{
		friend class BCStrHashMap;
	public:
		iterator() : m_pIndex(NULL){}
		~iterator(){}

		const char	*	First()
		{
			return (const char *)m_pIndex->pSelf->pKey;
		}
		_ValueType	&	Second()
		{
			_ValueType *pItem = (_ValueType *)m_pIndex->pSelf->pValue;
			ASSERT(pItem);
			return *pItem;
		}

		iterator &		operator = (const iterator &other)
		{
			m_pIndex= other.m_pIndex;
			return *this;
		}

		bool			operator == (const iterator &other)
		{
			return m_pIndex == other.m_pIndex;
		}

		bool			operator != (const iterator &other)
		{
			return m_pIndex != other.m_pIndex;
		}
	protected:
		iterator(BCHashTable::HashIndexType *pIndex) : m_pIndex(pIndex)
		{
			ASSERT(m_pIndex);
		}
	private:
		BCHashTable::HashIndexType		*	m_pIndex;
	};
public:
	BCStrHashMap(){};
	virtual ~BCStrHashMap(){};

	_ValueType	&	operator [](const char *szIndex)
	{
		_ValueType *pValue;

		if (szIndex == NULL)
		{
			ASSERT(szIndex);
			return m_sWildPtr;
		}

		pValue = (_ValueType *)m_htMap.Get(szIndex, HASH_KEY_STRING);
		if (pValue == NULL)
		{
			pValue = _NewValue();
			if (pValue == NULL)
			{
				ASSERT(0);
				return m_sWildPtr;
			}
			m_htMap.Set(szIndex, HASH_KEY_STRING, pValue);
		}
		return *pValue;
	}

	_ValueType	Erase(const char *szIndex)
	{
		_ValueType *pValue, sValue;

		if (szIndex == NULL)
		{
			ASSERT(szIndex);
			return NULL;
		}

		pValue = (_ValueType *)m_htMap.Get(szIndex, HASH_KEY_STRING);
		if (pValue)
		{
			sValue = *pValue;
			uninitTypeArray<_ValueType>(pValue, 1);
			m_htMap.Set(szIndex, HASH_KEY_STRING, NULL);
			if (m_htMap.Count() == 0)
			{
				Clear();
			}
			return sValue;
		}
		return m_sWildPtr;
	}

	_ValueType &	Find(const char *szIndex)
	{
		_ValueType *pValue;

		if (szIndex == NULL)
		{
			return m_sWildPtr;
		}

		pValue = (_ValueType *)m_htMap.Get(szIndex, HASH_KEY_STRING);
		if (pValue)
		{
			return *pValue;
		}
		return m_sWildPtr;
	}

	iterator First()
	{
		BCHashTable::HashIndexType *pHashIndex;

		pHashIndex = m_htMap.First(NULLPOOL);
		if (pHashIndex)
		{
			return iterator(pHashIndex);
		}
		return m_sIterEnd;
	}

	iterator Next(iterator &iter)
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

	iterator &	End()
	{
		return m_sIterEnd;
	}

	void Clear()
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
			uninitTypeArray<_ValueType>((void *)pHashEntry->pValue, 1);
			pHashEntry->pValue = NULL;
		}
		m_htMap.Clear();
		m_sPool.Clear();
	}

	uint32_t Count() const
	{
		return m_htMap.Count();
	}
protected:
	_ValueType	*	_NewValue()
	{
		_ValueType *pItem;

		pItem = (_ValueType *)m_sPool.Calloc(sizeof(_ValueType));
		ASSERT(pItem);
		initTypeArray<_ValueType>(pItem, 1);
		return pItem;
	}
private:
	DECLARE_NO_COPY_CLASS(BCStrHashMap);
	BCHashTable			m_htMap;
	_ValueType			m_sWildPtr;
	iterator			m_sIterEnd;
	StackMemPool		m_sPool;
};

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif // BC_BCHASHTABLE_INCLUDE__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
