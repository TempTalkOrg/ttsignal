
//#include <BC/BCLogger.h>
#include <BC/BCMemPool.h>
#ifdef WIN32
#pragma warning(disable:4355)
#endif // WIN32
#include <BC/BCException.h>
#include <BC/Utils.h>
#include <atomic>

#ifdef _WIN32
typedef struct iovec
{
	char*			iov_base;
	size_t			iov_len;
}iovec;
#endif // _WIN32

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

#ifdef UINT32_MAX
#define BC_UINT32_MAX  UINT32_MAX
#else
#define BC_UINT32_MAX  (0xffffffffU)
#endif

//#define MIN_ALLOC 8192
//#define MAX_INDEX   20

//#define BOUNDARY_INDEX 12
//#define BOUNDARY_SIZE (1 << BOUNDARY_INDEX)

/** The base size of a memory node - aligned.  */
#define SIZEOF_BCMEMNODE	ALIGN_DEFAULT(sizeof(BCMemNode))
#define SIZEOF_BCMEMPOOL	ALIGN_DEFAULT(sizeof(BCMemPool))

/** Symbolic constants */
#define ALLOCATOR_MAX_FREE_UNLIMITED 0

#define ALIGN(size, boundary) \
	(((size) + ((boundary) - 1)) & ~((boundary) - 1))

/** Default alignment */
#define ALIGN_DEFAULT(size) ALIGN(size, 8)


static BCMemPool			*	global_pool = NULL;
static BCMemNodeAllocator	*	global_allocator = NULL;
static std::atomic<int>			bc_pools_initialized(0);


static void *MemoryAllocator(uint32_t nSize)
{
	return malloc(nSize);
}

static void MemoryFreer(void *pObject)
{
	free(pObject);
}

///////////////////////////////////////////////////////////////////////////////
// BCMemNode
///////////////////////////////////////////////////////////////////////////////

void *BCMemNode::operator new(size_t objSize, void *pObject)
{
	ASSERT(objSize == sizeof(BCMemNode));
	return pObject;
}


BCMemNode::BCMemNode(uint32_t bufferSize, uint32_t nSizeIndex)
	: m_pNext(this)
	, m_ppNodeRef(&m_pNext)
	, m_nSizeIndex(nSizeIndex)
	, m_nFreeIndex(0)
{
	this->m_pFirstAvail = (char *)this + SIZEOF_BCMEMNODE;
	this->m_pEndp = (char *)this + bufferSize;
}

BCMemNode::~BCMemNode()
{
	// Do nothing
}

void *BCMemNode::Recycle()
{
	void* buf = this;
	delete this;
	return buf;
}

inline uint32_t BCMemNode::GetFreespace() const
{
	return (uint32_t)(m_pEndp - m_pFirstAvail);
}

inline void *BCMemNode::NodeAlloc(uint32_t size)
{
	void *pMem = m_pFirstAvail;
	m_pFirstAvail += size;
	return pMem;
}

inline void BCMemNode::SetFirstAvail(void *pFirstAvail)
{
	m_pFirstAvail = (char *)pFirstAvail;
}

inline uint32_t BCMemNode::GetNodeSize() const
{
	return (uint32_t)(m_pEndp - (char *)this);
}

inline uint32_t BCMemNode::GetSizeIndex() const
{
	return m_nSizeIndex;
}

///////////////////////////////////////////////////////////////////////////////
// BCMemNodeAllocator
///////////////////////////////////////////////////////////////////////////////

BCMemNodeAllocator::BCMemNodeAllocator(
	BoundaryIndexE eBoundaryIndex /*= DEFALT_BOUNDARY_INDEX*/,
	uint32_t nMaxFreeSize /*= 0*/)
		: m_nMaxIndex(0)
		, m_nMaxFreeIndex(0)
		, m_nCurrentFreeIndex(0)
		, m_pOwner(NULL)
		, m_nMinAllocSize(1 << (eBoundaryIndex + 1))
		, m_nBoundaryIndex(eBoundaryIndex)
{
	if (0 == bc_pools_initialized)
	{
		InitializeMemoryPool();
	}
	memset(this->m_Free, 0, sizeof(m_Free));
	if (0 < nMaxFreeSize)
	{
		SetMaxFree(nMaxFreeSize);
	}
}

BCMemNodeAllocator::~BCMemNodeAllocator()
{
	//LOG_INFO(_LOCAL_, "BCMemNodeAllocator destroyed.");
	this->Destroy();
}

void BCMemNodeAllocator::Destroy()
{
	uint32_t index;
	BCMemNode *pNode, **ppRef;

	BCSpinMutex::Owner lock(m_sNodeListLock);
	for (index = 0; index < MAX_INDEX; index++)
	{
		ppRef = &this->m_Free[index];
		while ((pNode = *ppRef) != NULL)
		{
			*ppRef = pNode->m_pNext;
			MemoryFreer(pNode);
		}
	}
}

void BCMemNodeAllocator::SetOwner(BCMemPool *pool)
{
	this->m_pOwner = pool;
}

BCMemPool * BCMemNodeAllocator::GetOwner() const
{
	return this->m_pOwner;
}

void BCMemNodeAllocator::SetMaxFree(uint32_t in_size)
{
	BCSpinMutex::Owner lock(m_sMemberLock);
	uint32_t max_free_index;
	uint32_t size = in_size;

	max_free_index = ALIGN(size, 1 << m_nBoundaryIndex) >> m_nBoundaryIndex;
	this->m_nCurrentFreeIndex += max_free_index;
	this->m_nCurrentFreeIndex -= this->m_nMaxFreeIndex;
	this->m_nMaxFreeIndex = max_free_index;
	if (this->m_nCurrentFreeIndex > max_free_index)
	{
		this->m_nCurrentFreeIndex = max_free_index;
	}
}

uint32_t BCMemNodeAllocator::GetMinAllocSize() const
{
	return m_nMinAllocSize;
}

uint32_t BCMemNodeAllocator::GetBoundaryIndex() const
{
	return m_nBoundaryIndex;
}

inline uint32_t BCMemNodeAllocator::CalcNodeSize(uint32_t size) const
{
	/* Round up the block size to the pNext boundary, but always
	* allocate at least a certain size (MIN_ALLOC).
	*/
	size = ALIGN(size + SIZEOF_BCMEMNODE, 1 << m_nBoundaryIndex);
	if (size < m_nMinAllocSize)
	{
		size = m_nMinAllocSize;
	}
	return size;
}

