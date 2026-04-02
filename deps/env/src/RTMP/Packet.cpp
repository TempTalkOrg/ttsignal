///////////////////////////////////////////////////////////////////////////////
// file : Packet.cpp
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////

#include <BC/BCLog.h>
#include <RTMP/FLVUtils.h>
#include <RTMP/Packet.h>
#include <RTMP/IHandler.h>

#ifdef _MSC_VER
#pragma warning(disable:4172)
#endif // _MSC_VER

using namespace FLVUtils;



///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

//////////////////////////////////////////////////////////////////////////
/// class : RTMPPacket
//////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////
///// class : RAWVideo
////////////////////////////////////////////////////////////////////////////
//
//IMPLEMENT_FIXED_ALLOC(RAWVideo, 8);
//
////////////////////////////////////////////////////////////////////////////
///// class : RAWAudio
////////////////////////////////////////////////////////////////////////////
//
//IMPLEMENT_FIXED_ALLOC(RAWAudio, 8);

//////////////////////////////////////////////////////////////////////////
/// class PHeader
//////////////////////////////////////////////////////////////////////////

PHeader::PHeader()
	: m_nChannelId(0)
	, m_nTimestamp(0)
	, m_nDataSize(0)
	, m_eDataType(0)
	, m_nStreamId(0)
	, m_eChunkDataType(CHUNK_NONE)
	, m_nFinishedSize(0)
	, m_nChunkSize(0)
	, m_nFlags(0)
	, m_nAbsTime(0)
	, m_nTotalTime(0)
	, m_eLastChunkType(-1)
{
	//
}

PHeader::PHeader(
	int32_t nChannelId,
	int32_t nTimestamp,
	int32_t nDataSize,
	int32_t eDataType,
	int32_t nStreamId)
		: m_nChannelId(nChannelId)
		, m_nTimestamp(nTimestamp)
		, m_nDataSize(nDataSize)
		, m_eDataType(eDataType)
		, m_nStreamId(nStreamId)
		, m_eChunkDataType(CHUNK_NONE)
		, m_nFinishedSize(0)
		, m_nChunkSize(0)
		, m_nFlags(0)
		, m_nAbsTime(0)
		, m_nTotalTime(0)
		, m_eLastChunkType(-1)
{
	//
}

PHeader::PHeader(const PHeader &other)
{
	m_nChannelId = other.m_nChannelId;
	m_nTimestamp = other.m_nTimestamp;
	m_nDataSize = other.m_nDataSize;
	m_eDataType = other.m_eDataType;
	m_nStreamId = other.m_nStreamId;
	m_eChunkDataType = other.m_eChunkDataType;
	m_nFinishedSize = other.m_nFinishedSize;
	m_nChunkSize = other.m_nChunkSize;
	m_nFlags = other.m_nFlags;
	m_nAbsTime = other.m_nAbsTime;
	m_nTotalTime = other.m_nTotalTime;
	m_eLastChunkType = other.m_eLastChunkType;
}

PHeader::~PHeader()
{
	//
}

PHeader &PHeader::operator = (const PHeader &other)
{
	m_nChannelId = other.m_nChannelId;
	m_nTimestamp = other.m_nTimestamp;
	m_nDataSize = other.m_nDataSize;
	m_eDataType = other.m_eDataType;
	m_nStreamId = other.m_nStreamId;
	m_eChunkDataType = other.m_eChunkDataType;
	m_nFinishedSize = other.m_nFinishedSize;
	m_nChunkSize = other.m_nChunkSize;
	m_nFlags = other.m_nFlags;
	m_nAbsTime = other.m_nAbsTime;
	m_nTotalTime = other.m_nTotalTime;
	m_eLastChunkType = other.m_eLastChunkType;
	return *this;
}

void PHeader::Reset()
{
	this->m_nChannelId = 0;
	this->m_nDataSize = 0;
	this->m_eDataType = 0;
	this->m_nStreamId = 0;
	this->m_nTimestamp = 0;
	this->m_eChunkDataType = CHUNK_NONE;
	this->m_nFinishedSize = 0;
	this->m_nChunkSize = 0;
	this->m_nFlags = 0;
	this->m_nAbsTime = 0;
	this->m_nTotalTime = 0;
	this->m_eLastChunkType = -1;
}

void PHeader::Dump() const
{
	LogDebug(_LOCAL_,
		"Channel id  [%d]\n"
		"Data size   [%d]\n"
		"Data type   [%d]\n"
		"Stream id   [%d]\n"
		"Timestamp   [%d]\n"
		"Chunk Type  [%d]\n"
		"Finish size [%d]\n"
		"Chunk Size  [%d]\n",
		m_nChannelId,
		m_nDataSize,
		m_eDataType,
		m_nStreamId,
		m_nTimestamp,
		m_eChunkDataType,
		m_nFinishedSize,
		m_nChunkSize);
}

///////////////////////////////////////////////////////////////////////////////
// PPacket
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(PPacket, 32);

PPacket::PPacket()
	: m_pHeader(new PHeader())
	, m_pAction(NULL)
	, m_pArgs(NULL)
	, m_nRef(0)
	, m_pUserData(NULL)
{
	//
}

PPacket::PPacket(const PPacket &sPacket, bool bRef)
	: BCNodeList::Node()
	, m_pAction(sPacket.m_pAction)
	, m_pArgs(sPacket.m_pArgs)
	, m_nRef(0)
	, m_pUserData(sPacket.m_pUserData)
{
	if (bRef)
	{
		m_pHeader = sPacket.m_pHeader;
		sPacket.m_sBody.RefClone(&m_sBody);
	} 
	else
	{
		m_pHeader.reset(new PHeader(*sPacket.m_pHeader));
		sPacket.m_sBody.Clone(m_sBody);
	}
}

PPacket::~PPacket()
{
	//
}

PPacket &PPacket::operator = (const PPacket &sPacket)
{
	*m_pHeader = *sPacket.m_pHeader;
	sPacket.m_sBody.Clone(m_sBody);
	m_pAction = sPacket.m_pAction;
	m_pArgs = sPacket.m_pArgs;
	m_pUserData = sPacket.m_pUserData;

	return *this;
}

void PPacket::SplitFrom(
	PHeader &refHeader,
	BCBuffer &refBody,
	uint32_t nChunkSize)
{
	uint32_t nReadLen, nDataLen;
	bool bFirstChunk = true;
	bool bWriteContHeader = false;
	uint32_t nSizeToRead;
	uint8_t *pData;

	*m_pHeader = refHeader;
	nDataLen = refBody.UsedLength();
	ASSERT(nDataLen == m_pHeader->m_nDataSize);
	// Reset buffer
	refBody.Rewind();
	m_sBody.Reset(1);

	while(nDataLen > 0)
	{
		nSizeToRead = nChunkSize > nDataLen?nDataLen:nChunkSize;
		pData = (uint8_t *)refBody.ReadBlock(nSizeToRead, nReadLen);
		if (pData != NULL && nReadLen > 0)
		{
			if (bFirstChunk)	// First chunk
			{
				m_sBody.Add(MAX_CHUNK_HEADER_SIZE);
				bFirstChunk = false;
			}
			else if (bWriteContHeader) // Continue chunk
			{
				WriteContinueHeader();
			}
			nSizeToRead -= nReadLen;
			if (nSizeToRead == 0)
			{
				nSizeToRead = nChunkSize;
				bWriteContHeader = true;
			}
			else
			{
				bWriteContHeader = false;
			}
			m_sBody.Write(pData, nReadLen);
			nDataLen -= nReadLen;
		}
	}
}

uint32_t PPacket::ChunkMessage(PHeader &refLastHeader)
{
	BCBOStream sWriter(&m_sBody);
	uint32_t nPacketMask = 0;
	uint32_t nChannelId = m_pHeader->m_nChannelId;
	uint32_t nTimestamp = m_pHeader->m_nTimestamp;
	uint32_t nStartPos = 0;
	uint32_t nUsedLen = m_sBody.UsedLength();

	if(0 != refLastHeader.m_eDataType)
	{
		if (m_pHeader->m_nStreamId == refLastHeader.m_nStreamId)
		{
			nPacketMask ++;
		}
		if (m_pHeader->m_eDataType == refLastHeader.m_eDataType
			&& m_pHeader->m_nDataSize == refLastHeader.m_nDataSize)
		{
			nPacketMask ++;
			if (nTimestamp == refLastHeader.m_nTimestamp)
			{
				nPacketMask ++;
			}
		}
	}

	if (m_pHeader->IsAbsTime() || nTimestamp < refLastHeader.m_nTimestamp)
	{
		nPacketMask = PHeader::HEADER_NEW;
	}

	// Reset write position
	m_sBody.Reset();
	m_sBody.Add(MAX_CHUNK_HEADER_SIZE);

	// Write extend timestamp
	if (nTimestamp > 0xFFFFFF)
	{
		m_sBody.Subtract(4);
		sWriter.WriteUInt32BE(nTimestamp);
		m_sBody.Subtract(4);
		nTimestamp = 0xFFFFFF;
	}

	// Write chunk header
	switch(nPacketMask)
	{
	case PHeader::HEADER_NEW:
		m_sBody.Subtract(11);
		sWriter.WriteUInt24BE(nTimestamp);
		sWriter.WriteUInt24BE(m_pHeader->m_nDataSize);
		sWriter.WriteUInt8(m_pHeader->m_eDataType);
		// Flash Player specially to read this value little endian
		sWriter.WriteUInt32LE(m_pHeader->m_nStreamId);
		m_sBody.Subtract(11);
		refLastHeader = *m_pHeader;
		break;
	case PHeader::HEADER_SAME_SOURCE:
		ASSERT(nTimestamp >= refLastHeader.m_nTimestamp);
		nTimestamp -= refLastHeader.m_nTimestamp;
		m_sBody.Subtract(7);
		sWriter.WriteUInt24BE(nTimestamp);
		sWriter.WriteUInt24BE(m_pHeader->m_nDataSize);
		sWriter.WriteUInt8(m_pHeader->m_eDataType);
		m_sBody.Subtract(7);
		refLastHeader = *m_pHeader;
		break;
	case PHeader::HEADER_TIMER_CHANGE:
		ASSERT(nTimestamp >= refLastHeader.m_nTimestamp);
		nTimestamp -= refLastHeader.m_nTimestamp;
		m_sBody.Subtract(3);
		sWriter.WriteUInt24BE(nTimestamp);
		m_sBody.Subtract(3);
		refLastHeader.m_nTimestamp = m_pHeader->m_nTimestamp;
		break;
	}

	// Write base chunk header
	if (nChannelId > 319)
	{
		m_sBody.Subtract(3);
		sWriter.WriteUInt8((nPacketMask<<6) + 1);
		sWriter.WriteUInt16LE(nChannelId - 64);
		m_sBody.Subtract(3);
	}
	else if (nChannelId >= 64)
	{
		m_sBody.Subtract(2);
		sWriter.WriteUInt8(nPacketMask << 6);
		sWriter.WriteUInt8(nChannelId - 64);
		m_sBody.Subtract(2);
	}
	else if (nChannelId >= 2)
	{
		m_sBody.Subtract(1);
		sWriter.WriteUInt8((nPacketMask << 6) + nChannelId);
		m_sBody.Subtract(1);
	}
	else
	{
		ASSERT(false);
	}
	nStartPos = m_sBody.UsedLength();
	m_sBody.Reset();
	m_sBody.Add(nUsedLen);
	m_sBody.Forward(nStartPos);
	return m_sBody.RemainingLength();
}

