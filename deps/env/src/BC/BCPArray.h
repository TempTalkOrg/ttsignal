
#ifndef BC_BCPARRAY_INCLUDE__
#define BC_BCPARRAY_INCLUDE__

#include <BC/Exports.h>
#include <BC/BCFixedAlloc.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class : BCByteArray
///////////////////////////////////////////////////////////////////////////////

class BC_API BCByteArray
{
	DECLARE_FIXED_ALLOC(BCByteArray);
public:

	// Construction
	BCByteArray();

	// Attributes
	INT_PTR			GetSize() const;
	INT_PTR			GetCount() const;
	BOOL			IsEmpty() const;
	INT_PTR			GetUpperBound() const;
	void			SetSize(INT_PTR nNewSize, INT_PTR nGrowBy = -1);

	// Operations
	// Clean up
	void			FreeExtra();
	void			RemoveAll();

	// Accessing elements
	BYTE			GetAt(INT_PTR nIndex) const;
	void			SetAt(INT_PTR nIndex, BYTE newElement);

	BYTE		&	ElementAt(INT_PTR nIndex);

	// Direct Access to the element data (may return NULL)
	const BYTE	*	GetData() const;
	BYTE		*	GetData();

	// Potentially growing the array
	void			SetAtGrow(INT_PTR nIndex, BYTE newElement);

	INT_PTR			Add(BYTE newElement);

	INT_PTR			Append(LPCVOID lpData, size_t nSize);
	INT_PTR			Append(const BCByteArray& src);
	void			Copy(const BCByteArray& src);

	// overloaded operator helpers
	BYTE			operator[](INT_PTR nIndex) const;
	BYTE		&	operator[](INT_PTR nIndex);

	// Operations that move elements around
	void			InsertAt(INT_PTR nIndex, BYTE newElement, INT_PTR nCount = 1);

	void			RemoveAt(INT_PTR nIndex, INT_PTR nCount = 1);
	void			InsertAt(INT_PTR nStartIndex, BCByteArray* pNewArray);

	// Implementation
protected:
	BYTE		*	m_pData;	 // the actual array of data
	INT_PTR			m_nSize;     // # of elements (upperBound - 1)
	INT_PTR			m_nMaxSize;  // max allocated
	INT_PTR			m_nGrowBy;   // grow amount
	StackMemPool	m_sPool1;    // memory pool1
	StackMemPool	m_sPool2;    // memory pool2
	BOOL			m_bUsePool1; // pool reminder

public:
	~BCByteArray();
#ifdef _DEBUG
	void AssertValid() const;
#endif

protected:
	// local typedefs for class templates
	typedef BYTE BASE_TYPE;
	typedef BYTE BASE_ARG_TYPE;
};

////////////////////////////////////////////////////////////////////////////

inline INT_PTR BCByteArray::GetSize() const
	{ return m_nSize; }
inline INT_PTR BCByteArray::GetCount() const
	{ return m_nSize; }
inline BOOL BCByteArray::IsEmpty() const
	{ return m_nSize == 0; }
inline INT_PTR BCByteArray::GetUpperBound() const
	{ return m_nSize-1; }
inline void BCByteArray::RemoveAll()
	{ SetSize(0); }
inline BYTE BCByteArray::GetAt(INT_PTR nIndex) const
	{ ASSERT(nIndex >= 0 && nIndex < m_nSize);
		if( nIndex < 0 || nIndex >= m_nSize )
			throw -1;
		return m_pData[nIndex]; }
inline void BCByteArray::SetAt(INT_PTR nIndex, BYTE newElement)
	{ ASSERT(nIndex >= 0 && nIndex < m_nSize);
		if( nIndex < 0 || nIndex >= m_nSize )
			throw -1;
		m_pData[nIndex] = newElement; }

inline BYTE& BCByteArray::ElementAt(INT_PTR nIndex)
	{ ASSERT(nIndex >= 0 && nIndex < m_nSize);
		if( nIndex < 0 || nIndex >= m_nSize )
			throw -1;
		return m_pData[nIndex]; }
inline const BYTE* BCByteArray::GetData() const
	{ return (const BYTE*)m_pData; }
inline BYTE* BCByteArray::GetData()
	{ return (BYTE*)m_pData; }
inline INT_PTR BCByteArray::Add(BYTE newElement)
	{ INT_PTR nIndex = m_nSize;
		SetAtGrow(nIndex, newElement);
		return nIndex; }