inline uint32_t BCMemNodeAllocator::CalcFreeIndex(BCMemNode *pNode) const
{
	if (pNode)
	{
		uint32_t nFreeIndex = (ALIGN(pNode->GetFreespace() + 1,
			(1 << m_nBoundaryIndex)) - (1 << m_nBoundaryIndex))
			>> m_nBoundaryIndex;
		return nFreeIndex;
	}
	return 0;
}

inline void BCMemNodeAllocator::NodeListInsert(
	BCMemNode *pNewNode,
	BCMemNode *pInsertPoint)
{
	BCSpinMutex::Owner lock(m_sNodeListLock);

	if (pNewNode && pInsertPoint)
	{
		pNewNode->m_ppNodeRef = pInsertPoint->m_ppNodeRef;
		*pNewNode->m_ppNodeRef = pNewNode;
		pNewNode->m_pNext = pInsertPoint;
		pInsertPoint->m_ppNodeRef = &pNewNode->m_pNext;
	}
}

inline void BCMemNodeAllocator::NodeListRemove(
	BCMemNode *pNode)
{
	BCSpinMutex::Owner lock(m_sNodeListLock);

	if (pNode)
	{
		*pNode->m_ppNodeRef = pNode->m_pNext;
		pNode->m_pNext->m_ppNodeRef = pNode->m_ppNodeRef;
	}
}

BCMemNode *BCMemNodeAllocator::GetNodeFromBucket(uint32_t index)
{
	BCMemNode *pNode, **arrayIter;
	uint32_t max_index, i;

	/* First see if there are any nodes in the area we know
	* our pNode will fit into.
	*/
	if (index <= this->m_nMaxIndex)
	{
		BCSpinMutex::Owner lock(m_sMemberLock);
		max_index = this->m_nMaxIndex;
		arrayIter = &this->m_Free[index];
		i = index;
		while (*arrayIter == NULL && i < max_index)
		{
			arrayIter++;
			i++;
		}

		if ((pNode = *arrayIter) != NULL)
		{
			/* If we have found a pNode and it doesn't have any
			* nodes waiting in line behind it _and_ we are on
			* the highest available index, find the new highest
			* available index
			*/
			if ((*arrayIter = pNode->m_pNext) == NULL && i >= max_index)
			{
				do
				{
					arrayIter--;
					max_index--;
				}
				while (*arrayIter == NULL && max_index > 0);

				this->m_nMaxIndex = max_index;
			}

			this->m_nCurrentFreeIndex += pNode->m_nSizeIndex;
			if (this->m_nCurrentFreeIndex > this->m_nMaxFreeIndex)
			{
				this->m_nCurrentFreeIndex = this->m_nMaxFreeIndex;
			}

			return (BCMemNode *)pNode->Recycle();
		}
	}

	/* If we found nothing, seek the sink (at index 0), if
	* it is not empty.
	*/
	else if (this->m_Free[0])
	{
		BCSpinMutex::Owner lock(m_sMemberLock);
		/* Walk the free list to see if there are
		* any nodes on it of the requested size
		*/
		arrayIter = &this->m_Free[0];
		while ((pNode = *arrayIter) != NULL && index > pNode->m_nSizeIndex)
		{
			arrayIter = &pNode->m_pNext;
		}

		if (pNode)
		{
			*arrayIter = pNode->m_pNext;

			this->m_nCurrentFreeIndex += pNode->m_nSizeIndex;
			if (this->m_nCurrentFreeIndex > this->m_nMaxFreeIndex)
			{
				this->m_nCurrentFreeIndex = this->m_nMaxFreeIndex;
			}

			return (BCMemNode *)pNode->Recycle();
		}
	}
	return NULL;
}

BCMemNode *BCMemNodeAllocator::PutNodeIntoBucket(BCMemNode *pNode)
{
	BCMemNode *pNext, *freelist = NULL;
	uint32_t index, max_index;
	uint32_t max_free_index, current_free_index;

	BCSpinMutex::Owner lock(m_sMemberLock);
	max_index = this->m_nMaxIndex;
	max_free_index = this->m_nMaxFreeIndex;
	current_free_index = this->m_nCurrentFreeIndex;

	/* Walk the list of submitted nodes and free them one by one,
	* shoving them in the right 'size' buckets as we go.
	*/
	do
	{
		pNext = pNode->m_pNext;
		index = pNode->m_nSizeIndex;

		if (max_free_index != ALLOCATOR_MAX_FREE_UNLIMITED
			&& index > current_free_index)
		{
			pNode->m_pNext = freelist;
			freelist = pNode;
		}
		else if (index < MAX_INDEX)
		{
			/* Add the node to the appropiate 'size' bucket.  Adjust
			* the max_index when appropiate.
			*/
			if ((pNode->m_pNext = this->m_Free[index]) == NULL
				&& index > max_index)
			{
				max_index = index;
			}
			this->m_Free[index] = pNode;
			if (current_free_index >= index)
			{
				current_free_index -= index;
			}
			else
			{
				current_free_index = 0;
			}
		}
		else
		{
			/* This node is too large to keep in a specific size bucket,
			* just add it to the sink (at index 0).
			*/
			pNode->m_pNext = this->m_Free[0];
			this->m_Free[0] = pNode;
			if (current_free_index >= index)
			{
				current_free_index -= index;
			}
			else
			{
				current_free_index = 0;
			}
		}
	}
	while ((pNode = pNext) != NULL);

	this->m_nMaxIndex = max_index;
	this->m_nCurrentFreeIndex = current_free_index;

	return freelist;
}

/*
* Allocate - allocate a memory node block for use
* @return - memory block pointer if succeeded, or NULL if failed.
* @remark - the memory block returned needs to be constructed by approprate c_tor
*/

