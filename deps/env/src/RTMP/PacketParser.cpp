///////////////////////////////////////////////////////////////////////////////
// file : PacketParser.cpp
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////

#include <BC/BCException.h>
#include <BC/BCLog.h>
#include <BC/BCNet.h>
#include <BC/BCNetAddress.h>
#include <BC/Base64.h>
#include <BC/BCSockAddr.h>
#include <RTMP/FLVUtils.h>
#include "PacketParser.h"

using namespace FLVUtils;



///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

#define VALID_PACKET_TYPE(x)										\
	if ((!(x)) || (0x09 < (x) && 0x0F > (x)) || (0x16 < (x))) {		\
		throw BCException(__FUNCTION__, "Invalid packet type!");	\
	}

#define VALID_CHUNK_TYPE(x)											\
	if (3 < (x)) {													\
		throw BCException(__FUNCTION__, "Invalid base chunk type!");\
	}


///////////////////////////////////////////////////////////////////////////////
// class : PacketParser
///////////////////////////////////////////////////////////////////////////////

PacketParser::PacketParser()
	: m_pHandler(NULL)
	, m_sRecvBuffer()
	, m_sReader(&m_sRecvBuffer)
	, m_eRecvState(RECV_BASE_CHUNK_HEADER_BYTE)
	, m_nRequireDataSize(1)
	, m_nChunkDataLen(0)
	, m_nChannelId(0)
	, m_eChunkType(0)
	, m_nChunkSize(RTMP_DEFAULT_CHUNK_SIZE)
{
	SetChunkSize(RTMP_DEFAULT_CHUNK_SIZE);
}

PacketParser::~PacketParser()
{
	if (m_sChunk.base)
	{
		free(m_sChunk.base);
	}
}

BCRESULT PacketParser::Create(IChunkHandler *pHandler)
{
	ASSERT(pHandler != NULL);
	m_pHandler = pHandler;

	return BC_R_SUCCESS;
}

void PacketParser::Initialize()
{
	m_eRecvState = RECV_BASE_CHUNK_HEADER_BYTE;
	m_nRequireDataSize = 1;
	m_nChunkDataLen = 0;
	m_nChannelId = 0;
	m_eChunkType = 0;
	m_nChunkSize = RTMP_DEFAULT_CHUNK_SIZE;
	if (m_sChunk.base)
	{
		free(m_sChunk.base);
	}
	m_sChunk.Reset();
	m_sRegion.Reset();
	SetChunkSize(RTMP_DEFAULT_CHUNK_SIZE);
	m_mapRecvChunkHdr.clear();
	m_sRecvBuffer.Reset(0);
}

void PacketParser::Parse()
{
	BOOL bContinue = TRUE;

	while(bContinue)
	{
		if (m_nRequireDataSize > m_sRecvBuffer.RemainingLength())
		{
			break;
		}
		switch(m_eRecvState)
		{
		case RECV_BASE_CHUNK_HEADER_BYTE:
			bContinue = _ParseBaseChunkHeader();
			break;
		case RECV_EXT_CHANNEL_ID:
			bContinue = _ParseExtChannelId();
			break;
		case RECV_CHUNK_HEADER:
			bContinue = _ParseChunkHeader();
			break;
		case RECV_EXT_TIMESTAMP:
			bContinue = _ParseExtTimestamp();
			break;
		case RECV_CHUNK_DATA:
			bContinue = _FinishChunk();
			break;
		default:
			//LogError(_LOCAL_, "Invalid parse state[%"_U32BITARG_"]", m_eRecvState);
			break;
		}
	}
	if (m_sRecvBuffer.RemainingLength() == 0)
	{
		m_sRecvBuffer.Reset();
	}
	else
	{
		m_sRecvBuffer.RemoveConsumed();
	}
}

BCBuffer *PacketParser::GetRecvBuf()
{
	return &m_sRecvBuffer;
}

void PacketParser::SetChunkSize(uint32_t nSize)
{
	m_nChunkSize = nSize;
	if (m_nChunkSize > m_sChunk.length)
	{
		if (m_sChunk.base)
		{
			m_sChunk.base = (uint8_t *)realloc(m_sChunk.base, m_nChunkSize);
		} 
		else
		{
			m_sChunk.base = (uint8_t *)malloc(m_nChunkSize);
		}
		if (!m_sChunk.base)
		{
			throw BCException("PacketParser::SetChunkSize", 
				"No enough memory.");
		}
		m_sChunk.length = m_nChunkSize;
	}
}

uint32_t PacketParser::GetChunkSize() const
{
	return m_nChunkSize;
}

void PacketParser::Cleanup()
{
	m_mapRecvChunkHdr.clear();
	m_sRecvBuffer.Reset(0);
}

bool PacketParser::TrySetDataFrame(
	const PPacket &refPacket,
	PPacket &refDstPkt)
{
	BCBIStream sReader(&refDstPkt.m_sBody);
	static const char bufSetDataFrame[] =
	{
		0x02, 0x00, 0x0D, '@', 's', 'e', 't', 'D',
		'a', 't', 'a', 'F', 'r', 'a', 'm', 'e'
	};
	char sBuffer[sizeof(bufSetDataFrame)];

	refPacket.Clone(refDstPkt);
	if (sReader.RemainingLength() < sizeof(bufSetDataFrame))
	{
		return false;
	}
	sReader.Read(sBuffer, sizeof(bufSetDataFrame));
	if (memcmp(bufSetDataFrame, sBuffer, sizeof(bufSetDataFrame)) == 0)
	{
		refDstPkt.m_sBody.RemoveConsumed();
		refDstPkt.m_pHeader->m_nDataSize = sReader.RemainingLength();

		return true;
	}
	return false;
}

