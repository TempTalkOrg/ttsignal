///////////////////////////////////////////////////////////////////////////////
// file : FLVUtils.cpp
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#include <BC/Utils.h>
#include <BC/BCLog.h>
#include <BC/BCException.h>
#include <RTMP/PacketParser.h>
#include <RTMP/FLVUtils.h>

#ifdef _MSC_VER
#pragma warning(disable:4018)
#endif // _MSC_VER



///////////////////////////////////////////////////////////////////////////////
// Namespace : FLVUtils
///////////////////////////////////////////////////////////////////////////////

namespace FLVUtils
{



// MPEG versions - use [version]
const uint8_t mpeg_versions[4] = { 25, 0, 2, 1 };

// Layers - use [layer]
const uint8_t mpeg_layers[4] = { 0, 3, 2, 1 };

// Bitrates - use [version][layer][bitrate]
const uint16_t mpeg_bitrates[4][4][16] = {
	{ // Version 2.5
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Reserved
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 }, // Layer 3
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 }, // Layer 2
		{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 }  // Layer 1
	},
	{ // Reserved
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Invalid
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Invalid
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Invalid
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }  // Invalid
	},
	{ // Version 2
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Reserved
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 }, // Layer 3
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 }, // Layer 2
		{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 }  // Layer 1
	},
	{ // Version 1
		{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, // Reserved
		{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 }, // Layer 3
		{ 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 }, // Layer 2
		{ 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 }, // Layer 1
	}
};

// Sample rates - use [version][srate]
const uint16_t mpeg_srates[4][4] = {
	{ 11025, 12000, 8000, 0 }, // MPEG 2.5
	{ 0, 0, 0, 0 }, // Reserved
	{ 22050, 24000, 16000, 0 }, // MPEG 2
	{ 44100, 48000, 32000, 0 }  // MPEG 1
};

// Samples per frame - use [version][layer]
const uint16_t mpeg_frame_samples[4][4] = {
	//    Rsvd     3     2     1  < Layer  v Version
	{ 0, 576, 1152, 384 }, //       2.5
	{ 0, 0, 0, 0 }, //       Reserved
	{ 0, 576, 1152, 384 }, //       2
	{ 0, 1152, 1152, 384 }  //       1
};

// Slot size (MPEG unit of measurement) - use [layer]
const uint8_t mpeg_slot_size[4] = { 0, 1, 1, 4 }; // Rsvd, 3, 2, 1


static uint16_t mpg_get_frame_size(char *hdr) {

	// Quick validity check
	if ((((unsigned char)hdr[0] & 0xFF) != 0xFF)
		|| (((unsigned char)hdr[1] & 0xE0) != 0xE0)   // 3 sync bits
		|| (((unsigned char)hdr[1] & 0x18) == 0x08)   // Version rsvd
		|| (((unsigned char)hdr[1] & 0x06) == 0x00)   // Layer rsvd
		|| (((unsigned char)hdr[2] & 0xF0) == 0xF0)   // Bitrate rsvd
		) return 0;

	// Data to be extracted from the header
	uint8_t   ver = (hdr[1] & 0x18) >> 3;   // Version index
	uint8_t   lyr = (hdr[1] & 0x06) >> 1;   // Layer index
	uint8_t   pad = (hdr[2] & 0x02) >> 1;   // Padding? 0/1
	uint8_t   brx = (hdr[2] & 0xf0) >> 4;   // Bitrate index
	uint8_t   srx = (hdr[2] & 0x0c) >> 2;   // SampRate index

	// Lookup real values of these fields
	uint32_t  bitrate = mpeg_bitrates[ver][lyr][brx] * 1000;
	uint32_t  samprate = mpeg_srates[ver][srx];
	uint16_t  samples = mpeg_frame_samples[ver][lyr];
	uint8_t   slot_size = mpeg_slot_size[lyr];

	// In-between calculations
	float     bps = (float)samples / 8.0;
	float     fsize = ((bps * (float)bitrate) / (float)samprate)
		+ ((pad) ? slot_size : 0);

	// Frame sizes are truncated integers
	return (uint16_t)fsize;
}

// AAC silence packet

const uint8_t aac_silence_data[] = { 
	0xaf, 0x01, 0x21, 0x00, 0x49, 0x90, 0x02, 0x19, 0x00, 0x23, 0x80 
};

#define SPEEX_COPY(dst, src, n) \
	(memcpy((dst), (src), (n)*sizeof(*(dst)) + 0*((dst)-(src)) ))