BCMemNode * BCMemNodeAllocator::Allocate(uint32_t size)
{
	BCMemNode *pNode;
	uint32_t index;

	/* Round up the block size to the next boundary, but always
	* allocate at least a certain size (MIN_ALLOC).
	*/
	size = CalcNodeSize(size);

	/* Find the index for this node size by
	* dividing its size by the boundary size
	*/
	index = (size >> m_nBoundaryIndex) - 1;

	if (index > BC_UINT32_MAX)
	{
		return NULL;
	}

	pNode = GetNodeFromBucket(index);
	if (pNode)
	{
		return pNode;
	}

	/* If we haven't got a suitable node, malloc a new one
	* and initialize it.
	*/
	if ((pNode = (BCMemNode *)MemoryAllocator(size)) == NULL)
	{
		return NULL;
	}

	pNode->m_nSizeIndex = index;
	pNode->m_pEndp = (char *)pNode + size;

	return pNode;
}

void BCMemNodeAllocator::DeAllocate(BCMemNode *pNode)
{
	BCMemNode *pFreelist = NULL;

	if (!pNode)
	{
		return;
	}
	pFreelist = PutNodeIntoBucket(pNode);

	while (pFreelist != NULL)
	{
		pNode = pFreelist;
		pFreelist = pNode->m_pNext;
		// Real release object instance
		pNode->Recycle();
		MemoryFreer(pNode);
	}
}

BCMemPool *BCMemNodeAllocator::CreatePool(
	BCMemPool *pParentPool /*= NULL*/,
	bool bUseSameAllocator /*= true*/)
{
	return CreatePool(pParentPool, bUseSameAllocator?this:NULL);
}

BCMemPool *BCMemNodeAllocator::CreatePool(
	BCMemPool *pParentPool /* = NULL */,
	BCMemNodeAllocator *pAllocator /* = NULL */)
{
	BCMemPool *pPool = NULL;
	if (bc_pools_initialized == 0)
	{
		InitializeMemoryPool();
	}
	if (pParentPool == NULL)
	{
		pParentPool = global_pool;
	}
	if (pAllocator == NULL)
	{
		pAllocator = global_allocator;
	}

	BCMemNode *node = pAllocator->Allocate(
		pAllocator->m_nMinAllocSize - SIZEOF_BCMEMPOOL);
	pPool = new((void *)node)BCMemPool(pParentPool, pAllocator,
		node->m_pEndp - (char *)node, node->m_nSizeIndex);
	return pPool;
}

///////////////////////////////////////////////////////////////////////////////
// BCMemPool
///////////////////////////////////////////////////////////////////////////////

void *BCMemPool::operator new(size_t objSize, void *pObject)
{
	ASSERT(objSize == sizeof(BCMemPool));
	return pObject;
}

BCMemPool::BCMemPool(
	BCMemPool *pParentPool,
	BCMemNodeAllocator * pAllocator,
	uint32_t bufferSize,
	uint32_t nIndex)
		: BCMemNode(bufferSize, nIndex)
		, m_nRefCount(0)
{
	this->m_pSelfFirstAvail = (char *)this + SIZEOF_BCMEMPOOL;
	this->m_pFirstAvail = this->m_pSelfFirstAvail;
	if (!pParentPool)
	{
		pParentPool = global_pool;
	}
	if (pAllocator == NULL)
	{
		pAllocator = global_pool->m_pAllocator;
	}
	this->m_pAllocator = pAllocator;
	this->m_pActive = this->m_pSelf = this;
	this->m_pAbortFunc = NULL;
	this->m_pCleanups = NULL;
	this->m_pFreeCleanups = NULL;
	this->m_pPreCleanups = NULL;
	this->m_pFreePreCleanups = NULL;
	this->m_pTag = NULL;
	this->m_reserveSize = 0;
}

BCMemPool::~BCMemPool()
{
	//Do nothing, DO NOT Call Destroy(), which will cause dead lock
	// in BCMemNodeAllocator::PutNodeToBucket while acquire semaphore
}

void * BCMemPool::Alloc(uint32_t size)
{
	BCMemNode *pActive, *pNode;
	void *pMem;
	uint32_t nFreeIndex;

	size = ALIGN_DEFAULT(size);
	pActive = this->m_pActive;

	/* If the pActive node has enough bytes left, use it. */
	if (size <= pActive->GetFreespace())
	{
		return pActive->NodeAlloc(size);
	}

	pNode = pActive->m_pNext;
	/* The node behind the current active node always has the biggest free index
	*  Note that 'free index' here is different with 'index' in allocator means,
	*  and the nodes used by memory pool are organized as a RING list!!!
	*/
	if (pNode && size <= pNode->GetFreespace())
	{
		m_pAllocator->NodeListRemove(pNode);
	}
	else
	{
		if ((pNode = this->m_pAllocator->Allocate(size)) == NULL)
		{
			if (this->m_pAbortFunc)
			{
				this->m_pAbortFunc(STATUS_ENOMEM);
			}
			return NULL;
		}

		// Reconstruct node instance
		pNode = new(pNode)BCMemNode(pNode->GetNodeSize(), pNode->GetSizeIndex());
	}

	pNode->m_nFreeIndex = 0;

	pMem = pNode->NodeAlloc(size);

	m_pAllocator->NodeListInsert(pNode, pActive);

	m_pActive = pNode;

	nFreeIndex = m_pAllocator->CalcFreeIndex(pActive);

	pActive->m_nFreeIndex = nFreeIndex;
	pNode = pActive->m_pNext;
	/* If the old active node has no next node,
	*  or its free index bigger than whose
	*/
	if (nFreeIndex >= pNode->m_nFreeIndex)
	{
		return pMem;
	}

	/* If the old active node has next node,
	*  and whose free index smaller than the node
	*/
	do
	{
		pNode = pNode->m_pNext;
	}
	while (nFreeIndex < pNode->m_nFreeIndex);

	m_pAllocator->NodeListRemove(pActive);
	m_pAllocator->NodeListInsert(pActive, pNode);

	return pMem;
}

void * BCMemPool::Calloc(uint32_t size)
{
	void *mem;

	size = ALIGN_DEFAULT(size);
	if ((mem = this->Alloc(size)) != NULL)
	{
		memset(mem, 0, size);
	}

	return mem;
}

void * BCMemPool::Realloc(
	const void *pOriginal,
	uint32_t oldSize,
	uint32_t newSize)
{
	if (oldSize >= newSize)
	{
		return (void *)pOriginal;
	}
	else
	{
		const char *pEndp = (const char *)pOriginal + oldSize;
		BCMemNode *pNode = FindNode(pOriginal);
		uint32_t nFreeSpace = pNode->GetFreespace();
		if (pEndp == pNode->m_pFirstAvail && nFreeSpace >= newSize - oldSize)
		{
			pNode->m_pFirstAvail += newSize - oldSize;
			return (void *)pOriginal;
		}
		else
		{
			void *pNew = this->Alloc(newSize);
			if (pOriginal && pNew)
			{
				memcpy(pNew, pOriginal, oldSize);
			}
			return pNew;
		}
	}
}

