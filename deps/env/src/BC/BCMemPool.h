#if defined (_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

#ifndef BC_BCMEMPOOL_INCLUDE__
#define BC_BCMEMPOOL_INCLUDE__


#include <BC/Exports.h>
#include <BC/BCThread.h>

typedef struct iovec	iovec;


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

#define MAX_INDEX   20

///////////////////////////////////////////////////////////////////////////////
// BCMemNode
///////////////////////////////////////////////////////////////////////////////
class BC_API BCMemNode
{
public:
	void *operator new(size_t objSize, void *pObject); // for instance reuse
	void operator delete(void *, void *){};
	void operator delete(void *){};

protected:
	// Overrides
	virtual void *	Recycle();

protected:
	friend class BCMemNodeAllocator;
	friend class BCMemPool;

	BCMemNode(uint32_t bufferSize, uint32_t nSizeIndex);
	virtual ~BCMemNode();

	inline uint32_t GetFreespace() const;
	inline void *	NodeAlloc(uint32_t size);
	inline void		SetFirstAvail(void *pFirstAvail);
	inline uint32_t GetNodeSize() const;
	inline uint32_t GetSizeIndex() const;

protected:
	BCMemNode				*m_pNext;
	BCMemNode				**m_ppNodeRef;
	uint32_t				m_nSizeIndex;		/**< size */
	uint32_t				m_nFreeIndex;		/**< how much free */
	char					*m_pFirstAvail;     /**< pointer to first free memory */
	char					*m_pEndp;           /**< pointer to end of free memory */

private:

	DECLARE_NO_COPY_CLASS(BCMemNode);
};

///////////////////////////////////////////////////////////////////////////////
// BCMemPool
///////////////////////////////////////////////////////////////////////////////

class BCMemPool;

class BC_API BCMemNodeAllocator
{
	friend class BCMemPool;
	friend class BCMemNode;
public:
	typedef enum BoundaryIndexE
	{
		SIZE_512B_BOUNDARY			=  9,
		SIZE_1KB_BOUNDARY			= 10,
		SIZE_2KB_BOUNDARY			= 11,
		SIZE_4KB_BOUNDARY			= 12,
		SIZE_8KB_BOUNDARY			= 13,
		SIZE_16KB_BOUNDARY			= 14,
		SIZE_32KB_BOUNDARY			= 15,
		SIZE_64KB_BOUNDARY			= 16,
		SIZE_128KB_BOUNDARY			= 17,
		SIZE_256KB_BOUNDARY			= 18,
		SIZE_512KB_BOUNDARY			= 19,
		SIZE_1MB_BOUNDARY			= 20,
	}BoundaryIndexE;

#define DEFALT_BOUNDARY_INDEX SIZE_4KB_BOUNDARY

public:
	BCMemNodeAllocator(
		BoundaryIndexE eBoundaryIndex = DEFALT_BOUNDARY_INDEX,
		uint32_t nMaxFreeSize = 0);
	~BCMemNodeAllocator();

	void SetOwner(BCMemPool *owner);

	BCMemPool *GetOwner()const;

	void SetMaxFree(uint32_t in_size);

	uint32_t GetMinAllocSize() const;

	uint32_t GetBoundaryIndex() const;

	BCMemNode *GetNodeFromBucket(uint32_t index);

	BCMemNode *PutNodeIntoBucket(BCMemNode *pNode);

	BCMemNode *Allocate(uint32_t size);

	void DeAllocate(BCMemNode *node);

	void Destroy();

	BCMemPool *CreatePool(
		BCMemPool *pParentPool = NULL,
		bool bUseSameAllocator = true);

