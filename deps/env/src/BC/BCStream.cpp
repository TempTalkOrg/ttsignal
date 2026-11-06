
#include "ByteOrder.h"
#include "BCStream.h"
#include "BCBuffer.h"

///////////////////////////////////////////////////////////////////////////////
// Namespace: BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

///////////////////////////////////////////////////////////////////////////////
// class: BCIStream
///////////////////////////////////////////////////////////////////////////////

BCIStream::BCIStream()
	: m_pUserData(0)
{
	//
}

BCIStream::~BCIStream()
{
	//
}

size_t BCIStream::ReadUInt8(uint8_t* val)
{
	return Read(val,sizeof(uint8_t));
}

size_t BCIStream::ReadUInt16BE(uint16_t* val)
{
	size_t bread = Read(val,sizeof(uint16_t));
	*val = ByteOrder::swapBytesShort(*val);
	return bread;
}

size_t BCIStream::ReadUInt16LE(uint16_t* val)
{
	return Read(val,sizeof(uint16_t));
}

size_t BCIStream::ReadUInt24BE(uint32_t* val)
{
	*val = 0;
	uint8_t octets[3];
	memset(octets, 0, 3);
	size_t bread = Read(octets,3);
	//std::cout<<"AFTER READING TRIO ::::"<<std::endl;
	//HexDump::dumpHex(//std::cout,octets,3);
	*val = ((uint32_t)octets[0]) << 16;
	*val |= ((uint32_t)octets[1]) << 8;
	*val |= ((uint32_t)octets[2]);
	//std::cout<<"Medium Value Should Be : "<<ret<<std::endl;
	return bread;
}

size_t BCIStream::ReadUInt24LE(uint32_t* val)
{
	*val = 0;
	uint8_t octets[3];
	memset(octets, 0, 3);
	size_t bread = Read(octets,3);
	//std::cout<<"AFTER READING TRIO ::::"<<std::endl;
	//HexDump::dumpHex(//std::cout,octets,3);
	*val = ((uint32_t)octets[0]);
	*val |= ((uint32_t)octets[1]) << 8;
	*val |= ((uint32_t)octets[2]) << 16;
	//std::cout<<"Medium Value Should Be : "<<ret<<std::endl;
	return bread;
}

size_t BCIStream::ReadUInt32BE(uint32_t* val)
{
	size_t bread = Read(val,sizeof(uint32_t));
	*val = ByteOrder::swapBytesLong(*val);
	return bread;
}

size_t BCIStream::ReadUInt32LE(uint32_t* val)
{
	return Read(val,sizeof(uint32_t));
}

size_t BCIStream::ReadUInt64BE(uint64_t* val)
{
	size_t bread = Read(val,sizeof(uint64_t));
	*val = ByteOrder::swapBytesLongLong(*val);
	return bread;
}

size_t BCIStream::ReadUInt64LE(uint64_t* val)
{
	return Read(val,sizeof(uint64_t));
}

size_t BCIStream::ReadFloat32BE(float32_t* val)
{
	size_t bread = Read(val,sizeof(float32_t));
	*val = ByteOrder::swapBytesFloat(*val);
	return bread;
}

size_t BCIStream::ReadFloat32LE(float32_t* val)
{
	return Read(val,sizeof(float32_t));
}

size_t BCIStream::ReadFloat64BE(float64_t* val)
{
	size_t bread = Read(val,sizeof(float64_t));
	*val = ByteOrder::swapBytesDouble(*val);
	return bread;
}

size_t BCIStream::ReadFloat64LE(float64_t* val)
{
	return Read(val,sizeof(float64_t));
}

/// Read len bytes into a string which does not include the null terminator
size_t BCIStream::ReadStringExact(BCPString& str, size_t len)
{
	str.clear();
	char *buf = str.GetWriteBuffer(len);
	memset(buf, 0, len);
	size_t bread = Read(buf,len);
	str.UngetWriteBuffer(len);
	return bread;
}

size_t BCIStream::PeekUInt8(uint8_t* val)
{
	return Peek(val,sizeof(uint8_t));
}