uint8_t * BCMemPool::alloc(uint32_t size)
{
	return (uint8_t *)this->Alloc(size);
}

uint8_t * BCMemPool::allocz(int32_t n, uint32_t size)
{
	return (uint8_t *)this->Calloc(n*size);
}

/*
* Pool creation/destruction
*/

void BCMemPool::Clear(
	bool bKeepReserve /*= true*/)
{
	BCMemNode *active;

	/* Run pre destroy cleanups */
	this->RunCleanups(&this->m_pPreCleanups);
	this->m_pPreCleanups = NULL;
	this->m_pFreePreCleanups = NULL;

	/* Run cleanups */
	this->RunCleanups(&this->m_pCleanups);
	this->m_pCleanups = NULL;
	this->m_pFreeCleanups = NULL;

	/* Find the node attached to the pool structure, reset it, make
	* it the active node and free the rest of the nodes.
	*/
	active = this->m_pActive = this->m_pSelf;
	if (bKeepReserve)
	{
		active->SetFirstAvail(this->m_pSelfFirstAvail + this->m_reserveSize);
	}
	else
	{
		active->SetFirstAvail(this->m_pSelfFirstAvail);
	}

	if (active->m_pNext == active)
	{
		return;
	}

	*active->m_ppNodeRef = NULL;
	m_pAllocator->DeAllocate(active->m_pNext);
	active->m_pNext = active;
	active->m_ppNodeRef = &active->m_pNext;
}

void BCMemPool::Clear(uint32_t reserveSize)
{
	Clear(false);
	Alloc(reserveSize);
}

void BCMemPool::ClearRootPool()
{
	if (global_pool)
	{
		global_pool->Clear();
	}
}

void *BCMemPool::Recycle()
{
	delete this;
	return this;
}

void BCMemPool::Destroy()
{
	BCMemNode *active;
	BCMemNodeAllocator *pAllocator;

	/* Run pre destroy cleanups */
	this->RunCleanups(&this->m_pPreCleanups);
	this->m_pPreCleanups = NULL;
	this->m_pFreePreCleanups = NULL;

	/* Run cleanups */
	RunCleanups(&this->m_pCleanups);

	pAllocator = this->m_pAllocator;
	active = this->m_pSelf;
	*active->m_ppNodeRef = NULL;

	pAllocator->DeAllocate(active);

	if (pAllocator->GetOwner() == this)
	{
		delete pAllocator;
	}
}

uint32_t BCMemPool::GetNodeSize(void *pNode)
{
	BCMemNode *node = (BCMemNode *)pNode;
	if (node)
	{
		return (uint32_t)(node->m_pEndp - (char *)node);
	}
	return 0;
}

uint32_t BCMemPool::GetNodeAvailSize(void *pNode)
{
	BCMemNode *node = (BCMemNode *)pNode;
	if (node)
	{
		return node->GetFreespace();
	}
	return 0;
}

/*
* Pool properties operation
*/

void BCMemPool::SetTag(const char *pTag)
{
	this->m_pTag = pTag;
}

void BCMemPool::SetAbort(abortfunc_t abort_fn)
{
	this->m_pAbortFunc = abort_fn;
}

BCMemPool::abortfunc_t BCMemPool::GetAbort()
{
	return this->m_pAbortFunc;
}

BCMemNodeAllocator * BCMemPool::GetAllocator()
{
	return this->m_pAllocator;
}

void BCMemPool::SetReserveSize(uint32_t sizeReserve)
{
	if (m_pSelfFirstAvail+sizeReserve > m_pEndp)
	{
		sizeReserve = m_pEndp - m_pSelfFirstAvail;
	}
	m_reserveSize = sizeReserve;
}

uint32_t BCMemPool::GetReserveSize() const
{
	return m_reserveSize;
}

BCMemNode *BCMemPool::FindNode(const void *pMemPtr) const
{
	BCMemNode *pNode = m_pActive;
	do
	{
		if ((char *)pNode < (const char *)pMemPtr
			&& (const char *)pMemPtr < pNode->m_pEndp)
		{
			return pNode;
		}
		pNode = pNode->m_pNext;
	}
	while(pNode != m_pActive);

	return NULL;
}

char *BCMemPool::GetSelfFirstAvail() const
{
	return m_pSelfFirstAvail;
}

/*
* Cleanup
*/

void BCMemPool::RegisterCleanup(
	const void *data,
	status_t (*plain_cleanup_fn)(void *data),
	status_t (*child_cleanup_fn)(void *data))
{
	cleanup_t *c;

	if (this->m_pFreeCleanups)
	{
		/* reuse a cleanup structure */
		c = this->m_pFreeCleanups;
		this->m_pFreeCleanups = c->next;
	}
	else
	{
		c = (cleanup_t *)this->Alloc(sizeof(cleanup_t));
	}
	c->data = data;
	c->plain_cleanup_fn = plain_cleanup_fn;
	c->child_cleanup_fn = child_cleanup_fn;
	c->next = this->m_pCleanups;
	this->m_pCleanups = c;
}

void BCMemPool::RegisterPreCleanup(
	const void *data,
	status_t (*plain_cleanup_fn)(void *data))
{
	cleanup_t *c;

	if (this->m_pFreePreCleanups)
	{
		/* reuse a cleanup structure */
		c = this->m_pFreePreCleanups;
		this->m_pFreePreCleanups = c->next;
	}
	else
	{
		c = (cleanup_t *)this->Alloc(sizeof(cleanup_t));
	}
	c->data = data;
	c->plain_cleanup_fn = plain_cleanup_fn;
	c->next = this->m_pCleanups;
	this->m_pCleanups = c;
}

void BCMemPool::KillCleanup(
	const void *data,
	status_t (*cleanup_fn)(void *))
{
	cleanup_t *c, **lastp;

	c = this->m_pCleanups;
	lastp = &this->m_pCleanups;
	while (c)
	{
		if (c->data == data && c->plain_cleanup_fn == cleanup_fn)
		{
			*lastp = c->next;
			/* move to freelist */
			c->next = this->m_pFreeCleanups;
			this->m_pFreeCleanups = c;
			break;
		}

		lastp = &c->next;
		c = c->next;
	}
}