/** Convert little endian */
static inline int32_t le_int(int32_t i)
{
#if !defined(__LITTLE_ENDIAN__) && ( defined(WORDS_BIGENDIAN) || defined(__BIG_ENDIAN__) )
	uint32_t ui, ret;
	ui = i;
	ret = ui >> 24;
	ret |= (ui >> 8) & 0x0000ff00;
	ret |= (ui << 8) & 0x00ff0000;
	ret |= (ui << 24);
	return ret;
#else
	return i;
#endif
}

#define ENDIAN_SWITCH(x) {x=le_int(x);}

///////////////////////////////////////////////////////////////////////////////
// class : FLVUri
///////////////////////////////////////////////////////////////////////////////

FLVUri::FLVUri()
	: m_nRecvLen(0)
	, m_szUri(NULL)
	, m_nArgCount(0)
{
	memzero(m_szUrlBuf, sizeof(m_szUrlBuf));
	memzero(m_szRecvBuf, sizeof(m_szRecvBuf));
	memzero(m_arrayArgs, sizeof(m_arrayArgs));
}

FLVUri::FLVUri(LPCSTR szUrl)
	: m_nRecvLen(0)
	, m_szUri(NULL)
	, m_nArgCount(0)
{
	memzero(m_szUrlBuf, sizeof(m_szUrlBuf));
	memzero(m_szRecvBuf, sizeof(m_szRecvBuf));
	memzero(m_arrayArgs, sizeof(m_arrayArgs));
	Parse(szUrl, strlen(szUrl));
}

FLVUri::~FLVUri()
{
	//
}

BCRESULT FLVUri::Parse(LPCSTR szUrl, uint32_t nLength)
{
	char *szQueryString;

	nLength = (nLength > URI_MAXLEN)?URI_MAXLEN:nLength;
	m_nRecvLen = nLength;
	strncpy(m_szUrlBuf, szUrl, m_nRecvLen);
	strncpy(m_szRecvBuf, szUrl, m_nRecvLen);
	m_szRecvBuf[m_nRecvLen] = 0;
	m_szUri = m_szRecvBuf;

	/*
	 * Now, see if there is a ? mark in the URL.  If so, this is
	 * part of the query string, and we will split it from the URL.
	 */
	szQueryString = strchr(m_szUri, '?');
	if (szQueryString != NULL)
	{
		char *pArg, *pValue;
		*(szQueryString) = 0;
		pArg = szQueryString;
		szQueryString++;
		do
		{
			*pArg = 0;
			pArg++;
			m_arrayArgs[m_nArgCount++].szArg = pArg;		
		}while((pArg = strchr(pArg, '&')) != NULL);
		for (uint32_t i = 0;i < m_nArgCount;i++)
		{
			pValue = (LPSTR)strchr(m_arrayArgs[i].szArg, '=');
			if (pValue)
			{
				*pValue = 0;
				pValue++;
				m_arrayArgs[i].szValue = pValue;
			}
		}		
	}

	return (BC_R_SUCCESS);
}

LPCSTR FLVUri::GetParam(LPCSTR szArg, BOOL bCaseSensitive /* = FALSE */)
{
	ASSERT(szArg);
	if (bCaseSensitive)
	{
		for (uint32_t i = 0;i < m_nArgCount;i++)
		{
			ASSERT(m_arrayArgs[i].szArg);
			if (strcmp(m_arrayArgs[i].szArg, szArg) == 0)
			{
				return m_arrayArgs[i].szValue;
			}
		}
	}
	else
	{
		for (uint32_t i = 0;i < m_nArgCount;i++)
		{
			ASSERT(m_arrayArgs[i].szArg);
			if (strcasecmp(m_arrayArgs[i].szArg, szArg) == 0)
			{
				return m_arrayArgs[i].szValue;
			}
		}
	}
	return NULL;
}

FLVUri &FLVUri::operator = (const FLVUri &other)
{
	Parse(other.m_szUrlBuf, other.m_nRecvLen);

	return *this;
}

///////////////////////////////////////////////////////////////////////////////
// Utilities
///////////////////////////////////////////////////////////////////////////////

#define FLV_UI24(x) (int)(((x[0]) << 16) + ((x[1]) << 8) + (x[2]))

///////////////////////////////////////////////////////////////////////////////
// class : FlvVideoInfo
///////////////////////////////////////////////////////////////////////////////

FLVVideoInfoS FLVInfo::AnalyseVideo(uint8_t t)
{
	FLVVideoInfoS sInfo;
	sInfo.eFrameType = t >> 4;

	if (sInfo.eFrameType != FRAME_KEY)
	{
		sInfo.eFrameType = FRAME_INTER;
	}

	sInfo.eCodecId = t & 0xf;
	return sInfo;
}

