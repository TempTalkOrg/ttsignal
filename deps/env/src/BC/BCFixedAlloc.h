
#ifndef BC_BCFIXEDALLOC_INCLUDE__
#define BC_BCFIXEDALLOC_INCLUDE__

// fixalloc.h - declarations for fixed block allocator

#include <BC/Exports.h>
#include <BC/BCNodeList.h>
#include <BC/BCThread.h>
#include <BC/BCMemPool.h>

using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// Namespace :
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

class BCFObject;

///////////////////////////////////////////////////////////////////////////////
// class : CPlex
///////////////////////////////////////////////////////////////////////////////

struct CPlex     // warning variable length structure
{
	CPlex* pNext;
#ifndef _WIN64
#if (_AFX_PACKING >= 8)
	DWORD dwReserved[1];    // align on 8 byte boundary
#endif
#endif
	// BYTE data[maxNum*elementSize];

	void* data() { return this+1; }

	static CPlex* PASCAL Create(CPlex*& head, UINT_PTR nMax, UINT_PTR cbElement);
			// like 'calloc' but no zero fill
			// may throw memory exceptions

	void FreeDataChain();       // free this one and links
};

/////////////////////////////////////////////////////////////////////////////
// BCFixedAllocNoSync
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFixedAllocNoSync
{
// Constructors
public:
	BCFixedAllocNoSync(UINT nAllocSize, UINT nBlockSize = 64);

// Attributes
	UINT GetAllocSize() { return m_nAllocSize; }

// Operations
public:
	void* Alloc();  // return a chunk of memory of nAllocSize
	void Free(void* p); // free chunk of memory returned from Alloc
	void* AllocNode();  // return a chunk of memory of nAllocSize
	void FreeNode(void* p); // free chunk of memory returned from Alloc
	uint32_t FreeAll(uint32_t nRemaining = 0); // free everything allocated from this allocator

// Implementation
public:
	~BCFixedAllocNoSync();

protected:
	struct CNode
	{
		CNode* pNext;	// only valid when in free list
	};

	UINT			m_nAllocSize;	// size of each block from Alloc
	UINT			m_nBlockSize;	// number of blocks to get at a time
	CPlex		*	m_pBlocks;		// linked list of blocks (is nBlocks*nAllocSize)
	CNode		*	m_pNodeFree;	// first free node (NULL if no free nodes)
	uint64_t		m_nAllocCount;
	uint64_t		m_nUsageCount;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCFixedAlloc
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFixedAlloc 
	: public BCNodeList::Node
	, public BCFixedAllocNoSync
{
	typedef class BCFixedAllocNoSync base;

// Constructors
public:
	BCFixedAlloc(LPCSTR lpszClsName, UINT nAllocSize, UINT nBlockSize = 64);

// Operations
public:
	void		*	Alloc();		// return a chunk of memory of nAllocSize
	void			Free(void* p);	// free chunk of memory returned from Alloc
	uint32_t		FreeAll(uint32_t nRemaining = 0);// free everything allocated from this allocator
	uint64_t		GetAllocCount() const;
	uint64_t		GetUsageCount() const;
	inline LPCSTR	GetClsName() const
	{
		return m_szClsName;
	}

// Implementation
public:
	~BCFixedAlloc();

protected:
	BCSpinMutex				m_protect;
	char					m_szClsName[64];
};


///////////////////////////////////////////////////////////////////////////////
// class : BCFixedAllocObject
//       - Base class of some small fixed alloc object that need space less than
//         8 bytes
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFixedAllocObject
{
public:
	BCFixedAllocObject() : m_lpNoUsePointer(NULL){};
	~BCFixedAllocObject(){};

protected:
private:
	DECLARE_NO_COPY_CLASS(BCFixedAllocObject);
	void		*	m_lpNoUsePointer;
};

//#ifndef _DEBUG

// DECLARE_FIXED_ALLOC -- used in class definition
#define DECLARE_FIXED_ALLOC(class_name) \
public: \
	void* operator new(size_t size) \
	{ \
		ASSERT(size == s_alloc.GetAllocSize()); \
		UNUSED(size); \
		return s_alloc.Alloc(); \
	} \
	void* operator new(size_t, void* p) \
		{ return p; } \
	void operator delete(void *p1, void *p2){ UNUSED(p1);UNUSED(p2);}\
	void operator delete(void* p) { s_alloc.Free(p); } \
	void* operator new(size_t size, LPCSTR, int) \
	{ \
		ASSERT(size == s_alloc.GetAllocSize()); \
		UNUSED(size); \
		return s_alloc.Alloc(); \
	} \
protected: \
	static BCFixedAlloc s_alloc \

// IMPLEMENT_FIXED_ALLOC -- used in class implementation file
#define IMPLEMENT_FIXED_ALLOC(class_name, block_size) \
BCFixedAlloc class_name::s_alloc(#class_name, sizeof(class_name), block_size) \

// DECLARE_FIXED_ALLOC -- used in class definition
#define DECLARE_FIXED_ALLOC_NOSYNC(class_name) \
public: \
	void* operator new(size_t size) \
	{ \
		ASSERT(size == s_alloc.GetAllocSize()); \
		UNUSED(size); \
		return s_alloc.Alloc(); \
	} \
	void* operator new(size_t, void* p) \
		{ return p; } \
	void operator delete(void* p) { s_alloc.Free(p); } \
	void* operator new(size_t size, LPCSTR, int) \
	{ \
		ASSERT(size == s_alloc.GetAllocSize()); \
		UNUSED(size); \
		return s_alloc.Alloc(); \
	} \
protected: \
	static BCFixedAllocNoSync s_alloc \

// IMPLEMENT_FIXED_ALLOC_NOSYNC -- used in class implementation file
#define IMPLEMENT_FIXED_ALLOC_NOSYNC(class_nbame, block_size) \
BCFixedAllocNoSync class_name::s_alloc(sizeof(class_name), block_size) \

//#else //!_DEBUG
//
//#define DECLARE_FIXED_ALLOC(class_name) // nothing in debug
//#define IMPLEMENT_FIXED_ALLOC(class_name, block_size) // nothing in debug
//#define DECLARE_FIXED_ALLOC_NOSYNC(class_name) // nothing in debug
//#define IMPLEMENT_FIXED_ALLOC_NOSYNC(class_name, block_size) // nothing in debug
//
//#endif //!_DEBUG

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////
}

///////////////////////////////////////////////////////////////////////////////
// Get fixed alloc statistics :
///////////////////////////////////////////////////////////////////////////////
BC_API
BC::BCFObject *BCStatFixedAlloc(uint32_t nFilter);

BC_API
void BCFreeAllFixedAlloc(uint32_t eRemaining = 0);

#endif // BC_BCFIXEDALLOC_INCLUDE__

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