void BCMemPool::SetChildCleanup(
	const void *data,
	status_t (*plain_cleanup_fn)(void *),
	status_t (*child_cleanup_fn)(void *))
{
	cleanup_t *c;

	c = this->m_pCleanups;
	while (c)
	{
		if (c->data == data && c->plain_cleanup_fn == plain_cleanup_fn)
		{
			c->child_cleanup_fn = child_cleanup_fn;
			break;
		}

		c = c->next;
	}
}

BCMemPool::status_t BCMemPool::RunCleanup(
	void *data,
	status_t (*cleanup_fn)(void *))
{
	status_t result = STATUS_SUCCESS;
	this->KillCleanup(data, cleanup_fn);
#ifndef _DEBUG
	try
	{
#endif
		result = (*cleanup_fn)(data);
#ifndef _DEBUG
	}
	catch(...)
	{
		//LogError(_LOCAL_, "Unexcepted occured with clean up function.");
	}
#endif
	return result;
}

void BCMemPool::RunCleanups(cleanup_t **cref)
{
	cleanup_t *c = *cref;

	while (c)
	{
		*cref = c->next;
#ifndef _DEBUG
		try
		{
#endif
			(*c->plain_cleanup_fn)((void *)c->data);
#ifndef _DEBUG
		}
		catch(...)
		{
			//LogError(_LOCAL_, "Unexcepted occured with clean up function.");
		}
#endif
		c = *cref;
	}
}

/************************************************************************/
/* pool strings function  */
/************************************************************************/

#define MAX_SAVED_LENGTHS  6

char * BCMemPool::strdup(const char *s)
{
	char *res;
	uint32_t len;

	if (s == NULL)
	{
		return NULL;
	}
	len = strlen(s) + 1;
	res = (char *)this->Alloc(len);
	memcpy(res, s, len);
	return res;
}

char * BCMemPool::strndup(const char *s, uint32_t n)
{
	char *res;
	const char *end;

	if (s == NULL)
	{
		return NULL;
	}
	end = (const char *)memchr(s, '\0', n);
	if (end != NULL)
		n = end - s;
	res = (char *)this->Alloc(n + 1);
	memcpy(res, s, n);
	res[n] = '\0';
	return res;
}

char * BCMemPool::strmemdup(const char *s, uint32_t n)
{
	char *res;

	if (s == NULL)
	{
		return NULL;
	}
	res = (char *)this->Alloc(n + 1);
	memcpy(res, s, n);
	res[n] = '\0';
	return res;
}

void * BCMemPool::memdup(const void *m, uint32_t n)
{
	void *res;

	if (m == NULL)
		return NULL;
	res = this->Alloc(n);
	memcpy(res, m, n);
	return res;
}



char * BCMemPool::vstrcat(const char *pFirstStr, va_list vargs)
{
	char *cp, *argp, *res;
	uint32_t saved_lengths[MAX_SAVED_LENGTHS];
	int nargs = 0;

	/* Pass one --- find length of required string */

	uint32_t len = 0;
	va_list adummy;

	va_copy(adummy, vargs);

	cp = *(char **)&pFirstStr;
	do
	{
		uint32_t cplen = strlen(cp);
		if (nargs < MAX_SAVED_LENGTHS)
		{
			saved_lengths[nargs++] = cplen;
		}
		len += cplen;
	}while ((cp = va_arg(adummy, char *)) != NULL);

	/* Allocate the required string */

	res = (char *) this->Alloc(len + 1);
	cp = res;

	/* Pass two --- copy the argument strings into the result space */

	va_copy(adummy, vargs);

	nargs = 0;
	argp = *(char **)&pFirstStr;
	do
	{
		if (nargs < MAX_SAVED_LENGTHS)
		{
			len = saved_lengths[nargs++];
		}
		else
		{
			len = strlen(argp);
		}

		memcpy(cp, argp, len);
		cp += len;
	}while ((argp = va_arg(adummy, char *)) != NULL);

	/* Return the result string */

	*cp = '\0';

	return res;
}

char * BCMemPool::strcat(const char *pFirstStr, ...)
{
	char *cp, *argp, *res;
	uint32_t saved_lengths[MAX_SAVED_LENGTHS];
	int nargs = 0;

	/* Pass one --- find length of required string */

	uint32_t len = 0;
	va_list adummy;

	va_start(adummy, pFirstStr);

	cp = *(char **)&pFirstStr;
	do
	{
		uint32_t cplen = strlen(cp);
		if (nargs < MAX_SAVED_LENGTHS)
		{
			saved_lengths[nargs++] = cplen;
		}
		len += cplen;
	}while ((cp = va_arg(adummy, char *)) != NULL);

	va_end(adummy);

	/* Allocate the required string */

	res = (char *) this->Alloc(len + 1);
	cp = res;

	/* Pass two --- copy the argument strings into the result space */

	va_start(adummy, pFirstStr);

	nargs = 0;
	argp = *(char **)&pFirstStr;
	do
	{
		if (nargs < MAX_SAVED_LENGTHS)
		{
			len = saved_lengths[nargs++];
		}
		else
		{
			len = strlen(argp);
		}

		memcpy(cp, argp, len);
		cp += len;
	}while ((argp = va_arg(adummy, char *)) != NULL);

	va_end(adummy);

	/* Return the result string */

	*cp = '\0';

	return res;
}

char * BCMemPool::strcatv(
	const struct iovec *vec,
	uint32_t nvec,
	uint32_t *nbytes)
{
	uint32_t i;
	uint32_t len;
	const struct iovec *src;
	char *res;
	char *dst;

	/* Pass one --- find length of required string */
	len = 0;
	src = vec;
	for (i = nvec; i; i--)
	{
		len += src->iov_len;
		src++;
	}
	if (nbytes)
	{
		*nbytes = len;
	}

	/* Allocate the required string */
	res = (char *) this->Alloc(len + 1);

	/* Pass two --- copy the argument strings into the result space */
	src = vec;
	dst = res;
	for (i = nvec; i; i--)
	{
		memcpy(dst, src->iov_base, src->iov_len);
		dst += src->iov_len;
		src++;
	}

	/* Return the result string */
	*dst = '\0';

	return res;
}