FLVAudioInfoS FLVInfo::AnalyseAudio(uint8_t t)
{
	FLVAudioInfoS sInfo;
	sInfo.eCodecId = t >> 4;
	sInfo.nRate = (t>>2) & 0x3;
	sInfo.nSize = (t>>1) & 0x1;
	sInfo.eType = t & 0x1;
	return sInfo;
}

AVCVideoPacketS FLVInfo::AnalyseAVC(uint32_t t)
{
	AVCVideoPacketS sInfo;
	sInfo.eAVCPacketType = t >> 24;
	sInfo.nCompositeTime = 0x00FFFFFF & t;
	return sInfo;
}


FLVUtils::MP3ConfigS FLVInfo::ParseMP3Info(LPVOID lpData, size_t nSize)
{
	MP3ConfigS config;
	if (nSize < 4)
	{
		throw BCException(__FUNCTION__, "Invalid MP3 data");
	}
	uint32_t data = BC_UI32BEI((uint8_t *)lpData);
	config.version = (data >> 19) & 0x03;
	config.layer = (data >> 17) & 0x03;
	LPBYTE hdr = (LPBYTE)lpData;
	// Quick validity check
	if ((((unsigned char)hdr[0] & 0xFF) != 0xFF)
		|| (((unsigned char)hdr[1] & 0xE0) != 0xE0)   // 3 sync bits
		|| (((unsigned char)hdr[1] & 0x18) == 0x08)   // Version rsvd
		|| (((unsigned char)hdr[1] & 0x06) == 0x00)   // Layer rsvd
		|| (((unsigned char)hdr[2] & 0xF0) == 0xF0)   // Bitrate rsvd
		){
		throw BCException(__FUNCTION__, "Invalid MP3 data");
	}

	// Data to be extracted from the header
	uint8_t   ver = (hdr[1] & 0x18) >> 3;   // Version index
	uint8_t   lyr = (hdr[1] & 0x06) >> 1;   // Layer index
	uint8_t   pad = (hdr[2] & 0x02) >> 1;   // Padding? 0/1
	uint8_t   brx = (hdr[2] & 0xf0) >> 4;   // Bitrate index
	uint8_t   srx = (hdr[2] & 0x0c) >> 2;   // SampRate index

	// Lookup real values of these fields
	uint32_t  bitrate = mpeg_bitrates[ver][lyr][brx] * 1000;
	uint32_t  samprate = mpeg_srates[ver][srx];
	uint16_t  samples = mpeg_frame_samples[ver][lyr];
	uint8_t   slot_size = mpeg_slot_size[lyr];

	// In-between calculations
	float     bps = (float)samples / 8.0;
	float     fsize = ((bps * (float)bitrate) / (float)samprate)
		+ ((pad) ? slot_size : 0);

	// Frame sizes are truncated integers
	config.frameSize = (uint16_t)fsize;
	config.sampleRate = samprate;
	config.samples = samples;
	config.slotSize = slot_size;

	return config;
}

FLVUtils::SpeexHeader FLVInfo::ParseSpeexHeader(LPVOID lpData, size_t nSize)
{
	SpeexHeader sHeader, *le_header = &sHeader;
	SpeexHeader *header = (SpeexHeader *)lpData;

	SPEEX_COPY(le_header, header, 1);

	/*Make sure everything is now little-endian*/
	ENDIAN_SWITCH(le_header->speex_version_id);
	ENDIAN_SWITCH(le_header->header_size);
	ENDIAN_SWITCH(le_header->rate);
	ENDIAN_SWITCH(le_header->mode);
	ENDIAN_SWITCH(le_header->mode_bitstream_version);
	ENDIAN_SWITCH(le_header->nb_channels);
	ENDIAN_SWITCH(le_header->bitrate);
	ENDIAN_SWITCH(le_header->frame_size);
	ENDIAN_SWITCH(le_header->vbr);
	ENDIAN_SWITCH(le_header->frames_per_packet);
	ENDIAN_SWITCH(le_header->extra_headers);

	return sHeader;
}