size_t BCIStream::PeekUInt16BE(uint16_t* val)
{
	size_t bread = Peek(val,sizeof(uint16_t));
	*val = ByteOrder::swapBytesShort(*val);
	return bread;
}

size_t BCIStream::PeekUInt16LE(uint16_t *val)
{
	return Peek(val, sizeof(uint16_t));
}

size_t BCIStream::PeekUInt24BE(uint32_t* val)
{
	*val = 0;
	uint8_t octets[3];
	memset(octets, 0, 3);
	size_t bread = Peek(octets,3);
	*val = ((uint32_t)octets[0]) << 16;
	*val |= ((uint32_t)octets[1]) << 8;
	*val |= ((uint32_t)octets[2]);
	//std::cout<<"Medium Value Should Be : "<<ret<<std::endl;
	return bread;
}

size_t BCIStream::PeekUInt24LE(uint32_t* val)
{
	*val = 0;
	uint8_t octets[3];
	memset(octets, 0, 3);
	size_t bread = Peek(octets,3);
	*val = ((uint32_t)octets[0]);
	*val |= ((uint32_t)octets[1]) << 8;
	*val |= ((uint32_t)octets[2]) << 16;
	//std::cout<<"Medium Value Should Be : "<<ret<<std::endl;
	return bread;
}

size_t BCIStream::PeekUInt32BE(uint32_t* val)
{
	size_t bread = Peek(val,sizeof(uint32_t));
	*val = ByteOrder::swapBytesLong(*val);
	return bread;
}

size_t BCIStream::PeekUInt32LE(uint32_t *val)
{
	return Peek(val, sizeof(uint32_t));
}

size_t BCIStream::PeekUInt64BE(uint64_t* val)
{
	size_t bread = Peek(val,sizeof(uint64_t));
	*val = ByteOrder::swapBytesLongLong(*val);
	return bread;
}

size_t BCIStream::PeekUInt64LE(uint64_t* val)
{
	return Peek(val,sizeof(uint64_t));
}

size_t BCIStream::PeekFloat32BE(float32_t* val)
{
	size_t bread = Peek(val,sizeof(float32_t));
	*val = ByteOrder::swapBytesFloat(*val);
	return bread;
}

size_t BCIStream::PeekFloat32LE(float32_t* val)
{
	return Peek(val,sizeof(float32_t));
}

size_t BCIStream::PeekFloat64BE(float64_t* val)
{
	size_t bread = Peek(val,sizeof(float64_t));
	*val = ByteOrder::swapBytesDouble(*val);
	return bread;
}

size_t BCIStream::PeekFloat64LE(float64_t* val)
{
	return Peek(val,sizeof(float64_t));
}

/// Peek len bytes into a string which does not include the null terminator
size_t BCIStream::PeekStringExact(BCPString& str, size_t len)
{
	str.clear();
	char *buf = str.GetWriteBuffer(len);
	memset(buf, 0, len);
	size_t bread = Peek(buf,len);
	str.UngetWriteBuffer(len);
	return bread;
}

void BCIStream::SetUserData(void *pData)
{
	m_pUserData = pData;
}

void *BCIStream::GetUserData() const
{
	return m_pUserData;
}

///////////////////////////////////////////////////////////////////////////////
// class: BCOStream
///////////////////////////////////////////////////////////////////////////////

BCOStream::BCOStream()
	: m_pUserData(0)
{
	//
}

BCOStream::~BCOStream()
{
	//
}

size_t BCOStream::WriteFrom(BCIStream &refReader, size_t len)
{
	size_t nDataLen;
	uint32_t nDataRead, nDataWrite;
	uint8_t szBuf[1024];

	nDataWrite = 0;
	nDataLen = len;
	nDataRead = nDataLen > sizeof(szBuf) ? sizeof(szBuf) : nDataLen;
	while (nDataRead > 0 && (nDataRead = refReader.Read(szBuf, nDataRead)) > 0)
	{
		nDataWrite += Write(szBuf, nDataRead);
		nDataLen -= nDataRead;
		nDataRead = nDataLen > sizeof(szBuf) ? sizeof(szBuf) : nDataLen;
	}
	return nDataWrite;
}