/*
* Reference count function
*/

long BCMemPool::IncRef()
{
	return (++m_nRefCount);
}

long BCMemPool::DecRef()
{
	if (m_nRefCount == 0)
	{
		// Error! double release
		//throw BCException(_T("BCMemPool::DecRef()"), _T("m_nRefCount is already 0"));
		//LogError(_LOCAL_, "BCMemPool::DefRef(), m_nRefCount is already 0");
	}

	long nRefCount = 0;
	if (0 == (--m_nRefCount))
	{
		//LOG_INFO(_LOCAL_, "BCMemPool destroyed.");
		Destroy();
	}
	else
	{
		nRefCount = m_nRefCount;
	}
	return nRefCount;
}

///////////////////////////////////////////////////////////////////////////////
// StackMemPool - BCMemPool delegate class
///////////////////////////////////////////////////////////////////////////////

StackMemPool::StackMemPool(
	bool bHaveOwnAllocator /* = false */,
	bool bAutoCreate /*= true*/)
		: m_pMemoryPool(NULL)
{
	if (bAutoCreate)
	{
		Create(bHaveOwnAllocator);
	}
}

StackMemPool::StackMemPool(
	BCMemPool *pParentPool,
	BCMemNodeAllocator *pAllocator /*= NULL*/)
		: m_pMemoryPool(NULL)
{
	m_pMemoryPool = BCMemNodeAllocator::CreatePool(pParentPool, pAllocator);
	if (m_pMemoryPool)
	{
		m_pMemoryPool->IncRef();
	}
}

// copy and assignment
StackMemPool::StackMemPool(const StackMemPool& src)
	: m_pMemoryPool(src.m_pMemoryPool)
{
	if (m_pMemoryPool)
	{
		m_pMemoryPool->IncRef();
	}
}

StackMemPool& StackMemPool::operator = (const StackMemPool& src)
{
	BCMemPool *pOldPool;

	pOldPool = m_pMemoryPool;
	m_pMemoryPool = src.m_pMemoryPool;
	if (m_pMemoryPool)
	{
		m_pMemoryPool->IncRef();
	}
	if (pOldPool)
	{
		pOldPool->DecRef();
	}
	return *this;
}

StackMemPool::~StackMemPool()
{
	if (m_pMemoryPool)
	{
		//LOG_INFO(_LOCAL_, "StackMemPool destroyed.");
		m_pMemoryPool->DecRef();
		m_pMemoryPool = NULL;
	}
}

// Compares
bool StackMemPool::operator ==(const StackMemPool &src)
{
	return m_pMemoryPool == src.m_pMemoryPool;
}

bool StackMemPool::operator !=(const StackMemPool &src)
{
	return m_pMemoryPool != src.m_pMemoryPool;
}

bool StackMemPool::operator ==(const void *p)
{
	return m_pMemoryPool == p;
}

bool StackMemPool::operator !=(const void *p)
{
	return m_pMemoryPool != p;
}

StackMemPool::operator const BCMemPool *() const
{
	return m_pMemoryPool;
}

bool StackMemPool::Create(bool bHaveOwnAllocator /*= true*/)
{
	if (bHaveOwnAllocator)
	{
		BCMemNodeAllocator *pAllocator = new BCMemNodeAllocator(
			BCMemNodeAllocator::SIZE_512B_BOUNDARY);
		if (pAllocator)
		{
			m_pMemoryPool = pAllocator->CreatePool(NULL, true);
			if (m_pMemoryPool)
			{
				pAllocator->SetOwner(m_pMemoryPool);
				m_pMemoryPool->IncRef();
				return true;
			}
		}
		else
		{
			ASSERT(0);
			throw BCException(__FUNCTION__, "No enough memory.");
		}
	}
	else
	{
		m_pMemoryPool = BCMemNodeAllocator::CreatePool(
			(BCMemPool *)NULL, (BCMemNodeAllocator *)NULL);
		if (m_pMemoryPool)
		{
			m_pMemoryPool->IncRef();
			return true;
		}
		else
		{
			throw BCException(__FUNCTION__, "Failed to create Memory Pool.");
		}
	}
	return false;
}

void StackMemPool::Assign(BCMemPool *pMemoryPool)
{
	BCMemPool *pOldPool;

	pOldPool = m_pMemoryPool;
	m_pMemoryPool = pMemoryPool;
	if (m_pMemoryPool)
	{
		m_pMemoryPool->IncRef();
	}
	if (pOldPool)
	{
		pOldPool->DecRef();
	}
}

void *StackMemPool::Alloc(uint32_t size)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->Alloc(size);
	}
	return NULL;
}

void *StackMemPool::Calloc(uint32_t size)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->Calloc(size);
	}
	return NULL;
}

uint8_t *StackMemPool::alloc(uint32_t size)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->alloc(size);
	}
	return NULL;
}

uint8_t *StackMemPool::allocz(int32_t n, uint32_t size)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->allocz(n, size);
	}
	return NULL;
}

void *StackMemPool::Realloc(
	const void *pOrigMem,
	uint32_t nOrigSize,
	uint32_t nNewSize)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->Realloc(pOrigMem, nOrigSize, nNewSize);
	}
	return NULL;
}

void StackMemPool::Clear(bool bKeepReserve /*= true*/)
{
	if (m_pMemoryPool && m_pMemoryPool->m_nRefCount == 1)
	{
		m_pMemoryPool->Clear(bKeepReserve);
	}
}

void StackMemPool::Clear(uint32_t reserveSize)
{
	if (m_pMemoryPool && m_pMemoryPool->m_nRefCount == 1)
	{
		m_pMemoryPool->Clear(reserveSize);
	}
}

void StackMemPool::Destroy()
{
	if (m_pMemoryPool)
	{
		m_pMemoryPool->Destroy();
		m_pMemoryPool = NULL;
	}
}

uint32_t StackMemPool::GetReserveSize() const
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->GetReserveSize();
	}
	return 0;
}

void StackMemPool::SetReserveSize(uint32_t nSize)
{
	if (m_pMemoryPool)
	{
		m_pMemoryPool->SetReserveSize(nSize);
	}
}

long StackMemPool::IncRef()
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->IncRef();
	}
	return 0;
}