FLVUtils::AACConfigS FLVInfo::ParseAACSeqHeader(LPVOID lpData, size_t nSize)
{
	AACConfigS config;
	if (nSize < 2)
	{
		throw BCException(__FUNCTION__, "Invalid aac GASpecificConfig data");
	}

	uint8_t b0 = ((uint8_t *)lpData)[0];
	uint8_t b1 = ((uint8_t *)lpData)[1];
	const uint32_t sample_rates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
		24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0, };

	/**
	* 5 bit: audioObjectType
	* 4 bit: sample_frequency_index
	* 4 bit: channle_configuration
	* GASpecificConfig
	* 1 bit: framgeLengthFlag, 0 => 1024, 1 => 960
	* 1 bit: dependsOnCoreCoder
	* 1 bit: extensionFlag
	*/
	config.objtype = b0 >> 3;
	if (config.objtype == 0 || config.objtype == 0x1f){
		LogError(_LOCAL_, "unsupported adts objecttype");
		throw BCException(__FUNCTION__, "unsupported adts objecttype");
	}
	if (config.objtype > 4){
		config.objtype = 2;
	}

	config.srindex = (((b0 << 1) & 0x0f) | (b1 & 0x80) >> 7);
	config.chconf = (b1 >> 3) & 0x0f;
	config.sampleRate = sample_rates[config.srindex];

	config.frameLengthFlag = (b1 >> 2) & 0x01;
	config.dependsOnCoreCoder = (b1 >> 1) & 0x01;
	config.extensionFlag = b1 & 0x01;
	config.frameLength = (config.frameLengthFlag) ? 960 : 1024;

	return config;
}

void FLVInfo::ParseH264SPS(
	BCIStream &refStream,
	FLVMeta &flvmetadata)
{
	int avcpackettype;
	int i, length, nSPS;
	unsigned char *avcc;
	KBPool sPool;
	uint8_t *pBuffer;
	uint8_t temp[10];

	int32_t nReadPos = refStream.ConsumedLength();

	refStream.Read(&temp, 4);
	avcpackettype = temp[0];

#ifdef DEBUG
	fprintf(stderr, "[FLV] AVCPacketType = %d\n", avcpackettype);
#endif

	if(avcpackettype == 0) 	// AVCDecoderConfigurationRecord (14496-15, 5.2.4.1.1)
	{
		refStream.Read(temp, 6);
		avcc = temp;

		nSPS = avcc[5] & 0x1f;

#ifdef DEBUG
		fprintf(stderr, "[AVC/H.264] AVCDecoderConfigurationRecord\n");
		fprintf(stderr, "[AVC/H.264] configurationVersion = %d\n", avcc[0]);
		fprintf(stderr, "[AVC/H.264] AVCProfileIndication = %d\n", avcc[1]);
		fprintf(stderr, "[AVC/H.264] profile_compatibility = %d\n", avcc[2]);
		fprintf(stderr, "[AVC/H.264] AVCLevelIndication = %d\n", avcc[3]);
		fprintf(stderr, "[AVC/H.264] lengthSizeMinusOne = %d\n", avcc[4] & 0x3);
		fprintf(stderr, "[AVC/H.264] numOfSequenceParameterSets = %d\n", nSPS);
#endif

		for(i = 0; i < nSPS; i++)
		{
			refStream.Read(temp, 2);
			length = (temp[0] << 8) + temp[1];
			pBuffer = (uint8_t *)sPool.Alloc(length);
			refStream.Read(pBuffer, length);
#ifdef DEBUG
			fprintf(stderr, "[AVC/H.264]\tsequenceParameterSetLength = %d bit\n", 8 * length);
#endif
			_ReadH264NALUnit(pBuffer, length, flvmetadata);
			sPool.Clear();
		}
	}

	refStream.Rewind();
	refStream.Skip(nReadPos);

	return;
}

void FLVInfo::ReadFLVH264VideoPacket(
	BCFIStream &refStream,
	FLVMeta &flvmetadata)
{
	int avcpackettype;
	int i, length, nSPS;
	unsigned char *avcc;
	KBPool sPool;
	uint8_t *pBuffer;
	uint8_t temp[10];

	int32_t nFilePos = refStream.ConsumedLength();

	refStream.Read(&temp, 4);
	avcpackettype = temp[0];

#ifdef AVC_DEBUG
	fprintf(stderr, "[FLV] AVCPacketType = %d\n", avcpackettype);
#endif

	if(avcpackettype == 0) 	// AVCDecoderConfigurationRecord (14496-15, 5.2.4.1.1)
	{
		refStream.Read(temp, 6);
		avcc = temp;

		nSPS = avcc[5] & 0x1f;

#ifdef AVC_DEBUG
		fprintf(stderr, "[AVC/H.264] AVCDecoderConfigurationRecord\n");
		fprintf(stderr, "[AVC/H.264] configurationVersion = %d\n", avcc[0]);
		fprintf(stderr, "[AVC/H.264] AVCProfileIndication = %d\n", avcc[1]);
		fprintf(stderr, "[AVC/H.264] profile_compatibility = %d\n", avcc[2]);
		fprintf(stderr, "[AVC/H.264] AVCLevelIndication = %d\n", avcc[3]);
		fprintf(stderr, "[AVC/H.264] lengthSizeMinusOne = %d\n", avcc[4] & 0x3);
		fprintf(stderr, "[AVC/H.264] numOfSequenceParameterSets = %d\n", nSPS);
#endif

		for(i = 0; i < nSPS; i++)
		{
			refStream.Read(temp, 2);
			length = (temp[0] << 8) + temp[1];
			pBuffer = (uint8_t *)sPool.Alloc(length);
			refStream.Read(pBuffer, length);
#ifdef AVC_DEBUG
			fprintf(stderr, "[AVC/H.264]\tsequenceParameterSetLength = %d bit\n", 8 * length);
#endif
			_ReadH264NALUnit(pBuffer, length, flvmetadata);
			sPool.Clear();
		}
	}

	refStream.Seek(nFilePos, SEEK_SET);

	return;
}