	// Static methods
	static BCMemPool *CreatePool(
		BCMemPool *pParentPool  = NULL ,
		BCMemNodeAllocator *pAllocator  = NULL );

protected:
	inline uint32_t CalcNodeSize(uint32_t size) const;
	inline uint32_t CalcFreeIndex(BCMemNode *pNode) const;
	inline void		NodeListInsert(
		BCMemNode *pNewNode,
		BCMemNode *pInsertPoint);
	inline void		NodeListRemove(BCMemNode *pNode);
private:
	BCSpinMutex				m_sMemberLock;
	// Node list operations lock, ensure only one thread can operate
	// node list at one time
	BCSpinMutex				m_sNodeListLock;
	/** largest used index into m_Free[], always < MAX_INDEX */
	uint32_t				m_nMaxIndex;
	/** Total size (in BOUNDARY_SIZE multiples) of unused memory before
	* blocks are given back. @see SetMaxFree().
	* @note Initialized to ALLOCATOR_MAX_FREE_UNLIMITED,
	* which means to never give back blocks.
	*/
	uint32_t				m_nMaxFreeIndex;
	/**
	* Memory size (in BOUNDARY_SIZE multiples) that currently must be freed
	* before blocks are given back. Range: 0..m_nMaxFreeIndex
	*/
	uint32_t				m_nCurrentFreeIndex;
	BCMemPool				*m_pOwner;
	/**
	* Lists of free nodes. Slot 0 is used for oversized nodes,
	* and the slots 1..MAX_INDEX-1 contain nodes of sizes
	* (i+1) * BOUNDARY_SIZE. Example for m_nBoundaryIndex == 12:
	* slot  0: nodes larger than 81920
	* slot  1: size  8192
	* slot  2: size 12288
	* ...
	* slot 19: size 81920
	*/
	BCMemNode				*m_Free[MAX_INDEX];

	uint32_t				m_nMinAllocSize;
	uint32_t				m_nBoundaryIndex;

	DECLARE_NO_COPY_CLASS(BCMemNodeAllocator);
};

///////////////////////////////////////////////////////////////////////////////
// BCMemPool
///////////////////////////////////////////////////////////////////////////////

class BC_API BCMemPool : public BCMemNode
{
public:
	typedef enum status_t
	{
		STATUS_SUCCESS		= 0,
		STATUS_ENOMEM		= 1,
		STATUS_ESYSERR		= 2,
		STATUS_EBUSY		= 3,
		STATUS_EINVALIDPTR	= 4,
		STATUS_EOF			= 5,
		STATUS_ESQLITE		= 6,
		STATUS_EZIP			= 7,
		STATUS_EPTHREAD		= 8,
		STATUS_EGENERAL		= 9,
		STATUS_ENOENT		= 10,
		STATUS_ENOTIMPL		= 11,
		STATUS_BADARG		= 12,
		STATUS_ENOTSOCK		= 13,
		STATUS_ENOSPC		= 14,
		STATUS_EINVAL		= 15,
		STATUS_EBADIP		= 16,
		STATUS_EBADMASK		= 17,
		STATUS_EEXIST		= 18,
		STATUS_ENOTHREAD	= 19,
		STATUS_ENOPOOL		= 20,
		STATUS_EDBD			= 21,
		STATUS_INCOMPLETE	= 22,
	}status_t;

	typedef struct cleanup_t
	{
		struct cleanup_t		*next;
		const void				*data;
		status_t (*plain_cleanup_fn)(void *data);
		status_t (*child_cleanup_fn)(void *data);
	}cleanup_t;

	typedef int (*abortfunc_t)(int retcode);

public:
	void *operator new(size_t objSize, void *pObject); // for instance reuse
	void operator delete(void *, void *){};
	void operator delete(void *){};

public :

	friend class BCMemNodeAllocator;
	friend class StackMemPool;
	friend void InitializeMemoryPoolOnce(void *arg);
	friend bool InitializeMemoryPool();
	friend bool UnInitializeMemoryPool();

protected:

	void *Alloc(uint32_t size);
	void *Calloc(uint32_t size);
	void *Realloc(
		const void *pOriginal,
		uint32_t oldSize,
		uint32_t newSize);
	uint8_t *alloc(uint32_t size);
	uint8_t *allocz(int32_t n, uint32_t size);

