#ifndef RTMP_RTPUTILS_H_INCLUDED__
#define RTMP_RTPUTILS_H_INCLUDED__


///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

///////////////////////////////////////////////////////////////////////////////
// class : RtpHeader
///////////////////////////////////////////////////////////////////////////////


#define IP_PACKET_SIZE 1500 

typedef enum
{
	RTPTYPE_RTP			= 0,
	RTPTYPE_RTCP		= 1,
	RTPTYPE_RTP_RTCP	= 2,
}FlvAacPacketTypeE;

enum
{
	PAYLOAD_TYPE_SPEEX_16K		= 84,
	PAYLOAD_TYPE_SPEEX_8K		= 85,
	PAYLOAD_TYPE_OPUS			= 120,
};

enum {
	kRtcpExpectedVersion = 2,
	kRtcpMinHeaderLength = 4,
	kRtcpMinParseLength = 8,

	kRtpExpectedVersion = 2,
	kRtpMinParseLength = 12
};

// RTP
enum { kRtpCsrcSize = 15 }; // RFC 3550 page 13

// 0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |      defined by profile       |           length              |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                        header extension                       |
// |                             ....                              |

class RtpHeader {
public:
	static const int MIN_SIZE = 12;
	uint32_t cc : 4;
	uint32_t hasextension : 1;
	uint32_t padding : 1;
	uint32_t version : 2;
	uint32_t payloadtype : 7;
	uint32_t marker : 1;
	uint32_t seqnum : 16;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t extensionpayload : 16;
	uint32_t extensionlength : 16;
	/*    RFC 5285
	0                   1                   2                   3
	0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|       0xBE    |    0xDE       |           length=3            |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|  ID   | L=0   |     data      |  ID   |  L=1  |   data...
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	...data   |    0 (pad)    |    0 (pad)    |  ID   | L=3   |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	|                          data                                 |
	+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

	*/
	uint32_t extensions;

	inline RtpHeader() :
		cc(0), hasextension(0), padding(0), version(2), payloadtype(0), marker(
			0), seqnum(0), timestamp(0), ssrc(0), extensionpayload(0), extensionlength(0) {
		// No implementation required
	}

	inline uint8_t hasPadding() const {
		return padding;
	}

	inline void setPadding(uint8_t has_padding) {
		padding = has_padding;
	}

	inline uint8_t getVersion() const {
		return version;
	}
	inline void setVersion(uint8_t aVersion) {
		version = aVersion;
	}
	inline uint8_t getMarker() const {
		return marker;
	}
	inline void setMarker(uint8_t aMarker) {
		marker = aMarker;
	}
	inline uint8_t getExtension() const {
		return hasextension;
	}
	inline void setExtension(uint8_t ext) {
		hasextension = ext;
	}
	inline uint8_t getCc() const {
		return cc;
	}
	inline void setCc(uint8_t theCc) {
		cc = theCc;
	}
	inline uint8_t getPayloadType() const {
		return payloadtype;
	}
	inline void setPayloadType(uint8_t aType) {
		payloadtype = aType;
	}
	inline uint16_t getSeqNumber() const {
		return ntohs(seqnum);
	}
	inline void setSeqNumber(uint16_t aSeqNumber) {
		seqnum = htons(aSeqNumber);
	}
	inline uint32_t getTimestamp() const {
		return ntohl(timestamp);
	}
	inline void setTimestamp(uint32_t aTimestamp) {
		timestamp = htonl(aTimestamp);
	}
	inline uint32_t getSSRC() const {
		return ntohl(ssrc);
	}
	inline void setSSRC(uint32_t aSSRC) {
		ssrc = htonl(aSSRC);
	}
	inline uint16_t getExtId() const {
		return ntohs(extensionpayload);
	}
	inline void setExtId(uint16_t extensionId) {
		extensionpayload = htons(extensionId);
	}
	inline uint16_t getExtLength() const {
		return ntohs(extensionlength);
	}
	inline void setExtLength(uint16_t extensionLength) {
		extensionlength = htons(extensionLength);
	}
	inline int getHeaderLength() const {
		return MIN_SIZE + cc * 4 + hasextension * (4 + ntohs(extensionlength) * 4);
	}
};

struct RTPHeaderExtension {
	int32_t transmissionTimeOffset;
	uint32_t absoluteSendTime;
};

struct RTPHeader {
	bool markerBit;
	uint8_t payloadType;
	uint16_t sequenceNumber;
	uint32_t timestamp;
	uint32_t ssrc;
	uint8_t numCSRCs;
	uint32_t arrOfCSRCs[kRtpCsrcSize];
	uint8_t paddingLength;
	uint16_t headerLength;
	int payload_type_frequency;
	RTPHeaderExtension extension;
};

///////////////////////////////////////////////////////////////////////////////
// class : RTPHeaderParser
///////////////////////////////////////////////////////////////////////////////

class RTPHeaderParser
{
public:
	RTPHeaderParser(const uint8_t* rtpData,
		const uint32_t rtpDataLength);
	~RTPHeaderParser();

	bool Parse(RTPHeader& parsedPacket) const;

private:

	const uint8_t* const _ptrRTPDataBegin;
	const uint8_t* const _ptrRTPDataEnd;
};

///////////////////////////////////////////////////////////////////////////////
// Macros & utilities
///////////////////////////////////////////////////////////////////////////////

bool GetDeviceSpecificAudioCodecParameters(
	BCBuffer &refBuffer,
	uint32_t &sampleRate,
	uint32_t &samplesPerFrame);
bool GetDeviceSpecificAudioData(
	BCBuffer &refBuffer,
	LPVOID &lpData,
	uint32_t &nDataSize);

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_RTPUTILS_H_INCLUDED__