uint32_t PPacket::WriteContinueHeader()
{
	uint32_t nChannelId = m_pHeader->m_nChannelId;
	uint8_t sHeader[3];
	uint32_t nHeaderSize = 0;

	// Write base chunk header
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
		ASSERT(false);
	}

	return m_sBody.Write(sHeader, nHeaderSize);
}

void PPacket::Reset(int32_t nReserveBufData /*= -1*/)
{
	m_pHeader.reset(new PHeader);
	m_sBody.Reset(nReserveBufData);
	m_pArgs = NULL;
	m_nRef = 0;
}

void PPacket::Destroy()
{
}

void PPacket::Dump()
{
	//LogBin(_LOCAL_, m_sPBody.GetBuffer(), m_sPBody.GetDataLen());
}

void PPacket::ParseHeader()
{
	switch (m_pHeader->m_eDataType)
	{
	case MTYPE_AUDIODATA:
		if (m_pHeader->m_nDataSize >= 2)
		{
			uint8_t *pData;
			FLVAudioInfoS sAInfo;

			pData = (uint8_t *)m_sBody.Current();
			sAInfo = FLVInfo::AnalyseAudio(pData[0]);
			switch (sAInfo.eCodecId)
			{
			case FLV_AAC_AUDIO:
				if (pData[1] == AACPTYPE_SEQHEADER)
				{
					m_pHeader->SetASeqHdr(true);
				}
				break;
			default:
				break;
			}
			m_pHeader->SetAKFrame(true);
		}
		break;
	case MTYPE_VIDEODATA:
		if (m_pHeader->m_nDataSize >= 2)
		{
			uint8_t *pData;
			FLVVideoInfoS videoInfo;

			pData = (uint8_t *)m_sBody.Current();
			videoInfo = FLVInfo::AnalyseVideo(*pData);
			switch (videoInfo.eCodecId)
			{
			case FLV_RTP_RTCP_PACKET:
				break;
			case FLV_H264VIDEOPACKET:
				if (pData[1] == AVCPTYPE_SEQHEADER)
				{
					m_pHeader->SetVSeqHdr(true);
				}
				break;
			default:
				break;
			}
			m_pHeader->SetVKFrame(videoInfo.eFrameType == FLV_KEYFRAME);
		}
		break;
	default:
		break;
	}
}

PPacket* PPacket::RefClone() const
{
	return new PPacket(*this, true);
}

PPacket& PPacket::Clone(PPacket &destPacket) const
{
	*destPacket.m_pHeader = *m_pHeader;
	m_sBody.Clone(destPacket.m_sBody);
	destPacket.m_pAction = m_pAction;
	destPacket.m_pArgs = m_pArgs;
	return destPacket;
}

//////////////////////////////////////////////////////////////////////////
/// class MBase
//////////////////////////////////////////////////////////////////////////

MBase::MBase(
	uint32_t nChannelId,
	uint32_t nTimestamp,
	uint32_t eMsgType,
	uint32_t nStreamId)
		: m_nChannelId(nChannelId)
		, m_nTimestamp(nTimestamp)
		, m_nDataSize(0)
		, m_eDataType(eMsgType)
		, m_nStreamId(nStreamId)
		, m_bAbsoluteTime(false)
{
	//
}

MBase::~MBase()
{
	//
}

//////////////////////////////////////////////////////////////////////////
/// class MCtrlChunkSize
//////////////////////////////////////////////////////////////////////////

MCtrlChunkSize::MCtrlChunkSize(uint32_t nChunkSize)
	: MBase(2, 0, MTYPE_SETCHUNKSIZE, 0)
	, m_nChunkSize(nChunkSize)
{
	//
}

MCtrlChunkSize::~MCtrlChunkSize()
{
	//
}

bool MCtrlChunkSize::Decode(PPacket &refInPacket)
{
	BCBIStream sReader(&refInPacket.m_sBody);
	if (!sReader.Eof())
	{
		sReader.ReadUInt32BE(&m_nChunkSize);
		return true;
	}
	return false;
}

bool MCtrlChunkSize::Encode(PPacket &refOutPacket)
{
	BCBOStream sWriter(&refOutPacket.m_sBody, false);
	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = 4;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	sWriter.WriteUInt32BE(m_nChunkSize);
	return true;
}

void MCtrlChunkSize::Dump()
{
	LogDebug(_LOCAL_,
		"\nProtocol control message : set chunk size ( m_nChunkSize = %d ).\n",
		m_nChunkSize);
}

//////////////////////////////////////////////////////////////////////////
/// class MCtrlChunkAbort
//////////////////////////////////////////////////////////////////////////

MCtrlChunkAbort::MCtrlChunkAbort()
	: MBase(2, 0, MTYPE_CHUNKABORT, 0)
	, m_nChunkStreamId(0)
{
	//
}

MCtrlChunkAbort::~MCtrlChunkAbort()
{
	//
}

bool MCtrlChunkAbort::Decode(PPacket &refInPacket)
{
	BCBIStream sReader(&refInPacket.m_sBody);
	if (!sReader.Eof())
	{
		sReader.ReadUInt32BE(&m_nChunkStreamId);
		return true;
	}
	return false;
}

bool MCtrlChunkAbort::Encode(PPacket &refOutPacket)
{
	BCBOStream sWriter(&refOutPacket.m_sBody, false);
	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = 4;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	sWriter.WriteUInt32BE(m_nChunkStreamId);
	return true;
}

void MCtrlChunkAbort::Dump()
{
	LogDebug(_LOCAL_,
		"\nProtocol control message : chunk abort ( m_nChunkStreamId = %d ).\n",
		m_nChunkStreamId);
}

//////////////////////////////////////////////////////////////////////////
/// class MCtrlAckWndSize
//////////////////////////////////////////////////////////////////////////

MCtrlAckWndSize::MCtrlAckWndSize(uint32_t nSegNumber /*= 0*/)
	: MBase(2, 0, MTYPE_ACKWNDSIZE, 0)
	, m_nSequenceNumber(nSegNumber)
{
	//
}

MCtrlAckWndSize::~MCtrlAckWndSize()
{
	//
}

bool MCtrlAckWndSize::Decode(PPacket &refInPacket)
{
	BCBIStream sReader(&refInPacket.m_sBody);
	if (!sReader.Eof())
	{
		sReader.ReadUInt32BE(&m_nSequenceNumber);
		return true;
	}
	return false;
}

bool MCtrlAckWndSize::Encode(PPacket &refOutPacket)
{
	BCBOStream sWriter(&refOutPacket.m_sBody, false);

	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = 4;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	sWriter.WriteUInt32BE(m_nSequenceNumber);
	return true;
}

void MCtrlAckWndSize::Dump()
{
	LogDebug(_LOCAL_,
		"\nProtocol control message : acknowledge window size"
		" ( m_nSequenceNumber = %d ).\n", m_nSequenceNumber);
}

//////////////////////////////////////////////////////////////////////////
/// class MCtrlUserCtrl
//////////////////////////////////////////////////////////////////////////

MCtrlUserCtrl::MCtrlUserCtrl()
	: MBase(2, 0, MTYPE_USERCTRLEVENT, 0)
	, m_eEventType(0)
	, m_dwEventData(0)
	, m_dwEventDataExt(0)
{
	//
}

MCtrlUserCtrl::MCtrlUserCtrl(
	uint32_t eEventType,
	uint32_t dwEventData,
	uint32_t dwEventDataEx /*= 0*/)
		: MBase(2, 0, MTYPE_USERCTRLEVENT, 0)
		, m_eEventType(eEventType)
		, m_dwEventData(dwEventData)
		, m_dwEventDataExt(dwEventDataEx)
{
	//
}

MCtrlUserCtrl::~MCtrlUserCtrl()
{
	//
}

bool MCtrlUserCtrl::Decode(PPacket &refInPacket)
{
	BCBIStream sReader(&refInPacket.m_sBody);
	if (!sReader.Eof())
	{
		sReader.ReadUInt16BE(&m_eEventType);
		if (!sReader.Eof())
		{
			sReader.ReadUInt32BE(&m_dwEventData);
		}
		if(!sReader.Eof() && MUCTRL_SET_BUFFER_LENGTH == m_eEventType)
		{
			sReader.ReadUInt32BE(&m_dwEventDataExt);
		}
		return true;
	}
	return false;
}