	void Clear(bool bKeepReserve = true);
	void Clear(uint32_t reserveSize);
	void *Recycle();
	void Destroy();

	void RegisterCleanup(
		const void *data,
		status_t (*plain_cleanup_fn)(void *data),
		status_t (*child_cleanup_fn)(void *data));
	void RegisterPreCleanup(
		const void *data,
		status_t (*plain_cleanup_fn)(void *data));
	void KillCleanup(
		const void *data,
		status_t (*cleanup_fn)(void *));
	void SetChildCleanup(
		const void *data,
		status_t (*plain_cleanup_fn)(void *),
		status_t (*child_cleanup_fn)(void *));
	status_t RunCleanup(
		void *data,
		status_t (*cleanup_fn)(void *));
	void RunCleanups(cleanup_t **cref);
	// pool properties operations
//public:
	void SetTag(const char *pTag);
	void SetAbort(abortfunc_t abort_fn);
	abortfunc_t GetAbort();
	BCMemNodeAllocator * GetAllocator();
	void SetReserveSize(uint32_t sizeReserve);
	uint32_t GetReserveSize() const;
	BCMemNode *FindNode(const void *pMemPtr) const;
	char *GetSelfFirstAvail() const;
public:
	static uint32_t GetNodeSize(void *pNode);
	static uint32_t GetNodeAvailSize(void *pNode);
	static void ClearRootPool();
protected:
	/************************************************************************/
	/* pool strings function  */
	/************************************************************************/
	char * strdup(const char *s);
	char * strndup(const char *s, uint32_t n);
	char * strmemdup(const char *s, uint32_t n);
	void * memdup(const void *m, uint32_t n);
	/*
	* @remark Note:the last parameter must be 'NULL'
	*/
	char * vstrcat(const char *pFirstStr, va_list vargs);
	char * strcat(const char *pFirstStr, ...);
	char * strcatv(
		const struct iovec *vec,
		uint32_t nvec,
		uint32_t *nbytes);
	/************************************************************************/
	/* reference count function  */
	/************************************************************************/
	long IncRef();
	long DecRef();

private:
	BCMemPool(
		BCMemPool *pParentPool,
		BCMemNodeAllocator * pAllocator,
		uint32_t bufferSize,
		uint32_t nIndex);
	~BCMemPool();

	DECLARE_NO_COPY_CLASS(BCMemPool);

private:
	cleanup_t				*	m_pCleanups;
	cleanup_t				*	m_pFreeCleanups;
	BCMemNodeAllocator		*	m_pAllocator;
	abortfunc_t					m_pAbortFunc;
	const char				*	m_pTag;
	BCMemNode				*	m_pActive;
	BCMemNode				*	m_pSelf; /* The node containing the pool itself */
	char					*	m_pSelfFirstAvail;
	cleanup_t				*	m_pPreCleanups;
	cleanup_t				*	m_pFreePreCleanups;

	bc_atomic_t					m_nRefCount;
	uint32_t					m_reserveSize;
};

///////////////////////////////////////////////////////////////////////////////
// stack base memory pool - BCMemPool delegate class
///////////////////////////////////////////////////////////////////////////////

class BC_API StackMemPool
{
public:
public:
	StackMemPool(
		bool bHaveOwnAllocator = false,
		bool bAutoCreate = true);
	StackMemPool(
		BCMemPool *pParentPool,
		BCMemNodeAllocator *pAllocator = NULL);
	StackMemPool(const StackMemPool& src);

	StackMemPool& operator=(const StackMemPool& src);
	~StackMemPool();

	// Compares
	bool operator == (const StackMemPool &src);
	bool operator != (const StackMemPool &src);
	bool operator == (const void *p);
	bool operator != (const void *p);

	operator const BCMemPool *() const;	// Intend to use 'if(pool)' syntax

	bool Create(bool bHaveOwnAllocator = true);

	void Assign(BCMemPool *pMemoryPool);

	void *Alloc(uint32_t size);