long StackMemPool::DecRef()
{
	if (m_pMemoryPool)
	{
		long retval = m_pMemoryPool->DecRef();
		if (0 == retval)
		{
			m_pMemoryPool = NULL;
		}
		return retval;
	}
	return 0;
}

char *StackMemPool::GetSelfFirstAvail() const
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->m_pSelfFirstAvail;
	}
	return NULL;
}

BCMemPool *StackMemPool::GetInterPool() const
{
	return this->m_pMemoryPool;
}

/************************************************************************/
/* pool strings function  */
/************************************************************************/
char * StackMemPool::Strdup(const char *s)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->strdup(s);
	}
	return NULL;
}

char * StackMemPool::Strndup(const char *s, uint32_t n)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->strndup(s, n);
	}
	return NULL;
}

char * StackMemPool::Strmemdup(const char *s, uint32_t n)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->strmemdup(s, n);
	}
	return NULL;
}

void * StackMemPool::memdup(const void *m, uint32_t n)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->memdup(m, n);
	}
	return NULL;
}
/*
* @remark Note:the last parameter must be 'NULL'
*/
char * StackMemPool::vstrcat(const char *pFirstStr, va_list vargs)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->vstrcat(pFirstStr, vargs);
	}
	return NULL;
}
char * StackMemPool::Strcat(const char *pFirstStr, ...)
{
	char *result = NULL;
	if (m_pMemoryPool)
	{
		va_list vargs;
		va_start(vargs, pFirstStr);
		result = m_pMemoryPool->vstrcat(pFirstStr, vargs);
		va_end(vargs);
		return result;
	}
	return NULL;
}

