
#ifndef BC_BCFSTREAM_INCLUDE__
#define BC_BCFSTREAM_INCLUDE__

#include <BC/BCStream.h>


///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
/// class: BCFIStream
///
///	A Big Endian File Input Stream
///
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFIStream : public BCIStream
{
public:
	BCFIStream();
	virtual ~BCFIStream();
	bool			Open(LPCSTR lpFileName, LPCSTR lpszMode = "rb");
	void			Close();

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

	// File pointer operate
	int32_t			Seek(int32_t nOffset, int32_t origin);
protected:
	DECLARE_NO_COPY_CLASS(BCFIStream);
	FILE		*	m_pFile;
	BCPString		m_strFileName;
};

///////////////////////////////////////////////////////////////////////////////
/// class : BCFOStream
///
///	A Big Endian File Output Stream
///
///////////////////////////////////////////////////////////////////////////////

class BC_API BCFOStream : public BCOStream
{
public:
	BCFOStream();
	virtual ~BCFOStream();

	bool			Open(LPCSTR lpFileName, LPCSTR lpMode = "wb");
	void			Close();

	bool			IsValid() const;
	size_t			Write(const void* buf, size_t len);
	size_t			Skip(size_t nSize);
	size_t			Unskip(size_t nSize);
	size_t			UsedLength() const;
	size_t			GetSize() const;
	void			Rewind();
	void		*	Base() const;
	void		*	Current() const;

	// File pointer operate
	int32_t			Seek(int32_t nOffset, int32_t origin);
	int32_t 		Tell();
	// File stream specific
	int32_t			Flush();

protected:
	DECLARE_NO_COPY_CLASS(BCFOStream);
	FILE		*	m_pFile;
	BCPString		m_strFileName;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace
///////////////////////////////////////////////////////////////////////////////
};
#endif

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