BOOL PacketParser::_RequireData(
	uint32_t nSize,
	uint32_t nForward,
	NetIoStateE eRecvState)
{
	UNUSED(nForward);
	// Change state needs previous process finished.
	if (m_nRequireDataSize == 0)
	{
		m_eRecvState = eRecvState;
	}
	m_nRequireDataSize += nSize;
	if (m_sRecvBuffer.RemainingLength() >= m_nRequireDataSize)
	{
		return TRUE;
	}
	return FALSE;
}

BOOL PacketParser::_RequireBaseChunkHeader()
{
	return _RequireData(1, 0, RECV_BASE_CHUNK_HEADER_BYTE);
}

BOOL PacketParser::_ParseBaseChunkHeader()
{
	uint8_t nMask;

	m_sReader.ReadUInt8(&nMask);
	ASSERT(m_nRequireDataSize >= 1);
	m_nRequireDataSize -= 1;

	m_eChunkType = nMask >> 6;
	VALID_CHUNK_TYPE(m_eChunkType);
	m_nChannelId = nMask & 0x3F;
	if (m_nChannelId > 65599)
	{
		throw BCException("PacketParser::_ParseBaseChunkHeader", 
			"Invalid channel id.");
	}
	// Get real channel id
	if (m_nChannelId == 0)
	{
		return _RequireData(1, 0, RECV_EXT_CHANNEL_ID);
	}
	else if (m_nChannelId == 1)
	{
		return _RequireData(2, 0, RECV_EXT_CHANNEL_ID);
	}
	else
	{
		return _RequireChunkHeader();
	}
}

BOOL PacketParser::_ParseExtChannelId()
{
	ASSERT(m_nChannelId == 0 || m_nChannelId == 1);
	if (m_nChannelId == 0)
	{
		uint8_t nByte;
		m_sReader.ReadUInt8(&nByte);
		ASSERT(m_nRequireDataSize >= 1);
		m_nRequireDataSize -= 1;
		m_nChannelId = 64 + nByte;
	}
	else	// m_nChannelId == 1
	{
		uint16_t nShort;

		m_sReader.ReadUInt16LE(&nShort);
		ASSERT(m_nRequireDataSize >= 2);
		m_nRequireDataSize -= 2;
		m_nChannelId = 64 + nShort;
	}

	// Recv remain header data
	return _RequireChunkHeader();
}

BOOL PacketParser::_RequireChunkHeader()
{
	uint32_t nHeaderSize = 0;

	switch(m_eChunkType)
	{
	case HEADER_NEW:
		nHeaderSize += 11;
		break;
	case HEADER_SAME_SOURCE:
		nHeaderSize += 7;
		break;
	case HEADER_TIMER_CHANGE:
		nHeaderSize += 3;
		break;
	case HEADER_CONTINUE:
		break;
	}
	if (nHeaderSize > 0)
	{
		return _RequireData(nHeaderSize, 0, RECV_CHUNK_HEADER);
	}
	else
	{
		return _ParseChunkHeader();
	}
}

BOOL PacketParser::_RequireExtTimestamp()
{
	PHeader &sChunkHdr = m_mapRecvChunkHdr[m_nChannelId];

	sChunkHdr.m_nChannelId = m_nChannelId;
	if (sChunkHdr.IsExtTime())
	{
		return _RequireData(4, 0, RECV_EXT_TIMESTAMP);
	}
	else
	{
		if (sChunkHdr.m_nDataSize > 0)
		{
			return _RequireChunkData(sChunkHdr);
		}
		else
		{
			return _RequireData(0, 0, RECV_CHUNK_DATA);
		}
	}
}

BOOL PacketParser::_RequireExtTimestamp(PHeader &refChunkHdr)
{
	refChunkHdr.m_nChannelId = m_nChannelId;
	if (refChunkHdr.IsExtTime())
	{
		return _RequireData(4, 0, RECV_EXT_TIMESTAMP);
	}
	else
	{
		if (refChunkHdr.m_nDataSize > 0)
		{
			return _RequireChunkData(refChunkHdr);
		}
		else
		{
			return _RequireData(0, 0, RECV_CHUNK_DATA);
		}
	}
}

BOOL PacketParser::_RequireChunkData(PHeader &sChunkHdr)
{
	uint32_t nChunkSize;

	if (sChunkHdr.m_nDataSize <= sChunkHdr.m_nFinishedSize)
	{
		//LogFatal(_LOCAL_, "Invalid chunk header.");
		throw BCException("PacketParser::_RequireChunkData", 
			"Invalid chunk header.");
		return FALSE;
	}

	nChunkSize = sChunkHdr.m_nDataSize - sChunkHdr.m_nFinishedSize;
	nChunkSize = BCMIN(nChunkSize, m_nChunkSize);
	return _RequireData(nChunkSize, 0, RECV_CHUNK_DATA);
}