char * StackMemPool::strcatv(
	const struct iovec *vec,
	uint32_t nvec,
	uint32_t *nbytes)
{
	if (m_pMemoryPool)
	{
		return m_pMemoryPool->strcatv(vec, nvec, nbytes);
	}
	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Class : SmallPool
///////////////////////////////////////////////////////////////////////////////

SmallPool::SmallPool()
	: StackMemPool(NULL, G_p1KBMemPoolAllocator)
{
	//
}

SmallPool::SmallPool(const SmallPool &other)
	: StackMemPool(other)
{
	//
}

SmallPool::SmallPool(const StackMemPool &other)
	: StackMemPool(other)
{
	//
}

SmallPool::~SmallPool()
{
	//
}

SmallPool &SmallPool::operator = (const SmallPool &other)
{
	StackMemPool::operator =(other);
	return *this;
}

SmallPool &SmallPool::operator = (const StackMemPool &other)
{
	StackMemPool::operator =(other);
	return *this;
}


///////////////////////////////////////////////////////////////////////////////
// class : BCFixedMemPool
///////////////////////////////////////////////////////////////////////////////

#undef max
#define max(x,y) (((x)>(y)) ? (x) : (y))

MemoryPoolReportFunc_t BCFixedMemPool::g_ReportFunc = 0;

//-----------------------------------------------------------------------------
// Error reporting...  (debug only)
//-----------------------------------------------------------------------------

void BCFixedMemPool::SetErrorReportFunc( MemoryPoolReportFunc_t func )
{
	g_ReportFunc = func;
}

//-----------------------------------------------------------------------------
// Purpose: Constructor
//-----------------------------------------------------------------------------

BCFixedMemPool::BCFixedMemPool(uint32_t blockSize, uint32_t numElements, uint32_t growMode)
{
	m_BlockSize = blockSize < sizeof(void*) ? sizeof(void*) : blockSize;
	m_BlocksPerBlob = numElements;
	m_PeakAlloc = 0;
	m_GrowMode = growMode;
	Init();
	AddNewBlob();
}

//-----------------------------------------------------------------------------
// Purpose: Frees the memory contained in the mempool, and invalidates it for
//			any further use.
// Input  : *memPool - the mempool to shutdown
//-----------------------------------------------------------------------------
BCFixedMemPool::~BCFixedMemPool()
{
	if (m_BlocksAllocated > 0)
	{
		ReportLeaks();
	}
	Clear();
}


//-----------------------------------------------------------------------------
// Resets the pool
//-----------------------------------------------------------------------------
void BCFixedMemPool::Init()
{
	m_NumBlobs = 0;
	m_BlocksAllocated = 0;
	m_pHeadOfFreeList = 0;
	m_BlobHead.m_pNext = m_BlobHead.m_pPrev = &m_BlobHead;
}


//-----------------------------------------------------------------------------
// Frees everything
//-----------------------------------------------------------------------------
void BCFixedMemPool::Clear()
{
	// Free everything..
	CBlob *pNext;
	for( CBlob *pCur = m_BlobHead.m_pNext; pCur != &m_BlobHead; pCur = pNext )
	{
		pNext = pCur->m_pNext;
		free( pCur );
	}
	Init();
}

//-----------------------------------------------------------------------------
// Purpose: Reports memory leaks
//-----------------------------------------------------------------------------

void BCFixedMemPool::ReportLeaks()
{
	if (!g_ReportFunc)
		return;

	g_ReportFunc("Memory leak: mempool blocks left in memory: %d\n", m_BlocksAllocated);

#ifdef _DEBUG
	// walk and destroy the free list so it doesn't intefere in the scan
	while (m_pHeadOfFreeList != NULL)
	{
		void *next = *((void**)m_pHeadOfFreeList);
		memset(m_pHeadOfFreeList, 0, m_BlockSize);
		m_pHeadOfFreeList = next;
	}

	g_ReportFunc("Dumping memory: \'");

	for( CBlob *pCur=m_BlobHead.m_pNext; pCur != &m_BlobHead; pCur=pCur->m_pNext )
	{
		// scan the memory block and dump the leaks
		char *scanPoint = (char *)pCur->m_Data;
		char *scanEnd = pCur->m_Data + pCur->m_NumBytes;
		bool needSpace = false;

		while (scanPoint < scanEnd)
		{
			// search for and dump any strings
			if (isprint(*scanPoint))
			{
				g_ReportFunc("%c", *scanPoint);
				needSpace = true;
			}
			else if (needSpace)
			{
				needSpace = false;
				g_ReportFunc(" ");
			}

			scanPoint++;
		}
	}

	g_ReportFunc("\'\n");
#endif // _DEBUG
}


//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
void BCFixedMemPool::AddNewBlob()
{
	int sizeMultiplier;

	if( m_GrowMode == GROW_SLOW )
	{
		sizeMultiplier = 1;
	}
	else if( m_GrowMode == GROW_NONE )
	{
		// Can only have one allocation when we're in this mode
		if( m_NumBlobs != 0 )
		{
			ASSERT( !"BCFixedMemPool::AddNewBlob: mode == GROW_NONE" );
			return;
		}
	}

	// GROW_FAST and GROW_NONE use this.
	sizeMultiplier = m_NumBlobs + 1;

	// maybe use something other than malloc?
	int nElements = m_BlocksPerBlob * sizeMultiplier;
	int blobSize = m_BlockSize * nElements;
	CBlob *pBlob = (CBlob*)MemoryAllocator( sizeof(CBlob) + blobSize - 1 );
	ASSERT( pBlob );

	// Link it in at the end of the blob list.
	pBlob->m_NumBytes = blobSize;
	pBlob->m_pNext = &m_BlobHead;
	pBlob->m_pPrev = pBlob->m_pNext->m_pPrev;
	pBlob->m_pNext->m_pPrev = pBlob->m_pPrev->m_pNext = pBlob;

	// setup the free list
	m_pHeadOfFreeList = pBlob->m_Data;
	ASSERT (m_pHeadOfFreeList);

	void **newBlob = (void**)m_pHeadOfFreeList;
	for (int j = 0; j < nElements-1; j++)
	{
		newBlob[0] = (char*)newBlob + m_BlockSize;
		newBlob = (void**)newBlob[0];
	}

	// null terminate list
	newBlob[0] = NULL;
	m_NumBlobs++;
}


void* BCFixedMemPool::Alloc()
{
	return Alloc( m_BlockSize );
}


//-----------------------------------------------------------------------------
// Purpose: Allocs a single block of memory from the pool.
// Input  : amount -
//-----------------------------------------------------------------------------
void *BCFixedMemPool::Alloc( uint32_t amount )
{
	void *returnBlock;

	if ( amount > (unsigned int)m_BlockSize )
		return NULL;

	if( !m_pHeadOfFreeList )
	{
		// returning NULL is fine in GROW_NONE
		if( m_GrowMode == GROW_NONE )
		{
			//Assert( !"BCFixedMemPool::Alloc: tried to make new blob with GROW_NONE" );
			return NULL;
		}

		// overflow
		AddNewBlob();

		// still failure, error out
		if( !m_pHeadOfFreeList )
		{
			ASSERT( !"BCFixedMemPool::Alloc: ran out of memory" );
			return NULL;
		}
	}
	m_BlocksAllocated++;
	m_PeakAlloc = max(m_PeakAlloc, m_BlocksAllocated);

	returnBlock = m_pHeadOfFreeList;

	// move the pointer the next block
	m_pHeadOfFreeList = *((void**)m_pHeadOfFreeList);

	return returnBlock;
}

//-----------------------------------------------------------------------------
// Purpose: Frees a block of memory
// Input  : *memBlock - the memory to free
//-----------------------------------------------------------------------------
void BCFixedMemPool::Free( void *memBlock )
{
	if ( !memBlock )
		return;  // trying to delete NULL pointer, ignore

#ifdef _DEBUG
	// check to see if the memory is from the allocated range
	bool bOK = false;
	for( CBlob *pCur=m_BlobHead.m_pNext; pCur != &m_BlobHead; pCur=pCur->m_pNext )
	{
		if (memBlock >= pCur->m_Data && (char*)memBlock < (pCur->m_Data + pCur->m_NumBytes))
		{
			bOK = true;
		}
	}
	ASSERT (bOK);
#endif // _DEBUG

#ifdef _DEBUG
	// invalidate the memory
	memset( memBlock, 0xDD, m_BlockSize );
#endif

	m_BlocksAllocated--;

	// make the block point to the first item in the list
	*((void**)memBlock) = m_pHeadOfFreeList;

	// the list head is now the new block
	m_pHeadOfFreeList = memBlock;
}

/************************************************************************/
/* BCMemPool function utilities definition  */
/************************************************************************/

const StackMemPool					NullStackMemPool(true, false);
BCMemNodeAllocator				*	G_p1KBMemPoolAllocator = NULL;
BCOnceS								G_Init_Once = BC_ONCE_INIT;

/************************************************************************/
/* BCMemPool function utilities definition  */
/************************************************************************/

void InitializeMemoryPoolOnce(void* arg)
{
	if ((++bc_pools_initialized) > 1)
	{
		return;
	}

	global_allocator = new BCMemNodeAllocator();
	if (global_allocator == NULL)
	{
		bc_pools_initialized = 0;
		return;
	}

	global_pool = BCMemNodeAllocator::CreatePool(NULL, global_allocator);
	if (global_pool == NULL)
	{
		delete global_allocator;
		global_allocator = NULL;
		bc_pools_initialized = 0;
		return;
	}

	global_pool->SetTag("global_pool");

	global_allocator->SetOwner(global_pool);

	global_allocator->SetMaxFree(1024*1024*512);

	// 1KB MemNodeAllocator initialize
	G_p1KBMemPoolAllocator = new BCMemNodeAllocator(
		BCMemNodeAllocator::SIZE_512B_BOUNDARY, 1024*1024*1024);
	if (G_p1KBMemPoolAllocator == NULL)
	{
		global_pool->Destroy();
		global_allocator = NULL;
		global_pool = NULL;
		return;
	}

	return;
}

bool InitializeMemoryPool()
{
	bc_once_do(&G_Init_Once, InitializeMemoryPoolOnce, NULL);

	return true;
}

bool UnInitializeMemoryPool()
{
	if (!bc_pools_initialized)
	{
		return true;
	}
	if ((--bc_pools_initialized) > 0)
	{
		return true;
	}
	global_pool->Destroy(); /* This will also destroy the mutex */
	global_pool = NULL;

	global_allocator = NULL;

	// Uninitialize 1KB MemNodeAllocator
	BC_SAFE_DELETE_PTR(G_p1KBMemPoolAllocator);

	return true;
}

#ifdef _DEBUG
// TEST
int32_t StackMemPool::Test()
{
	// test 1 : variable alloc
	{
		StackMemPool sMemPool;

		for (int i = 0;i < 10000; i++)
		{
			sMemPool.Alloc(i);
		}
	}
	return 0;
}

#endif // _DEBUG

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