void FLVInfo::_ReadH264NALUnit(
	unsigned char *nalu,
	int length,
	FLVMeta &flvmetadata)
{
	int i, numBytesInRBSP;
	int nal_unit_type;
	bitstream_t bitstream;
	KBPool sPool;

	// See 14496-10, 7.3.1
#ifdef AVC_DEBUG
	fprintf(stderr, "[AVC/H.264]\tNALU Header: %02x\n", nalu[0]);
	fprintf(stderr, "[AVC/H.264]\t\tforbidden_zero_bit = %d\n", (nalu[0] >> 7) & 0x1);
	fprintf(stderr, "[AVC/H.264]\t\tnal_ref_idc = %d\n", (nalu[0] >> 5) & 0x3);
	fprintf(stderr, "[AVC/H.264]\t\tnal_unit_type = %d\n", nalu[0] & 0x1f);
	fprintf(stderr, "[AVC/H.264]\tRBSP: ");
	for(i = 1; i < length; i++)
		fprintf(stderr, "%02x ", nalu[i]);
	fprintf(stderr, "\n");
#endif

	nal_unit_type = nalu[0] & 0x1f;

	// We are only interested in NALUnits of type 7 (sequence parameter set, SPS)
	if(nal_unit_type != 7)
		return;

	bitstream.bytes = (unsigned char *)sPool.Calloc(length - 1);

	numBytesInRBSP = 0;
	for(i = 1; i < length; i++)
	{
		if(i + 2 < length && nalu[i] == 0x00
			&& nalu[i + 1] == 0x00 && nalu[i + 2] == 0x03)
		{
			bitstream.bytes[numBytesInRBSP++] = nalu[i];
			bitstream.bytes[numBytesInRBSP++] = nalu[i + 1];

			i += 2;
		}
		else
		{
			bitstream.bytes[numBytesInRBSP++] = nalu[i];
		}
	}

	bitstream.length = numBytesInRBSP;
	bitstream.byte = 0;
	bitstream.bit = 0;

#ifdef AVC_DEBUG
	fprintf(stderr, "[AVC/H.264]\tSODB: ");
	for(i = 0; i < (int)bitstream.length; i++)
		fprintf(stderr, "%02x ", bitstream.bytes[i]);
	fprintf(stderr, "\n");
#endif

	_ReadH264SPS(&bitstream, flvmetadata);

	return;
}