	void *Calloc(uint32_t size);

	uint8_t *alloc(uint32_t size);

	uint8_t *allocz(int32_t n, uint32_t size);

	void *Realloc(const void *pOrigMem, uint32_t nOrigSize, uint32_t nNewSize);

	void Clear(bool bKeepReserve = true);
	void Clear(uint32_t reserveSize);

	uint32_t GetReserveSize() const;

	void SetReserveSize(uint32_t nSize);

	long IncRef();

	long DecRef();

	char *GetSelfFirstAvail() const;

	/************************************************************************/
	/* pool strings function  */
	/************************************************************************/
	char * Strdup(const char *s);
	char * Strndup(const char *s, uint32_t n);
	char * Strmemdup(const char *s, uint32_t n);
	void * memdup(const void *m, uint32_t n);
	/*
	* @remark Note:the last parameter must be 'NULL'
	*/
	char * vstrcat(const char *pFirstStr, va_list vargs);
	char * Strcat(const char *pFirstStr, ...);
	char * strcatv(
		const struct iovec *vec,
		uint32_t nvec,
		uint32_t *nbytes);

#ifdef _DEBUG
	static int32_t	Test();
#endif

	// attributes operation
protected:
	BCMemPool *GetInterPool() const;

	void Destroy();
private:
	BCMemPool				*m_pMemoryPool; // heap based memory pool
};


///////////////////////////////////////////////////////////////////////////////
// Class : SmallPool
///////////////////////////////////////////////////////////////////////////////

class BC_API SmallPool : public StackMemPool
{
public:
	SmallPool();
	SmallPool(const SmallPool &other);
	SmallPool(const StackMemPool &other);
	~SmallPool();

	SmallPool & operator = (const SmallPool &other);
	SmallPool & operator = (const StackMemPool &other);
};

typedef SmallPool	KBPool;

//-----------------------------------------------------------------------------
// Methods to invoke the constructor, copy constructor, and destructor
//-----------------------------------------------------------------------------

template <class T>
inline void Construct( T* pMemory )
{
	new( pMemory ) T;
}

template <class T>
inline void CopyConstruct( T* pMemory, T const& src )
{
	new( pMemory ) T(src);
}

template <class T>
inline void Destruct( T* pMemory )
{
	pMemory->~T();

#ifdef _DEBUG
	memset( pMemory, 0xDD, sizeof(T) );
#endif
}

typedef void (*MemoryPoolReportFunc_t)( char const* pMsg, ... );

///////////////////////////////////////////////////////////////////////////////
// class : BCFixedMemPool
//-----------------------------------------------------------------------------
// Purpose: Optimized pool memory allocator
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFixedMemPool
{
public:
	// Ways the memory pool can grow when it needs to make a new blob.
	enum
	{
		GROW_NONE=0,		// Don't allow new blobs.
		GROW_FAST=1,		// New blob size is numElements * (i+1)  (ie: the blocks it allocates
							// get larger and larger each time it allocates one).
		GROW_SLOW=2			// New blob size is numElements.
	};

				BCFixedMemPool(uint32_t blockSize, uint32_t numElements, uint32_t growMode = GROW_FAST);
				~BCFixedMemPool();

	void*		Alloc();	// Allocate the element size you specified in the constructor.
	void*		Alloc( uint32_t amount );
	void		Free(void *pMem);

	// Frees everything
	void		Clear();

	// Error reporting...
	static void SetErrorReportFunc( MemoryPoolReportFunc_t func );


private:
	class CBlob
	{
	public:
		CBlob	*m_pPrev, *m_pNext;
		int		m_NumBytes;		// Number of bytes in this blob.
		char	m_Data[1];
	};


	// Resets the pool
	void		Init();
	void		AddNewBlob();
	void		ReportLeaks();


private:

	int			m_BlockSize;
	int			m_BlocksPerBlob;

	int			m_GrowMode;	// GROW_ enum.

	// FIXME: Change m_ppMemBlob into a growable array?
	CBlob			m_BlobHead;
	void			*m_pHeadOfFreeList;
	int				m_BlocksAllocated;
	int				m_PeakAlloc;
	unsigned short	m_NumBlobs;

	static MemoryPoolReportFunc_t g_ReportFunc;
};