bool MCtrlUserCtrl::Encode(PPacket &refOutPacket)
{
	BCBOStream sWriter(&refOutPacket.m_sBody, false);

	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	sWriter.WriteUInt16BE(m_eEventType);
	sWriter.WriteUInt32BE(m_dwEventData);
	if (MUCTRL_SET_BUFFER_LENGTH == m_eEventType)
	{
		sWriter.WriteUInt32BE(m_dwEventDataExt);
	}
	refOutPacket.m_pHeader->m_nDataSize = refOutPacket.m_sBody.UsedLength();
	return true;
}

void MCtrlUserCtrl::Dump()
{
	BCPString strOutbuf;
	strOutbuf << "\nUser control message : \n\t";
	switch (m_eEventType)
	{
	case MUCTRL_STREAM_BEGIN:
		strOutbuf << "CTRL_STREAM_BEGIN\n\t";
		break;
	case MUCTRL_STREAM_EOF:
		strOutbuf << "CTRL_STREAM_EOF\n\t";
		break;
	case MUCTRL_STREAM_DRY:
		strOutbuf << "CTRL_STREAM_DRY\n\t";
		break;
	case MUCTRL_SET_BUFFER_LENGTH:
		strOutbuf << "CTRL_SET_BUFFER_LENGTH\n\t";
		break;
	case MUCTRL_STREAM_IS_RECORDED:
		strOutbuf << "CTRL_STREAM_IS_RECORDED\n\t";
		break;
	case MUCTRL_PING_REQUEST:
		strOutbuf << "CTRL_PING_REQUEST\n\t";
		break;
	case MUCTRL_PING_RESPONSE:
		strOutbuf << "CTRL_PING_RESPONSE\n\t";
		break;
	}
	strOutbuf << "Event data : " << (int32_t)m_dwEventData << "\n\t";
	if (MUCTRL_SET_BUFFER_LENGTH == m_eEventType)
	{
		strOutbuf << "buffer length : " << m_dwEventDataExt << "\n\t";
	}
	LogDebug(_LOCAL_, strOutbuf.c_str());
}

//////////////////////////////////////////////////////////////////////////
/// class MCtrlSetWndSize
//////////////////////////////////////////////////////////////////////////

MCtrlSetWndSize::MCtrlSetWndSize(uint32_t nWindowSize /*= 1024*1024*/)
	: MBase(2, 0, MTYPE_SETWNDSIZE, 0)
	, m_nWindowSize(nWindowSize)
{
	//
}

MCtrlSetWndSize::~MCtrlSetWndSize()
{
	//
}

bool MCtrlSetWndSize::Decode(PPacket &refInPacket)
{
	BCBIStream sReader(&refInPacket.m_sBody);
	if (!sReader.Eof())
	{
		sReader.ReadUInt32BE(&m_nWindowSize);
		return true;
	}
	return false;
}

bool MCtrlSetWndSize::Encode(PPacket &refOutPacket)
{
	BCBOStream sWriter(&refOutPacket.m_sBody, false);

	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = 4;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	sWriter.WriteUInt32BE(m_nWindowSize);
	return true;
}

void MCtrlSetWndSize::Dump()
{
	LogDebug(_LOCAL_,
		"\nProtocol control message : set window size"
		" ( m_nWindowSize = %d ).\n", m_nWindowSize);
}

//////////////////////////////////////////////////////////////////////////
/// class MCtrlSetBandWidth
//////////////////////////////////////////////////////////////////////////

MCtrlSetBandWidth::MCtrlSetBandWidth(
	uint32_t nBW /*= 512*1024*/,
	uint32_t eBWType/* = BW_DYNAMIC*/)
	: MBase(2, 0, MTYPE_SETBW, 0)
	, m_nWindowSize(nBW)
	, m_eLimitType(eBWType)
{
	//
}

MCtrlSetBandWidth::~MCtrlSetBandWidth()
{
	//
}

bool MCtrlSetBandWidth::Decode(PPacket &refInPacket)
{
	BCBIStream sReader(&refInPacket.m_sBody);
	if (!sReader.Eof())
	{
		sReader.ReadUInt32BE(&m_nWindowSize);
		sReader.ReadUInt8(&m_eLimitType);
		return true;
	}
	return false;
}

bool MCtrlSetBandWidth::Encode(PPacket &refOutPacket)
{
	BCBOStream sWriter(&refOutPacket.m_sBody, false);

	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = 5;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	sWriter.WriteUInt32BE(m_nWindowSize);
	sWriter.WriteUInt8(m_eLimitType);
	return true;
}

void MCtrlSetBandWidth::Dump()
{
	BCPString strOutbuf;
	strOutbuf << "\nProtocol control message : set bandwidth"
		" ( m_nWindowSize = " << m_nWindowSize << "\n\t";
	switch(m_eLimitType)
	{
	case MCTRL_BW_HARD:
		strOutbuf << "Band width type : hard.\n\t";
		break;
	case MCTRL_BW_SOFT:
		strOutbuf << "Band width type : soft.\n\t";
		break;
	case MCTRL_BW_DYNAMIC:
		strOutbuf << "Band width type : dynamic.\n\t";
		break;
	}
	LogDebug(_LOCAL_, strOutbuf.c_str());
}

//////////////////////////////////////////////////////////////////////////
/// class MCtrlInterServer
//////////////////////////////////////////////////////////////////////////

MCtrlInterServer::MCtrlInterServer()
	: MBase(2, 0, MTYPE_INTERSERVER, 0)
{
	//
}

MCtrlInterServer::~MCtrlInterServer()
{
	//
}

bool MCtrlInterServer::Decode(PPacket &refInPacket)
{
	UNUSED(refInPacket);
	return false;
}

bool MCtrlInterServer::Encode(PPacket &refOutPacket)
{
	BCBOStream sWriter(&refOutPacket.m_sBody, false);

	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	refOutPacket.m_pHeader->m_nDataSize = refOutPacket.m_sBody.UsedLength();
	return false;
}

void MCtrlInterServer::Dump()
{
	LogDebug(_LOCAL_, "\nProtocol control message : inter server.\n");
}

//////////////////////////////////////////////////////////////////////////
/// class MAudioData
//////////////////////////////////////////////////////////////////////////

MAudioData::MAudioData()
	: MBase(0, 0, MTYPE_AUDIODATA, 0)
{
	//
}

MAudioData::~MAudioData()
{
	//
}

bool MAudioData::Decode(PPacket &refInPacket)
{
	m_nChannelId = refInPacket.m_pHeader->m_nChannelId;
	m_nDataSize = refInPacket.m_pHeader->m_nDataSize;
	m_nTimestamp = refInPacket.m_pHeader->m_nTimestamp;
	m_nStreamId = refInPacket.m_pHeader->m_nStreamId;
	return true;
}

bool MAudioData::Encode(PPacket &refOutPacket)
{
	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = m_nDataSize;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	return true;
}

void MAudioData::Dump()
{
	LogDebug(_LOCAL_, "\nAudio data message.\n");
}

//////////////////////////////////////////////////////////////////////////
/// class MVideoData
//////////////////////////////////////////////////////////////////////////

MVideoData::MVideoData()
	: MBase(0, 0, MTYPE_VIDEODATA, 0)
{
	//
}

MVideoData::~MVideoData()
{
	//
}

bool MVideoData::Decode(PPacket &refInPacket)
{
	m_nChannelId = refInPacket.m_pHeader->m_nChannelId;
	m_nDataSize = refInPacket.m_pHeader->m_nDataSize;
	m_nTimestamp = refInPacket.m_pHeader->m_nTimestamp;
	m_nStreamId = refInPacket.m_pHeader->m_nStreamId;
	return true;
}

bool MVideoData::Encode(PPacket &refOutPacket)
{
	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = m_nDataSize;
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	return true;
}

void MVideoData::Dump()
{
	LogDebug(_LOCAL_, "\nVideo data message.\n");
}

//////////////////////////////////////////////////////////////////////////
/// class MMetaData
//////////////////////////////////////////////////////////////////////////

MMetaData::MMetaData()
	: MBase(0, 0, MTYPE_METADATA, 0)
{
	//
}

MMetaData::~MMetaData()
{
	for(int s = 0; s < m_vecMetaData.size(); s++)
	{
		BC_SAFE_DELETE_PTR(m_vecMetaData[s]);
	}
	m_vecMetaData.clear();
}

bool MMetaData::Decode(PPacket &refInPacket)
{
	m_nChannelId = refInPacket.m_pHeader->m_nChannelId;
	m_nDataSize = refInPacket.m_pHeader->m_nDataSize;
	m_nTimestamp = refInPacket.m_pHeader->m_nTimestamp;
	m_nStreamId = refInPacket.m_pHeader->m_nStreamId;

	BCBIStream sReader(&refInPacket.m_sBody);
	m_sCodecCtx.Clear();
	sReader.SetUserData(&m_sCodecCtx);
	m_sCodecCtx.SetEncoding(ObjectEncoding::AMF0);
	while(!sReader.Eof())
	{
		AMFVarPtr tmp = AMFCodec::Decode(&sReader);
		m_vecMetaData.push_back(new AMFVarWrapper(tmp));
	}
	return true;
}

bool MMetaData::Encode(PPacket &refOutPacket)
{
	BCBOStream sWriter(&refOutPacket.m_sBody, false);
	m_sCodecCtx.Clear();
	sWriter.SetUserData(&m_sCodecCtx);
	uint32_t nSize = m_vecMetaData.size();
	for (uint32_t i = 0;i < nSize;i++)
	{
		AMFVarWrapper *pAmf = m_vecMetaData[i];
		if (pAmf)
		{
			AMFCodec::Encode(&sWriter, pAmf->var);
		}
	}
	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = refOutPacket.m_sBody.UsedLength();
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	return true;
}

