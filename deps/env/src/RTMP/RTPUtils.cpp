#include "Precompile.h"
#include <RTMP/FLVUtils.h>
#include "RTPUtils.h"

using namespace FLVUtils;

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

///////////////////////////////////////////////////////////////////////////////
// File-scope macros & utilities
///////////////////////////////////////////////////////////////////////////////



///////////////////////////////////////////////////////////////////////////////
// Class : RTPHeaderParser
///////////////////////////////////////////////////////////////////////////////

RTPHeaderParser::RTPHeaderParser(
	const uint8_t* rtpData,
	const uint32_t rtpDataLength)
	: _ptrRTPDataBegin(rtpData)
	, _ptrRTPDataEnd(rtpData ? (rtpData + rtpDataLength) : NULL) 
{
}

RTPHeaderParser::~RTPHeaderParser() {
}

bool RTPHeaderParser::Parse(RTPHeader& header) const {
	const int length = _ptrRTPDataEnd - _ptrRTPDataBegin;
	if (length < kRtpMinParseLength) {
		return false;
	}

	// Version
	const uint8_t V = _ptrRTPDataBegin[0] >> 6;
	// Padding
	const bool          P = ((_ptrRTPDataBegin[0] & 0x20) == 0) ? false : true;
	// eXtension
	const bool          X = ((_ptrRTPDataBegin[0] & 0x10) == 0) ? false : true;
	const uint8_t CC = _ptrRTPDataBegin[0] & 0x0f;
	const bool          M = ((_ptrRTPDataBegin[1] & 0x80) == 0) ? false : true;

	const uint8_t PT = _ptrRTPDataBegin[1] & 0x7f;

	const uint16_t sequenceNumber = (_ptrRTPDataBegin[2] << 8) +
		_ptrRTPDataBegin[3];

	const uint8_t* ptr = &_ptrRTPDataBegin[4];

	uint32_t RTPTimestamp = *ptr++ << 24;
	RTPTimestamp += *ptr++ << 16;
	RTPTimestamp += *ptr++ << 8;
	RTPTimestamp += *ptr++;

	uint32_t SSRC = *ptr++ << 24;
	SSRC += *ptr++ << 16;
	SSRC += *ptr++ << 8;
	SSRC += *ptr++;

	if (V != kRtpExpectedVersion) {
		return false;
	}

	const uint8_t CSRCocts = CC * 4;

	if ((ptr + CSRCocts) > _ptrRTPDataEnd) {
		return false;
	}

	header.markerBit = M;
	header.payloadType = PT;
	header.sequenceNumber = sequenceNumber;
	header.timestamp = RTPTimestamp;
	header.ssrc = SSRC;
	header.numCSRCs = CC;
	header.paddingLength = P ? *(_ptrRTPDataEnd - 1) : 0;

	for (unsigned int i = 0; i < CC; ++i) {
		uint32_t CSRC = *ptr++ << 24;
		CSRC += *ptr++ << 16;
		CSRC += *ptr++ << 8;
		CSRC += *ptr++;
		header.arrOfCSRCs[i] = CSRC;
	}

	header.headerLength = 12 + CSRCocts;

	// If in effect, MAY be omitted for those packets for which the offset
	// is zero.
	header.extension.transmissionTimeOffset = 0;

	// May not be present in packet.
	header.extension.absoluteSendTime = 0;

	if (X) {
		/* RTP header extension, RFC 3550.
		0                   1                   2                   3
		0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
		+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		|      defined by profile       |           length              |
		+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
		|                        header extension                       |
		|                             ....                              |
		*/
		const int remain = _ptrRTPDataEnd - ptr;
		if (remain < 4) {
			return false;
		}

		header.headerLength += 4;

		uint16_t definedByProfile = *ptr++ << 8;
		definedByProfile += *ptr++;

		uint16_t XLen = *ptr++ << 8;
		XLen += *ptr++; // in 32 bit words
		XLen *= 4; // in octs

		if (remain < (4 + XLen)) {
			return false;
		}
		//if (definedByProfile == kRtpOneByteHeaderExtensionId) {
		//	const uint8_t* ptrRTPDataExtensionEnd = ptr + XLen;
		//	ParseOneByteExtensionHeader(header,
		//		ptrExtensionMap,
		//		ptrRTPDataExtensionEnd,
		//		ptr);
		//}
		header.headerLength += XLen;
	}
	return true;
}