BOOL PacketParser::_ParseChunkHeader()
{
	uint8_t nByte;
	uint32_t nTimestamp = 0;
	PHeader &sChunkHdr = m_mapRecvChunkHdr[m_nChannelId];

	sChunkHdr.m_nChannelId = m_nChannelId;
	switch (m_eChunkType)
	{
	case HEADER_NEW:
		{
			m_sReader.ReadUInt24BE(&nTimestamp);// absolute timestamp
			m_sReader.ReadUInt24BE(&sChunkHdr.m_nDataSize);
			m_sReader.ReadUInt8(&nByte);
			VALID_PACKET_TYPE(nByte);
			sChunkHdr.m_eDataType		= nByte;
			// Flash Player specially to write this value little endian
			m_sReader.ReadUInt32LE(&sChunkHdr.m_nStreamId);
			if (nTimestamp == 0xFFFFFF)
			{
				sChunkHdr.SetExtTime(true);
			}
			else
			{
				sChunkHdr.SetExtTime(false);
			}
			sChunkHdr.m_nTimestamp		= nTimestamp;
			ASSERT(m_nRequireDataSize >= 11);
			m_nRequireDataSize -= 11;
			if (sChunkHdr.m_nFinishedSize == 0) // first chunk
			{
				sChunkHdr.SetAbsTime(true);
				sChunkHdr.m_nTotalTime = nTimestamp;
				//LogDebug(_LOCAL_, "Timestamp : %" _U32BITARG_ "; totalTime : %" _U64BITARG_, 
				//	sChunkHdr.m_nTimestamp, sChunkHdr.m_nTotalTime);
			}
		}
		break;
	case HEADER_SAME_SOURCE:
		{
			m_sReader.ReadUInt24BE(&nTimestamp);// delta timestamp
			m_sReader.ReadUInt24BE(&sChunkHdr.m_nDataSize);
			m_sReader.ReadUInt8(&nByte);
			VALID_PACKET_TYPE(nByte);
			sChunkHdr.m_eDataType		= nByte;
			if (nTimestamp == 0xFFFFFF)
			{
				sChunkHdr.SetExtTime(true);
			}
			else
			{
				sChunkHdr.SetExtTime(false);
			}
			sChunkHdr.m_nTimestamp		= nTimestamp;
			ASSERT(m_nRequireDataSize >= 7);
			m_nRequireDataSize -= 7;
			if (sChunkHdr.m_nFinishedSize == 0) // first chunk
			{
				sChunkHdr.SetAbsTime(false);
				sChunkHdr.m_nTotalTime += nTimestamp;
				//LogDebug(_LOCAL_, "Timestamp : %" _U32BITARG_ "; totalTime : %" _U64BITARG_,
				//	sChunkHdr.m_nTimestamp, sChunkHdr.m_nTotalTime);
			}
		}
		break;
	case HEADER_TIMER_CHANGE:
		{
			m_sReader.ReadUInt24BE(&nTimestamp);// delta timestamp
			if (nTimestamp == 0xFFFFFF)
			{
				sChunkHdr.SetExtTime(true);
			}
			else
			{
				sChunkHdr.SetExtTime(false);
			}
			sChunkHdr.m_nTimestamp		= nTimestamp;
			ASSERT(m_nRequireDataSize >= 3);
			m_nRequireDataSize -= 3;
			if (sChunkHdr.m_nFinishedSize == 0) // first chunk
			{
				sChunkHdr.SetAbsTime(false);
				sChunkHdr.m_nTotalTime += nTimestamp;
				//LogDebug(_LOCAL_, "Timestamp : %" _U32BITARG_ "; totalTime : %" _U64BITARG_,
				//	sChunkHdr.m_nTimestamp, sChunkHdr.m_nTotalTime);
			}
		}
		break;
	case HEADER_CONTINUE:
		if (sChunkHdr.m_nFinishedSize == 0) // first chunk
		{
			sChunkHdr.SetAbsTime(false);
			sChunkHdr.m_nTotalTime += sChunkHdr.m_nTimestamp;
			//LogDebug(_LOCAL_, "Timestamp : %" _U32BITARG_ "; totalTime : %" _U64BITARG_,
			//	sChunkHdr.m_nTimestamp, sChunkHdr.m_nTotalTime);
		}
		break;
	}
	return _RequireExtTimestamp(sChunkHdr);
}

BOOL PacketParser::_ParseExtTimestamp()
{
	uint32_t nTimestamp;
	PHeader &sChunkHdr = m_mapRecvChunkHdr[m_nChannelId];

	if (!sChunkHdr.IsExtTime())
	{
		throw BCException("PacketParser::_ParseExtTimestamp", 
			"Not extension timestamp.");
	}
	nTimestamp = 0;
	m_sReader.ReadUInt32BE(&nTimestamp);
	ASSERT(m_nRequireDataSize >= 4);
	m_nRequireDataSize -= 4;
	sChunkHdr.m_nTimestamp = nTimestamp;
	if (sChunkHdr.m_nFinishedSize == 0) // first chunk
	{
		if (sChunkHdr.IsAbsTime())
		{
			sChunkHdr.m_nTotalTime = nTimestamp;
		} 
		else
		{
			sChunkHdr.m_nTotalTime += nTimestamp;
		}
	}

	return _RequireChunkData(sChunkHdr);
}

