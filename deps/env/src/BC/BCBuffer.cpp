
#include "BCBuffer.h"
#include "BC/Utils.h"
#include <errno.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : BC
///////////////////////////////////////////////////////////////////////////////

namespace BC
{

static const uint32_t G_nBlockSize = BCBufferData::BLOCK_SIZE;

///////////////////////////////////////////////////////////////////////////////
// class : BCFixedBuffer
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCFixedBuffer, 8);

BCFixedBuffer::BCFixedBuffer(
	uint32_t size /*= BCFixedBuffer::BLOCK_SIZE*/)
		: m_data(NULL)
		, m_nSize(0)
		, m_nUsed(0)
		, m_nCurrent(0)
{
	if (size)
	{
		m_data = malloc(size);
		ASSERT(m_data);
		if (!m_data)
		{
#ifdef _WIN32
			int32_t errNo = ::GetLastError();
#else // !_WIN32
			int32_t errNo = errno;
#endif // _WIN32
			printf("No enough memory![error no:%d]\n", errNo);
		}
		m_nSize = size;
	}
	Reset();
}

BCFixedBuffer::~BCFixedBuffer()
{
	free(m_data);
}

uint32_t BCFixedBuffer::Write(const void *pData, uint32_t nSize)
{
	uint32_t nDataLen, nWriteLen, nBufLen;
	uint8_t *pBuffer, *pU8Data;

	pU8Data = (uint8_t *)pData;
	nDataLen = nSize;
	nBufLen = m_nSize - m_nUsed;
	nWriteLen = nBufLen > nDataLen?nDataLen:nBufLen;
	if (nWriteLen == 0)
	{
		return 0;
	}

	// Write first data block into used bufferdata
	pBuffer = ((uint8_t *)m_data) + m_nUsed;
	memcpy(pBuffer, pU8Data, nWriteLen);
	m_nUsed += nWriteLen;

	return nWriteLen;
}

uint32_t BCFixedBuffer::Read(void *pBuffer, uint32_t nSize)
{
	uint32_t nReadLen;

	nReadLen = Peek(pBuffer, nSize);
	m_nCurrent += nReadLen;

	return nReadLen;
}

uint32_t BCFixedBuffer::Peek(void *pBuffer, uint32_t nSize)
{
	uint32_t nDataLen, nReadLen, nRemainLen;
	uint8_t *pData, *pU8Buffer;

	pU8Buffer = (uint8_t *)pBuffer;
	nDataLen = nSize;
	nRemainLen = m_nUsed - m_nCurrent;
	nReadLen = nRemainLen > nDataLen?nDataLen:nRemainLen;
	if (nReadLen == 0)
	{
		return 0;
	}

	// Write first data block into used bufferdata
	pData = ((uint8_t *)m_data) + m_nCurrent;
	memcpy(pU8Buffer, pData, nReadLen);

	return nReadLen;
}

uint32_t BCFixedBuffer::GetRemainingLength() const
{
	ASSERT(m_nUsed >= m_nCurrent);
	return m_nUsed - m_nCurrent;
}


uint32_t BCFixedBuffer::Forward(uint32_t nLen)
{
	uint32_t nForward;
	nForward = nLen < m_nUsed - m_nCurrent?nLen:m_nUsed - m_nCurrent;
	m_nCurrent += nForward;
	return nForward;
}

uint32_t BCFixedBuffer::Backward(uint32_t nLen)
{
	uint32_t nBackward;
	nBackward = nLen < m_nCurrent?nLen:m_nCurrent;
	m_nCurrent -= nBackward;
	return nBackward;
}

uint32_t BCFixedBuffer::Add(uint32_t nAddSize)
{
	uint32_t nResult = BCMIN(m_nSize - m_nUsed, nAddSize);
	m_nUsed += nResult;
	return nResult;
}

uint32_t BCFixedBuffer::Subtract(uint32_t nSubSize)
{
	uint32_t nSubtract = BCMIN(m_nUsed, nSubSize);
	m_nUsed -= nSubtract;
	return nSubtract;
}

uint32_t BCFixedBuffer::Size() const
{
	return m_nSize;
}

uint32_t BCFixedBuffer::UsedLength() const
{
	return m_nUsed;
}

