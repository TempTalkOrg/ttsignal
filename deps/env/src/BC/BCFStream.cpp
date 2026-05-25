
#include <BC/BCFStream.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class: BCFIStream
///////////////////////////////////////////////////////////////////////////////

BCFIStream::BCFIStream() 
	: m_pFile(NULL)
{
}

BCFIStream::~BCFIStream()
{
	Close();
}

bool BCFIStream::Open(LPCSTR lpFileName, LPCSTR lpszMode)
{
	Close();
	m_pFile = fopen(lpFileName, lpszMode);
	if (m_pFile)
	{
		m_strFileName = lpFileName;
		return true;
	}
	return false;
}

void BCFIStream::Close()
{
	if (m_pFile)
	{
		fclose(m_pFile);
		m_pFile = NULL;
	}
}

bool BCFIStream::Eof() const
{
	int64_t here;
	int64_t end;
	bool retval = false;

	if (m_pFile == NULL)
	{
		return false;
	}
	/* See if the current position equals the end of the file. */

	if (((here = fseek(m_pFile, 0, SEEK_CUR)) == -1) ||
		((end = fseek(m_pFile, 0, SEEK_END)) == -1))
		retval = false;
	else if (here == end)
		retval = true;
	else
	{
		fseek(m_pFile, here, SEEK_SET);
		retval = false;
	}
	return retval;
}

bool BCFIStream::IsValid() const
{
	return (!!m_pFile);
}

size_t BCFIStream::Read(void* buffer, size_t len)
{
	if (IsValid())
	{
		int nRead = fread(buffer, len, 1, m_pFile);
		if (nRead == 1)
		{
			return len;
		}
	}
	return 0;
}

size_t BCFIStream::Peek(void* buffer, size_t len)
{
	if(IsValid())
	{
		uint32_t pos = ftell(m_pFile);
		fread(buffer, len, 1, m_pFile);
		uint32_t pos2 = ftell(m_pFile);
		fseek(m_pFile, pos, SEEK_SET);
		if(pos2 > pos)
		{
			return pos2 - pos;
		}
	}
	return 0;
}

size_t BCFIStream::Skip(size_t len)
{
	if (IsValid())
	{
		uint32_t pos = ftell(m_pFile);
		fseek(m_pFile, len, SEEK_CUR);
		uint32_t pos2 = ftell(m_pFile);
		if (pos2 > pos)
		{
			return pos2 - pos;
		}
	}
	return 0;
}

size_t BCFIStream::Unskip(size_t len)
{
	if (IsValid())
	{
		uint32_t pos = ftell(m_pFile);
		len = BCMIN(len, pos);
		int32_t nSize = -(int32_t)len;
		fseek(m_pFile, nSize, SEEK_CUR);
		return nSize;
	}
	return 0;
}

size_t BCFIStream::ConsumedLength() const
{
	return IsValid()?ftell(m_pFile):0;
}

size_t BCFIStream::RemainingLength() const
{
	if (IsValid())
	{
		return UsedLength() - ftell(m_pFile);
	}
	return 0;
}

size_t BCFIStream::UsedLength() const
{
	if (IsValid())
	{
		int32_t start, size;

		start = ftell(m_pFile);
		if (fseek(m_pFile, 0, SEEK_END) != -1)
		{
			size = ftell(m_pFile);
			fseek(m_pFile, start, SEEK_SET);
			return size;
		}
	}
	return 0;
}

void   BCFIStream::Rewind(uint32_t offset)
{
	if (IsValid())
	{
		fseek(m_pFile, offset, SEEK_SET);
	}
}

LPCVOID	BCFIStream::Base() const
{
	throw BC_R_INVALIDPTR;
}

LPCVOID BCFIStream::Current() const
{
	throw BC_R_INVALIDPTR;
}

/*
 * File pointer operate
 */
int32_t	BCFIStream::Seek(int32_t nOffset, int32_t origin)
{
	return fseek(m_pFile, nOffset, origin);
}

///////////////////////////////////////////////////////////////////////////////
// class : BCFOStream
///////////////////////////////////////////////////////////////////////////////

BCFOStream::BCFOStream()
	: m_pFile(NULL)
{
}

BCFOStream::~BCFOStream()
{
	Close();
}

bool BCFOStream::Open(LPCSTR lpFileName, LPCSTR lpMode)
{
	Close();
	m_pFile = fopen(lpFileName, lpMode);
	if (m_pFile)
	{
		m_strFileName = lpFileName;
		return true;
	}
	return false;
}

void BCFOStream::Close()
{
	if (m_pFile)
	{
		fclose(m_pFile);
		m_pFile = NULL;
		m_strFileName.clear();
	}
}

bool BCFOStream::IsValid() const
{
	return !!m_pFile;
}

size_t BCFOStream::Write(const void* buf, size_t len)
{
	if (m_pFile)
	{
		return fwrite(buf, len, 1, m_pFile);
	}
	else
	{
		return 0;
	}
}

size_t BCFOStream::Skip(size_t nSize)
{
	if (m_pFile)
	{
		return fseek(m_pFile, nSize, SEEK_CUR);
	} 
	else
	{
		return 0;
	}
}

size_t BCFOStream::Unskip(size_t nSize)
{
	if (m_pFile)
	{
		int32_t nOffset = (int32_t)nSize;
		return fseek(m_pFile, -nOffset, SEEK_CUR);
	}
	else
	{
		return 0;
	}
}

size_t BCFOStream::UsedLength() const
{
	return ftell(m_pFile);
}

size_t BCFOStream::GetSize() const
{
	throw BC_R_INVALIDPTR;
}

void BCFOStream::Rewind()
{
	if (m_pFile)
	{
		fseek(m_pFile, 0, SEEK_SET);
	} 
}

void * BCFOStream::Base() const
{
	throw BC_R_INVALIDPTR;
}

void * BCFOStream::Current() const
{
	throw BC_R_INVALIDPTR;
}

int32_t BCFOStream::Seek(int32_t nOffset, int32_t origin)
{
	if (m_pFile)
	{
		return fseek(m_pFile, nOffset, origin);
	}
	else
	{
		return 0;
	}
}

int32_t BCFOStream::Tell()
{
	if (m_pFile)
	{
		return ftell(m_pFile);
	}
	else
	{
		return 0;
	}
}

int32_t BCFOStream::Flush()
{
	if (m_pFile)
	{
		return fflush(m_pFile);
	}
	else
	{
		return 0;
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace
///////////////////////////////////////////////////////////////////////////////
};

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
