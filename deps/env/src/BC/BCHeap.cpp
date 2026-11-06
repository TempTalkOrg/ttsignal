
#include "BC/Utils.h"
#include "BC/BCHeap.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

/*@{*/
/*%
 * Note: to make heap_parent and heap_left easy to compute, the first
 * element of the heap array is not used; i.e. heap subscripts are 1-based,
 * not 0-based.  The parent is index/2, and the left-child is index*2.
 * The right child is index*2+1.
 */
#define heap_parent(i)			((i) >> 1)
#define heap_left(i)			((i) << 1)
/*@}*/

#define SIZE_INCREMENT			1024

#define HEAP_MAGIC				BC_MAGIC('H', 'E', 'A', 'P')
#define VALID_HEAP(h)			BC_MAGIC_VALID(h, HEAP_MAGIC)

/*%
 * When the heap is in a consistent state, the following invariant
 * holds true: for every element i > 1, heap_parent(i) has a priority
 * higher than or equal to that of i.
 */
#define HEAPCONDITION(i) ((i) == 1 || \
	! m_lpfnCompare(m_pArray[(i)], \
	m_pArray[heap_parent(i)]))

///////////////////////////////////////////////////////////////////////////////
// class : BCHeap
///////////////////////////////////////////////////////////////////////////////

BCHeap::BCHeap()
	: BCMagic(HEAP_MAGIC)
	, m_nSize(0)
	, m_nSizeIncrement(0)
	, m_nLast(0)
	, m_pArray(NULL)
	, m_lpfnCompare(NULL)
	, m_lpfnIndex(NULL)
	, m_bPool1(TRUE)
{
	//
}

BCHeap::~BCHeap()
{
	//
}

BCRESULT BCHeap::Create(
	LPFN_BCHeapCompare lpfnCompare,
	LPFN_BCHeapIndex lpfnIndex,
	uint32_t nSizeIncrement)
{
	ASSERT(lpfnCompare != NULL);

	m_nSize = 0;
	if (nSizeIncrement == 0)
		m_nSizeIncrement = SIZE_INCREMENT;
	else
		m_nSizeIncrement = nSizeIncrement;
	m_nLast = 0;
	m_pArray = NULL;
	m_lpfnCompare = lpfnCompare;
	m_lpfnIndex = lpfnIndex;

	return (BC_R_SUCCESS);
}

BOOL BCHeap::_ReSize()
{
	void **new_array;
	size_t new_size;

	ASSERT(VALID_HEAP(this));

	new_size = m_nSize + m_nSizeIncrement;
	if (m_bPool1)
	{
		new_array = (void **)m_sPool2.Calloc(new_size * sizeof(void *));
		if (new_array == NULL)
		{
			return FALSE;
		}
		m_bPool1 = FALSE;
		if (m_pArray != NULL)
		{
			memcpy2(new_array, m_pArray, m_nSize * sizeof(void *));
			m_sPool1.Clear();
		}
	}
	else
	{
		new_array = (void **)m_sPool1.Calloc(new_size * sizeof(void *));
		if (new_array == NULL)
		{
			return FALSE;
		}
		m_bPool1 = TRUE;
		if (m_pArray != NULL)
		{
			memcpy2(new_array, m_pArray, m_nSize * sizeof(void *));
			m_sPool2.Clear();
		}
	}
	m_nSize = new_size;
	m_pArray = new_array;

	return (TRUE);
}

void BCHeap::_FloatUp(uint32_t i, void *pElt)
{
	uint32_t p;

	for (p = heap_parent(i) ;
		i > 1 && m_lpfnCompare(pElt, m_pArray[p]) ;
		i = p, p = heap_parent(i))
	{
		m_pArray[i] = m_pArray[p];
		if (m_lpfnIndex != NULL)
			(m_lpfnIndex)(m_pArray[i], i);
	}
	m_pArray[i] = pElt;
	if (m_lpfnIndex != NULL)
		(m_lpfnIndex)(m_pArray[i], i);

	ASSERT(HEAPCONDITION(i));
}

void BCHeap::_SinkDown(uint32_t i, void *elt)
{
	uint32_t j, size, half_size;
	size = m_nLast;
	half_size = size / 2;
	while (i <= half_size)
	{
		/* Find the smallest of the (at most) two children. */
		j = heap_left(i);
		if (j < size && m_lpfnCompare(m_pArray[j+1], m_pArray[j]))
			j++;
		if (m_lpfnCompare(elt, m_pArray[j]))
			break;
		m_pArray[i] = m_pArray[j];
		if (m_lpfnIndex != NULL)
			(m_lpfnIndex)(m_pArray[i], i);
		i = j;
	}
	m_pArray[i] = elt;
	if (m_lpfnIndex != NULL)
		(m_lpfnIndex)(m_pArray[i], i);

	ASSERT(HEAPCONDITION(i));
}

BCRESULT BCHeap::Insert(void *elt)
{
	uint32_t i;

	ASSERT(VALID_HEAP(this));

	i = ++m_nLast;
	if (m_nLast >= m_nSize && !_ReSize())
		return (BC_R_NOMEMORY);

	_FloatUp(i, elt);

	return (BC_R_SUCCESS);
}

void BCHeap::Delete(uint32_t nIndex)
{
	void *elt;
	BOOL less;

	ASSERT(VALID_HEAP(this));
	ASSERT(nIndex >= 1 && nIndex <= m_nLast);

	if (nIndex == m_nLast)
	{
		m_pArray[m_nLast] = NULL;
		m_nLast--;
	}
	else
	{
		elt = m_pArray[m_nLast];
		m_pArray[m_nLast] = NULL;
		m_nLast--;

		less = m_lpfnCompare(elt, m_pArray[nIndex]);
		m_pArray[nIndex] = elt;
		if (less)
			_FloatUp(nIndex, m_pArray[nIndex]);
		else
			_SinkDown(nIndex, m_pArray[nIndex]);
	}
}

void BCHeap::Increased(uint32_t nIndex)
{
	ASSERT(VALID_HEAP(this));
	ASSERT(nIndex >= 1 && nIndex <= m_nLast);

	_FloatUp(nIndex, m_pArray[nIndex]);
}

void BCHeap::Decreased(uint32_t nIndex)
{
	ASSERT(VALID_HEAP(this));
	ASSERT(nIndex >= 1 && nIndex <= m_nLast);

	_SinkDown(nIndex, m_pArray[nIndex]);
}

void *BCHeap::Element(uint32_t nIndex)
{
	ASSERT(VALID_HEAP(this));
	ASSERT(nIndex >= 1);

	if (nIndex <= m_nLast)
		return (m_pArray[nIndex]);
	return (NULL);
}

void BCHeap::ForEach(LPFN_BCHeapAction action, void *uap)
{
	unsigned int i;

	ASSERT(VALID_HEAP(this));
	ASSERT(action != NULL);

	for (i = 1 ; i <= m_nLast ; i++)
		(action)(m_pArray[i], uap);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