uint32_t BCFixedBuffer::Space() const
{
	ASSERT(m_nSize >= m_nUsed);
	return m_nSize - m_nUsed;
}

void	* BCFixedBuffer::Base() const
{
	return m_data;
}

void BCFixedBuffer::Reset()
{
	m_nUsed = 0;
	m_nCurrent = 0;
}

void BCFixedBuffer::Rewind()
{
	m_nCurrent = 0;
}

void BCFixedBuffer::Flush()
{
	memset(m_data, 0xbe, m_nSize);
	m_nUsed = 0;
	m_nCurrent = 0;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCBufferData
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCBufferData, 8);

BCBufferData::BCBufferData()
	: m_nSize(BLOCK_SIZE)
{
	//Flush();
	//ShowTraceStack("BCBufferData constructor");
}

BCBufferData::~BCBufferData()
{
	//Flush();
	//ShowTraceStack("BCBufferData destructor");
}

uint32_t BCBufferData::Write(void *pData, uint32_t nSize, uint32_t nOffset)
{
	uint32_t nDataLen, nWriteLen, nBufLen;
	uint8_t *pBuffer, *pU8Data;

	pU8Data = (uint8_t *)pData;
	nDataLen = nSize;
	nBufLen = m_nSize - nOffset;
	nWriteLen = nBufLen > nDataLen?nDataLen:nBufLen;
	if (nWriteLen == 0)
	{
		return 0;
	}

	// Write first data block into used bufferdata
	pBuffer = ((uint8_t *)m_data) + nOffset;
	memcpy(pBuffer, pU8Data, nWriteLen);

	return nWriteLen;
}

uint32_t BCBufferData::Read(void *pBuffer, uint32_t nSize, uint32_t nOffset)
{
	uint32_t nDataLen, nReadLen, nRemainLen;
	uint8_t *pData, *pU8Buffer;

	pU8Buffer = (uint8_t *)pBuffer;
	nDataLen = nSize;
	nRemainLen = m_nSize - nOffset;
	nReadLen = nRemainLen > nDataLen?nDataLen:nRemainLen;
	if (nReadLen == 0)
	{
		return 0;
	}

	// Write first data block into used bufferdata
	pData = ((uint8_t *)m_data) + nOffset;
	memcpy(pU8Buffer, pData, nReadLen);

	return nReadLen;
}

void BCBufferData::Flush()
{
	memset(m_data, 0xbe, m_nSize);
}

///////////////////////////////////////////////////////////////////////////////
// class : BCBuffer
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(BCBuffer, 64);

BCBuffer::BCBuffer()
	: BCMagic(BCBUFFER_MAGIC)
	, m_nSize(0)
	, m_nUsed(0)
	, m_nCurrent(0)
{
}

BCBuffer::BCBuffer(const BCBuffer& other)
	: BCMagic(BCBUFFER_MAGIC)
	, m_lstData(other.m_lstData)
	, m_nSize(0)
	, m_nUsed(0)
	, m_nCurrent(0)
{
	operator = (other);
}


BCBuffer::~BCBuffer()
{

}

void BCBuffer::operator = (const BCBuffer &other)
{
	Reset();
	other.WriteTo(*this);
	//m_lstData = other.m_lstData;
	//m_nUsed = other.m_nUsed;
	//m_nSize = other.m_nSize;
	//m_pCurrBufData = NULL;
	//m_nUsedBufIndex = NULL;
	//m_nCurrent = 0;
}

uint32_t BCBuffer::Write(const void *pData, uint32_t nSize)
{
	uint32_t nDataLen, nWriteLen, nOffset, nUsedBufIndex;
	uint8_t *pU8Data;

	if (pData == NULL || nSize == 0)
	{
		return 0;
	}
	pU8Data = (uint8_t *)pData;
	nDataLen = nSize;

	while(nDataLen > 0)
	{
		// Check current write bufferdata
		if (m_nUsed == m_nSize) // Extend buffer if no space
		{
			BufferDataPtr buffer(new BCBufferData);
			m_lstData.push_back(buffer);
			m_nSize += buffer->m_nSize;
			nOffset = 0;
		}
		else
		{
			nOffset = m_nUsed % G_nBlockSize;
		}
		nUsedBufIndex = m_nUsed / G_nBlockSize;
		nWriteLen = m_lstData[nUsedBufIndex]->Write(pU8Data, nDataLen, nOffset);
		pU8Data += nWriteLen;
		nDataLen -= nWriteLen;
		m_nUsed += nWriteLen;
	}
	return nSize - nDataLen;
}

uint32_t BCBuffer::WriteTo(BCBuffer &refBuffer) const
{
	uint32_t nDataLen = m_nUsed;
	uint32_t nWriteLen;
	for (auto &it : m_lstData)
	{
		if (nDataLen <= 0)
		{
			break;
		}
		nWriteLen = BCMIN(nDataLen, G_nBlockSize);
		refBuffer.Write(it->m_data, nWriteLen);
		nDataLen -= nWriteLen;
	}
	return m_nUsed;
}

uint32_t BCBuffer::WriteTo(BCOStream &refWriter)
{
	void *pData;
	uint32_t nReadLen, nCurrent;

	nReadLen = 0;
	nCurrent = m_nCurrent;
	// Write remaining data to temporary buffer
	while((pData = ReadBlock(INFINITE, nReadLen)) && nReadLen > 0)
	{
		refWriter.Write(pData, nReadLen);
	}
	Rewind(nCurrent);
	return (m_nUsed - nCurrent);
}

uint32_t BCBuffer::WriteMultiCharAsHex(const char *szStrVal, uint32_t nSize)
{
	uint32_t nDecodeSize, nInLen, nWriteLen;
	char *pBinPtr, *pBinEnd, *pBuffer, *pWriter;

	nInLen = (uint32_t)::strlen(szStrVal);
	nInLen = BCMIN(nInLen, nSize);
	if (nInLen == 0)
	{
		return 0;
	}
	pBinPtr = const_cast<char*>(szStrVal);
	pBinEnd = pBinPtr + nInLen;
	pBuffer = (char *)malloc(nInLen + 1);
	ASSERT(pBuffer);
	if (pBuffer == NULL)
	{
		return 0;
	}
	pWriter = pBuffer;
	nDecodeSize = 0;
	for(; pBinPtr < pBinEnd; pBinPtr+=2)
	{
		if(!IsHexChar(*pBinPtr) || !IsHexChar(*(pBinPtr+1)))
			break;
		++nDecodeSize;
		*(pWriter++) = HEX2BYTE(*pBinPtr,*(pBinPtr+1));
	}
	nWriteLen = Write(pBuffer, nDecodeSize);
	free(pBuffer);
	return nWriteLen;
}

uint32_t BCBuffer::Read(void *pBuffer, uint32_t nSize)
{
	uint32_t nDataLen, nReadLen, nOffset, nBufSize, nCurrBufIndex;
	uint8_t *pU8Buffer;

	if (pBuffer == NULL || nSize == 0)
	{
		return 0;
	}
	pU8Buffer = (uint8_t *)pBuffer;
	nBufSize = BCMIN(nSize, m_nUsed - m_nCurrent);
	nDataLen = nBufSize;

	while (m_nCurrent < m_nUsed && nDataLen > 0)
	{
		nCurrBufIndex = m_nCurrent / G_nBlockSize;
		nOffset = m_nCurrent % G_nBlockSize;
		nReadLen = m_lstData[nCurrBufIndex]->Read(pU8Buffer, nDataLen, nOffset);
		pU8Buffer += nReadLen;
		nDataLen -= nReadLen;
		m_nCurrent += nReadLen;
	}
	return nBufSize - nDataLen;
}

uint32_t BCBuffer::ReadFrom(BCIStream &refReader, uint32_t nSize)
{
	uint32_t nDataLen, nBufSize, nWriteLen;
	uint8_t *pU8Buffer;

	nDataLen = BCMIN(nSize, refReader.RemainingLength());
	nWriteLen = 0;
	while(nDataLen > 0 && refReader.RemainingLength() > 0)
	{
		nBufSize = nDataLen;
		pU8Buffer = (uint8_t *)GetWritableBlock(nBufSize);
		nWriteLen += refReader.Read(pU8Buffer, nBufSize);
		UngetWritableBlock(nBufSize);
		nDataLen -= nBufSize;
	}
	return nWriteLen;
}