size_t BCOStream::WriteFrom(BCBuffer &refBuffer, size_t len)
{
	size_t nDataLen;
	uint32_t nDataRead, nDataWrite;
	void* pData;

	nDataWrite = 0;
	nDataLen = len;
	nDataRead = len;
	while ((pData = refBuffer.ReadBlock(nDataLen, nDataRead)) && nDataRead > 0)
	{
		nDataWrite += Write(pData, nDataRead);
		nDataLen -= nDataRead;
		nDataRead = nDataLen;
	}
	return nDataWrite;
}

size_t BCOStream::WriteUInt8(uint8_t val)
{
	return Write(&val,1);
}

size_t BCOStream::WriteUInt16BE(uint16_t val)
{
	val = ByteOrder::swapBytesShort(val);
	return Write(&val,2);
}

size_t BCOStream::WriteUInt16LE(uint16_t val)
{
	return Write(&val, 2);
}

size_t BCOStream::WriteUInt24BE(uint32_t val)
{
	uint8_t octets[3];

	octets[0] = (uint8_t)((val & 0xFF0000) >> 16);
	octets[1] = (uint8_t)((val & 0xFF00) >> 8);
	octets[2] = (uint8_t)(val & 0xFF);
	return Write(octets, 3);
}

size_t BCOStream::WriteUInt24LE(uint32_t val)
{
	uint8_t octets[3];

	octets[0] = (uint8_t)(val & 0xFF);
	octets[1] = (uint8_t)((val & 0xFF00) >> 8);
	octets[2] = (uint8_t)((val & 0xFF0000) >> 16);
	return Write(octets, 3);
}

size_t BCOStream::WriteUInt32BE(uint32_t val)
{
	uint8_t octets[4];

	octets[0] = (uint8_t)((val & 0xFF000000) >> 24);
	octets[1] = (uint8_t)((val & 0xFF0000) >> 16);
	octets[2] = (uint8_t)((val & 0xFF00) >> 8);
	octets[3] = (uint8_t)(val & 0xFF);
	return Write(octets, 4);
}

size_t BCOStream::WriteUInt32LE(uint32_t val)
{
	uint8_t octets[4];

	octets[0] = (uint8_t)(val & 0xFF);
	octets[1] = (uint8_t)((val & 0xFF00) >> 8);
	octets[2] = (uint8_t)((val & 0xFF0000) >> 16);
	octets[3] = (uint8_t)((val & 0xFF000000) >> 24);
	return Write(octets, 4);
}

size_t BCOStream::WriteUInt64BE(uint64_t val)
{
	val = ByteOrder::swapBytesLongLong(val);
	return Write(&val,8);
}

size_t BCOStream::WriteUInt64LE(uint64_t val)
{
	return Write(&val,8);
}

size_t BCOStream::WriteFloat32BE(float32_t val)
{
	val = ByteOrder::swapBytesFloat(val);
	return Write(&val,4);
}

size_t BCOStream::WriteFloat32LE(float32_t val)
{
	return Write(&val,4);
}

size_t BCOStream::WriteFloat64BE(float64_t val)
{
	val = ByteOrder::swapBytesDouble(val);
	return Write(&val,8);
}

size_t BCOStream::WriteFloat64LE(float64_t val)
{
	return Write(&val,8);
}

size_t BCOStream::WriteStringExact(LPCSTR str)
{
	return Write(str, strlen(str));
}

size_t BCOStream::WriteStringExact(const BCPString& str)
{
	return Write(str.sdata(), str.Len());
}

size_t BCOStream::WriteStringWithNull(LPCSTR str)
{
	return Write(str, strlen(str)+1);
}

size_t BCOStream::WriteStringWithNull(const BCPString& str)
{
	return Write(str.sdata(), str.Len()+1);
}

void BCOStream::SetUserData(void *pData)
{
	m_pUserData = pData;
}