void FLVInfo::_ReadH264SPS(bitstream_t *bitstream, FLVMeta &flvmetadata)
{
	int i, j;
	unsigned int profile_idc;
	unsigned int chroma_format_idc = 1, separate_color_plane_flag = 0;

	unsigned int pic_width_in_mbs_minus1, pic_height_in_map_units_minus1;
	unsigned int frame_mbs_only_flag;
	unsigned int frame_cropping_flag;
	unsigned int frame_crop_left_offset = 0, frame_crop_right_offset = 0,
		frame_crop_top_offset = 0, frame_crop_bottom_offset = 0;

	unsigned int chromaArrayType;
/*
	We need these values from SPS

	chroma_format_idc
	separate_color_plane_flag
	pic_width_in_mbs_minus1
	pic_height_in_map_units_minus1
	frame_mbs_only_flag
	frame_cropping_flag
	frame_crop_left_offset
	frame_crop_right_offset
	frame_crop_top_offset
	frame_crop_bottom_offset
*/

	profile_idc = _ReadCodedU(bitstream, 8, "profile_idc");
	_ReadCodedU(bitstream, 1, "constraint_set0_flag");
	_ReadCodedU(bitstream, 1, "constraint_set1_flag");
	_ReadCodedU(bitstream, 1, "constraint_set2_flag");
	_ReadCodedU(bitstream, 1, "constraint_set3_flag");
	_ReadCodedU(bitstream, 4, "reserved_zero_4bits");
	_ReadCodedU(bitstream, 8, "level_idc");
	_ReadCodedUE(bitstream, "seq_parameter_set_id");

	if(
		profile_idc == 100 ||
		profile_idc == 110 ||
		profile_idc == 122 ||
		profile_idc == 244 ||
		profile_idc == 44 ||
		profile_idc == 83 ||
		profile_idc == 86)
	{
		chroma_format_idc = _ReadCodedUE(bitstream, "chroma_format_idc");

		if(chroma_format_idc == 3)
			separate_color_plane_flag = _ReadCodedU(bitstream, 1, "separate_color_plane_flag");

		_ReadCodedUE(bitstream, "bit_depth_luma_minus8");
		_ReadCodedUE(bitstream, "bit_depth_chroma_minus8");
		_ReadCodedU(bitstream, 1, "qpprime_y_zero_transform_bypass_flag");

		unsigned int seq_scaling_matrix_present_flag
			= _ReadCodedU(bitstream, 1, "seq_scaling_matrix_present_flag");
		if(seq_scaling_matrix_present_flag == 1)
		{
			int sizeOfScalingList, delta_scale, lastScale, nextScale;

			int seq_scaling_matrix_count = (chroma_format_idc != 3) ? 8 : 12;
			unsigned int seq_scaling_list_present_flag;
			for(i = 0; i < seq_scaling_matrix_count; i++)
			{
				seq_scaling_list_present_flag
					= _ReadCodedU(bitstream, 1, "seq_scaling_list_present_flag");

				if(seq_scaling_list_present_flag == 1)
				{
					sizeOfScalingList = (i < 6) ? 16 : 64;
					lastScale = nextScale = 8;
					for(j = 0; j < sizeOfScalingList; j++)
					{
						if(nextScale != 0)
						{
							delta_scale = _ReadCodedSE(bitstream, "delta_scale");

							nextScale = (lastScale + delta_scale + 256) % 256;
						}

						lastScale = (nextScale == 0) ? lastScale : nextScale;
					}
				}
			}
		}
	}

	_ReadCodedUE(bitstream, "log2_max_frame_num_minus4");
	unsigned int pic_order_cnt_type = _ReadCodedUE(bitstream, "pic_order_cnt_type");

	if(pic_order_cnt_type == 1) 
	{
		_ReadCodedU(bitstream, 1, "delta_pic_order_always_zero_flag");
		_ReadCodedSE(bitstream, "offset_for_non_ref_pic");
		_ReadCodedSE(bitstream, "offset_for_top_to_bottom_field");

		unsigned int num_ref_frames_in_pic_order_cnt_cycle
			= _ReadCodedUE(bitstream, "num_ref_frames_in_pic_order_cnt_cycle");
		for(i = 0; i < (int)num_ref_frames_in_pic_order_cnt_cycle; i++)
			_ReadCodedSE(bitstream, "offset_for_ref_frame");
	}
	else if(pic_order_cnt_type == 0)
		_ReadCodedUE(bitstream, "log2_max_pic_order_cnt_lsb_minus4");

	_ReadCodedUE(bitstream, "max_num_ref_frames");
	_ReadCodedU(bitstream, 1, "gaps_in_frame_num_value_allowed_flag");

	pic_width_in_mbs_minus1 = _ReadCodedUE(bitstream, "pic_width_in_mbs_minus1");
	pic_height_in_map_units_minus1 = _ReadCodedUE(bitstream, "pic_height_in_map_units_minus1");

	frame_mbs_only_flag = _ReadCodedU(bitstream, 1, "frame_mbs_only_flag");
	if(frame_mbs_only_flag == 0)
		_ReadCodedU(bitstream, 1, "mb_adaptive_frame_field_flag");

	_ReadCodedU(bitstream, 1, "direct_8x8_inference_flag");

	frame_cropping_flag = _ReadCodedU(bitstream, 1, "frame_cropping_flag");
	if(frame_cropping_flag == 1) 
	{
		frame_crop_left_offset = _ReadCodedUE(bitstream, "frame_crop_left_offset");
		frame_crop_right_offset = _ReadCodedUE(bitstream, "frame_crop_right_offset");
		frame_crop_top_offset = _ReadCodedUE(bitstream, "frame_crop_top_offset");
		frame_crop_bottom_offset = _ReadCodedUE(bitstream, "frame_crop_bottom_offset");
	}

	_ReadCodedU(bitstream, 1, "vui_parameters_present_flag");

	// and so on ... VUI is not interesting for us. We have everything we need.

	// Now we have enough information to compute the width and height of this video stream

	unsigned int picWidthInMbs = (pic_width_in_mbs_minus1 + 1);
	unsigned int picHeightInMapUnits = (pic_height_in_map_units_minus1 + 1);
	unsigned int frameHeightInMbs = (2 - frame_mbs_only_flag) * picHeightInMapUnits;

	unsigned int width = picWidthInMbs * 16;
	unsigned int height = frameHeightInMbs * 16;

#ifdef AVC_DEBUG
	fprintf(stderr, "[AVC/H.264] width = %u (pre crop)\n", width);
	fprintf(stderr, "[AVC/H.264] height = %u (pre crop)\n", height);
#endif

	// Cropping

	int cropLeft, cropRight;
	int cropTop, cropBottom;

	if(frame_cropping_flag == 1)
	{
		// See 14496-10, Table 6-1
		int subWidthC[4] = {1, 2, 2, 1};
		int subHeightC[4] = {1, 2, 1, 1};

		unsigned int cropUnitX, cropUnitY;

		if(separate_color_plane_flag == 0)
			chromaArrayType = chroma_format_idc;
		else
			chromaArrayType = 0;

		if(chromaArrayType == 0)
		{
			cropUnitX = 1;
			cropUnitY = 2 - frame_mbs_only_flag;
		}
		else
		{
			cropUnitX = subWidthC[chroma_format_idc];
			cropUnitY = subHeightC[chroma_format_idc] * (2 - frame_mbs_only_flag);
		}

		cropLeft = cropUnitX * frame_crop_left_offset;
		cropRight = cropUnitX * frame_crop_right_offset;
		cropTop = cropUnitY * frame_crop_top_offset;
		cropBottom = cropUnitY * frame_crop_bottom_offset;
	}
	else
	{
		cropLeft = 0;
		cropRight = 0;
		cropTop = 0;
		cropBottom = 0;
	}

	width = width - cropLeft - cropRight;
	height = height - cropTop - cropBottom;

#ifdef AVC_DEBUG
	fprintf(stderr, "[AVC/H.264] width = %u\n", width);
	fprintf(stderr, "[AVC/H.264] height = %u\n", height);
#endif

	flvmetadata.width = (uint16_t)width;
	flvmetadata.height = (uint16_t)height;

	return;
}