uint32_t BCBuffer::Peek(void *pBuffer, uint32_t nSize)
{
	uint32_t nDataLen, nPeekLen, nOffset, nBufSize, nCurrBufIndex;
	uint8_t *pU8Buffer;
	uint32_t nCurrPeek = m_nCurrent;

	if (pBuffer == NULL || nSize == 0)
	{
		return 0;
	}
	pU8Buffer = (uint8_t *)pBuffer;
	nBufSize = BCMIN(nSize, m_nUsed - nCurrPeek);
	nDataLen = nBufSize;

	while (nCurrPeek < m_nUsed && nDataLen > 0)
	{
		nCurrBufIndex = nCurrPeek / G_nBlockSize;
		nOffset = nCurrPeek % G_nBlockSize;
		nPeekLen = m_lstData[nCurrBufIndex]->Read(pU8Buffer, nDataLen, nOffset);
		pU8Buffer += nPeekLen;
		nDataLen -= nPeekLen;
		nCurrPeek += nPeekLen;
	}
	return nBufSize - nDataLen;
}

uint32_t BCBuffer::RemainingLength() const
{
	ASSERT(m_nUsed >= m_nCurrent);
	return m_nUsed - m_nCurrent;
}

void *BCBuffer::ReadBlock(uint32_t nSizeToRead, uint32_t &refReadSize)
{
	uint32_t nOffset, nDataLen, nCurrBufIndex;
	void *pRetval = NULL;

	nOffset = m_nCurrent % G_nBlockSize;
	nDataLen = BCMIN(G_nBlockSize - nOffset, m_nUsed - m_nCurrent);
	if (nDataLen > 0)
	{
		// Get current Read bufferdata
		nCurrBufIndex = m_nCurrent / G_nBlockSize;
		refReadSize = BCMIN(nSizeToRead, nDataLen);
		m_nCurrent += refReadSize;
		pRetval = (uint8_t *)m_lstData[nCurrBufIndex]->m_data + nOffset;
	}
	else
	{
		refReadSize = 0;
	}
	return pRetval;
}

void *BCBuffer::GetWritableBlock(uint32_t &refBlockSize)
{
	uint32_t nBlockSize;
	void *pBlock;

	pBlock = Used();
	nBlockSize = G_nBlockSize - m_nUsed%G_nBlockSize;
	if (nBlockSize < refBlockSize)
	{
		refBlockSize = nBlockSize;
	}
	return pBlock;
}

void BCBuffer::UngetWritableBlock(uint32_t nBlockSize)
{
	ASSERT(nBlockSize + m_nUsed%G_nBlockSize <= G_nBlockSize);
	Add(nBlockSize);
}

uint32_t BCBuffer::UsedLength() const
{
	return m_nUsed;
}

uint32_t BCBuffer::ConsumedLength() const
{
	return m_nCurrent;
}

void *BCBuffer::Used() const
{
	uint32_t nUsedBufIndex, nOffset;

	nUsedBufIndex = m_nUsed / G_nBlockSize;
	if (nUsedBufIndex == m_lstData.size())
	{
		BufferDataPtr buffer(new BCBufferData);
		m_lstData.push_back(buffer);
		m_nSize += buffer->m_nSize;
		return buffer->m_data;
	}
	else
	{
		nOffset = m_nUsed % G_nBlockSize;
		return ((uint8_t *)m_lstData[nUsedBufIndex]->m_data) + nOffset;
	}
}

void *BCBuffer::Current() const
{
	uint32_t nCurrBufIndex, nOffset;

	nCurrBufIndex = m_nCurrent / G_nBlockSize;
	if (m_nCurrent == m_lstData.size())
	{
		BufferDataPtr buffer(new BCBufferData);
		m_lstData.push_back(buffer);
		m_nSize += buffer->m_nSize;
		return buffer->m_data;
	}
	else
	{
		nOffset = m_nCurrent % G_nBlockSize;

		return ((uint8_t*)m_lstData[nCurrBufIndex]->m_data) + nOffset;
	}
}

void *BCBuffer::Base() const
{
	if (m_lstData.size() > 0)
	{
		return m_lstData[0]->m_data ;
	}
	else
	{
		BufferDataPtr buffer(new BCBufferData);
		m_lstData.push_back(buffer);
		m_nSize += buffer->m_nSize;
		return buffer->m_data;
	}
}