//-----------------------------------------------------------------------------
// Wrapper macro to make an allocator that returns particular typed allocations
// and construction and destruction of objects.
//-----------------------------------------------------------------------------
template< class T >
class BCClassMemPool : public BCFixedMemPool
{
public:
	BCClassMemPool(int numElements, int growMode = GROW_FAST)	:
		BCFixedMemPool( sizeof(T), numElements, growMode ) {}

	T*		Alloc();
	void	Free( T *pMem );
};


template< class T >
T* BCClassMemPool<T>::Alloc()
{
	T *pRet = (T*)BCFixedMemPool::Alloc();
	if ( pRet )
	{
		Construct( pRet );
	}
	return pRet;
}


template< class T >
void BCClassMemPool<T>::Free(T *pMem)
{
	if ( pMem )
	{
		Destruct( pMem );
	}

	BCFixedMemPool::Free( pMem );
}


//-----------------------------------------------------------------------------
// Macros that make it simple to make a class use a fixed-size allocator
// Put DECLARE_FIXEDSIZE_ALLOCATOR in the private section of a class,
// Put DEFINE_FIXEDSIZE_ALLOCATOR in the CPP file
//-----------------------------------------------------------------------------
#define DECLARE_FIXEDSIZE_ALLOCATOR( _class )									\
   public:																		\
      inline void* operator new( size_t size ) { return s_Allocator.Alloc(size); }   \
      inline void* operator new( size_t size, int nBlockUse, const char *pFileName, int nLine ) { return s_Allocator.Alloc(size); }   \
      inline void  operator delete( void* p ) { s_Allocator.Free(p); }		\
      inline void  operator delete( void* p, int nBlockUse, const char *pFileName, int nLine ) { s_Allocator.Free(p); }   \
  private:																		\
      static   BCFixedMemPool   s_Allocator

#define DEFINE_FIXEDSIZE_ALLOCATOR( _class, _initsize, _grow )					\
   BCFixedMemPool   _class::s_Allocator(sizeof(_class), _initsize, _grow)


//-----------------------------------------------------------------------------
// Macros that make it simple to make a class use a fixed-size allocator
// This version allows us to use a memory pool which is externally defined...
// Put DECLARE_FIXEDSIZE_ALLOCATOR_EXTERNAL in the private section of a class,
// Put DEFINE_FIXEDSIZE_ALLOCATOR_EXTERNAL in the CPP file
//-----------------------------------------------------------------------------

#define DECLARE_FIXEDSIZE_ALLOCATOR_EXTERNAL( _class, _allocator )				\
   public:																		\
      inline void* operator new( size_t size )  { return s_pAllocator->Alloc(size); }   \
      inline void* operator new( size_t size, int nBlockUse, const char *pFileName, int nLine )  { return s_pAllocator->Alloc(size); }   \
      inline void  operator delete( void* p )   { s_pAllocator->Free(p); }		\
   private:																		\
      static   BCFixedMemPool*   s_pAllocator

#define DEFINE_FIXEDSIZE_ALLOCATOR_EXTERNAL( _class, _allocator )				\
   BCFixedMemPool*   _class::s_pAllocator = _allocator




///////////////////////////////////////////////////////////////////////////////
// namespace scope functionalities
///////////////////////////////////////////////////////////////////////////////

extern BCDLLEXPORT const StackMemPool NullStackMemPool;

#define NULLPOOL			NullStackMemPool

extern BCDLLEXPORT BCMemNodeAllocator	*	G_p1KBMemPoolAllocator;

bool InitializeMemoryPool();
bool UnInitializeMemoryPool();

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

} // End of namespace BC


#endif //BC_BCMEMPOOL_INCLUDE__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