inline BYTE BCByteArray::operator[](INT_PTR nIndex) const
	{ return GetAt(nIndex); }
inline BYTE& BCByteArray::operator[](INT_PTR nIndex)
	{ return ElementAt(nIndex); }


///////////////////////////////////////////////////////////////////////////////
// class : BCPtrArray
///////////////////////////////////////////////////////////////////////////////

class BC_API BCPtrArray
{
	DECLARE_FIXED_ALLOC(BCPtrArray);
public:
	// Construction
	BCPtrArray();

	// Attributes
	INT_PTR			GetSize() const;
	INT_PTR			GetCount() const;
	BOOL			IsEmpty() const;
	INT_PTR			GetUpperBound() const;
	void			SetSize(INT_PTR nNewSize, INT_PTR nGrowBy = -1);

	// Operations
	// Clean up
	void			FreeExtra();
	void			RemoveAll();

	// Accessing elements
	void		*	GetAt(INT_PTR nIndex) const;
	void			SetAt(INT_PTR nIndex, void* newElement);

	void		*&	ElementAt(INT_PTR nIndex);

	// Direct Access to the element data (may return NULL)
	const void	**	GetData() const;
	void		**	GetData();

	// Potentially growing the array
	void			SetAtGrow(INT_PTR nIndex, void* newElement);

	INT_PTR			Add(void* newElement);

	INT_PTR			Append(const BCPtrArray& src);
	void			Copy(const BCPtrArray& src);

	// overloaded operator helpers
	void		*	operator[](INT_PTR nIndex) const;
	void		*&	operator[](INT_PTR nIndex);

	// Operations that move elements around
	void			InsertAt(INT_PTR nIndex, void* newElement, INT_PTR nCount = 1);

	void			RemoveAt(INT_PTR nIndex, INT_PTR nCount = 1);
	void			InsertAt(INT_PTR nStartIndex, BCPtrArray* pNewArray);

	// Implementation
protected:
	void		**	m_pData;     // the actual array of data
	INT_PTR			m_nSize;     // # of elements (upperBound - 1)
	INT_PTR			m_nMaxSize;  // max allocated
	INT_PTR			m_nGrowBy;   // grow amount
	StackMemPool	m_sPool1;    // memory pool1
	StackMemPool	m_sPool2;    // memory pool2
	BOOL			m_bUsePool1; // pool reminder


public:
	~BCPtrArray();
#ifdef _DEBUG
	void AssertValid() const;
#endif

protected:
	// local typedefs for class templates
	typedef void* BASE_TYPE;
	typedef void* BASE_ARG_TYPE;
};



////////////////////////////////////////////////////////////////////////////

inline INT_PTR BCPtrArray::GetSize() const
	{ return m_nSize; }
inline INT_PTR BCPtrArray::GetCount() const
	{ return m_nSize; }
inline BOOL BCPtrArray::IsEmpty() const
	{ return m_nSize == 0; }
inline INT_PTR BCPtrArray::GetUpperBound() const
	{ return m_nSize-1; }
inline void BCPtrArray::RemoveAll()
	{ SetSize(0); }
inline void* BCPtrArray::GetAt(INT_PTR nIndex) const
	{ ASSERT(nIndex >= 0 && nIndex < m_nSize);
		if( nIndex < 0 || nIndex >= m_nSize )
			throw -1;
		return m_pData[nIndex]; }
inline void BCPtrArray::SetAt(INT_PTR nIndex, void* newElement)
	{ ASSERT(nIndex >= 0 && nIndex < m_nSize);
		if( nIndex < 0 || nIndex >= m_nSize )
			throw -1;
		m_pData[nIndex] = newElement; }

inline void*& BCPtrArray::ElementAt(INT_PTR nIndex)
	{ ASSERT(nIndex >= 0 && nIndex < m_nSize);
		if( nIndex < 0 || nIndex >= m_nSize )
			throw -1;
		return m_pData[nIndex]; }
inline const void** BCPtrArray::GetData() const
	{ return (const void**)m_pData; }
inline void** BCPtrArray::GetData()
	{ return (void**)m_pData; }
inline INT_PTR BCPtrArray::Add(void* newElement)
	{ INT_PTR nIndex = m_nSize;
		SetAtGrow(nIndex, newElement);
		return nIndex; }

inline void* BCPtrArray::operator[](INT_PTR nIndex) const
	{ return GetAt(nIndex); }
inline void*& BCPtrArray::operator[](INT_PTR nIndex)
	{ return ElementAt(nIndex); }


///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////

}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