void MMetaData::Dump()
{
	BCPString strOutbuf;
	strOutbuf << "\nMeta data : \n\t";
	LogDebug(_LOCAL_, strOutbuf.c_str());
}

//////////////////////////////////////////////////////////////////////////
/// class MSharedObject
//////////////////////////////////////////////////////////////////////////

// Shared object event

MSOEvent::MSOEvent()
	: m_eType(0)
	, m_pKey(NULL)
	, m_pValue(NULL)
{
	//
}

MSOEvent::MSOEvent(const MSOEvent &refEvent)
	: BCNodeList::Node()
	, m_eType(refEvent.m_eType)
	, m_pKey(NULL)
	, m_pValue(NULL)
{
	*this = refEvent;
}

MSOEvent::~MSOEvent()
{
	Clear();
}

bool MSOEvent::Decode(BCIStream &refReader)
{
	uint32_t nLen;

	// Read event type
	if (!refReader.Eof())
	{
		refReader.ReadUInt8(&m_eType);
	}
	// Read event data
	if (!refReader.Eof())
	{
		AMFTableEntry *pData;

		// Read event data length
		refReader.ReadUInt32BE(&nLen);
		// Read event data
		if (m_eType == MSO_CLIENT_STATUS)
		{
			if (!m_pKey)
			{
				m_pKey = AMFVarPtr(AMFCodec::CreateVar(AMF_STRING));
			}
			ASSERT(m_pKey);
			AMFCodec::DecodeString(&refReader, m_pKey);
			// Status level
			AMFCodec::DecodeString(&refReader, m_pValue);
		}
		else if (m_eType == MSO_CLIENT_UPDATE_DATA)
		{
			uint32_t nStart;

			//BC_SAFE_DELETE_PTR(m_pKey);
			nStart = refReader.ConsumedLength();
			while(refReader.ConsumedLength() - nStart < nLen)
			{
				pData = new AMFTableEntry();
				ASSERT(pData);
				pData->Read(&refReader);
				m_lstVars.PushBack(pData);
			}
		}
		else if (m_eType == MSO_SERVER_SEND_MESSAGE)
		{
			AMFVarPtr pValue;
			uint32_t nStart;

			nStart = refReader.ConsumedLength();
			// the "send" event seems to encode the handler name
			// as complete AMF string including the string type byte
			//BC_SAFE_DELETE_PTR(m_pKey);
			m_pKey = AMFCodec::Decode(&refReader);
			ASSERT(m_pKey && m_pKey->GetType() == AMF_STRING);

			// read parameters
			while (refReader.ConsumedLength() - nStart < nLen)
			{
				pValue = AMFCodec::Decode(&refReader);
				m_lstVars.PushBack(new AMFVarWrapper(pValue));
			}
		}
		else
		{
			//BC_SAFE_DELETE_PTR(m_pValue);
			if (nLen > 0)
			{
				if (!m_pKey)
				{
					m_pKey = AMFVarPtr(AMFCodec::CreateVar(AMF_STRING));
				}
				ASSERT(m_pKey);
				AMFCodec::DecodeString(&refReader, m_pKey);
				// Status level
				m_pValue = AMFCodec::Decode(&refReader);
			}
		}
	}
	return true;
}

bool MSOEvent::Encode(BCOStream &refWriter)
{
	BCBuffer sBuffer;
	BCBOStream sWriter(&sBuffer);

	sWriter.SetUserData(refWriter.GetUserData());
	// Write event data
	switch(m_eType)
	{
	case MSO_SERVER_CONNECT:
	case MSO_CLIENT_INITIAL_DATA:
	case MSO_CLIENT_CLEAR_DATA:
		// Write event type
		refWriter.WriteUInt8(m_eType);
		// Write event data length
		refWriter.WriteUInt32BE(0);
		break;
	case MSO_SERVER_DELETE_ATTRIBUTE:
	case MSO_CLIENT_DELETE_DATA:
	case MSO_CLIENT_UPDATE_ATTRIBUTE:
		{
			// Write event type
			refWriter.WriteUInt8(m_eType);
			// Prepare event data
			sBuffer.Reset(1);
			// Write property name
			ASSERT(m_pKey);
			AMFCodec::EncodeString(&sWriter, m_pKey);
			// Write event data length
			refWriter.WriteUInt32BE(sBuffer.UsedLength());
			// Write event data length
			sBuffer.WriteTo(refWriter);
		}
		break;
	case MSO_SERVER_SET_ATTRIBUTE:
	case MSO_CLIENT_UPDATE_DATA:
		if (!m_pKey)
		{
			AMFVarWrapper *pEntry, *pEnd;

			// Update multiple attributes in one request
			pEntry = (AMFVarWrapper *)m_lstVars.Begin();
			pEnd = (AMFVarWrapper *)m_lstVars.End();
			for (;pEntry != pEnd;pEntry = (AMFVarWrapper *)m_lstVars.Next(pEntry))
			{
				// Write event type
				refWriter.WriteUInt8(m_eType);
				// Prepare event data
				sBuffer.Reset(1);
				pEntry->Write(&sWriter);
				// Write event data length
				refWriter.WriteUInt32BE(sBuffer.UsedLength());
				// Write event data length
				sBuffer.WriteTo(refWriter);
			}
		}
		else
		{
			// Write event type
			refWriter.WriteUInt8(m_eType);
			// Prepare event data
			sBuffer.Reset(1);
			// Write property name
			ASSERT(m_pKey);
			AMFCodec::EncodeString(&sWriter, m_pKey);
			ASSERT(m_pValue);
			AMFCodec::Encode(&sWriter, m_pValue);
			// Write event data length
			refWriter.WriteUInt32BE(sBuffer.UsedLength());
			// Write event data length
			sBuffer.WriteTo(refWriter);
		}
		break;
	case MSO_SERVER_SEND_MESSAGE:
		{
			AMFVarWrapper *pEntry, *pEnd;

			// Write event type
			refWriter.WriteUInt8(m_eType);
			// Send method name and value
			sBuffer.Reset(1);
			// Serialize name of the handler to call...
			ASSERT(m_pKey);
			AMFCodec::EncodeString(&sWriter, m_pKey, true);
			// ...and the arguments
			pEntry = m_lstVars.Begin();
			pEnd = m_lstVars.End();
			for (;pEntry != pEnd;pEntry = m_lstVars.Next(pEntry))
			{
				pEntry->Write(&sWriter);
			}
			// Write event data length
			refWriter.WriteUInt32BE(sBuffer.UsedLength());
			// Write event data length
			sBuffer.WriteTo(refWriter);
		}
		break;
	case MSO_CLIENT_STATUS:
		{
			// Write event type
			refWriter.WriteUInt8(m_eType);
			// Prepare event data
			sBuffer.Reset(1);
			// Write status string
			ASSERT(m_pKey);
			AMFCodec::EncodeString(&sWriter, m_pKey);
			// Write message string
			ASSERT(m_pValue);
			m_pValue->Write(&sWriter);
			// Write event data length
			refWriter.WriteUInt32BE(sBuffer.UsedLength());
			// Write event data length
			sBuffer.WriteTo(refWriter);
		}
		break;
	default:
		{
			//log.error("Unknown event " + event.getType());
			// XXX: come back here, need to make this work in server or client mode
			// talk to joachim about this part.
			// Write event type
			refWriter.WriteUInt8(m_eType);
			// Prepare event data
			sBuffer.Reset(1);
			// Write property name
			if (m_pKey)
			{
				AMFCodec::EncodeString(&sWriter, m_pKey);
			}
			if (m_pValue)
			{
				AMFCodec::Encode(&sWriter, m_pValue);
			}
			// Write event data length
			refWriter.WriteUInt32BE(sBuffer.UsedLength());
			// Write event data length
			sBuffer.WriteTo(refWriter);
		}
		break;
	}
	return true;
}

MSOEvent *MSOEvent::Clone() const
{
	MSOEvent *pEvent;

	pEvent = new MSOEvent(*this);

	return pEvent;
}

MSOEvent &MSOEvent::operator = (const MSOEvent &refEvent)
{
	m_eType = refEvent.m_eType;
	Clear();
	if (refEvent.m_pKey)
	{
		m_pKey.reset(refEvent.m_pKey->Clone());
	}
	if (refEvent.m_pValue)
	{
		m_pValue.reset(refEvent.m_pValue->Clone());
	}
	if (refEvent.m_lstVars.Count())
	{
		AMFVarWrapper *pEntry, *pEnd;

		pEntry = refEvent.m_lstVars.Begin();
		pEnd = refEvent.m_lstVars.End();
		for (;pEntry != pEnd;pEntry = refEvent.m_lstVars.Next(pEntry))
		{
			m_lstVars.PushBack(pEntry->Clone());
		}
	}
	return *this;
}

void MSOEvent::SetKey(LPCSTR szKey)
{
	if (!m_pKey)
	{
		m_pKey.reset(AMFCodec::CreateVar(AMF_STRING));
	}
	ASSERT(m_pKey);
	AMFCast<AMFString>(m_pKey)->SetValue(szKey);
}

LPCSTR MSOEvent::GetKey() const
{
	if (m_pKey)
	{
		return AMFCast<AMFString>(m_pKey)->GetValue();
	}
	return NULL;
}

AMFVarPtr MSOEvent::GetValue() const
{
	return m_pValue;
}

void MSOEvent::SetValue(bool bValue)
{
	AMFBool *pVar;

	ASSERT(m_lstVars.Count() == 0);

	//BC_SAFE_DELETE_PTR(m_pValue);
	pVar = new AMFBool();
	ASSERT(pVar);
	pVar->SetValue(bValue);
	m_pValue.reset(pVar);
}