BOOL PacketParser::_FinishChunk()
{
	uint32_t nChunkSize;
	PHeader &sChunkHdr = m_mapRecvChunkHdr[m_nChannelId];

	nChunkSize = sChunkHdr.m_nDataSize - sChunkHdr.m_nFinishedSize;
	nChunkSize = BCMIN(nChunkSize, m_nChunkSize);
	sChunkHdr.m_nFinishedSize += nChunkSize;
	sChunkHdr.m_nChunkSize = nChunkSize;

	if (sChunkHdr.m_nFinishedSize > sChunkHdr.m_nDataSize)
	{
		throw BCException("PacketParser::_FinishChunk", 
			"Invalid packet size.");
	}

	// Check chunk data type
	if (sChunkHdr.m_nDataSize <= m_nChunkSize)
	{
		sChunkHdr.m_eChunkDataType = PHeader::CHUNK_SINGLE;
	}
	else if (sChunkHdr.m_nFinishedSize == m_nChunkSize)
	{
		sChunkHdr.m_eChunkDataType = PHeader::CHUNK_START;
	}
	else if (sChunkHdr.m_nFinishedSize == sChunkHdr.m_nDataSize)
	{
		sChunkHdr.m_eChunkDataType = PHeader::CHUNK_END;
	}
	else
	{
		sChunkHdr.m_eChunkDataType = PHeader::CHUNK_MIDDLE;
	}

	m_sReader.Read(m_sChunk.base, nChunkSize);
	ASSERT(m_nRequireDataSize >= nChunkSize);
	m_nRequireDataSize -= nChunkSize;

	m_sRegion.base = m_sChunk.base;
	m_sRegion.length = nChunkSize;
	// Notify chunk finished
	m_pHandler->OnChunkParsed(sChunkHdr, m_sRegion);

	if (sChunkHdr.m_nFinishedSize == sChunkHdr.m_nDataSize)
	{
		sChunkHdr.m_nFinishedSize = 0;
	}

	return _RequireBaseChunkHeader();
}

///////////////////////////////////////////////////////////////////////////////
// class : Notifier
///////////////////////////////////////////////////////////////////////////////

Notifier::Notifier()
	: m_eType(0)
	, m_pAction(NULL)
	, m_wParam(NULL)
	, m_lParam(0)
	, m_cbDestroy(NULL)
{
	//
}

Notifier::Notifier(
	uint32_t eType,
	uint32_t nId,
	LPFN_Notification pAction,
	void *wParam,
	uint64_t lParam,
	LPFN_NotifierDtor cbDestroy)
		: m_eType(eType)
		, m_pAction(pAction)
		, m_wParam(wParam)
		, m_lParam(lParam)
		, m_cbDestroy(cbDestroy)
{
	UNUSED(nId);
}

Notifier::~Notifier()
{
	//
}

Notifier &Notifier::operator = (const Notifier &refOther)
{
	m_eType		= refOther.m_eType;
	m_sHeader	= refOther.m_sHeader;
	m_pAction	= refOther.m_pAction;
	m_wParam	= refOther.m_wParam;
	m_lParam	= refOther.m_lParam;
	m_cbDestroy	= refOther.m_cbDestroy;

	return *this;
}

void Notifier::Init(
	uint32_t eType,
	uint32_t nStreamId,
	LPFN_Notification pAction,
	void *wParam,
	uint64_t lParam,
	LPFN_NotifierDtor cbDestroy)
{
	m_eType					= eType;
	m_sHeader.m_nStreamId	= nStreamId;
	m_pAction				= pAction;
	m_wParam				= wParam;
	m_lParam				= lParam;
	m_cbDestroy				= cbDestroy;
}

void Notifier::Init(
	uint32_t eType,
	uint32_t nStreamId,
	PHeader &refHeader,
	LPFN_Notification pAction,
	void *wParam,
	uint64_t lParam,
	LPFN_NotifierDtor cbDestroy)
{
	m_eType					= eType;
	m_sHeader				= refHeader;
	m_sHeader.m_nStreamId	= nStreamId;
	m_pAction				= pAction;
	m_wParam				= wParam;
	m_lParam				= lParam;
	m_cbDestroy				= cbDestroy;
}

void Notifier::Notify()
{
	if (m_pAction != NULL)
	{
		(m_pAction)(*this);
	}
}

void Notifier::Destroy()
{
	if (m_cbDestroy != NULL)
	{
		(m_cbDestroy)(*this);
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : MsgItem
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(MsgItem, 32);

MsgItem::MsgItem(uint32_t eType /*= MTYPE_INVALID*/)
	: m_eType(eType)
	, m_pBuffer(NULL)
	, m_pNotifier(NULL)
	, m_pPacket(NULL)
	, m_pPacketizer(NULL)
{
	if (MTYPE_NOTIFIER == eType)
	{
		m_pNotifier = new Notifier();
	} 
	else
	{
		m_pBuffer = new BCBuffer();
	}
}

MsgItem::~MsgItem()
{
	BC_SAFE_DELETE_PTR(m_pBuffer);
	BC_SAFE_DELETE_PTR(m_pNotifier);
	BC_SAFE_DELETE_PTR(m_pPacket);
}

///////////////////////////////////////////////////////////////////////////////
// class : MsgQueue
///////////////////////////////////////////////////////////////////////////////

MsgQueue::MsgQueue()
	: m_nChunkSize(RTMP_DEFAULT_CHUNK_SIZE)
	, m_bInitialized(FALSE)
	, m_tmBaseOfAllChannels(0)
{
	//
}

MsgQueue::~MsgQueue()
{
	Cleanup();
}

BCRESULT MsgQueue::Append(BCBuffer &refBody, uint32_t eType)
{
	if (refBody.RemainingLength() > 0)
	{
		MsgItem *pNewItem;
		pNewItem = new MsgItem(eType);
		if (!pNewItem)
		{
			throw BCException("MsgQueue::AppendEx", "No enough memory.");
		}
		refBody.RefClone(pNewItem->m_pBuffer);
		m_lstItems.PushBack(pNewItem);
	}

	return BC_R_SUCCESS;
}

BCRESULT MsgQueue::Append(
	PHeader &refHeader,
	BCBuffer &refBody,
	MsgItemPacketizer *pPacketizer)
{
	MsgItem *pItem = new MsgItem(refHeader.m_eDataType);
	if (!pItem)
	{
		return BC_R_NOMEMORY;
	}
	pItem->m_pPacket = new PPacket();
	if (!pItem->m_pPacket)
	{
		delete pItem;
		return BC_R_NOMEMORY;
	}
	*pItem->m_pPacket->m_pHeader = refHeader;
	refBody.RefClone(&pItem->m_pPacket->m_sBody);
	pItem->m_pPacketizer = pPacketizer;
	m_lstItems.PushBack(pItem);
	return BC_R_SUCCESS;
}

BCRESULT MsgQueue::Append(Notifier *pNotifier)
{
	MsgItem *pNewItem;

	pNewItem = new MsgItem(MTYPE_NOTIFIER);
	if (!pNewItem)
	{
		throw BCException("MsgQueue::Append", "No enough memory.");
	}
	
	*pNewItem->m_pNotifier = *pNotifier;
	m_lstItems.PushBack(pNewItem);
	return BC_R_SUCCESS;
}

MsgItem *MsgQueue::PopFront(SEIDelayAppender* pSeiAppender)
{
	MsgItem *pItem = m_lstItems.PopFront();
	if (pItem->m_pPacket)
	{
		if (pSeiAppender)
		{
			pSeiAppender->ProcessPacket(*pItem->m_pPacket);
		}
		if (pItem->m_pPacketizer) // use custom packetizer
		{
			pItem->m_pPacketizer->Packetize(*pItem->m_pPacket, *pItem->m_pBuffer);
		}
		else // use rtmp packetizer
		{
			PHeader& refHeader = *pItem->m_pPacket->m_pHeader;
			BCBuffer& refBody = pItem->m_pPacket->m_sBody;
			uint32_t nDataLen, nSizeToRead, nStartPos/*, nCurrentTs*/;
			BOOL bFirstChunk, bAllocated = FALSE;
			BCBIStream sReader(&refBody);

			/**
			 * Type 0 chunk headers are 11 bytes long. This type MUST be used at
			 * the start of a chunk stream, and whenever the stream timestamp goes
			 * backward(e.g., because of a backward seek).
			 * For a type-0 chunk, the absolute timestamp of the message is sent here.
			**/
			if (m_mapSendChunkHdr.find(refHeader.m_nChannelId) == m_mapSendChunkHdr.end())
			{
				refHeader.SetAbsTime(true);
			}
			PHeader& refLastHeader = m_mapSendChunkHdr[refHeader.m_nChannelId];

			// Write chunk data and continue chunk headers
			bFirstChunk = TRUE;
			nStartPos = refBody.ConsumedLength();
			nDataLen = refBody.RemainingLength();
			if (nDataLen == 0)
			{
				// Write first chunk header
				_WriteFirstHeader(refLastHeader, refHeader, *pItem->m_pBuffer);
			}
			else
			{
				while (nDataLen > 0)
				{
					nSizeToRead = BCMIN(m_nChunkSize, nDataLen);
					if (bFirstChunk)	// First chunk
					{
						// Write first chunk header
						_WriteFirstHeader(refLastHeader, refHeader,
							*pItem->m_pBuffer);
						bFirstChunk = FALSE;
					}
					else
					{
						_WriteContHeader(refLastHeader, *pItem->m_pBuffer);
					}
					pItem->m_pBuffer->ReadFrom(sReader, nSizeToRead);
					nDataLen -= nSizeToRead;
				}
			}

			// Recovery buffer
			refBody.Rewind();
			refBody.Forward(nStartPos);
		}
	}
	return pItem;
}

uint32_t MsgQueue::RemoveNotifierById(uint32_t nId)
{
	MsgItem *pIter, *pIterEnd, *pIterOld;
	uint32_t nCounter;

	nCounter = 0;
	pIter = m_lstItems.Begin();
	pIterEnd = m_lstItems.End();
	for (;pIter != pIterEnd;)
	{
		if (pIter->m_eType == MTYPE_NOTIFIER &&
			pIter->m_pNotifier->m_sHeader.m_nStreamId == nId)
		{
			pIterOld = pIter;
			pIter = m_lstItems.Next(pIter);
			pIterOld->RemoveFromList();
			pIterOld->m_pNotifier->Destroy();
			delete pIterOld;
			nCounter++;
		}
		else
		{
			pIter = m_lstItems.Next(pIter);
		}
	}

	return nCounter;
}

void MsgQueue::Cleanup()
{
	MsgItem *pItem;
	while((pItem = m_lstItems.PopFront()) != NULL)
	{
		if (pItem->m_eType == MTYPE_NOTIFIER)
		{
			pItem->m_pNotifier->Destroy();
		}
		delete pItem;
	}
}

BOOL MsgQueue::IsEmpty() const
{
	return m_lstItems.IsEmpty();
}

uint32_t MsgQueue::Count() const
{
	return m_lstItems.Count();
}

void MsgQueue::SetChunkSize(uint32_t nSize)
{
	m_nChunkSize = nSize;
}

uint32_t MsgQueue::GetChunkSize() const
{
	return m_nChunkSize;
}

void MsgQueue::_WriteFirstHeader(
	PHeader &refLastHeader,
	PHeader &refHeader,
	BCBuffer &refBuffer)
{
	uint32_t nPacketMask, nChannelId, nTimestamp;
	BCBOStream sWriter(&refBuffer);

	nPacketMask = PHeader::HEADER_NEW;
	nChannelId = refHeader.m_nChannelId;
	// Correct channel id
	if (nChannelId == 0)
	{
		refHeader.m_nChannelId = 2;
		nChannelId = 2;
	}

	nTimestamp = refHeader.m_nTimestamp;

	if (refHeader.IsAbsTime())
	{
		nPacketMask = PHeader::HEADER_NEW;
	} 
	else if(0 != refLastHeader.m_eDataType)
	{
		if (refHeader.m_nStreamId == refLastHeader.m_nStreamId)
		{
			nPacketMask ++;
			if (refHeader.m_eDataType == refLastHeader.m_eDataType
				&& refHeader.m_nDataSize == refLastHeader.m_nDataSize)
			{
				nPacketMask ++;
				if (nTimestamp == refLastHeader.m_nTimestamp)
				{
					nPacketMask ++;
				}
			}
		}
	}

	// Write base chunk header
	if (nChannelId > 319)
	{
		sWriter.WriteUInt8((nPacketMask<<6) + 1);
		sWriter.WriteUInt16LE(nChannelId - 64);
	}
	else if (nChannelId >= 64)
	{
		sWriter.WriteUInt8(nPacketMask << 6);
		sWriter.WriteUInt8(nChannelId - 64);
	}
	else if (nChannelId >= 2)
	{
		sWriter.WriteUInt8((nPacketMask << 6) + nChannelId);
	}
	else
	{
		throw BCException("MsgQueue::_WriteFirstHeader", 
			"Invalid channel id.");
	}

	// Write chunk header
	switch(nPacketMask)
	{
	case PHeader::HEADER_NEW:
		if (nTimestamp >= 0xFFFFFF)
		{
			sWriter.WriteUInt24BE(0xFFFFFF);
			refHeader.SetExtTime(true);
		}
		else
		{
			sWriter.WriteUInt24BE(nTimestamp);
			refHeader.SetExtTime(false);
		}
		sWriter.WriteUInt24BE(refHeader.m_nDataSize);
		sWriter.WriteUInt8(refHeader.m_eDataType);
		// Flash Player specially to read this value little endian
		sWriter.WriteUInt32LE(refHeader.m_nStreamId);
		refLastHeader = refHeader;
		refLastHeader.SetAbsTime(true);
		break;
	case PHeader::HEADER_SAME_SOURCE:
		if (nTimestamp >= 0xFFFFFF)
		{
			sWriter.WriteUInt24BE(0xFFFFFF);
			refHeader.SetExtTime(true);
		}
		else
		{
			sWriter.WriteUInt24BE(nTimestamp);
			refHeader.SetExtTime(false);
		}
		sWriter.WriteUInt24BE(refHeader.m_nDataSize);
		sWriter.WriteUInt8(refHeader.m_eDataType);
		refLastHeader = refHeader;
		refLastHeader.SetAbsTime(false);
		break;
	case PHeader::HEADER_TIMER_CHANGE:
		if (nTimestamp >= 0xFFFFFF)
		{
			sWriter.WriteUInt24BE(0xFFFFFF);
			refHeader.SetExtTime(true);
		}
		else
		{
			sWriter.WriteUInt24BE(nTimestamp);
			refHeader.SetExtTime(false);
		}
		refLastHeader = refHeader;
		refLastHeader.SetAbsTime(false);
		break;
	}

	// Write extend timestamp
	if (refLastHeader.IsExtTime())
	{
		if (refLastHeader.m_nTimestamp <= 0xFFFFFF)
		{
			throw BCException("MsgQueue::_WriteFirstHeader",
				"Invalid packet timestamp");
		}
		sWriter.WriteUInt32BE(nTimestamp);
	}
}

void MsgQueue::_WriteContHeader(
	PHeader &refLastHeader,
	BCBuffer &refBuffer)
{
	uint32_t nChannelId;
	uint8_t sHeader[3];
	uint32_t nHeaderSize = 0;

	// Write base chunk header
	nChannelId = refLastHeader.m_nChannelId;
	if (nChannelId > 319)
	{
		sHeader[0] = (uint8_t)((PHeader::HEADER_CONTINUE<<6) + 1);
		sHeader[1] = (uint8_t)(nChannelId - 64);
		sHeader[2] = (uint8_t)((nChannelId - 64) >> 8);
		nHeaderSize = 3;
	}
	else if (nChannelId >= 64)
	{
		sHeader[0] = (uint8_t)(PHeader::HEADER_CONTINUE << 6);
		sHeader[1] = (uint8_t)(nChannelId - 64);
		nHeaderSize = 2;
	}
	else if (nChannelId >= 2)
	{
		sHeader[0] = (uint8_t)((PHeader::HEADER_CONTINUE << 6) + nChannelId);
		nHeaderSize = 1;
	}
	else
	{
		throw BCException("MsgQueue::_WriteContHeader", 
			"Invalid channel id.");
	}

	refBuffer.Write(sHeader, nHeaderSize);
	if (refLastHeader.m_nTimestamp >= 0xFFFFFF)
	{
		BCBOStream sWriter(&refBuffer);

		sWriter.WriteUInt32BE(refLastHeader.m_nTimestamp);
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : TimstampChecker
///////////////////////////////////////////////////////////////////////////////

TimstampChecker::TimstampChecker()
	: m_bRtmpProtocol(false)
	, m_eClientOS(CLIENT_OS_UNKNOWN)
	, m_nSdkVer(0)
	, m_bAllChannelRebase(false)
	, m_tmBaseOfAVChannels(0)
	, m_bVideoRebase(false)
	, m_tmLastVideoPacket(0)
	, m_bAudioRebase(false)
	, m_tmLastAudioPacket(0)
{
	//
}

TimstampChecker::~TimstampChecker()
{
	//
}

BCRESULT TimstampChecker::Create(
	bool bRtmpProtocol, 
	ClientOS eClientOS, 
	uint64_t nVer)
{
	m_bRtmpProtocol = bRtmpProtocol;
	m_eClientOS = eClientOS;
	m_nSdkVer = nVer;
	return BC_R_SUCCESS;
}

void TimstampChecker::ResetTimestamp(PHeader &refHeader)
{
	if (refHeader.IsVSeqHdr() || refHeader.IsASeqHdr())
	{
		refHeader.m_nTimestamp = 0;
		refHeader.SetAbsTime(false);
	}
	else
	{
		if (m_eClientOS == CLIENT_OS_FMS_WIN ||
			m_eClientOS == CLIENT_OS_FMS_LINUX ||
			m_eClientOS == CLIENT_OS_UNKNOWN)
		{
			refHeader.m_nTimestamp = refHeader.m_nTotalTime;
			refHeader.SetAbsTime(true);
		} 
		else
		{
			if (!m_bAllChannelRebase)
			{
				m_tmBaseOfAVChannels = refHeader.m_nTotalTime;
				m_bAllChannelRebase = true;
			}
			if (refHeader.m_nTotalTime >= m_tmBaseOfAVChannels)
			{
				refHeader.m_nTotalTime -= m_tmBaseOfAVChannels;
			}
			else
			{
				refHeader.m_nTotalTime = 0;
			}
			switch (refHeader.m_eDataType)
			{
			case MTYPE_AUDIODATA:
				//LogDebug(_LOCAL_, "Audio timestamp : %" _U32BITARG_ "; totalTime : %" _U64BITARG_,
				//	refHeader.m_nTimestamp, refHeader.m_nTotalTime);
				if (!m_bAudioRebase)
				{
					m_tmLastAudioPacket = refHeader.m_nTotalTime;
					m_bAudioRebase = true;
				}
				if (refHeader.m_nTotalTime >= m_tmLastAudioPacket)
				{
					refHeader.m_nTimestamp = refHeader.m_nTotalTime - m_tmLastAudioPacket;
				}
				else
				{
					refHeader.m_nTimestamp = 0;
				}
				refHeader.SetAbsTime(false);
				m_tmLastAudioPacket = refHeader.m_nTotalTime;
				//LogDebug(_LOCAL_, "Audio timestamp : %" _U32BITARG_ "; totalTime : %" _U64BITARG_,
				//	refHeader.m_nTimestamp, refHeader.m_nTotalTime);
				break;
			case MTYPE_VIDEODATA:
				//LogDebug(_LOCAL_, "Video timestamp : %" _U32BITARG_ "; totalTime : %" _U64BITARG_,
				//	refHeader.m_nTimestamp, refHeader.m_nTotalTime);
				if (!m_bVideoRebase)
				{
					m_tmLastVideoPacket = refHeader.m_nTotalTime;
					m_bVideoRebase = true;
				}
				if (refHeader.m_nTotalTime >= m_tmLastVideoPacket)
				{
					refHeader.m_nTimestamp = refHeader.m_nTotalTime - m_tmLastVideoPacket;
				}
				else
				{
					refHeader.m_nTimestamp = 0;
				}
				refHeader.SetAbsTime(false);
				m_tmLastVideoPacket = refHeader.m_nTotalTime;
				//LogDebug(_LOCAL_, "Video timestamp : %" _U32BITARG_ "; totalTime : %" _U64BITARG_,
				//	refHeader.m_nTimestamp, refHeader.m_nTotalTime);
				break;
			default:
				break;
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : SEIDelayAppender
///////////////////////////////////////////////////////////////////////////////

SEIDelayAppender::SEIDelayAppender()
	: m_eType(0)
	, m_ip(0)
{
}

SEIDelayAppender::~SEIDelayAppender()
{
}

BCRESULT SEIDelayAppender::Create(uint8_t type, LPCSTR ip)
{
	BCRESULT result = BC_R_FAILURE;

	struct in_addr ina;
	if (bc_net_pton(PF_INET, ip, &ina) <= 0)
	{
		BCNetAddressList addresses(ip);
		if (addresses.numAddresses() == 0) {
			LogError(_LOCAL_, "Failed to find network address for \"%s\"", ip);
			result = BC_R_INVALIDARG;
			goto return_error;
		}
		else
		{
			BCNetAddress address = *(addresses.firstAddress());
			ina.s_addr = *(netAddressBits*)(address.data());
		}
	}
	m_ip = ina.s_addr;
	m_eType = type;
	return BC_R_SUCCESS;

return_error:
	return result;
}

BCRESULT SEIDelayAppender::ProcessPacket(PPacket& refPacket)
{
	PHeader& refHeader = *refPacket.m_pHeader;
	BCBuffer& refBody = refPacket.m_sBody;
	switch (refPacket.m_pHeader->m_eDataType)
	{
	case MTYPE_VIDEODATA:
		if (refHeader.m_nDataSize >= 12)
		{
			uint8_t* pData;
			FLVVideoInfoS videoInfo;

			pData = (uint8_t*)refBody.Current();
			videoInfo = FLVInfo::AnalyseVideo(*pData);
			switch (videoInfo.eCodecId)
			{
			case FLV_H264VIDEOPACKET:
				if (pData[1] == AVCPTYPE_NALU && pData[9] == NAL_SEI)
				{
					_ProcessSEI(refHeader, refBody);
				}
				break;
			default:
				break;
			}
		}
		break;
	default:
		break;
	}
	return BC_R_SUCCESS;
}

BCRESULT SEIDelayAppender::_ProcessSEI(
	PHeader& refHeader, 
	BCBuffer& refBody)
{
	uint8_t* pData;
	int payload_type, payload_size, data_size;

	data_size = refBody.RemainingLength();
	if (data_size < 2)
	{
		return BC_R_NOMORE;
	}
	pData = (uint8_t*)refBody.Base();
	payload_type = pData[10];
	payload_size = pData[11];
	if (payload_size > data_size - 8)
	{
		return BC_R_NOMORE;
	}
	if (payload_type == 100)
	{
		int ret;
		size_t out_size = 3 * (payload_size / 4) + 1 + 15;
		BCFixedBuffer statsBuffer(out_size);
		if ((ret = base64_decode((uint8_t *)statsBuffer.Base(), 
			(char*)(pData + 12), payload_size)) < 0) {
			return BC_R_UNEXPECTED;
		}
		BCFBOStream statsWriter(statsBuffer.Base(), statsBuffer.Size());
		_ParseAndEncodeSEI(statsWriter.Base(), ret, statsWriter);
		BCBuffer outBody;
		BCBOStream sWriter(&outBody);
		sWriter.Write(pData, 9);
		sWriter.WriteUInt8(NAL_SEI);
		sWriter.WriteUInt8(0x64); // SEI delay info
		sWriter.WriteUInt8(0); // size reserved
		if ((ret = base64_encode((char *)outBody.Used(), 
			(uint8_t *)statsWriter.Base(), statsWriter.UsedLength())) < 0) {
			return BC_R_UNEXPECTED;
		}
		outBody.Add(ret);
		sWriter.WriteUInt8(0x80); // tailing bits
		// Adjust packet size
		uint8_t* out_data = (uint8_t*)outBody.Base();
		// sei type + payload type + payload size + tailing byte
		BC_UI32BEO((out_data + 5), (ret + 1 + 1 + 1 + 1));
		out_data[11] = ret;
		// Copy remaining data
		uint32_t nSkipSize;
		BCBIStream reader(&refBody);
		reader.Rewind(5);
		reader.ReadUInt32BE(&nSkipSize);
		reader.Skip(nSkipSize);
		sWriter.WriteFrom(reader, reader.RemainingLength());
		outBody.RefClone(&refBody);
		refHeader.m_nDataSize = outBody.RemainingLength();
	}
	return BC_R_SUCCESS;
}

uint32_t SEIDelayAppender::_ParseAndEncodeSEI(
	const void* data, 
	size_t data_size,
	BCOStream& refWriter)
{
	uint8_t ver;
	uint16_t ver_tag_size, version, ntp_tag_len;
	uint32_t result = 0;
	NTPItem item;
	NTPItemList ntpList;
	BCFBIStream sReader(data, data_size);

	sReader.ReadUInt8(&ver);
	sReader.ReadUInt16BE(&ver_tag_size);
	sReader.ReadUInt16BE(&version);
	for (uint8_t i = 0; i < ver_tag_size; i++)
	{
		sReader.ReadUInt8(&item.type);
		sReader.ReadUInt16BE(&ntp_tag_len);
		sReader.ReadUInt32BE(&item.ip);
		sReader.ReadUInt64BE(&item.timestamp);
		ntpList.push_back(item);
	}
	// add current npt stats
	item.type = m_eType;
	item.ip = m_ip;
	item.timestamp = bc_time_now()/1000;
	ntpList.push_back(item);
	// encode ntp header
	result += refWriter.WriteUInt8(ver);
	result += refWriter.WriteUInt16BE(ntpList.size());
	result += refWriter.WriteUInt16BE(version);
	// encode ntp items
	for (NTPItem& item : ntpList)
	{
		result += refWriter.WriteUInt8(item.type);
		result += refWriter.WriteUInt16BE(12);
		result += refWriter.WriteUInt32BE(item.ip);
		result += refWriter.WriteUInt64BE(item.timestamp);

		BCSockAddrS addr;
		bc_sockaddr_any(&addr);
		memcpy((void *)bc_sockaddr_getaddr(&addr), &item.ip, sizeof(item.ip));
		addr.length = sizeof(item.ip);
		char peerIp[32];
		bc_sockaddr_format(&addr, peerIp, sizeof(peerIp));
		LogInfo(_LOCAL_, "SEI stats info[type : %2d, ip : %s, timestamp : %"
			_U64BITARG_, (int)item.type, peerIp, item.timestamp);
	}

	return result;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file : PacketParser.cpp
///////////////////////////////////////////////////////////////////////////////
