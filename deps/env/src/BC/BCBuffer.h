
#ifndef BC_BCBUFFER_H_INCLUDED__
#define BC_BCBUFFER_H_INCLUDED__

#include <memory> // use std::shared_ptr
#include <string>
#include <deque>
#include <BC/Exports.h>
#include <BC/BCStream.h>
#include <BC/ScopedPointer.h>
#include <BC/BCMagic.h>
#include <BC/BCFixedAlloc.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

class BCBuffer;

///////////////////////////////////////////////////////////////////////////////
// class : BCFixedBuffer
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFixedBuffer
{
	DECLARE_FIXED_ALLOC(BCFixedBuffer);
public:
	// the initial size and also the size added by ResizeIfNeeded()
	enum { BLOCK_SIZE = 1024 };

	BCFixedBuffer(
		uint32_t size = BCFixedBuffer::BLOCK_SIZE);
	~BCFixedBuffer();

	uint32_t	Write(const void *pData, uint32_t nSize);
	uint32_t	Read(void *pBuffer, uint32_t nSize);
	uint32_t	Peek(void *pBuffer, uint32_t nSize);
	uint32_t	GetRemainingLength() const;
	uint32_t	Forward(uint32_t nLen);
	uint32_t	Backward(uint32_t nLen);
	uint32_t	Add(uint32_t nAddSize);
	uint32_t	Subtract(uint32_t nSubSize);
	uint32_t	Size() const;
	uint32_t	UsedLength() const;
	uint32_t	Space() const;
	void	*	Base() const;
	void		Reset();
	void		Rewind();
	void		Flush();

private:
	// the buffer containing the data
	void				*	m_data;
	// the size of the buffer
	uint32_t				m_nSize;
	// used size of the buffer
	uint32_t				m_nUsed;
	// current Read posetion
	uint32_t				m_nCurrent;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCBufferData - Fixed size buffer, cannt resize
// This class manages the actual data buffer pointer and is ref-counted.
///////////////////////////////////////////////////////////////////////////////

class BC_API BCBufferData
{
	DECLARE_FIXED_ALLOC(BCBufferData);

	friend class BCBuffer;
public:
	// the initial size and also the size added by ResizeIfNeeded()
	enum { BLOCK_SIZE = 8192 };

	BCBufferData();
	~BCBufferData();

	uint32_t	Write(void *pData, uint32_t nSize, uint32_t nOffset);
	uint32_t	Read(void *pBuffer, uint32_t nSize, uint32_t nOffset);
	void		Flush();

	// the size of the buffer
	uint32_t				m_nSize;
	// the buffer containing the data
	uint8_t					m_data[BLOCK_SIZE];
};

///////////////////////////////////////////////////////////////////////////////
// class : BCBuffer
// a buffer store data in fixed size BCBufferData list
///////////////////////////////////////////////////////////////////////////////

class BC_API BCBuffer
	: public BCMagic
{
	DECLARE_FIXED_ALLOC(BCBuffer);
public:
	BCBuffer();
	BCBuffer(const BCBuffer &other);
	~BCBuffer();

	void			operator = (const BCBuffer &);
	uint32_t		Write(const void *pData, uint32_t nSize);
	uint32_t		WriteTo(BCBuffer &refBuffer) const;
	uint32_t		WriteTo(BCOStream &refWriter);
	uint32_t		WriteMultiCharAsHex(const char *szStrVal, uint32_t nSize);
	uint32_t		Read(void *pBuffer, uint32_t nSize);
	uint32_t		ReadFrom(BCIStream &refReader, uint32_t nSize);
	uint32_t		Peek(void *pBuffer, uint32_t nSize);
	uint32_t		RemainingLength() const;
	void		*	ReadBlock(uint32_t nSizeToRead, uint32_t &refReadSize);
	void		*	GetWritableBlock(uint32_t &refBlockSize);
	void			UngetWritableBlock(uint32_t nBlockSize);
	uint32_t		UsedLength() const;
	uint32_t		ConsumedLength() const;
	void		*	Used() const;
	void		*	Current() const;
	void		*	Base() const;
	uint32_t		Forward(uint32_t nLen);
	uint32_t		Backward(uint32_t nLen);
	uint32_t		Add(uint32_t nAddSize);
	uint32_t		Subtract(uint32_t nSubSize);
	void			Rewind(uint32_t offset = 0);
	void			Flush();
	void			Reset(int32_t nBufDataReserve = -1);
	uint32_t		GetBlockSize() const;
	uint32_t		GetBlockCount() const;
	void			RemoveConsumed();
	void			RemoveConsumed2();
	uint32_t		Extract(BCBuffer *pBuffer, uint32_t nBlocks);
	BCBuffer 	*	Clone() const;
	BCBuffer 	&	Clone(BCBuffer &destBuffer) const;
	BCBuffer	*	RefClone() const;
	void			RefClone(BCBuffer* pDestBuffer) const;
	void		*	MapAddress(uint32_t nSize);
	BCRESULT		ToString(std::string& strOut);
	inline size_t	GetTotalSize() const
	{
		return m_nSize;
	}

protected:
private:
	//class BufferDataList : public TNodeList<BCBufferData>
	//{
	//public:
	//	BufferDataList() {}
	//	~BufferDataList()
	//	{
	//		Flush();
	//	}

	//	void Flush()
	//	{
	//		BCBufferData* pBufData;
	//		while ((pBufData = PopFront()) != NULL)
	//		{
	//			delete pBufData;
	//		}
	//	}

	//private:
	//	DECLARE_NO_COPY_CLASS(BufferDataList);
	//};
	//typedef std::shared_ptr<BufferDataList>	BufferDataListPtr;
	typedef std::shared_ptr<BCBufferData>	BufferDataPtr;
	typedef std::deque<BufferDataPtr>		BufferDataList;
	mutable BufferDataList		m_lstData;
	mutable uint32_t			m_nSize;
	uint32_t					m_nUsed;
	uint32_t					m_nCurrent;
};

#define BCBUFFER_MAGIC			BC_MAGIC('B', 'U', 'F', 'F')
#define VALID_BCBUFFER(buff)	BC_MAGIC_VALID(buff, BCBUFFER_MAGIC)

///////////////////////////////////////////////////////////////////////////////
// class : BCBIStream
///////////////////////////////////////////////////////////////////////////////

class BC_API BCBIStream : public BCIStream
{
public:
	BCBIStream(BCBuffer *pBuffer, bool bAutoDelete = false);
	virtual ~BCBIStream();

	bool			Eof() const;
	bool			IsValid() const;

	size_t			Read(void* buffer, size_t len);
	size_t			Peek(void* buffer, size_t len);
	size_t			Skip(size_t len);
	size_t			Unskip(size_t nSize);
	size_t			ConsumedLength() const;
	size_t			RemainingLength() const;
	size_t			UsedLength() const;
	void			Rewind(uint32_t offset = 0);
	LPCVOID			Base() const;
	LPCVOID			Current() const;
	BCBuffer*		Get();
	const BCBuffer* Get() const;
	BCBuffer*		Release();

private:
	DECLARE_NO_COPY_CLASS(BCBIStream);
	ScopedPointer<BCBuffer>		m_pBuffer;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCBOStream
///////////////////////////////////////////////////////////////////////////////

class BC_API BCBOStream : public BCOStream
{
public:
	BCBOStream();
	BCBOStream(BCBuffer* pBuffer, bool bDeleteOnExit = false);
	virtual ~BCBOStream();

	bool			IsValid() const;
	size_t			Write(const void* buf, size_t len);
	size_t			Skip(size_t nSize);
	size_t			Unskip(size_t nSize);
	size_t			UsedLength() const;
	size_t			GetSize() const;
	void			Rewind();
	void		*	Base() const;
	void		*	Current() const;
	BCBuffer*		Get();
	const BCBuffer* Get() const;
	BCBuffer*		Release();
private:
	DECLARE_NO_COPY_CLASS(BCBOStream);
	ScopedPointer<BCBuffer>		m_pBuffer;
	bool						m_bDeleteOnExit;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : BC
///////////////////////////////////////////////////////////////////////////////
}

#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