uint32_t BCBuffer::Forward(uint32_t nLen)
{
	uint32_t nForward;

	nForward = BCMIN(nLen, (m_nUsed - m_nCurrent));
	m_nCurrent += nForward;
	return nForward;
}

uint32_t BCBuffer::Backward(uint32_t nLen)
{
	uint32_t nBackward;

	nBackward = BCMIN(nLen, m_nCurrent);
	m_nCurrent -= nBackward;
	return nBackward;
}

uint32_t BCBuffer::Add(uint32_t nAddSize)
{
	BCBufferData *pAddBuf;
	uint32_t nResult;

	pAddBuf = NULL;
	nResult = m_nUsed + nAddSize;
	while(nResult >= m_nSize)
	{
		BufferDataPtr buffer(new BCBufferData);
		m_lstData.push_back(buffer);
		m_nSize += buffer->m_nSize;
	}
	m_nUsed += nAddSize;
	return nAddSize;
}

uint32_t BCBuffer::Subtract(uint32_t nSubSize)
{
	uint32_t nSubtract;

	nSubtract = nSubSize < m_nUsed?nSubSize:m_nUsed;
	m_nUsed -= nSubtract;
	return nSubtract;
}

void BCBuffer::Rewind(uint32_t offset)
{
	m_nCurrent = BCMIN(offset, m_nUsed);
}

void BCBuffer::Flush()
{
	m_lstData.clear();
	m_nUsed = 0;
	m_nCurrent = 0;
	m_nSize = 0;
}

void BCBuffer::Reset(int32_t nBufDataReserve /*= -1*/)
{
	m_nUsed = 0;
	m_nCurrent = 0;
	if (nBufDataReserve >= 0)
	{
		m_lstData.clear();
		m_nSize = 0;
		for (int32_t i = 0;i < nBufDataReserve;i++)
		{
			BufferDataPtr buffer(new BCBufferData);
			m_nSize += buffer->m_nSize;
			m_lstData.push_back(buffer);
		}
	}
}

uint32_t BCBuffer::GetBlockSize() const
{
	return G_nBlockSize;
}

uint32_t BCBuffer::GetBlockCount() const
{
	return m_lstData.size();
}

void BCBuffer::RemoveConsumed2()
{
	BufferDataPtr pBuffer, pUsedBuf;
	uint32_t nIndex, nBlockLen, nOffset, nReadLen, nRemaining;
	void *pData;

	if (m_nCurrent == 0)
	{
		return;
	}
	else if (m_nCurrent == m_nUsed)
	{
		Reset();
		return;
	}

	nIndex = 0;
	pBuffer = m_lstData[nIndex];
	pUsedBuf = NULL;
	nOffset = 0;
	nRemaining = 0;
	while(RemainingLength() > 0)
	{
		nOffset = 0;
		nBlockLen = G_nBlockSize;
		pData = ReadBlock(nBlockLen, nReadLen);
		if (pData == NULL)
		{
			break;
		}
		ASSERT(nBlockLen >= nReadLen);
		memcpy(pBuffer->m_data + nOffset, pData, nReadLen);
		pUsedBuf = pBuffer;
		nRemaining += nReadLen;
		if (RemainingLength() == 0)
		{
			break;
		}
		nOffset += nReadLen;
		nBlockLen = G_nBlockSize - nReadLen;
		if (nBlockLen > 0)
		{
			pData = ReadBlock(nBlockLen, nReadLen);
			ASSERT(pData != NULL);
			memcpy(pBuffer->m_data + nOffset, pData, nReadLen);
			nRemaining += nReadLen;
			if (RemainingLength() == 0)
			{
				break;
			}
			ASSERT(nBlockLen == nReadLen);
		}
		pBuffer = m_lstData[++nIndex];
	}
	m_nCurrent = 0;
	m_nUsed = nRemaining;
}

void BCBuffer::RemoveConsumed()
{
	if (m_nCurrent > 0)
	{
		BCBuffer buf;
		void *pData;
		uint32_t nReadLen;

		nReadLen = 0;
		// Write remaining data to temporary buffer
		while((pData = ReadBlock(G_nBlockSize, nReadLen)) && nReadLen > 0)
		{
			buf.Write(pData, nReadLen);
		}
		buf.RefClone(this);
	}
}