unsigned int FLVInfo::_ReadCodedU(bitstream_t *bitstream, int nbits, const char *name)
{
	// unsigned integer with n bits
	unsigned int bits = (unsigned int)_ReadBits(bitstream, nbits);

#ifdef AVC_DEBUG
	if(name != NULL)
		fprintf(stderr, "[AVC/H.264]\t\t%s = %u\n", name, bits);
#else // !DEBUG
	UNUSED(name);
#endif

	return bits;
}

unsigned int FLVInfo::_ReadCodedUE(bitstream_t *bitstream, const char *name)
{
	// unsigned integer Exp-Golomb coded (see 14496-10, 9.1)
	int leadingZeroBits = -1;
	int bit;
	unsigned int codeNum = 0;

	for(bit = 0; bit == 0; leadingZeroBits++)
		bit = _ReadBit(bitstream);

	codeNum = ((1 << leadingZeroBits) - 1 + (unsigned int)_ReadBits(bitstream, leadingZeroBits));

#ifdef AVC_DEBUG
	if(name != NULL)
		fprintf(stderr, "[AVC/H.264]\t\t%s = %u\n", name, codeNum);
#else // !DEBUG
	UNUSED(name);
#endif

	return codeNum;
}

int FLVInfo::_ReadCodedSE(bitstream_t *bitstream, const char *name)
{
	// signed integer Exp-Golomb coded (see 14496-10, 9.1 and 9.1.1)
	unsigned int codeNum;
	int codeNumSigned, sign;

	codeNum = _ReadCodedUE(bitstream, NULL);

	sign = (codeNum % 2) + 1;
	codeNumSigned = codeNum >> 1;
	if(sign == 0)
		codeNumSigned++;
	else
		codeNumSigned *= -1;

#ifdef AVC_DEBUG
	if(name != NULL)
		fprintf(stderr, "[AVC/H.264]\t\t%s = %d\n", name, codeNumSigned);
#else // !DEBUG
	UNUSED(name);
#endif

	return codeNumSigned;
}

int FLVInfo::_ReadBits(bitstream_t *bitstream, int nbits)
{
	int i, rv = 0;

	for(i = 0; i < nbits; i++)
	{
		rv = (rv << 1);
		rv += _ReadBit(bitstream);
	}

	return rv;
}

