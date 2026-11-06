
#ifndef BC_BCISTREAM_INCLUDE__
#define BC_BCISTREAM_INCLUDE__

#include "BC/Exports.h"
#include <BC/BCPString.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

class BCPString;
class BCBuffer;

///////////////////////////////////////////////////////////////////////////////
/// class: BCIStream
///
///	A Base class for a binary input stream which reads in big endian format
///
///////////////////////////////////////////////////////////////////////////////

class BC_API BCIStream
{
public:
	BCIStream();
	virtual ~BCIStream();

	virtual bool	Eof() const						= 0;
	virtual bool	IsValid() const					= 0;

	virtual size_t	Read(void* buffer, size_t len)	= 0;
	virtual size_t	Peek(void* buffer, size_t len)	= 0;
	virtual size_t	Skip(size_t len)				= 0;
	virtual size_t	ConsumedLength() const			= 0;
	virtual size_t	RemainingLength() const			= 0;
	virtual size_t	UsedLength() const				= 0;
	virtual void	Rewind(uint32_t offset = 0)		= 0;
	virtual LPCVOID	Base() const					= 0;
	virtual LPCVOID	Current() const					= 0;


	virtual size_t	ReadUInt8(uint8_t* val);
	virtual size_t	ReadUInt16BE(uint16_t* val);
	virtual size_t	ReadUInt16LE(uint16_t *val);
	virtual size_t	ReadUInt24BE(uint32_t* val);
	virtual size_t	ReadUInt24LE(uint32_t* val);
	virtual size_t	ReadUInt32BE(uint32_t* val);
	virtual size_t	ReadUInt32LE(uint32_t *val);
	virtual size_t	ReadUInt64BE(uint64_t* val);
	virtual size_t	ReadUInt64LE(uint64_t* val);
	virtual size_t	ReadFloat32BE(float32_t* val);
	virtual size_t	ReadFloat32LE(float32_t* val);
	virtual size_t	ReadFloat64BE(float64_t* val);
	virtual size_t	ReadFloat64LE(float64_t* val);
	/// Read len bytes into a string which does not include the null terminator
	virtual size_t	ReadStringExact(BCPString& str, size_t len);

	virtual size_t	PeekUInt8(uint8_t* val);
	virtual size_t	PeekUInt16BE(uint16_t* val);
	virtual size_t	PeekUInt16LE(uint16_t *val);
	virtual size_t	PeekUInt24BE(uint32_t* val);
	virtual size_t	PeekUInt24LE(uint32_t* val);
	virtual size_t	PeekUInt32BE(uint32_t *val);
	virtual size_t	PeekUInt32LE(uint32_t* val);
	virtual size_t	PeekUInt64BE(uint64_t* val);
	virtual size_t	PeekUInt64LE(uint64_t* val);
	virtual size_t	PeekFloat32BE(float32_t* val);
	virtual size_t	PeekFloat32LE(float32_t* val);
	virtual size_t	PeekFloat64BE(float64_t* val);
	virtual size_t	PeekFloat64LE(float64_t* val);
	/// Peek len bytes into a string which does not include the null terminator
	virtual size_t	PeekStringExact(BCPString& str, size_t len);

	void			SetUserData(void *pData);
	void		*	GetUserData() const;
private:
	DECLARE_NO_COPY_CLASS(BCIStream);
	void		*	m_pUserData;
};

///////////////////////////////////////////////////////////////////////////////
/// class: BCOStream
///
/// A Binary Big Endian output stream
///
///////////////////////////////////////////////////////////////////////////////

class BC_API BCOStream
{
public:
	BCOStream();
	virtual ~BCOStream();

	virtual bool    IsValid() const						= 0;
	virtual size_t  Write(const void* buf, size_t len)	= 0;
	virtual size_t	Skip(size_t nSize)					= 0;
	virtual size_t	Unskip(size_t nSize)				= 0;
	virtual size_t	UsedLength() const					= 0;
	virtual size_t	GetSize() const						= 0;
	virtual void	Rewind()							= 0;
	virtual void *	Base() const						= 0;
	virtual void *	Current() const						= 0;

	virtual size_t WriteFrom(BCIStream &refReader, size_t len);
	virtual size_t WriteFrom(BCBuffer &refBuffer, size_t len);
	virtual size_t WriteUInt8(uint8_t val);
	virtual size_t WriteUInt16BE(uint16_t val);
	virtual size_t WriteUInt16LE(uint16_t val);
	virtual size_t WriteUInt24BE(uint32_t val);
	virtual size_t WriteUInt24LE(uint32_t val);
	virtual size_t WriteUInt32BE(uint32_t val);
	virtual size_t WriteUInt32LE(uint32_t val);
	virtual size_t WriteUInt64BE(uint64_t val);
	virtual size_t WriteUInt64LE(uint64_t val);
	virtual size_t WriteFloat32BE(float32_t val);
	virtual size_t WriteFloat32LE(float32_t val);
	virtual size_t WriteFloat64BE(float64_t val);
	virtual size_t WriteFloat64LE(float64_t val);
	virtual size_t WriteStringExact(LPCSTR str);
	virtual size_t WriteStringExact(const BCPString& str);
	virtual size_t WriteStringWithNull(LPCSTR str);
	virtual size_t WriteStringWithNull(const BCPString& str);

	void			SetUserData(void *pData);
	void		*	GetUserData() const;

private:
	DECLARE_NO_COPY_CLASS(BCOStream);
	void		*	m_pUserData;
};

///////////////////////////////////////////////////////////////////////////////
/// class: BCFBIStream
///
///	An input stream that reads from an fixed length in memory byte buffer
///
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFBIStream : public BCIStream
{
public:
	BCFBIStream(
		const void* pBuffer,
		uint32_t nBufSize,
		bool bAudoDelete = false);
	virtual ~BCFBIStream();

	bool				Eof() const;
	bool				IsValid() const;
	size_t				Read(void* buffer, size_t len);
	size_t				Peek(void* buffer, size_t len);
	size_t				Skip(size_t len);
	size_t				Unskip(size_t nSize);
	size_t				ConsumedLength() const;
	size_t				RemainingLength() const;
	size_t				UsedLength() const;
	void				Rewind(uint32_t offset = 0);
	const void		*	Base() const;
	const void		*	Current() const;
private:
	DECLARE_NO_COPY_CLASS(BCFBIStream);
	const uint8_t	*	m_pBuffer;
	size_t				m_nUsedLen;
	size_t				m_nCurrent;
	bool				m_bAutoDelete;
};

///////////////////////////////////////////////////////////////////////////////
// class : BCFBOStream
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFBOStream : public BCOStream
{
public:
	BCFBOStream(
		void *lpBuffer,
		uint32_t nLen,
		bool bAutoDelete = false);
	virtual ~BCFBOStream();

	bool				IsValid() const;
	size_t				Write(const void* buf, size_t len);
	size_t				Skip(size_t nSize);
	size_t				Unskip(size_t nSize);
	size_t				UsedLength() const;
	size_t				GetSize() const;
	void				Rewind();
	void			*	Base() const;
	void			*	Current() const;
protected:
private:
	DECLARE_NO_COPY_CLASS(BCFBOStream);
	uint8_t			*	m_pBuffer;
	uint32_t			m_nSize;
	size_t				m_nUsedLen;
	bool				m_bAutoDelete;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace
///////////////////////////////////////////////////////////////////////////////
};
#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