uint32_t BCBuffer::Extract(BCBuffer *pBuffer, uint32_t nBlocks)
{
	uint32_t nExtract, nUsed;
	BufferDataPtr pData;

	ASSERT(pBuffer != NULL);

	pBuffer->Reset(0);
	Rewind();

	if (m_nUsed == 0)
	{
		return 0;
	}

	nUsed = (m_nUsed + G_nBlockSize - 1)/G_nBlockSize;
	nExtract = BCMIN(nBlocks, nUsed);
	for (uint32_t i = 0;i < nExtract;i++)
	{
		pData = m_lstData.front();
		m_lstData.pop_back();
		ASSERT(pData != NULL);
		pBuffer->m_lstData.push_back(pData);
		pBuffer->m_nSize += pData->m_nSize;
	}
	if (nExtract < nUsed)
	{
		pBuffer->m_nUsed = pBuffer->m_nSize;
		m_nUsed -= pBuffer->m_nUsed;
		m_nSize -= pBuffer->m_nSize;
	}
	else
	{
		pBuffer->m_nUsed = m_nUsed;
		m_nUsed = 0;
		m_nSize -= pBuffer->m_nSize;
	}

	return nExtract;
}

BCBuffer *BCBuffer::Clone() const
{
	BCBuffer *pBuffer;

	pBuffer = new BCBuffer();
	if (pBuffer)
	{
		WriteTo(*pBuffer);
	}
	return pBuffer;
}

BCBuffer &BCBuffer::Clone(BCBuffer &destBuffer) const
{
	destBuffer.Reset();
	WriteTo(destBuffer);
	return destBuffer;
}

BCBuffer* BCBuffer::RefClone() const
{
	BCBuffer* pBuffer;

	pBuffer = new BCBuffer();
	if (pBuffer)
	{
		RefClone(pBuffer);
	}
	return pBuffer;
	//return Clone();
}

void BCBuffer::RefClone(BCBuffer* pDestBuffer) const
{
	pDestBuffer->m_lstData = m_lstData;
	pDestBuffer->m_nSize = m_nSize;
	pDestBuffer->m_nUsed = m_nUsed;
	pDestBuffer->m_nCurrent = m_nCurrent;
}

void *BCBuffer::MapAddress(uint32_t nSize)
{
	uint32_t nIndex;
	uint32_t nPos;
	BufferDataPtr pBufData;
	if (m_lstData.size() == 0 || nSize > m_lstData.size() * G_nBlockSize)
	{
		return NULL;
	}

	nIndex = nSize/G_nBlockSize;
	nPos = nSize%G_nBlockSize;
	if (nPos == 0 && nIndex > 0)
	{
		nIndex--;
	}
	pBufData = m_lstData[nIndex];
	if (pBufData != NULL)
	{
		return ((uint8_t *)pBufData->m_data) + nPos;
	}
	return NULL;
}