void MSOEvent::SetValue(double dbValue)
{
	AMFNumber *pVar;

	ASSERT(m_lstVars.Count() == 0);

	//BC_SAFE_DELETE_PTR(m_pValue);
	pVar = new AMFNumber();
	ASSERT(pVar);
	pVar->SetDoubleValue(dbValue);
	m_pValue.reset(pVar);
}

void MSOEvent::SetValue(LPCSTR szValue)
{
	AMFString *pValue;

	ASSERT(szValue);
	ASSERT(m_lstVars.Count() == 0);

	//BC_SAFE_DELETE_PTR(m_pValue);
	pValue = new AMFString();
	ASSERT(pValue);
	pValue->SetValue(szValue);
	m_pValue.reset(pValue);
}

void MSOEvent::SetValue(AMFVar *pVar)
{
	ASSERT(pVar);
	ASSERT(m_lstVars.Count() == 0);

	//BC_SAFE_DELETE_PTR(m_pValue);
	m_pValue.reset(pVar->Clone());
}

void MSOEvent::Put(AMFVarPtr &pVar)
{
	if (m_eType == MSO_SERVER_SEND_MESSAGE)
	{
		ASSERT(!m_pValue);
		m_lstVars.PushBack(new AMFVarWrapper(pVar));
	}
	else
	{
		ASSERT(0);
	}
}

void MSOEvent::PutBool(bool bValue)
{
	AMFVarPtr pVar = AMFVarPtr(new AMFBool());
	if (pVar)
	{
		AMFCast<AMFBool>(pVar)->SetValue(bValue);
		Put(pVar);
	}
}

void  MSOEvent::PutDouble(double dbValue)
{
	AMFVarPtr pVar = AMFVarPtr(new AMFNumber());
	if (pVar)
	{
		AMFCast<AMFNumber>(pVar)->SetDoubleValue(dbValue);
		Put(pVar);
	}
}

void  MSOEvent::PutString(LPCSTR szValue)
{
	AMFVarPtr pVar = AMFVarPtr(AMFCodec::CreateVar(AMF_STRING));
	if (pVar)
	{
		AMFCast<AMFString>(m_pKey)->SetValue(szValue);
		Put(pVar);
	}
}

AMFVarPtr MSOEvent::Get(LPCSTR szKey)
{
	AMFVarWrapper *pEntry, *pEnd;
	AMFTableEntry* pTBEntry;

	pEntry = m_lstVars.Begin();
	pEnd = m_lstVars.End();
	for(;pEntry != pEnd;pEntry = m_lstVars.Next(pEntry))
	{
		pTBEntry = (AMFTableEntry *)pEntry;
		if(AMFCast<AMFString>(pTBEntry->GetKey())->GetValue() == szKey)
		{
			return pTBEntry->GetValue();
		}
	}
	return 0;
}

void MSOEvent::Put(LPCSTR szKey, AMFVarPtr &var)
{
	if (m_eType == MSO_SERVER_SET_ATTRIBUTE ||
		m_eType == MSO_CLIENT_UPDATE_DATA)
	{
		AMFVarWrapper* pEntry, *pEnd;
		AMFTableEntry *pTBEntry;

		ASSERT(!m_pKey && !m_pValue);
		pEntry = m_lstVars.Begin();
		pEnd = m_lstVars.End();
		for(;pEntry != pEnd;pEntry = m_lstVars.Next(pEntry))
		{
			ASSERT(pEntry->GetType() == AMF_TABLEENTRY);
			pTBEntry = (AMFTableEntry *)pEntry;
			if(AMFCast<AMFString>(pTBEntry->GetKey())->GetValue() == szKey)
			{
				pTBEntry->SetValue(var);
				return;
			}
		}

		// create a new entry and shove it into the list
		pTBEntry = new AMFTableEntry();
		ASSERT(pTBEntry);
		pTBEntry->SetKey(szKey);
		pTBEntry->SetValue(var);
		m_lstVars.PushBack(pTBEntry);
	}
	else
	{
		ASSERT(0);
	}
}

void MSOEvent::PutBool(LPCSTR key, bool bValue)
{
	AMFVarPtr pVar(new AMFBool());
	if (pVar)
	{
		AMFCast<AMFBool>(pVar)->SetValue(bValue);
		Put(key, pVar);
	}
}

void  MSOEvent::PutDouble(LPCSTR key, double dbValue)
{
	AMFVarPtr pVar(new AMFNumber());
	if (pVar)
	{
		AMFCast<AMFNumber>(pVar)->SetDoubleValue(dbValue);
		Put(key, pVar);
	}
}

void  MSOEvent::PutString(LPCSTR key, LPCSTR szValue)
{
	AMFVarPtr pVar(new AMFString());
	if (pVar)
	{
		AMFCast<AMFString>(pVar)->SetValue(szValue);
		Put(key, pVar);
	}
}

bool  MSOEvent::IsContainsKey(LPCSTR key)
{
	return !!Get(key);
}

void MSOEvent::Clear()
{
	AMFVarWrapper *pEntry;

	//BC_SAFE_DELETE_PTR(m_pKey);
	//BC_SAFE_DELETE_PTR(m_pValue);
	while((pEntry = m_lstVars.PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pEntry);
	}
}

MSharedObject::MSharedObject(bool bFlexSO /*= false*/)
	: MBase(0, 0, MTYPE_SHAREDOBJECT, 0)
	, m_nVersion(0)
	, m_bPersistent(false)
	, m_nOwnerId(0)
	, m_bFlexSO(bFlexSO)
{
	memzero(m_szPath, sizeof(m_szPath));
}

MSharedObject::~MSharedObject()
{
	Clear();
}

bool MSharedObject::Decode(PPacket &refInPacket)
{
	m_nChannelId = refInPacket.m_pHeader->m_nChannelId;
	m_nDataSize = refInPacket.m_pHeader->m_nDataSize;
	m_nTimestamp = refInPacket.m_pHeader->m_nTimestamp;
	m_nStreamId = refInPacket.m_pHeader->m_nStreamId;

	BCBIStream sReader(&refInPacket.m_sBody);
	sReader.SetUserData(&m_sCodecCtx);
	if (m_bFlexSO && !sReader.Eof())
	{
		uint8_t eEncoding;
		sReader.ReadUInt8(&eEncoding);
		ASSERT(eEncoding == ObjectEncoding::AMF0 || eEncoding == ObjectEncoding::AMF3);
	}
	if (!sReader.Eof())
	{
		AMFCodec::DecodeString(&sReader, m_strSOName);
	}
	if (!sReader.Eof())
	{
		sReader.ReadUInt32BE(&m_nVersion);
	}
	if (!sReader.Eof())
	{
		uint32_t bPersistent = 0;
		sReader.ReadUInt32BE(&bPersistent);
		m_bPersistent = (bPersistent == 2)?true:false;
		// read unknown 4 bytes
		sReader.ReadUInt32BE(&bPersistent);
	}
	while(!sReader.Eof())
	{
		MSOEvent *pEvent;

		pEvent = new MSOEvent();
		if (pEvent == NULL)
		{
			goto out;
		}
		if (!pEvent->Decode(sReader))
		{
			BC_SAFE_DELETE_PTR(pEvent);
			goto out;
		}
		m_lstEvents.PushBack(pEvent);
	}

	return true;
out:
	return false;
}

bool MSharedObject::Encode(PPacket &refOutPacket)
{
	MSOEvent *pIter, *pEnd;

	BCBOStream sWriter(&refOutPacket.m_sBody, false);
	sWriter.SetUserData(&m_sCodecCtx);
	// Write shared object name
	AMFCodec::EncodeString(&sWriter, m_strSOName);
	// Write version
	sWriter.WriteUInt32BE(m_nVersion);
	// Write flags
	sWriter.WriteUInt32BE(m_bPersistent?2:0);
	sWriter.WriteUInt32BE(0);
	// Write events
	pIter = m_lstEvents.Begin();
	pEnd = m_lstEvents.End();
	for (;pIter != pEnd;pIter = m_lstEvents.Next(pIter))
	{
		if (!pIter->Encode(sWriter))
		{
			goto out;
		}
	}
	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = refOutPacket.m_sBody.UsedLength();
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);

	return true;
out:
	return false;
}

void MSharedObject::Dump()
{
	BCPString strOutbuf;
	strOutbuf << "\nShared Object : \n\t";
	LogDebug(_LOCAL_, strOutbuf.c_str());
}

void MSharedObject::AddEvent(MSOEvent *pEvent)
{
	ASSERT(pEvent);

	pEvent->RemoveFromList();
	m_lstEvents.PushBack(pEvent);
}

void MSharedObject::Clear()
{
	MSOEvent *pEvent;

	//m_strSOName.clear();
	m_nVersion = 0;
	m_bPersistent = false;
	while ((pEvent = m_lstEvents.PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pEvent);
	}
}

MSharedObject *MSharedObject::Clone() const
{
	MSharedObject *pMSO;

	pMSO = new MSharedObject();
	if (pMSO)
	{
		*pMSO = *this;
	}
	return pMSO;
}