bool GetDeviceSpecificAudioCodecParameters(
	BCBuffer &refBuffer,
	uint32_t &sampleRate, 
	uint32_t &samplesPerFrame)
{
	RtpHeader* pHeader = NULL;
	bool bGotParams = false;
	FLVAudioInfoS sAInfo;
	uint8_t *pData = (uint8_t *)refBuffer.Base();
	sAInfo = FLVInfo::AnalyseAudio(pData[0]);
	if (sAInfo.eCodecId == FLV_DEVICE_SPECIFIC_SOUND_AUDIO)
	{
		uint8_t packetType = pData[1];
		switch (packetType)
		{
		case RTPTYPE_RTP:
		case RTPTYPE_RTCP:
			ASSERT(refBuffer.UsedLength() < IP_PACKET_SIZE);
			ASSERT(refBuffer.GetBlockSize() > IP_PACKET_SIZE);
			pHeader = reinterpret_cast<RtpHeader*>(&pData[2]);
			break;
		case RTPTYPE_RTP_RTCP:
			pHeader = reinterpret_cast<RtpHeader*>(&pData[5]);
			break;
		}
	}
	if (pHeader)
	{
		uint32_t payloadType = pHeader->getPayloadType();
		switch (payloadType)
		{
		case PAYLOAD_TYPE_SPEEX_16K:
			sampleRate = 16000;
			samplesPerFrame = 320;
			bGotParams = true;
			break;
		case PAYLOAD_TYPE_SPEEX_8K:
			sampleRate = 8000;
			samplesPerFrame = 160;
			bGotParams = true;
			break;
		case PAYLOAD_TYPE_OPUS:
			sampleRate = 48000;
			samplesPerFrame = 960;
			bGotParams = true;
			break;
		default:
			break;
		}
	}
	return bGotParams;
}

bool GetDeviceSpecificAudioData(
	BCBuffer &refBuffer,
	LPVOID &lpData, 
	uint32_t &nDataSize)
{
	RtpHeader* pHeader = NULL;
	FLVAudioInfoS sAInfo;
	uint8_t *pData = (uint8_t *)refBuffer.Base();
	uint32_t nOffset = 0, nPacketSize = 0;
	sAInfo = FLVInfo::AnalyseAudio(pData[0]);
	if (sAInfo.eCodecId == FLV_DEVICE_SPECIFIC_SOUND_AUDIO)
	{
		uint8_t packetType = pData[1];
		switch (packetType)
		{
		case RTPTYPE_RTP:
			ASSERT(refBuffer.UsedLength() < IP_PACKET_SIZE);
			ASSERT(refBuffer.GetBlockSize() > IP_PACKET_SIZE);
			pHeader = reinterpret_cast<RtpHeader*>(&pData[2]);
			nOffset = 2;
			nPacketSize = refBuffer.UsedLength() - 2;
			break;
		case RTPTYPE_RTP_RTCP:
			pHeader = reinterpret_cast<RtpHeader*>(&pData[5]);
			nOffset = 5;
			nPacketSize = BC_UI16BEI(pData + 2) - 1;
			break;
		}
	}
	if (pHeader)
	{
		uint32_t payloadType = pHeader->getPayloadType();
		uint32_t headerLen = pHeader->getHeaderLength();
		lpData = pData + nOffset + headerLen;
		nDataSize = nPacketSize - headerLen;
		return true;
	}
	return false;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP
