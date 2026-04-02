
///////////////////////////////////////////////////////////////////////////////
// File : TSConverter.cpp
///////////////////////////////////////////////////////////////////////////////

#include "Precompile.h"
#include <math.h> // for round()
#include <BC/BCLog.h>
#include <RTMP/AvcC.h>
#include <RTMP/Packet.h>
#include <RTMP/FLVUtils.h>
#include <RTMP/PacketQueue.h>
#include <RTMP/IHandler.h>
#include <RTMP/TSGenerator.h>

#ifdef _MSC_VER
#pragma warning(disable:4146 4311 4018)
#endif // _MSC_VER



///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

#define HLS_DELAY		63000
#define TS_LENGTH		188

#define NGX_RTMP_HLS_SLICING_PLAIN      1
#define NGX_RTMP_HLS_SLICING_ALIGNED    2


#define NGX_RTMP_HLS_NAMING_SEQUENTIAL  1
#define NGX_RTMP_HLS_NAMING_TIMESTAMP   2
#define NGX_RTMP_HLS_NAMING_SYSTEM      3

#define NGX_RTMP_HLS_BUFSIZE            (1024*1024)

#define ngx_movemem(dst, src, n)	(((u_char *)memmove(dst, src, n)) + (n))


static uint32_t WriteBuffer(BCOStream &refWriter, BCBuffer *pBuffer)
{
	BCBIStream sReader(pBuffer);
	size_t nSize = refWriter.WriteFrom(sReader, sReader.RemainingLength());
	sReader.Rewind();
	return nSize;
}

/**
*  Audio
*  frame.pid = 0x101;
*  frame.sid = 0xc0;
*
*  Video
*  frame.pid = 0x100
*  frame.sid = 0xe0;
**/
#define VIDEO_PID  0x100
#define AUDIO_PID  0x101

#define VIDEO_SID  0xe0
#define AUDIO_SID  0xc0


typedef struct mpegts_frame_t{
	uint64_t    pts;
	uint64_t    dts;
	uint32_t    pid;
	uint32_t    sid;
	uint32_t    cc;
	unsigned    key : 1;
	mpegts_frame_t() : pts(0), dts(0), pid(0), sid(0), cc(0), key(0){}
}mpegts_frame_t;

static uint8_t mpegts_pat_header[] = {
    /** Ts **/
    /** sync_byte = 0x47, pid = 0x00,  count = 0;
     *
     * 1 byte:  sync_byte, 0x47
     *
     *----- 2 byte ------
     * 1 bit :  transport_error_indicator
     * 1 bit :  payload_unit_start_indicator, true for PSI, or PES's first packet
     * 1 bit : transport_priority
     * 13 bit:  pid aka, packet Identifier, 0 for PAT table
     *
     * 2 bit : transport_scrambling_control
     * 2 bit : adaptation_field_control, 
     *      1 bit: adaptation_field
     *      1 bit: payload_field
     * 4 bit : counter
     *
     * if(adaptation_field_control == '10' || adaptation_field_control == '11'){
     *      adaptation_field();
     *
     *      1 byte: adaptation_field_length 
     *
     *      1 bit:  discontinuity_indicator:
     *      1 bit:  random_access_indicator
     *      1 bit:  delementary_stream_priority_indicator
     *      1 bit:  pcr_flag
     *      1 bit:  opcr_flag
     *      1 bit:  splicing_point_flag 
     *      1 bit:  transport_private_data_flag
     *      1 bit:  adaptation_field_extension_flag
     * }
     * if(adaptation_field_control == '01' || adaptation_field_control == '11'){
     *  for(i=0; i < n; i++){
     *      data_byte;
     *  }
     * }
     *
     *
     * pointer_field: 0x00 in first line
     *
     * 4 byte: crc32
     */
    0x47, 0x40, 0x00, 0x10, 0x00,
    /** PSI **/
    /** PAT struct
     * 1 byte: table_id, 0 => PAT
     * ------2 byte-------
     * 1 bit:  section_syntax_indicator, must be 1
     * 1 bit:  zero
     * 2 bit:  reserverd_1, must be 11
     * 12 bit: section_length, 0x0d => 13 byte
     *
     * 2 byte: transport_stream_id, 0x00, 0x01
     *
     * 2 bit:  reserverd2
     * 5 bit:  version_number
     * 1 bit:  current_next_indicator
     *
     * 1 byte: section_number
     * 1 byte: last_section_number
     * for(i=0; i < n; i++){
     *     2 byte: program_number
     *     3 bit:  reserverd3
     *     if(program_number == '0'){
     *          13 bit: network_pid
     *     }else {
     *          13 bit: program_map_PID
     *     }
     * }
     * 4 byte: CRC32
     */
    0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00,
    /** PAT **/
    0x00, 0x01, 0xf0, 0x01,
    /** CRC **/
    0x2e, 0x70, 0x19, 0x05,
    /** stuffing 167 bytes **/
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};


/** PMT AAC Struct **/
static uint8_t  mpegts_pmt_aac_header[] = {
    /** Ts **/
    0x47, 0x50, 0x01, 0x10, 0x00,
    /* PSI */
    /**
     * 1 byte:  table_id, 0x02 => PMT
     *
     * 1 bit :  section_syntax_indicator, must be 1
     * 1 bit :  zero 
     * 2 bit :  reserverd_1, must be 11
     * 12 bit:  section_length, 0x17 => 23
     *
     * 16 bit:  program_number:
     *
     * 2 bit:   reserverd2
     * 5 bit:   version_number
     * 1 bit:   current_next_indicator
     *
     * 8 bit:   section_number
     * 8 bit:   last_section_number
     *
     * 3 bit:   reserverd 
     * 13 bit:  pcr_pid
     *
     * 4 bit:   reserverd
     * 12bit:   program_info_length:
     * for(i=0; i < n; i++){
     *  descriptor();
     * }
     *
     * for(i=0; i < n1; i++){
     *   8 bit:  stream_type
     *
     *   3 bit:  reserved
     *   13 bit: elementary_pid
     *   
     *   4 bit:  reserved
     *   12 bit: es_info_length
     *   for(i=0; i < n2;i++){
     *      descriptor();
     *   }
     * }
     *
     */
    0x02,0xb0, 0x17, 0x00, 0x01, 0xc1, 0x00, 0x00,
    /** PMT **/
    0xe1, 0x00, // pcr_pid
    0xf0, 0x00, // program_info_length
    0x1b, 0xe1, 0x00, 0xf0, 0x00, /** h264 **/
    0x0f, 0xe1, 0x01, 0xf0, 0x00, /** aac **/
    /** 0x03, 0xe1, 0x101, 0xf0, 0x00, */ /** mp3 **/
    /** crc 32 **/
    0x2f, 0x44, 0xb9, 0x9b,
    /** stuffing 157 bytes */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

    /** PMT mp3 Struct **/
static uint8_t mpegts_pmt_mp3_header[] = {
    /** Ts **/
    0x47, 0x50, 0x01, 0x10, 0x00,
    /* PSI */
    /**
     * 1 byte:  table_id, 0x02 => PMT
     *
     * 1 bit :  section_syntax_indicator, must be 1
     * 1 bit :  zero 
     * 2 bit :  reserverd_1, must be 11
     * 12 bit:  section_length, 0x17 => 23
     *
     * 16 bit:  program_number:
     *
     * 2 bit:   reserverd2
     * 5 bit:   version_number
     * 1 bit:   current_next_indicator
     *
     * 8 bit:   section_number
     * 8 bit:   last_section_number
     *
     * 3 bit:   reserverd 
     * 13 bit:  pcr_pid
     *
     * 4 bit:   reserverd
     * 12bit:   program_info_length:
     * for(i=0; i < n; i++){
     *  descriptor();
     * }
     *
     * for(i=0; i < n1; i++){
     *   8 bit:  stream_type:
     *
     *   3 bit:  reserved
     *   13 bit: elementary_pid
     *   
     *   4 bit:  reserved
     *   12 bit: es_info_length
     *   for(i=0; i < n2;i++){
     *      descriptor();
     *   }
     * }
     *
     */
    0x02, 0xb0, 0x17, 0x00, 0x01, 0xc1, 0x00, 0x00,
    /** PMT **/
    0xe1, 0x00,
    0xf0, 0x00,
    0x1b, 0xe1, 0x00, 0xf0, 0x00, /** h264 **/
    //0x0f, 0xe1, 0x01, 0xf0, 0x00, /** aac **/
    0x03, 0xe1, 0x01, 0xf0, 0x00,  /** mp3 **/
    /** 
     * crc 32 for aac
     * 0x2f, 0x44, 0xb9, 0x9b 
     **/
    0x4e, 0x59, 0x3d, 0x1e,
    //0x4e, 0x59, 0x3d, 0x1e,
    /** stuffing 157 bytes */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};


/** PMT mp3 mpeg version 2 Struct **/
static uint8_t  mpegts_pmt_mp3_mpeg2_header[] = {
    /** Ts **/
    0x47, 0x50, 0x01, 0x10, 0x00,
    /* PSI */
    /**
     * 1 byte:  table_id, 0x02 => PMT
     *
     * 1 bit :  section_syntax_indicator, must be 1
     * 1 bit :  zero 
     * 2 bit :  reserverd_1, must be 11
     * 12 bit:  section_length, 0x17 => 23
     *
     * 16 bit:  program_number:
     *
     * 2 bit:   reserverd2
     * 5 bit:   version_number
     * 1 bit:   current_next_indicator
     *
     * 8 bit:   section_number
     * 8 bit:   last_section_number
     *
     * 3 bit:   reserverd 
     * 13 bit:  pcr_pid
     *
     * 4 bit:   reserverd
     * 12bit:   program_info_length:
     * for(i=0; i < n; i++){
     *  descriptor();
     * }
     *
     * for(i=0; i < n1; i++){
     *   8 bit:  stream_type:
     *
     *   3 bit:  reserved
     *   13 bit: elementary_pid
     *   
     *   4 bit:  reserved
     *   12 bit: es_info_length
     *   for(i=0; i < n2;i++){
     *      descriptor();
     *   }
     * }
     *
     */
    0x02, 0xb0, 0x17, 0x00, 0x01, 0xc1, 0x00, 0x00,
    /** PMT **/
    0xe1, 0x00,
    0xf0, 0x00,
    0x1b, 0xe1, 0x00, 0xf0, 0x00, /** h264 **/
    //0x0f, 0xe1, 0x01, 0xf0, 0x00, /** aac **/
    0x04, 0xe1, 0x01, 0xf0, 0x00,  /** mp3 mpeg2**/
    /* crc 32 **/
    0xb5, 0xba, 0x16, 0x0a,
    /** stuffing 157 bytes */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static u_char *
mpegts_write_pcr(u_char *p, uint64_t pcr)
{
	*p++ = (u_char)(pcr >> 25);
	*p++ = (u_char)(pcr >> 17);
	*p++ = (u_char)(pcr >> 9);
	*p++ = (u_char)(pcr >> 1);
	*p++ = (u_char)(pcr << 7 | 0x7e);
	*p++ = 0;

	return p;
}

static u_char *
mpegts_write_pts(u_char *p, uint32_t fb, uint64_t pts)
{
	uint32_t val;

	val = fb << 4 | (((pts >> 30) & 0x07) << 1) | 1;
	*p++ = (u_char)val;

	val = (((pts >> 15) & 0x7fff) << 1) | 1;
	*p++ = (u_char)(val >> 8);
	*p++ = (u_char)val;

	val = (((pts)& 0x7fff) << 1) | 1;
	*p++ = (u_char)(val >> 8);
	*p++ = (u_char)val;

	return p;
}

///////////////////////////////////////////////////////////////////////////////
// Macro definitions : 
///////////////////////////////////////////////////////////////////////////////

#define PCR_TIME_BASE 27000000

/* The section length is 12 bits. The first 2 are set to 0, the remaining
 * 10 bits should not exceed 1021. */
#define SECTION_LENGTH 1020


///////////////////////////////////////////////////////////////////////////////
// class : HLSConfig
///////////////////////////////////////////////////////////////////////////////

HLSConfig::HLSConfig()
	: fragLen(5000)
	, maxFragLen(fragLen*10)
	, sync(2)
	, muxDelay(700)
	, audioBufferSize(NGX_RTMP_HLS_BUFSIZE)
	, maxAudioDelay(300)
	, slicing(NGX_RTMP_HLS_SLICING_PLAIN)
	, winfrags(0)
	, naming(NGX_RTMP_HLS_NAMING_SEQUENTIAL)
	, granularity(0)
	, playLen(30000)
{
}

HLSConfig::HLSConfig(const HLSConfig &other)
{
	fragLen = other.fragLen;
	maxFragLen = other.maxFragLen;
	sync = other.sync;
	muxDelay = other.muxDelay;
	audioBufferSize = other.audioBufferSize;
	maxAudioDelay = other.maxAudioDelay;
	slicing = other.slicing;
	winfrags = other.winfrags;
	naming = other.naming;
	granularity = other.granularity;
	playLen = other.playLen;
}

HLSConfig::~HLSConfig()
{
}

HLSConfig & HLSConfig::operator=(const HLSConfig &other)
{
	fragLen = other.fragLen;
	maxFragLen = other.maxFragLen;
	sync = other.sync;
	muxDelay = other.muxDelay;
	audioBufferSize = other.audioBufferSize;
	maxAudioDelay = other.maxAudioDelay;
	slicing = other.slicing;
	winfrags = other.winfrags;
	naming = other.naming;
	granularity = other.granularity;
	playLen = other.playLen;
	return *this;
}

BCRESULT HLSConfig::Init(BCFObject *pConfig)
{
	BCFVar *pVar = pConfig->Get("fragLen");
	if (IS_BCF_INT(pVar))
	{
		fragLen = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("maxFragLen");
	if (IS_BCF_INT(pVar))
	{
		maxFragLen = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("sync");
	if (IS_BCF_INT(pVar))
	{
		sync = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("muxDelay");
	if (IS_BCF_INT(pVar))
	{
		muxDelay = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("audioBufferSize");
	if (IS_BCF_INT(pVar))
	{
		audioBufferSize = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("maxAudioDelay");
	if (IS_BCF_INT(pVar))
	{
		maxAudioDelay = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("slicing");
	if (IS_BCF_INT(pVar))
	{
		slicing = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("naming");
	if (IS_BCF_INT(pVar))
	{
		naming = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("granularity");
	if (IS_BCF_INT(pVar))
	{
		granularity = GET_BCF_INT(pVar);
	}
	pVar = pConfig->Get("playLen");
	if (IS_BCF_INT(pVar))
	{
		playLen = GET_BCF_INT(pVar);
	}
	winfrags = playLen / fragLen;
	return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : TSSplitter
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(TSSplitter, 8);

TSSplitter::TSSplitter()
	: m_nId(0)
	, m_pHandler(NULL)
	, m_nPipeId(0)
	, m_bAudioBaseSet(false)
	, m_nAudioBaseTime(0)
	, m_bVideoBaseSet(false)
	, m_nVideoBaseTime(0)
	, m_nSegCount(0)
	, audio_pts(0)
	, m_nAVFlags(0)
	, m_pAFrames(NULL)
	, m_nVideoCC(15)
	, m_bAVCSeqHdrSent(false)
	, m_bWorking(false)
	, m_eAudioFormat(0)
	, m_tmAudFramePTS(0)
	, m_nAudioCC(0)
	, m_nAudFrameCount(0)
	, m_nSampleRate(0)
	, m_tmAudFrameBase(0)
	, m_nSamplesPerFrame(0)
	, frag(0)
	, frag_ts(0)
	, nfrags(0)
	, frags(NULL)
	, opened(false)
	, avc_nal_bytes(0)
{

}


TSSplitter::~TSSplitter()
{
	BC_SAFE_DELETE_PTR(m_pAFrames);
}

BCRESULT TSSplitter::Create(
	BCFObject *pConfig,
	uint32_t nId, 
	PublishHandler *pHandler,
	uint32_t nPipeId)
{
	m_sConfig.Init(pConfig);
	frags = (TSFragConfig *)m_sPool.Calloc(sizeof(TSFragConfig)*(m_sConfig.winfrags*2 + 1));
	m_nId = nId;
	m_pHandler = pHandler;
	m_nPipeId = nPipeId;
	return BC_R_SUCCESS;
}

void TSSplitter::OnMP3Audio(LPVOID lpData, uint32_t nSize)
{
	m_eAudioFormat = FLV_MP3_AUDIO;
	m_sMP3Config = FLVInfo::ParseMP3Info((uint8_t *)lpData + 1, nSize - 1);
	m_nSampleRate = m_sMP3Config.sampleRate;
	m_nSamplesPerFrame = m_sMP3Config.samples;
	m_nAVFlags |= AVINFO_HAS_AUDIO;
}

void TSSplitter::OnAACSeqHdr(LPVOID lpData, uint32_t nSize)
{
	const uint8_t AnnexbStartCode[] = { 0x00, 0x00, 0x00, 0x01 };
	m_sAACConfig = FLVInfo::ParseAACSeqHeader((uint8_t *)lpData + 2, nSize - 2);
	m_eAudioFormat = FLV_AAC_AUDIO;
	m_nSampleRate = m_sAACConfig.sampleRate;//sampleRate[sInfo.nRate];
	m_nSamplesPerFrame = m_sAACConfig.frameLength;
	m_sAACSeqHeader.Append(AnnexbStartCode, sizeof(AnnexbStartCode));
	m_sAACSeqHeader.Add(0x09);
	m_sAACSeqHeader.Add(0xf0);
	m_nAVFlags |= AVINFO_HAS_AUDIO;
}

void TSSplitter::OnAVCSeqHdr(LPVOID lpData, uint32_t nSize)
{
	_ParseSpsPps(lpData, nSize, m_sAVCSeqHeader);
	avc_nal_bytes = (((uint8_t *)lpData)[9] & 0x03) + 1;
	m_nAVFlags |= AVINFO_HAS_VIDEO;
}

void TSSplitter::OnAudioStart(uint32_t nAVFlags)
{
	if (/*true || */((nAVFlags & AVINFO_HAS_VIDEO) && (m_nAVFlags & AVINFO_HAS_AUDIO)) || !(nAVFlags & AVINFO_HAS_VIDEO))
	{
		m_bWorking = true;
		//_OpenFrag(0, 0);
	}
}

void TSSplitter::OnVideoStart(uint32_t nAVFlags)
{
	if (((nAVFlags & AVINFO_HAS_AUDIO) && (m_nAVFlags & AVINFO_HAS_VIDEO)) || !(nAVFlags & AVINFO_HAS_AUDIO))
	{
		m_bWorking = true;
		//_OpenFrag(0, 0);
	}
}

void TSSplitter::OnAudioPacket(PPacket *pPacket)
{
	ScopedPointer<PPacket> dtor(pPacket);
	PHeader &refHeader = *pPacket->m_pHeader;
	BCBIStream sReader(&pPacket->m_sBody);
	uint8_t nByte;
	uint64_t                        pts, est_pts;
	int64_t                         dpts;
	bool		boundary;
	uint32_t	size;

	if (!m_pAFrames)
	{
		m_pAFrames = new BCBuffer();
	}
	BCBOStream sWriter(m_pAFrames);

	if (!m_bAudioBaseSet)
	{
		m_nAudioBaseTime = refHeader.m_nTotalTime - refHeader.m_nTimestamp;
		m_bAudioBaseSet = true;
	}
	pts = (uint64_t)(refHeader.m_nTotalTime - m_nAudioBaseTime) * 90;

	sReader.Rewind();
	sReader.ReadUInt8(&nByte);
	FLVAudioInfoS sInfo = FLVInfo::AnalyseAudio(nByte);
	switch (sInfo.eCodecId)
	{
	case FLV_MP3_AUDIO:
	{
		size = sReader.RemainingLength();
		boundary = m_sAVCSeqHeader.GetSize() == 0;

		/*
		 * start new fragment here if
		 * there's no video at all, otherwise
		 * do it in video handler
		 */
		_UpdateFragment(pts, boundary, 2);
		if (m_pAFrames->UsedLength() + size > m_sConfig.audioBufferSize)
		{
			_FlushAudio();
		}
		/* copy payload */
		uint32_t nWroteSize = sWriter.WriteFrom(sReader, sReader.RemainingLength());
		/* we have 5 free bytes + 2 bytes of RTMP frame header */
		if (sWriter.UsedLength() != nWroteSize) {
			m_nAudFrameCount++;
			goto quit;
		}

		m_tmAudFramePTS = pts;

		if (!m_sConfig.sync || m_nSampleRate == 0) {
			goto quit;
		}

		/* align audio frames */
		est_pts = m_tmAudFrameBase + m_nAudFrameCount * 90000 * m_nSamplesPerFrame / m_nSampleRate;
		dpts = (int64_t)(est_pts - pts);

		if (dpts <= (int64_t)m_sConfig.sync * 90 &&
			dpts >= (int64_t)m_sConfig.sync * -90)
		{
			m_nAudFrameCount++;
			m_tmAudFramePTS = est_pts;
			goto quit;
		}

		m_tmAudFrameBase = pts;
		m_nAudFrameCount = 1;
	}
	break;
	case FLV_AAC_AUDIO:
	{
		uint32_t nWroteSize = 0;
		uint32_t size;

		size = refHeader.m_nDataSize - 2 + 7;
		boundary = m_sAVCSeqHeader.GetSize() == 0;

		/*
		* start new fragment here if
		* there's no video at all, otherwise
		* do it in video handler
		*/
		_UpdateFragment(pts, boundary, 2);

		if (m_pAFrames->UsedLength() + size > m_sConfig.audioBufferSize)
		{
			_FlushAudio();
		}
		sReader.Skip(1);
		// Write ADTS header
		nWroteSize += sWriter.WriteUInt8(0xff);
		nWroteSize += sWriter.WriteUInt8(0xf1);
		nWroteSize += sWriter.WriteUInt8((u_char)(((m_sAACConfig.objtype - 1) << 6) | (m_sAACConfig.srindex << 2) |			
						((m_sAACConfig.chconf & 0x04) >> 2)));
		nWroteSize += sWriter.WriteUInt8((u_char)(((m_sAACConfig.chconf & 0x03) << 6) | ((size >> 11) & 0x03)));
		nWroteSize += sWriter.WriteUInt8((u_char)(size >> 3));
		nWroteSize += sWriter.WriteUInt8((u_char)((size << 5) | 0x1f));
		nWroteSize += sWriter.WriteUInt8(0xfc);
		/* copy payload */
		nWroteSize += sWriter.WriteFrom(sReader, sReader.RemainingLength());
		
		/* we have 5 free bytes + 2 bytes of RTMP frame header */
		if (sWriter.UsedLength() != nWroteSize) {
			m_nAudFrameCount++;
			goto quit;
		}

		m_tmAudFramePTS = pts;

		if (!m_sConfig.sync || m_nSampleRate == 0) {
			goto quit;
		}

		/* align audio frames */

		/* TODO: We assume here AAC frame size is 1024
		*       Need to handle AAC frames with frame size of 960 */

		est_pts = m_tmAudFrameBase + m_nAudFrameCount * 90000 * m_nSamplesPerFrame / m_nSampleRate;
		dpts = (int64_t)(est_pts - pts);

		if (dpts <= (int64_t)m_sConfig.sync * 90 &&	dpts >= (int64_t)m_sConfig.sync * -90)
		{
			m_nAudFrameCount++;
			m_tmAudFramePTS = est_pts;
			goto quit;
		}

		m_tmAudFrameBase = pts;
		m_nAudFrameCount = 1;
	}
	break;
	default:
		LogError(_LOCAL_, "Unsupported audio format");
		break;
	}
quit:
	sReader.Rewind();
}

void TSSplitter::OnVideoPacket(PPacket *pPacket)
{
	ScopedPointer<PPacket> dtor(pPacket);
	PHeader &refHeader = *pPacket->m_pHeader;
	FLVVideoInfoS sInfo;
	BCBIStream sReader(&pPacket->m_sBody);
	uint8_t nByte;

	sReader.ReadUInt8(&nByte);
	sInfo = FLVInfo::AnalyseVideo(nByte);
	if (sInfo.eCodecId == FLV_H264VIDEOPACKET) // Only convert AVC video data
	{
		uint32_t cts;
		uint64_t dts;

		if (!m_bVideoBaseSet)
		{
			m_nVideoBaseTime = refHeader.m_nTotalTime - refHeader.m_nTimestamp;
			m_bVideoBaseSet = true;
		}
		dts = (refHeader.m_nTotalTime - m_nVideoBaseTime) * 90;
		sReader.ReadUInt8(&nByte);
		if (nByte == 1)
		{
			/**
			* 3 bytes : decoder delay
			**/
			sReader.ReadUInt24BE(&cts);

			BCBuffer results ;
			BCBOStream sAnnexbWriter(&results);
			bool first = true, boundary;
			int32_t sps_pps_sent = 0, aud_sent = 0;
			uint8_t src_nal_type, nal_type;
			mpegts_frame_t frame;

			while (sReader.RemainingLength() > 0){
				uint32_t len = 0;

				// Read nalu length
				switch (avc_nal_bytes)
				{
				case 1:
					sReader.ReadUInt8(&nByte);
					len = nByte;
					break;
				case 2:
				{
					uint16_t nShort;
					sReader.ReadUInt16BE(&nShort);
					len = nShort;
					break;
				}
				case 3:
				{
					uint32_t dwRead;
					sReader.ReadUInt24BE(&dwRead);
					len = dwRead;
					break;
				}
				case 4:
				{
					uint32_t dwRead;
					sReader.ReadUInt32BE(&dwRead);
					len = dwRead;
					break;
				}
				default:
					goto quit;
				}

				if (len >  sReader.RemainingLength()){
					LogError(_LOCAL_, "Bad Nalu length[buffer size : %d; nal length : %d]", 
						sReader.RemainingLength(), len);
					goto quit;
				}

				if (len == 0){
					continue;
				}

				
				sReader.ReadUInt8(&src_nal_type);
				nal_type = src_nal_type & 0x1f;

				if (nal_type >= 7 && nal_type <= 9){
					//skip sps, pps , aud
					sReader.Skip(len - 1);
					continue;
				}

				if (!aud_sent/* && m_eAudioFormat == FLV_AAC_AUDIO*/) {
					switch (nal_type) {
					case 1:
					case 5:
					case 6:
					{
	#if 0
						sAnnexbWriter.Write(m_sAACSeqHeader.GetData(), m_sAACSeqHeader.GetSize());
	#else
						u_char aacSeqHeader[] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xf0 };
						sAnnexbWriter.Write(aacSeqHeader, sizeof(aacSeqHeader));
	#endif
					}
					case 9:
						aud_sent = 1;
						break;
					}
				}

				switch (nal_type) {
				case 1:
					sps_pps_sent = 0;
					break;
				case 5:
					if (sps_pps_sent) {
						break;
					}
					sAnnexbWriter.Write(m_sAVCSeqHeader.GetData(), m_sAVCSeqHeader.GetSize());
					sps_pps_sent = 1;
					break;
				}

				/** AnnexB prefix **/
				if (first){
					sAnnexbWriter.WriteUInt32BE(1);
					first = false;
				}
				else {
					sAnnexbWriter.WriteUInt24BE(1);
				}
				sAnnexbWriter.WriteUInt8(src_nal_type);
				sAnnexbWriter.WriteFrom(sReader, len - 1);
			}

			frame.dts = dts;
			frame.pts = frame.dts + cts * 90;
			frame.cc = m_nVideoCC;
			frame.pid = VIDEO_PID;
			frame.sid = VIDEO_SID;
			frame.key = refHeader.IsVKFrame();

			/*
			* start new fragment if
			* - we have video key frame AND
			* - we have audio buffered or have no audio at all or stream is closed
			*/

			boundary = frame.key && (!(m_nAVFlags & AVINFO_HAS_AUDIO) || !this->opened ||
				(m_pAFrames && m_pAFrames->RemainingLength() > 0));
			_UpdateFragment(dts, boundary, 1);
			if (!this->opened)
			{
				goto quit;
			}

			_WriteMpegtsFrame(&frame, results);
			m_nVideoCC = frame.cc;
		}
	}
quit:
	sReader.Rewind();
}

void TSSplitter::Close()
{
	if (m_pHandler && this->opened)
	{
		BCFObject *pFragInfo = new BCFObject();
		if (pFragInfo)
		{
			pFragInfo->PutBool("end", true);
			m_pHandler->OnTSClose(pFragInfo, m_nId, m_nPipeId);
		}
	}
}

void TSSplitter::Stop()
{

}

void TSSplitter::_UpdateFragment(uint64_t ts, bool boundary, uint32_t flush_rate)
{
	uint64_t                  ts_frag_len;
	int32_t                   same_frag, force, discont;
	BCBuffer                * b;
	int64_t                   d;
	TSFragConfig			* f;

	force = 0;
	discont = 1;
	f = NULL;

	if (this->opened) {
		f = _GetFrag(this->nfrags);
		d = (int64_t)(ts - this->frag_ts);

		if (d > (int64_t)m_sConfig.maxFragLen * 90 || d < -90000) {
			force = 1;
		}
		else {
			f->duration = d / 90000.;
			discont = 0;
		}
	}

	switch (m_sConfig.slicing) {
	case NGX_RTMP_HLS_SLICING_PLAIN:
		if (f && f->duration < m_sConfig.fragLen / 1000.) {
			boundary = 0;
		}
		break;

	case NGX_RTMP_HLS_SLICING_ALIGNED:

		ts_frag_len = m_sConfig.fragLen * 90;
		same_frag = this->frag_ts / ts_frag_len == ts / ts_frag_len;

		if (f && same_frag) {
			boundary = 0;
		}

		if (f == NULL && (this->frag_ts == 0 || same_frag)) {
			this->frag_ts = ts;
			boundary = 0;
		}

		break;
	}

	if (boundary || force) {
		_CloseFrag(f);
		_OpenFrag(ts, discont);
	}

	b = m_pAFrames;
	if (this->opened && b && b->RemainingLength() > 0 &&
		this->m_tmAudFramePTS + m_sConfig.maxAudioDelay * 90 / flush_rate < ts)
	{
		_FlushAudio();
	}
}

int32_t TSSplitter::_InitFragment(TSFragConfig *pFrag)
{
	BCBuffer *pBuffer = new BCBuffer();
	pBuffer->Write(mpegts_pat_header, sizeof(mpegts_pat_header));
	if (m_eAudioFormat == FLV_MP3_AUDIO){
		if (m_sMP3Config.version == 0x03){ // 0x03 MPEG-1
			pBuffer->Write(mpegts_pmt_mp3_header, sizeof(mpegts_pmt_mp3_header));
		}
		else { // 0x02 MPEG-2
			pBuffer->Write(mpegts_pmt_mp3_mpeg2_header, sizeof(mpegts_pmt_mp3_mpeg2_header));
		}
	}
	else /*if (m_eAudioFormat == FLV_AAC_AUDIO)*/{
		pBuffer->Write(mpegts_pmt_aac_header, sizeof(mpegts_pmt_aac_header));
	}
	if (m_pHandler)
	{
		// notify TSFileWriter to open a new ts file
		BCFObject *pFragInfo = new BCFObject();
		if (pFrag)
		{
			pFragInfo->PutDouble("id", pFrag->id);
			pFragInfo->PutDouble("duration", round(pFrag->duration * 1000));
			pFragInfo->PutDouble("discont", pFrag->discont);
		}
		m_pHandler->OnTSOpen(pFragInfo, m_nId, m_nPipeId);
		pFragInfo = new BCFObject();
		if (pFrag)
		{
			pFragInfo->PutDouble("id", pFrag->id);
			pFragInfo->PutDouble("duration", round(pFrag->duration * 1000));
			pFragInfo->PutDouble("discont", pFrag->discont);
		}
		//WriteBuffer(m_sRecorder, pBuffer);
		m_pHandler->OnTSFragment(pFragInfo, pBuffer, m_nId, m_nPipeId);
	}
	else
	{
		BC_SAFE_DELETE_PTR(pBuffer);
	}
	return 0;
}

int32_t TSSplitter::_FlushAudio()
{
	if (!this->opened || !m_pAFrames || !m_pAFrames->UsedLength())
	{
		return 0;
	}
	mpegts_frame_t frame;

	frame.dts = this->m_tmAudFramePTS;
	frame.pts = frame.dts;
	frame.cc = this->m_nAudioCC;
	frame.pid = AUDIO_PID;
	frame.sid = AUDIO_SID;
	frame.key = false;

	_WriteMpegtsFrame(&frame, *m_pAFrames);

	this->m_nAudioCC = frame.cc;
	m_pAFrames->Reset();

	return 0;
}

TSFragConfig *TSSplitter::_GetFrag(uint32_t n)
{
	return &this->frags[(this->frag + n) % (m_sConfig.winfrags * 2 + 1)];
}

void TSSplitter::_NextFrag()
{
	if (this->nfrags == m_sConfig.winfrags) {
		this->frag++;
	}
	else {
		this->nfrags++;
	}
}

int32_t TSSplitter::_CloseFrag(TSFragConfig *pFragConf)
{
	if (m_pHandler)
	{
		BCFObject *pFragInfo = new BCFObject();
		if (pFragConf && pFragInfo)
		{
			pFragInfo->PutDouble("id", pFragConf->id);
			pFragInfo->PutDouble("duration", round(pFragConf->duration * 1000));
			pFragInfo->PutDouble("discont", pFragConf->discont);
			m_pHandler->OnTSClose(pFragInfo, m_nId, m_nPipeId);
		}
		else
		{
			BC_SAFE_DELETE_PTR(pFragInfo);
		}
	}
	//m_sRecorder.Close();

	this->opened = false;
	_NextFrag();

	return 0;
}


int32_t TSSplitter::_OpenFrag(uint64_t ts, int32_t discont)
{
	uint64_t					id;
	uint32_t	                g;
	TSFragConfig	      *		f;

	if (this->opened) {
		return 0;
	}

	id = _GetFragId(ts);

	if (m_sConfig.granularity) {
		g = (uint32_t)m_sConfig.granularity;
		id = (uint64_t)(id / g) * g;
	}

	this->opened = true;

	f = _GetFrag(this->nfrags);

	f->Reset();

	f->active = 1;
	f->discont = discont;
	f->id = id;

	this->frag_ts = ts;

	//BCPString strFile;
	//strFile.Format("video/rec-%d.ts", f->id);
	//ASSERT(m_sRecorder.Open(strFile));
	_InitFragment(f);

	/* start fragment with audio to make iPhone happy */

	_FlushAudio();

	return 0;
}

uint64_t TSSplitter::_GetFragId(uint64_t ts)
{
	switch (m_sConfig.naming) {
	case NGX_RTMP_HLS_NAMING_TIMESTAMP:
		return ts;
	default: /* NGX_RTMP_HLS_NAMING_SEQUENTIAL */
		return this->frag + this->nfrags;
	}
}

int32_t TSSplitter::_ParseSpsPps(LPVOID lpData, uint32_t nSize, BCByteArray &outBA)
{
	uint8_t                         nnals;
	uint16_t                        len;
	int32_t                         n;

	BCFBIStream sReader(lpData, nSize);

	/*
	* Skip bytes:
	* - flv fmt
	* - H264 CONF/PICT (0x00)
	* - 0
	* - 0
	* - 0
	* - version
	* - profile
	* - compatibility
	* - level
	* - nal bytes
	*/

	sReader.Skip(10);

	/* number of SPS NALs */
	sReader.ReadUInt8(&nnals);

	nnals &= 0x1f; /* 5lsb */

	/* SPS */
	for (n = 0;; ++n) {
		for (; nnals; --nnals) {

			/* NAL length */
			sReader.ReadUInt16BE(&len);

			/* AnnexB prefix */
			outBA.Add(0);
			outBA.Add(0);
			outBA.Add(0);
			outBA.Add(1);

			/* NAL body */
			outBA.Append(sReader.Current(), len);
			sReader.Skip(len);
		}

		if (n == 1) {
			break;
		}

		/* number of PPS NALs */
		sReader.ReadUInt8(&nnals);
	}

	return 0;
}

int32_t TSSplitter::_WriteMpegtsFrame(mpegts_frame_t *f, BCBuffer &inBuffer)
{
	uint32_t  pes_size, header_size, body_size, in_size, stuff_size, flags;
	u_char      packet[188], *p, *base;
	int32_t   first, sent;
	ScopedPointer<BCBuffer> buffer(new BCBuffer());

	first = 1;
	sent = 0;

	while (inBuffer.RemainingLength() > 0) {
		p = packet;

		f->cc++;

		*p++ = 0x47;
		*p++ = (u_char)(f->pid >> 8);

		if (first) {
			p[-1] |= 0x40;
		}

		*p++ = (u_char)f->pid;
		*p++ = 0x10 | (f->cc & 0x0f); /* payload */

		if (first) {

			if (f->key) {
				packet[3] |= 0x20; /* adaptation */

				*p++ = 7;    /* size */
				*p++ = 0x50; /* random access + PCR */

				p = mpegts_write_pcr(p, f->dts - HLS_DELAY);
			}

			/* PES header */

			*p++ = 0x00;
			*p++ = 0x00;
			*p++ = 0x01;
			*p++ = (u_char)f->sid;

			header_size = 5;
			flags = 0x80; /* PTS */

			if (f->dts != f->pts) {
				header_size += 5;
				flags |= 0x40; /* DTS */
			}

			pes_size = inBuffer.RemainingLength() + header_size + 3;
			if (pes_size > 0xffff) {
				pes_size = 0;
			}

			*p++ = (u_char)(pes_size >> 8);
			*p++ = (u_char)pes_size;
			*p++ = 0x80; /* H222 */
			*p++ = (u_char)flags;
			*p++ = (u_char)header_size;

			p = mpegts_write_pts(p, flags >> 6, f->pts + HLS_DELAY);

			if (f->dts != f->pts) {
				p = mpegts_write_pts(p, 1, f->dts + HLS_DELAY);
			}

			first = 0;
		}

		body_size = (uint32_t)(packet + sizeof(packet) - p);
		in_size = (uint32_t)inBuffer.RemainingLength();

		if (body_size <= in_size) {
			inBuffer.Read(p, body_size);
		}
		else {
			stuff_size = (body_size - in_size);

			if (packet[3] & 0x20) {
				/* has adaptation */

				base = &packet[5] + packet[4];
				p = (u_char *)ngx_movemem(base + stuff_size, base, p - base);
				memset(base, 0xff, stuff_size);
				packet[4] += (u_char)stuff_size;
			}
			else {
				/* no adaptation */

				packet[3] |= 0x20;
				p = (u_char *)ngx_movemem(&packet[4] + stuff_size, &packet[4], p - &packet[4]);
				packet[4] = (u_char)(stuff_size - 1);
				if (stuff_size >= 2) {
					packet[5] = 0;
					memset(&packet[6], 0xff, stuff_size - 2);
				}
			}

			inBuffer.Read(p, in_size);
		}

		buffer->Write(packet, sizeof(packet));
	}
	if (m_pHandler && !buffer.IsNull() && buffer->RemainingLength() > 0)
	{
		TSFragConfig *pFrag = _GetFrag(this->nfrags);
		BCFObject *pFragInfo = new BCFObject();
		if (pFrag)
		{
			pFragInfo->PutDouble("id", pFrag->id);
			pFragInfo->PutDouble("duration", round(pFrag->duration * 1000));
			pFragInfo->PutDouble("discont", pFrag->discont);
		}
		//WriteBuffer(m_sRecorder, pBuffer);
		m_pHandler->OnTSFragment(pFragInfo, buffer.Release(), m_nId, m_nPipeId);
		return 0;
	}

	return 0;
}

///////////////////////////////////////////////////////////////////////////////
// class : TSGenerator
///////////////////////////////////////////////////////////////////////////////

TSGenerator::TSGenerator()
	: m_nNextWriterId(1)
{
	//
}

TSGenerator::~TSGenerator()
{
	TSSplitter *pWriter;

	while ((pWriter = m_lstWriters.PopFront()) != NULL)
	{
		delete pWriter;
	}
}

BCRESULT TSGenerator::Create()
{
	return BC_R_SUCCESS;
}

uint32_t TSGenerator::AddSplitter(
	BCFObject *pConfig, 
	PublishHandler *pHandler, 
	uint32_t nPipeId)
{
	TSSplitter *pWriter = new TSSplitter();
	if (!pWriter)
	{
		return 0;
	}
	uint32_t nId = m_nNextWriterId;
	BCRESULT result = pWriter->Create(pConfig, nId, pHandler, nPipeId);
	if (result != BC_R_SUCCESS)
	{
		BC_SAFE_DELETE_PTR(pWriter);
		return 0;
	}
	m_lstWriters.PushBack(pWriter);
	m_nNextWriterId++;
	return nId;
}

BCRESULT TSGenerator::RemoveSplitter(uint32_t nId)
{
	TSSplitter *pWriter, *pEnd;
	bool bGot = false;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		if (nId == pWriter->GetId())
		{
			bGot = true;
			break;
		}
	}
	if (bGot)
	{
		pWriter->RemoveFromList();
		pWriter->Close();
		delete pWriter;
		return BC_R_SUCCESS;
	}
	return BC_R_NOTFOUND;
}

uint32_t TSGenerator::GetSplitterCount() const
{
	return m_lstWriters.Count();
}

TSSplitter *TSGenerator::GetSplitterById(uint32_t nId)
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		if (nId == pWriter->GetId())
		{
			return pWriter;
		}
	}
	return NULL;
}

void TSGenerator::OnMP3Audio(LPVOID lpData, uint32_t nSize)
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		pWriter->OnMP3Audio(lpData, nSize);
	}
}

void TSGenerator::OnAACSeqHdr(LPVOID lpData, uint32_t nSize)
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		pWriter->OnAACSeqHdr(lpData, nSize);
	}
}

void TSGenerator::OnAVCSeqHdr(LPVOID lpData, uint32_t nSize)
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		pWriter->OnAVCSeqHdr(lpData, nSize);
	}
}

void TSGenerator::OnAudioStart(uint32_t nAVFlags)
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		pWriter->OnAudioStart(nAVFlags);
	}
}

void TSGenerator::OnVideoStart(uint32_t nAVFlags)
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		pWriter->OnVideoStart(nAVFlags);
	}
}

void TSGenerator::OnAudioPacket(PPacket *pPacket)
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		pWriter->OnAudioPacket(pPacket);
		pPacket->m_sBody.Rewind();
	}
}

void TSGenerator::OnVideoPacket(PPacket *pPacket)
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd; pWriter = m_lstWriters.Next(pWriter))
	{
		pWriter->OnVideoPacket(pPacket);
		pPacket->m_sBody.Rewind();
	}
}

void TSGenerator::Stop()
{
	TSSplitter *pWriter, *pEnd;

	pWriter = m_lstWriters.Begin();
	pEnd = m_lstWriters.End();
	for (; pWriter != pEnd;pWriter = m_lstWriters.Next(pWriter))
	{
		pWriter->Stop();
	}
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file : TSConverter.cpp
///////////////////////////////////////////////////////////////////////////////
