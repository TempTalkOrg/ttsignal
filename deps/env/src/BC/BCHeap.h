
#ifndef BCHEAP_INCLUDED__
#define BCHEAP_INCLUDED__

#include "BC/Exports.h"
#include "BC/BCMagic.h"
#include "BC/BCMemPool.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// Macros & typedefs
///////////////////////////////////////////////////////////////////////////////

/*%
 * The comparison function returns BC_TRUE if the first argument has
 * higher priority than the second argument, and BC_FALSE otherwise.
 */
typedef BOOL (*LPFN_BCHeapCompare)(void *, void *);

/*%
 * The index function allows the client of the heap to receive a callback
 * when an item's index number changes.  This allows it to maintain
 * sync with its external state, but still delete itself, since deletions
 * from the heap require the index be provided.
 */
typedef void (*LPFN_BCHeapIndex)(void *, unsigned int);

/*%
 * The heapaction function is used when iterating over the heap.
 *
 * NOTE:  The heap structure CANNOT BE MODIFIED during the call to
 * BCHeap::Foreach().
 */
typedef void (*LPFN_BCHeapAction)(void *, void *);

///////////////////////////////////////////////////////////////////////////////
// class : BCHeap
///////////////////////////////////////////////////////////////////////////////

class BC_API BCHeap : public BCMagic
{
public:
	BCHeap();
	virtual ~BCHeap();

	BCRESULT		Create(
						LPFN_BCHeapCompare lpfnCompare,
						LPFN_BCHeapIndex nIndex,
						uint32_t nSizeIncrement);
	BCRESULT		Insert(void *pElt);
	void			Delete(uint32_t nIndex);
	void			Increased(uint32_t nIndex);
	void			Decreased(uint32_t nIndex);
	void		*	Element(uint32_t nIndex);
	void			ForEach(
						LPFN_BCHeapAction lpfnAction,
						void *pUserArg);

protected:
	BOOL			_ReSize();
	void			_FloatUp(uint32_t i, void *pElt);
	void			_SinkDown(uint32_t i, void *elt);
private:
	DECLARE_NO_COPY_CLASS(BCHeap);
	uint32_t				m_nSize;
	uint32_t				m_nSizeIncrement;
	uint32_t				m_nLast;
	void				**	m_pArray;
	LPFN_BCHeapCompare		m_lpfnCompare;
	LPFN_BCHeapIndex		m_lpfnIndex;
	KBPool					m_sPool1;
	KBPool					m_sPool2;
	BOOL					m_bPool1;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////