BCRESULT BCBuffer::ToString(std::string& strOut)
{

	void* pData;
	uint32_t nStartPos, nReadLen;

	strOut.clear();
	nStartPos = m_nCurrent;
	nReadLen = 0;
	// Write remaining data to temporary buffer
	while ((pData = ReadBlock(G_nBlockSize, nReadLen)) != NULL)
	{
		strOut.append((char *)pData, nReadLen);
	}
	m_nCurrent = nStartPos;
	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : BCBIStream
///////////////////////////////////////////////////////////////////////////////

BCBIStream::BCBIStream(BCBuffer* buffer, bool)
	: m_pBuffer(buffer)
{
}

BCBIStream::~BCBIStream()
{
	Release();
}

bool BCBIStream::Eof() const
{
	if(IsValid())
	{
		return RemainingLength() == 0;
	}
	return true;
}

bool BCBIStream::IsValid() const
{
	return !m_pBuffer.IsNull();
}

size_t BCBIStream::Read(void* buffer, size_t len)
{
	if (IsValid())
	{
		return m_pBuffer->Read(buffer, len);
	}
	return 0;
}

size_t BCBIStream::Peek(void* buffer, size_t len)
{
	if (IsValid())
	{
		return m_pBuffer->Peek(buffer, len);
	}
	return 0;
}

size_t BCBIStream::Skip(size_t len)
{
	if (IsValid())
	{
		return m_pBuffer->Forward(len);
	}
	return 0;
}

size_t BCBIStream::Unskip(size_t len)
{
	if (IsValid())
	{
		return m_pBuffer->Backward(len);
	}
	return 0;
}

size_t BCBIStream::ConsumedLength() const
{
	if (IsValid())
	{
		return m_pBuffer->ConsumedLength();
	}
	return 0;
}

size_t BCBIStream::RemainingLength() const
{
	if(IsValid())
	{
		return m_pBuffer->RemainingLength();
	}
	return 0;
}

size_t BCBIStream::UsedLength() const
{
	if(IsValid())
	{
		return m_pBuffer->UsedLength();
	}
	return 0;
}

void   BCBIStream::Rewind(uint32_t offset)
{
	if (IsValid())
	{
		m_pBuffer->Rewind(offset);
	}
}

LPCVOID BCBIStream::Base() const
{
	if (IsValid())
	{
		return m_pBuffer->Base();
	}
	else
	{
		throw BC_R_INVALIDPTR;
	}
	return NULL;
}

LPCVOID BCBIStream::Current() const
{
	if (IsValid())
	{
		return m_pBuffer->Current();
	}
	else
	{
		throw BC_R_INVALIDPTR;
	}
	return NULL;
}

BCBuffer* BCBIStream::Get()
{
	return m_pBuffer.Get();
}

const BCBuffer* BCBIStream::Get() const
{
	return m_pBuffer.Get();
}

BCBuffer* BCBIStream::Release()
{
	return m_pBuffer.Release();
}

///////////////////////////////////////////////////////////////////////////////
// class : BCBOStream
///////////////////////////////////////////////////////////////////////////////

BCBOStream::BCBOStream()
	: m_pBuffer(new BCBuffer())
	, m_bDeleteOnExit(true)
{
}

BCBOStream::BCBOStream(
	BCBuffer* pBuffer,
	bool bDeleteOnExit)
		: m_pBuffer(pBuffer)
		, m_bDeleteOnExit(bDeleteOnExit)
{
}

BCBOStream::~BCBOStream()
{
	if(!m_bDeleteOnExit)
	{
		Release();
	}
}

bool BCBOStream::IsValid() const
{
	return !m_pBuffer.IsNull();
}

size_t BCBOStream::Write(const void* buf, size_t len)
{
	if(IsValid() && buf && len > 0)
	{
		return m_pBuffer->Write(buf, len);
	}
	return 0;
}

size_t BCBOStream::Skip(size_t nSize)
{
	if (IsValid())
	{
		return m_pBuffer->Add(nSize);
	}
	return 0;
}

size_t BCBOStream::Unskip(size_t nSize)
{
	if (IsValid())
	{
		return m_pBuffer->Subtract(nSize);
	}
	return 0;
}

size_t BCBOStream::UsedLength() const
{
	if (IsValid())
	{
		return m_pBuffer->UsedLength();
	}
	return 0;
}

size_t BCBOStream::GetSize() const
{
	if (IsValid())
	{
		return m_pBuffer->GetTotalSize();
	}
	return 0;
}

void BCBOStream::Rewind()
{
	if (IsValid())
	{
		m_pBuffer->Subtract(m_pBuffer->UsedLength());
	}
}

void * BCBOStream::Base() const
{
	if (IsValid())
	{
		return m_pBuffer->Base();
	}
	else
	{
		throw BC_R_INVALIDPTR;
	}
	return 0;
}

void * BCBOStream::Current() const
{
	if (IsValid())
	{
		return m_pBuffer->Current();
	}
	else
	{
		throw BC_R_INVALIDPTR;
	}
	return 0;
}

BCBuffer* BCBOStream::Get()
{
	return m_pBuffer.Get();
}

const BCBuffer* BCBOStream::Get() const
{
	return m_pBuffer.Get();
}

BCBuffer* BCBOStream::Release()
{
	return m_pBuffer.Release();
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace :
///////////////////////////////////////////////////////////////////////////////

}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