void *BCOStream::GetUserData() const
{
	return m_pUserData;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCFBIStream
///////////////////////////////////////////////////////////////////////////////

BCFBIStream::BCFBIStream(
	const void* pBuffer,
	uint32_t nBufSize,
	bool bAudoDelete /* = false */)
		: m_pBuffer((const uint8_t *)pBuffer)
		, m_nUsedLen(nBufSize)
		, m_nCurrent(0)
		, m_bAutoDelete(bAudoDelete)
{
	//
}

BCFBIStream::~BCFBIStream()
{
	if (m_bAutoDelete)
	{
		BC_SAFE_DELETE_ARRAY(m_pBuffer);
	}
}

bool BCFBIStream::Eof() const
{
	if(IsValid())
	{
		return (m_nCurrent == m_nUsedLen);
	}
	return true;
}

bool BCFBIStream::IsValid() const
{
	return (m_pBuffer != NULL);
}

size_t BCFBIStream::Read(void* buffer, size_t len)
{
	size_t bread = Peek(buffer,len);
	Skip(bread);
	return bread;
}

size_t BCFBIStream::Peek(void* buffer, size_t len)
{
	size_t bcount = RemainingLength();
	if(bcount > len)
	{
		bcount = len;
	}
	if(bcount > 0)
	{
		memset(buffer, 0, len);
		memcpy(buffer, m_pBuffer + m_nCurrent, bcount);
	}

	return bcount;

}

size_t BCFBIStream::Skip(size_t len)
{
	size_t bcount = RemainingLength();
	if(bcount > len)
	{
		bcount = len;
	}
	m_nCurrent += bcount;
	return bcount;
}

size_t BCFBIStream::Unskip(size_t len)
{
	len = BCMIN(m_nCurrent, len);
	m_nCurrent -= len;
	return len;
}

size_t BCFBIStream::ConsumedLength() const
{
	return m_nCurrent;
}

size_t BCFBIStream::RemainingLength() const
{
	if(m_pBuffer)
	{
		return m_nUsedLen - m_nCurrent;
	}
	return 0;
}

size_t BCFBIStream::UsedLength() const
{
	if(m_pBuffer)
	{
		return m_nUsedLen;
	}
	return 0;
}

void   BCFBIStream::Rewind(uint32_t offset)
{
	m_nCurrent = BCMIN(offset, m_nUsedLen);
}

const void *BCFBIStream::Base() const
{
	return m_pBuffer;
}

const void *BCFBIStream::Current() const
{
	return m_pBuffer?m_pBuffer + m_nCurrent:NULL;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCFBOStream
///////////////////////////////////////////////////////////////////////////////

BCFBOStream::BCFBOStream(
	void *lpBuffer, 
	uint32_t nLen, 
	bool bAutoDelete /* = false */)
		: m_pBuffer((uint8_t *)lpBuffer)
		, m_nSize(nLen)
		, m_nUsedLen(0)
		, m_bAutoDelete(bAutoDelete)
{
	//
}

BCFBOStream::~BCFBOStream()
{
	if (m_bAutoDelete)
	{
		BC_SAFE_DELETE_ARRAY(m_pBuffer);
	}
}

bool BCFBOStream::IsValid() const
{
	return m_pBuffer != NULL;
}

size_t BCFBOStream::Write(const void* buf, size_t len)
{
	if (IsValid())
	{
		uint32_t nWriteSize;

		nWriteSize = BCMIN(m_nSize - m_nUsedLen, len);
		memcpy2(m_pBuffer + m_nUsedLen, buf, nWriteSize);
		m_nUsedLen += nWriteSize;

		return nWriteSize;
	}
	return 0;
}

size_t BCFBOStream::Skip(size_t nSize)
{
	nSize = BCMIN(nSize, m_nSize - m_nUsedLen);
	m_nUsedLen += nSize;
	return nSize;
}

size_t BCFBOStream::Unskip(size_t nSize)
{
	nSize = BCMIN(nSize, m_nUsedLen);
	m_nUsedLen -= nSize;
	return nSize;
}

size_t BCFBOStream::UsedLength() const
{
	return m_nUsedLen;
}

size_t BCFBOStream::GetSize() const
{
	return m_nSize;
}

void BCFBOStream::Rewind()
{
	m_nUsedLen = 0;
}

void *BCFBOStream::Base() const
{
	return m_pBuffer;
}

void *BCFBOStream::Current() const
{
	return m_pBuffer + m_nUsedLen;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace
///////////////////////////////////////////////////////////////////////////////
};

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