int FLVInfo::_ReadBit(bitstream_t *bitstream)
{
	int bit;

	if(bitstream->byte == bitstream->length)
		return 0;

	bit = (bitstream->bytes[bitstream->byte] >> (7 - bitstream->bit)) & 0x01;

	bitstream->bit++;
	if(bitstream->bit == 8)
	{
		bitstream->byte++;
		bitstream->bit = 0;
	}

	return bit;
}

BCRESULT FLVInfo::ParseFileName(BCPString &strOut, LPCSTR szFlvName)
{
	BCRESULT result;
	BCPString strFlvName(szFlvName);
	FLVUri sUrl;
	int32_t nIndex = -1;

	result = sUrl.Parse(szFlvName, strlen(szFlvName));
	if (result != BC_R_SUCCESS)
	{
		return result;
	}
	strFlvName.MakeLower();
	if(-1 != (nIndex = strFlvName.Find("mp4:")))
	{
		strOut.Format("%s.mp4", sUrl.m_szUri);
	}
	else if (-1 != (nIndex = strFlvName.Find("mp3:")))
	{
		strOut.Format("%s.mp3", sUrl.m_szUri);
	}
	else if (-1 == (nIndex = strFlvName.Find(".flv")))
	{
		strOut.Format("%s.flv", sUrl.m_szUri);
	}
	else
	{
		strOut = szFlvName;
	}
	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : FLVMeta
///////////////////////////////////////////////////////////////////////////////

FLVMeta::FLVMeta()
{
	memzero(this, sizeof(FLVMeta));
}

FLVMeta::~FLVMeta()
{
	//
}

BCRESULT FLVMeta::Read(BCIStream *pStream)
{
	int32_t nRetval = 0;

	nRetval += pStream->ReadUInt64BE(&filesize);
	nRetval += pStream->ReadUInt8(&hasVideo);
	nRetval += pStream->ReadUInt8(&hasAudio);
	nRetval += pStream->ReadUInt32BE(&duration);
	nRetval += pStream->ReadUInt32BE(&videoDuration);
	nRetval += pStream->ReadUInt16BE(&height);
	nRetval += pStream->ReadUInt16BE(&width);
	nRetval += pStream->ReadUInt8(&videocodecid);
	nRetval += pStream->ReadUInt32BE(&videodatarate);
	nRetval += pStream->ReadUInt16BE(&framerate);
	nRetval += pStream->ReadUInt8(&audiocodecid);
	nRetval += pStream->ReadUInt32BE(&audiosamplerate);
	nRetval += pStream->ReadUInt32BE(&audiosamplesize);
	nRetval += pStream->ReadUInt8(&stereo);
	nRetval += pStream->ReadUInt32BE(&videotags);
	nRetval += pStream->ReadUInt32BE(&keyframes);
	nRetval += pStream->ReadUInt32BE(&totaltags);
	nRetval += pStream->ReadUInt8(&hasAudioSeqHeader);
	nRetval += pStream->ReadUInt8(&hasVideoSeqHeader);

	return BC_R_SUCCESS;
}

BCRESULT FLVMeta::Write(BCOStream *pStream)
{
	int32_t nRetval = 0;

	nRetval += pStream->WriteUInt64BE(filesize);
	nRetval += pStream->WriteUInt8(hasVideo);
	nRetval += pStream->WriteUInt8(hasAudio);
	nRetval += pStream->WriteUInt32BE(duration);
	nRetval += pStream->WriteUInt32BE(videoDuration);
	nRetval += pStream->WriteUInt16BE(height);
	nRetval += pStream->WriteUInt16BE(width);
	nRetval += pStream->WriteUInt8(videocodecid);
	nRetval += pStream->WriteUInt32BE(videodatarate);
	nRetval += pStream->WriteUInt16BE(framerate);
	nRetval += pStream->WriteUInt8(audiocodecid);
	nRetval += pStream->WriteUInt32BE(audiosamplerate);
	nRetval += pStream->WriteUInt32BE(audiosamplesize);
	nRetval += pStream->WriteUInt8(stereo);
	nRetval += pStream->WriteUInt32BE(videotags);
	nRetval += pStream->WriteUInt32BE(keyframes);
	nRetval += pStream->WriteUInt32BE(totaltags);
	nRetval += pStream->WriteUInt8(hasAudioSeqHeader);
	nRetval += pStream->WriteUInt8(hasVideoSeqHeader);

	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : FLVUtils
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : FLVUtils

///////////////////////////////////////////////////////////////////////////////
// End of file : FLVUtils.cpp
///////////////////////////////////////////////////////////////////////////////