MSharedObject &MSharedObject::operator = (const MSharedObject &other)
{
	MSOEvent *pIter, *pEnd, *pEvent;

	Clear();
	m_nChannelId = other.m_nChannelId;
	m_nTimestamp = other.m_nTimestamp;
	m_nDataSize = other.m_nDataSize;
	m_nStreamId = other.m_nStreamId;
	m_strSOName.reset(other.m_strSOName->Clone());
	memcpy2(m_szPath, other.m_szPath, strlen(other.m_szPath));
	m_nVersion = other.m_nVersion;
	m_bPersistent = other.m_bPersistent;
	m_bFlexSO = other.m_bFlexSO;
	pIter = other.m_lstEvents.Begin();
	pEnd = other.m_lstEvents.End();
	for (;pIter != pEnd;pIter = other.m_lstEvents.Next(pIter))
	{
		pEvent = pIter->Clone();
		if (pEvent == NULL)
		{
			break;
		}
		m_lstEvents.PushBack(pEvent);
	}
	m_nOwnerId = other.m_nOwnerId;

	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// class : MCommand
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(MCommand, 32);

const char	*MCommand::_result			= "_result";
const char	*MCommand::_error			= "_error";
const char	*MCommand::onStatus			= "onStatus";
const char	*MCommand::s_szLevel		= "level";
const char	*MCommand::s_szCode			= "code";
const char	*MCommand::s_szDescription	= "description";
const char	*MCommand::s_szResult		= "result";
const char	*MCommand::s_szError		= "error";
const char	*MCommand::s_szStatus		= "status";

#define PROPERTY_TYPE		AMF::AMFType::AMF0_OBJECT

MCommand::MCommand(bool bFlex /*= false*/)
	: MBase(0, 0, MTYPE_COMMAND, 0)
	, m_eCmdType(MCMD_CALL)
	, m_strCmdName(NULL)
	, m_nOwnerId(0)
	, m_nCmdId(0)
	, m_bInit(false)
	, m_bFlex(bFlex)
{
	_Initialize();
}

MCommand::MCommand(const MBase &refMsg)
	: MBase(refMsg.m_nChannelId, refMsg.m_nTimestamp,
		MTYPE_COMMAND, refMsg.m_nStreamId)
	, m_eCmdType(MCMD_CALL)
	, m_strCmdName(NULL)
	, m_nOwnerId(0)
	, m_nCmdId(0)
	, m_bInit(false)
	, m_bFlex(false)
{
	_Initialize();
}

MCommand::MCommand(const MCommand &refMsg)
	: MBase(refMsg.m_nChannelId, refMsg.m_nTimestamp,
		MTYPE_COMMAND, refMsg.m_nStreamId)
	, m_eCmdType(MCMD_CALL)
	, m_strCmdName(NULL)
	, m_nOwnerId(0)
	, m_nCmdId(0)
	, m_bInit(false)
	, m_bFlex(false)
{
	operator = (refMsg);
}

MCommand::MCommand(
	uint32_t nChannelId,
	uint32_t nTimestamp,
	uint32_t nStreamId)
	: MBase(nChannelId, nTimestamp, MTYPE_COMMAND, nStreamId)
	, m_eCmdType(MCMD_CALL)
	, m_strCmdName(NULL)
	, m_nOwnerId(0)
	, m_nCmdId(0)
	, m_bInit(false)
	, m_bFlex(false)
{
	_Initialize();
}

MCommand::~MCommand()
{
	Clear();
}

bool MCommand::Decode(PPacket &refInPacket)
{
	try
	{
		AMFVarPtr pVar;
		uint8_t eEncoding;
		int32_t nIndex = 0;

		m_nChannelId	= refInPacket.m_pHeader->m_nChannelId;
		m_nDataSize		= refInPacket.m_pHeader->m_nDataSize;
		m_nTimestamp	= refInPacket.m_pHeader->m_nTimestamp;
		m_nStreamId		= refInPacket.m_pHeader->m_nStreamId;

		Clear();
		BCBIStream sReader(&refInPacket.m_sBody);
		sReader.SetUserData(&m_sCtx);
		if (m_bFlex)
		{
			sReader.ReadUInt8(&eEncoding);
			if (eEncoding != ObjectEncoding::AMF0 && eEncoding != ObjectEncoding::AMF3)
			{
				throw BC_R_INVALIDARG;
			}
		}
		// store encoding setting
		eEncoding = m_sCtx.GetEncoding();
		while(!sReader.Eof())
		{
			if (nIndex < 3)
			{
				m_sCtx.SetEncoding(ObjectEncoding::AMF0);
				nIndex++;
			}
			else 
			{
				// Restore encoding setting
				m_sCtx.SetEncoding(eEncoding);
			}
			pVar = AMFCodec::Decode(&sReader);
			if (!pVar)
			{
				throw BC_R_UNEXPECTED;
			}
			m_lstVars.PushBack(new AMFVarWrapper(pVar));
		}
		AMFVarPtr pCmdName = m_lstVars[0]->var;
		if (pCmdName->GetType() != AMF_STRING)
		{
			throw BC_R_INVALIDARG;
		}
		m_strCmdName = AMFCast<AMFString>(pCmdName)->GetValue();
		m_eCmdType = GetCmdType(m_strCmdName);

		return true;
	}
	catch (...)
	{
		LogFatal(_LOCAL_, "MCommand::Decode error :");
		refInPacket.m_sBody.Rewind();
		LogBuffer(_LOCAL_, &refInPacket.m_sBody);
		return false;
	}
}

bool MCommand::Encode(PPacket &refOutPacket)
{
	AMF::AMFVarWrapper *pVar, *pEnd;
	long eEncoding;
	uint32_t nCount = 0;

	BCBOStream sWriter(&refOutPacket.m_sBody, false);
	sWriter.SetUserData(&m_sCtx);
	m_sCtx.Clear();
	eEncoding = m_sCtx.GetEncoding();
	pVar = m_lstVars.Begin();
	pEnd = m_lstVars.End();
	for (;pVar != pEnd;pVar = m_lstVars.Next(pVar))
	{
		if (nCount++ < 3)
		{
			m_sCtx.SetEncoding(AMF::ObjectEncoding::AMF0);
			AMF::AMFCodec::Encode(&sWriter, pVar->var);
		}
		else
		{
			AMF::AMFCodec::Encode(&sWriter, pVar->var);
		}
	}
	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = refOutPacket.m_sBody.UsedLength();
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	//LogBin(_LOCAL_, refOutPacket.m_sBody.Base(), refOutPacket.m_sBody.UsedLength());

	return true;
}

bool MCommand::Decode(BCBuffer &refInBuffer)
{
	try
	{
		//LogDebug(_LOCAL_,"MCommand::Decode enter\n");
		AMFVarPtr pVar;
		uint8_t eEncoding;
		int32_t nIndex = 0;

		Clear();
		BCBIStream sReader(&refInBuffer);
		sReader.SetUserData(&m_sCtx);
		if (m_bFlex)
		{
			sReader.ReadUInt8(&eEncoding);
			if (eEncoding != ObjectEncoding::AMF0 && eEncoding != ObjectEncoding::AMF3)
			{
				throw BC_R_INVALIDARG;
			}
		}
		// store encoding setting
		eEncoding = m_sCtx.GetEncoding();
		while(!sReader.Eof())
		{
			if (nIndex < 3)
			{
				m_sCtx.SetEncoding(ObjectEncoding::AMF0);
				nIndex++;
			}
			else
			{
				// Restore encoding setting
				m_sCtx.SetEncoding(eEncoding);
			}
			pVar = AMFCodec::Decode(&sReader);
			if (!pVar)
			{
				throw BC_R_UNEXPECTED;
			}
			m_lstVars.PushBack(new AMFVarWrapper(pVar));
		}
		AMFVarPtr pCmdName = m_lstVars[0]->var;
		if (pCmdName->GetType() != AMF::AMF_STRING)
		{
			throw BC_R_INVALIDARG;
		}
		m_strCmdName = AMFCast<AMFString>(pCmdName)->GetValue();
		m_eCmdType = GetCmdType(m_strCmdName);

		return true;
	}
	catch (...)
	{
		LogFatal(_LOCAL_, "MCommand::Decode error :");
		refInBuffer.Rewind();
		LogBuffer(_LOCAL_, &refInBuffer);
		return false;
	}
}

bool MCommand::Encode(BCBuffer &refOutBuffer)
{
	AMF::AMFVarWrapper *pVar, *pEnd;
	long eEncoding;
	uint32_t nCount = 0;

	BCBOStream sWriter(&refOutBuffer, false);
	sWriter.SetUserData(&m_sCtx);
	eEncoding = m_sCtx.GetEncoding();
	pVar = m_lstVars.Begin();
	pEnd = m_lstVars.End();
	for (;pVar != pEnd;pVar = m_lstVars.Next(pVar))
	{
		if (nCount++ < 3)
		{
			m_sCtx.SetEncoding(AMF::ObjectEncoding::AMF0);
			AMF::AMFCodec::Encode(&sWriter, pVar->var);
		}
		else
		{
			AMF::AMFCodec::Encode(&sWriter, pVar->var);
		}
	}

	return true;
}

void MCommand::Dump()
{
	printf("%s\n", GetCmdName().c_str());
}

uint32_t MCommand::GetCmdType(LPCSTR pCmdStr)
{
	struct CmdDef_t{
		const char			*szCmdStr;
		const uint32_t		eMCmdType;
	};
	static const CmdDef_t cmddefs[] =
	{
		{"connect",			MCMD_CONNECT},
		{"call",			MCMD_CALL},
		{"createStream",	MCMD_CREATESTREAM},
		{"close",			MCMD_CLOSE},
		{"play",			MCMD_PLAY},
		{"play2",			MCMD_PLAY2},
		{"deleteStream",	MCMD_DELETESTREAM},
		{"closeStream",		MCMD_CLOSESTREAM},
		{"releaseStream",	MCMD_RELEASESTREAM},
		{"receiveAudio",	MCMD_RECEIVEAUDIO},
		{"receiveVideo",	MCMD_RECEIVEVIDEO},
		{"FCPublish",       MCMD_FCPUBLISH},
		{"publish",			MCMD_PUBLISH},
		{"FCUnpublish",		MCMD_FCUNPUBLISH},
		{"seek",			MCMD_SEEK},
		{"pause",			MCMD_PAUSE},
		{"pauseRaw",		MCMD_PAUSERAW},
		{"disconnect",		MCMD_DISCONNECT},
		{"onStatus",		MCMD_ONSTATUS},
		{"_result",			MCMD_RESULT},
		{"_error",			MCMD_ERROR},
		{"_warning",		MCMD_WARNING},
		{"unpublish",		MCMD_UNPUBLISH},
		{NULL,				MCMD_NULL}
	};
	const CmdDef_t *pCmdDef = cmddefs;
	for (; pCmdDef->szCmdStr != NULL;pCmdDef++)
	{
		if (strlen(pCmdDef->szCmdStr) == strlen(pCmdStr)
			&& !strcmp(pCmdDef->szCmdStr, pCmdStr))
		{
			return pCmdDef->eMCmdType;
		}
	}
	return MCMD_CALL;
}

uint32_t MCommand::GetCmdType() const
{
	return m_eCmdType;
}

// Command name
void MCommand::SetCmdName(const char *szValue)
{
	AMF::AMFVarPtr pCmdName;

	pCmdName = _EnsureItemByType(CMDNAME_INDEX, AMF::AMFType::AMF_STRING);
	ASSERT(pCmdName && pCmdName->GetType() == AMF::AMFType::AMF_STRING);
	AMFCast<AMFString>(pCmdName)->SetValue(szValue);
	m_strCmdName = AMFCast<AMFString>(pCmdName)->GetValue();
	m_eCmdType = GetCmdType(m_strCmdName);
}

const BCPString MCommand::GetCmdName() const
{
	if (0 < m_lstVars.Count())
	{
		AMF::AMFVarPtr pCmdName;

		pCmdName = m_lstVars.Begin()->var;
		ASSERT(AMF::AMF_STRING == pCmdName->GetType());
		return AMFCast<AMFString>(pCmdName)->GetValue();
	}
	return BCPString(NULLPSTRING);
}

void MCommand::SetTransId(uint32_t nTransId)
{
	AMF::AMFVarPtr pTransId;

	pTransId = _EnsureItemByType(TRANSID_INDEX, AMF::AMFType::AMF_NUMBER);
	ASSERT(pTransId && pTransId->GetType() == AMF::AMFType::AMF_NUMBER);
	AMFCast<AMFNumber>(pTransId)->SetDoubleValue(nTransId);
}

uint32_t MCommand::GetTransId() const
{
	if (1 < m_lstVars.Count())
	{
		AMF::AMFVarWrapper *pTransId = m_lstVars[TRANSID_INDEX];
		ASSERT(AMF::AMFType::AMF_NUMBER == pTransId->var->GetType());
		return (uint32_t)AMFCast<AMFNumber>(pTransId->var)->GetDoubleValue();
	}
	return 0;
}

void MCommand::SetClientId(const char *szClientId)
{
	if (szClientId)
	{
		strncpy(m_szClientId, szClientId, sizeof(m_szClientId));
	}
}

LPCSTR MCommand::GetClientId() const 
{
	return m_szClientId;
}

// Add properties if response is AMF0Object, which will create AMF0Object type
// command object automatically
void MCommand::AddBoolProperty(const char *szProp, bool bValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(PROPERTY_INDEX, PROPERTY_TYPE);
	ASSERT(pVar && pVar->GetType() == PROPERTY_TYPE);
	AMFCast<AMF::AMFTable>(pVar)->PutBool(szProp, bValue);
}

void MCommand::AddDoubleProperty(const char *szProp, double dblValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(PROPERTY_INDEX, PROPERTY_TYPE);
	ASSERT(pVar && pVar->GetType() == PROPERTY_TYPE);
	AMFCast<AMF::AMFTable>(pVar)->PutDouble(szProp, dblValue);
}

void MCommand::AddStringProperty(const char *szProp, const char *szValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(PROPERTY_INDEX, PROPERTY_TYPE);
	ASSERT(pVar && pVar->GetType() == PROPERTY_TYPE);
	AMFCast<AMF::AMFTable>(pVar)->PutString(szProp, szValue);
}

void MCommand::AddVarProperty(const char *szPropKey, const AMF::AMFVarPtr &pValue)
{
	AMF::AMFVarPtr pVar;

	ASSERT(pValue);

	pVar = _EnsureItemByType(PROPERTY_INDEX, PROPERTY_TYPE);
	ASSERT(pVar && pVar->GetType() == PROPERTY_TYPE);
	AMFCast<AMF::AMFTable>(pVar)->Put(szPropKey, pValue);
}

void MCommand::AddProperty(const AMF::AMFVarPtr & pVar)
{
	GetProperty() = pVar;
}

AMF::AMFVarPtr MCommand::GetPropertyByKey(const char *szProp)
{
	if (m_lstVars.Count() >= 3
		&& m_lstVars[PROPERTY_INDEX]->var->GetType() == PROPERTY_TYPE)
	{
		AMFVarWrapper *pVar = m_lstVars[PROPERTY_INDEX];
		return AMFCast<AMF::AMFTable>(pVar->var)->Get(szProp);
	}
	return NULL;
}

AMF::AMFVarPtr &MCommand::GetProperty()
{
	return m_lstVars[PROPERTY_INDEX]->var;
}

AMF::AMFVarPtr MCommand::SetProperty(const AMF::AMFVarPtr &props)
{
	ASSERT(props->GetType() == PROPERTY_TYPE);
	AMF::AMFVarPtr pOldVar = m_lstVars[PROPERTY_INDEX]->var;
	m_lstVars[PROPERTY_INDEX]->var = props;
	return pOldVar;
}

// Add options if response is AMF0Object, which will create AMF0Object type
// response automatically
void MCommand::AddBoolOption(const char *szOption, bool bValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(OPTION_INDEX, PROPERTY_TYPE);
	ASSERT(pVar && pVar->GetType() == PROPERTY_TYPE);
	AMFCast<AMF::AMFTable>(pVar)->PutBool(szOption, bValue);
}

void MCommand::AddDoubleOption(const char *szOption, double dblValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(OPTION_INDEX, PROPERTY_TYPE);
	ASSERT(pVar && pVar->GetType() == PROPERTY_TYPE);
	AMFCast<AMF::AMFTable>(pVar)->PutDouble(szOption, dblValue);
}

void MCommand::AddStringOption(const char *szOption, const char *szValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(OPTION_INDEX, PROPERTY_TYPE);
	ASSERT(pVar && pVar->GetType() == PROPERTY_TYPE);
	AMFCast<AMF::AMFTable>(pVar)->PutString(szOption, szValue);
}

void MCommand::AddVarOption(const char *szOption, const AMF::AMFVarPtr & pOption)
{
	AMF::AMFVarPtr pVar;

	ASSERT(pOption);

	pVar = _EnsureItemByType(OPTION_INDEX, PROPERTY_TYPE);
	ASSERT(pVar && pVar->GetType() == PROPERTY_TYPE);
	AMFCast<AMF::AMFTable>(pVar)->Put(szOption, pOption);
}

AMF::AMFVarPtr MCommand::GetOptionByKey(const char *szProp)
{
	if (m_lstVars.Count() >= 4)
	{
		uint8_t eType = m_lstVars[OPTION_INDEX]->var->GetType();
		uint8_t eEncoding = m_lstVars[OPTION_INDEX]->var->GetEncoding();
		if ((eEncoding == ObjectEncoding::AMF0 && eType == PROPERTY_TYPE) ||
			(eEncoding == ObjectEncoding::AMF3 && eType == AMF3_OBJECT))
		{
			AMFVarWrapper *pVar = m_lstVars[OPTION_INDEX];
			return AMFCast<AMF::AMFTable>(pVar->var)->Get(szProp);
		}
	}
	
	return NULL;
}

const BCPString MCommand::GetResponseInfo(const char *szKey) /*const*/
{
	AMF::AMFVarPtr pTable, pVar;

	if (3 < m_lstVars.Count())
	{
		pTable = m_lstVars[OPTION_INDEX]->var;
		if (pTable && pTable->GetType() == PROPERTY_TYPE)
		{
			pVar = AMFCast<AMF::AMFTable>(pTable)->Get(szKey);
			if (pVar && pVar->GetType() == AMF::AMFType::AMF_STRING)
			{
				return AMFCast<AMF::AMFString>(pVar)->GetValue();
			}
		}
	}
	return BCPString(NULLPSTRING);
}

AMF::AMFVarPtr MCommand::GetResponse()
{
	return m_lstVars[OPTION_INDEX]->var;
}

// Set response type as AMF* type if response type is not AMF0Object
void MCommand::SetBoolResponse(bool bValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(OPTION_INDEX, AMF::AMFType::AMF_BOOL);
	ASSERT(pVar && pVar->GetType() == AMF::AMFType::AMF_BOOL);
	AMFCast<AMFBool>(pVar)->SetValue(bValue);
}

void MCommand::SetDoubleResponse(double dblValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(OPTION_INDEX, AMF::AMFType::AMF_NUMBER);
	ASSERT(pVar && pVar->GetType() == AMF::AMFType::AMF_NUMBER);
	AMFCast<AMFNumber>(pVar)->SetDoubleValue(dblValue);
}

void MCommand::SetStringResponse(const char *szValue)
{
	AMF::AMFVarPtr pVar;

	pVar = _EnsureItemByType(OPTION_INDEX, AMF::AMFType::AMF_STRING);
	ASSERT(pVar && pVar->GetType() == AMF::AMFType::AMF_STRING);
	AMFCast<AMFString>(pVar)->SetValue(szValue);
}

void MCommand::SetUndefinedResponse()
{
	_EnsureItemByType(OPTION_INDEX, AMF::AMFType::AMF_UNDEFINED);
}

void MCommand::SetVarResponse(const AMF::AMFVarPtr & pResp)
{
	ASSERT(pResp);
	if (m_lstVars.Count() <= OPTION_INDEX)
	{
		m_lstVars.PushBack(new AMFVarWrapper(pResp));
	}
	else
	{
		AMF::AMFVarWrapper *pOld = m_lstVars[OPTION_INDEX];
		pOld = m_lstVars.Replace(pOld, new AMFVarWrapper(pResp));
		BC_SAFE_DELETE_PTR(pOld);
	}
}

void MCommand::AppendNullResponse()
{
	AMF::AMFVarWrapper *pVar;

	pVar = new AMF::AMFVarWrapper(AMF_NULL);
	ASSERT(pVar);
	m_lstVars.PushBack(pVar);
}

void MCommand::AppendBoolResponse(bool bValue)
{
	AMF::AMFVarWrapper *pVar;

	pVar = new AMF::AMFVarWrapper(AMF_BOOL);
	ASSERT(pVar);
	AMFCast<AMFBool>(pVar->var)->SetValue(bValue);
	m_lstVars.PushBack(pVar);
}

void MCommand::AppendDoubleResponse(double dblValue)
{
	AMF::AMFVarWrapper *pVar;

	pVar = new AMF::AMFVarWrapper(AMF_NUMBER);
	ASSERT(pVar);
	AMFCast<AMFNumber>(pVar->var)->SetDoubleValue(dblValue);
	m_lstVars.PushBack(pVar);
}

void MCommand::AppendStringResponse(const char *szValue)
{
	AMF::AMFVarWrapper *pVar;

	ASSERT(szValue);

	pVar = new AMF::AMFVarWrapper(AMF_STRING);
	ASSERT(pVar);
	AMFCast<AMFString>(pVar->var)->SetValue(szValue);
	m_lstVars.PushBack(pVar);
}

void MCommand::AppendResponse(const AMF::AMFVarPtr & pVar)
{
	ASSERT(pVar);
	m_lstVars.PushBack(new AMFVarWrapper(pVar));
}

void  MCommand::Clear()
{
	m_lstVars.Clear();
}

void MCommand::Reset()
{
	m_lstVars.Clear();
	m_sCtx.SetEncoding(AMF::ObjectEncoding::AMF0);
	memzero(m_szClientId, sizeof(m_szClientId));
	m_lstVars.PushBack(new AMFVarWrapper(AMF_STRING)); // Command name
	m_lstVars.PushBack(new AMFVarWrapper(AMF_NUMBER)); // Transaction id
	m_lstVars.PushBack(new AMFVarWrapper(AMF_NULL));   // Command property
	m_bInit = true;
}

MCommand *MCommand::Clone() const
{
	MCommand *pCmd;

	pCmd = new MCommand();
	if (pCmd)
	{
		*pCmd = *this;
	}
	return pCmd;
}

MCommand &MCommand::operator=(const MCommand &other)
{
	AMF::AMFVarWrapper *pVar, *pEnd;

	m_lstVars.Clear();

	m_nChannelId = other.m_nChannelId;
	m_nTimestamp = other.m_nTimestamp;
	m_nDataSize = other.m_nDataSize;
	(uint32_t &)m_eDataType = other.m_eDataType;
	m_nStreamId = other.m_nStreamId;
	m_bAbsoluteTime;
	m_eCmdType	= other.m_eCmdType;
	m_nOwnerId	= other.m_nOwnerId;
	m_nCmdId	= other.m_nCmdId;
	pVar = other.m_lstVars.Begin();
	pEnd = other.m_lstVars.End();
	for (;pVar != pEnd;pVar = other.m_lstVars.Next(pVar))
	{
		m_lstVars.PushBack(pVar->Clone());
	}
	strncpy(m_szClientId, other.m_szClientId, sizeof(m_szClientId));
	if (0 < m_lstVars.Count())
	{
		AMF::AMFVarPtr pCmdName;

		pCmdName = m_lstVars.Begin()->var;
		ASSERT(AMF::AMF_STRING == pCmdName->GetType());
		m_strCmdName = AMFCast<AMFString>(pCmdName)->GetValue();
	}

	return *this;
}

AMF::AMFVarPtr MCommand::GetVar(uint32_t nIndex) const
{
	return m_lstVars[nIndex]->var;
}

uint32_t MCommand::GetVarCount() const
{
	return m_lstVars.Count();
}

AMF::AMFVarPtr MCommand::GetArguments(uint32_t nIndex) const
{
	ASSERT(nIndex > 0);
	uint32_t nCount = m_lstVars.Count();
	if (nCount > 3 && (nIndex+2) < nCount) {
		return m_lstVars[2 + nIndex]->var;
	}
	return AMFVarPtr();
}

uint32_t MCommand::GetArgumentsCount() const
{
	ASSERT(m_lstVars.Count() >= 3);
	return m_lstVars.Count() - 3;
}

void MCommand::_Initialize()
{
	if (m_bInit)
	{
		return;
	}
	m_lstVars.Clear();
	m_sCtx.SetEncoding(AMF::ObjectEncoding::AMF0);
	memzero(m_szClientId, sizeof(m_szClientId));
	m_lstVars.PushBack(new AMFVarWrapper(AMF_STRING)); // Command name
	m_lstVars.PushBack(new AMFVarWrapper(AMF_NUMBER)); // Transaction id
	m_lstVars.PushBack(new AMFVarWrapper(AMF_NULL));   // Command property
	m_bInit = true;
}

AMF::AMFVarPtr MCommand::_EnsureItemByType(uint32_t nIndex, uint32_t eType)
{
	uint32_t nSize;
	AMF::AMFVarWrapper *pVar, *pNewVar;

	nSize = m_lstVars.Count();
	for (;nSize <= nIndex;nSize++)
	{
		pVar = new AMFVarWrapper(AMF_NULL);
		m_lstVars.PushBack(pVar);
	}
	pVar = m_lstVars[nIndex];
	ASSERT(pVar);
	if (pVar->var->GetType() != eType)
	{
		pNewVar = new AMFVarWrapper(eType);
		ASSERT(pNewVar);
		pVar->ReplaceBy(pNewVar);
		BC_SAFE_DELETE_PTR(pVar);
		return pNewVar->var;
	}
	else
	{
		return pVar->var;
	}
}

//////////////////////////////////////////////////////////////////////////
/// class MAggregate
//////////////////////////////////////////////////////////////////////////

MAggregate::MAggregate()
	: MBase(0, 0, MTYPE_AGGREGATE, 0)
{
	//
}

MAggregate::~MAggregate()
{
	//
}

bool MAggregate::Decode(PPacket &refInPacket)
{
	m_nChannelId	= refInPacket.m_pHeader->m_nChannelId;
	m_nDataSize		= refInPacket.m_pHeader->m_nDataSize;
	m_nTimestamp	= refInPacket.m_pHeader->m_nTimestamp;
	m_nStreamId		= refInPacket.m_pHeader->m_nStreamId;
	refInPacket.m_sBody.Clone(m_sBody);

	return true;
}

bool MAggregate::Encode(PPacket &refOutPacket)
{
	refOutPacket.m_pHeader->m_nChannelId = m_nChannelId;
	refOutPacket.m_pHeader->m_nTimestamp = m_nTimestamp;
	refOutPacket.m_pHeader->m_nDataSize = m_sBody.UsedLength();
	refOutPacket.m_pHeader->m_eDataType = m_eDataType;
	refOutPacket.m_pHeader->m_nStreamId = m_nStreamId;
	refOutPacket.m_pHeader->SetAbsTime(m_bAbsoluteTime);
	m_sBody.Clone(refOutPacket.m_sBody);

	return true;
}

BCRESULT MAggregate::Append(PPacket &refAVPacket)
{
	const PHeader &refHeader = *refAVPacket.m_pHeader;

	if (refHeader.m_eDataType != MTYPE_AUDIODATA &&
		refHeader.m_eDataType != MTYPE_VIDEODATA &&
		refHeader.m_eDataType != MTYPE_METADATA)
	{
		return BC_R_NOTIMPLEMENTED;
	}
	BCBOStream sWriter(&m_sBody);

	sWriter.WriteUInt8(refHeader.m_eDataType); // data type
	sWriter.WriteUInt24BE(refHeader.m_nDataSize); // data size
	sWriter.WriteUInt24BE(refHeader.m_nTimestamp); // timestamp
	sWriter.WriteUInt8(refHeader.m_nTimestamp >> 24); // Ext timestamp
	sWriter.WriteUInt24BE(0); // stream id
	refAVPacket.m_sBody.WriteTo(sWriter); // body data
	sWriter.WriteUInt32BE(11 + refHeader.m_nDataSize); // Previous tag size

	m_nDataSize = m_sBody.RemainingLength();
	m_nTimestamp = refHeader.m_nTimestamp;

	return BC_R_SUCCESS;
}

void MAggregate::Reset()
{
	m_sBody.Reset();
	m_nDataSize = 0;
}

void MAggregate::Dump()
{
	LogDebug(_LOCAL_, "\nAggregate message.\n");
}

//////////////////////////////////////////////////////////////////////////
/// class MCodec
//////////////////////////////////////////////////////////////////////////

MCodec::MCodec()
{
	//
}

MCodec::~MCodec()
{
	//
}

bool MCodec::Decode(MBase &sOutMsg, PPacket &sInMsg)
{
	//if (sOutMsg.m_eDataType == MTYPE_COMMAND)
	//{
	//	((MCommand&)sOutMsg).SetEncoding(m_eObjectEncoding);
	//}
	return sOutMsg.Decode(sInMsg);
}

bool MCodec::Encode(PPacket &sOutMsg, MBase &sInMsg)
{
	//if (sInMsg.m_eDataType == MTYPE_COMMAND)
	//{
	//	((MCommand&)sInMsg).SetEncoding(m_eObjectEncoding);
	//}
	sOutMsg.m_pHeader->m_eChunkDataType = PHeader::CHUNK_SINGLE;
	return sInMsg.Encode(sOutMsg);
}

///////////////////////////////////////////////////////////////////////////////
// End of amespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file.
///////////////////////////////////////////////////////////////////////////////
