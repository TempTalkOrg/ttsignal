///////////////////////////////////////////////////////////////////////////////
// file : FLVUtils.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_FLVUTILS_H_INCLUDED__
#define RTMP_FLVUTILS_H_INCLUDED__

#include <BC/BCFStream.h>
#include "BC/BCException.h"
#include <RTMP/Packet.h>

namespace BC
{
	struct BCEventItemS;
}

namespace RTMP
{
	class PublishPipe;
	class Subscriber;
	class Publisher;
}

using namespace RTMP;

///////////////////////////////////////////////////////////////////////////////
// Namespace : FLVUtils
///////////////////////////////////////////////////////////////////////////////

namespace FLVUtils
{
	
/* NAL unit types */
enum {
    NAL_SLICE           = 1,
    NAL_DPA             = 2,
    NAL_DPB             = 3,
    NAL_DPC             = 4,
    NAL_IDR_SLICE       = 5,
    NAL_SEI             = 6,
    NAL_SPS             = 7,
    NAL_PPS             = 8,
    NAL_AUD             = 9,
    NAL_END_SEQUENCE    = 10,
    NAL_END_STREAM      = 11,
    NAL_FILLER_DATA     = 12,
    NAL_SPS_EXT         = 13,
    NAL_AUXILIARY_SLICE = 19,
    NAL_FF_IGNORE       = 0xff0f001,
};

/* offsets for packed values */
#define FLV_AUDIO_SAMPLESSIZE_OFFSET 1
#define FLV_AUDIO_SAMPLERATE_OFFSET  2
#define FLV_AUDIO_CODECID_OFFSET     4

#define FLV_VIDEO_FRAMETYPE_OFFSET   4

/* bitmasks to isolate specific values */
#define FLV_AUDIO_CHANNEL_MASK    0x01
#define FLV_AUDIO_SAMPLESIZE_MASK 0x02
#define FLV_AUDIO_SAMPLERATE_MASK 0x0c
#define FLV_AUDIO_CODECID_MASK    0xf0

#define FLV_VIDEO_CODECID_MASK    0x0f
#define FLV_VIDEO_FRAMETYPE_MASK  0xf0

#define AMF_END_OF_OBJECT         0x09

enum {
	FLV_HEADER_FLAG_HASVIDEO = 1,
	FLV_HEADER_FLAG_HASAUDIO = 4,
};

enum {
	FLV_TAG_TYPE_AUDIO = 0x08,
	FLV_TAG_TYPE_VIDEO = 0x09,
	FLV_TAG_TYPE_META  = 0x12,
};

enum {
	FLV_MONO   = 0,
	FLV_STEREO = 1,
};

enum {
	FLV_SAMPLESSIZE_8BIT  = 0,
	FLV_SAMPLESSIZE_16BIT = 1 << FLV_AUDIO_SAMPLESSIZE_OFFSET,
};

enum {
	FLV_SAMPLERATE_SPECIAL = 0, /**< signifies 5512Hz and 8000Hz in the case of NELLYMOSER */
	FLV_SAMPLERATE_11025HZ = 1 << FLV_AUDIO_SAMPLERATE_OFFSET,
	FLV_SAMPLERATE_22050HZ = 2 << FLV_AUDIO_SAMPLERATE_OFFSET,
	FLV_SAMPLERATE_44100HZ = 3 << FLV_AUDIO_SAMPLERATE_OFFSET,
};

enum {
	FLV_CODECID_PCM                   = 0,
	FLV_CODECID_ADPCM                 = 1 << FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_MP3                   = 2 << FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_PCM_LE                = 3 << FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_NELLYMOSER_16KHZ_MONO = 4 << FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_NELLYMOSER_8KHZ_MONO  = 5 << FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_NELLYMOSER            = 6 << FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_PCM_ALAW              = 7 << FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_PCM_MULAW             = 8 << FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_AAC                   = 10<< FLV_AUDIO_CODECID_OFFSET,
	FLV_CODECID_SPEEX                 = 11<< FLV_AUDIO_CODECID_OFFSET,
};

enum {
	FLV_CODECID_H263		= 2,
	FLV_CODECID_SCREEN		= 3,
	FLV_CODECID_VP6			= 4,
	FLV_CODECID_VP6A		= 5,
	FLV_CODECID_SCREEN2		= 6,
	FLV_CODECID_H264		= 7,
	FLV_CODECID_REALH263	= 8,
	FLV_CODECID_MPEG4		= 9,
};

enum {
	FLV_FRAME_KEY        = 1 << FLV_VIDEO_FRAMETYPE_OFFSET,
	FLV_FRAME_INTER      = 2 << FLV_VIDEO_FRAMETYPE_OFFSET,
	FLV_FRAME_DISP_INTER = 3 << FLV_VIDEO_FRAMETYPE_OFFSET,
	FLV_FRAME_GEN_KEY    = 4 << FLV_VIDEO_FRAMETYPE_OFFSET,
	FLV_FRAME_INF_CMD    = 5 << FLV_VIDEO_FRAMETYPE_OFFSET
};

typedef enum {
	AMF_DATA_TYPE_NUMBER      = 0x00,
	AMF_DATA_TYPE_BOOL        = 0x01,
	AMF_DATA_TYPE_STRING      = 0x02,
	AMF_DATA_TYPE_OBJECT      = 0x03,
	AMF_DATA_TYPE_NULL        = 0x05,
	AMF_DATA_TYPE_UNDEFINED   = 0x06,
	AMF_DATA_TYPE_REFERENCE   = 0x07,
	AMF_DATA_TYPE_MIXEDARRAY  = 0x08,
	AMF_DATA_TYPE_OBJECT_END  = 0x09,
	AMF_DATA_TYPE_ARRAY       = 0x0a,
	AMF_DATA_TYPE_DATE        = 0x0b,
	AMF_DATA_TYPE_LONG_STRING = 0x0c,
	AMF_DATA_TYPE_UNSUPPORTED = 0x0d,
} AMFDataType;


typedef enum FrameTypeE
{
	FRAME_INTER		= 0,
	FRAME_KEY		= 1,
}FrameTypeE;

typedef enum FlvAudioTypeE
{
	FLV_LINEAR_PCM_PLATFORMENDIAN_AUDIO		= 0,
	FLV_AD_PCM_AUDIO						= 1,
	FLV_MP3_AUDIO							= 2,
	FLV_LINEAR_PCM_LITTLEENDIAN_AUDIO		= 3,
	FLV_NELLYMOSER_16KHZ_MONO_AUDIO			= 4,
	FLV_NELLYMOSER_8KHZ_MONO_AUDIO			= 5,
	FLV_NELLYMOSER_AUDIO					= 6,
	FLV_G711_ALAW_LOGARITHMIC_PCM_AUDIO		= 7,
	FLV_G711_MULAW_LOGARITHMIC_PCM_AUDIO	= 8,
	FLV_RESERVED_AUDIO						= 9,
	FLV_AAC_AUDIO							= 10,
	FLV_SPEEX_AUDIO							= 11,
	FLV_MP3_8KHZ_AUDIO						= 14,
	FLV_DEVICE_SPECIFIC_SOUND_AUDIO			= 15,
}FlvAudioTypeE;

typedef enum FLVAudioSampleRateE	// Audio sampling rate
{
	FLV_SOUNDRATE_5500HZ			= 0,
	FLV_SOUNDRATE_11000HZ			= 1,
	FLV_SOUNDRATE_22050HZ			= 2,
	FLV_SOUNDRATE_44100HZ			= 3,	// For AAC : always 3
}FLVAudioSampleRateE;

typedef enum FlvFrameTypeE
{
	FLV_KEYFRAME					= 1,
	FLV_INTERFRAME					= 2,
	FLV_DISPOSABLEINTERFRAME		= 3,
	FLV_GENERATEDKEYFRAME			= 4,
	FLV_VIDEOINFO_COMMANDFRAME		= 5,
}FlvFrameTypeE;

typedef enum FlvVideoTypeE
{
	FLV_RTP_RTCP_PACKET				= 1,
	FLV_H263VIDEOPACKET				= 2,
	FLV_SCREENVIDEOPACKET			= 3,
	FLV_VP6VIDEOPACKET				= 4,
	FLV_VP6ALPHAVIDEOPACKET			= 5,
	FLV_SCREENV2VIDEOPACKET			= 6,
	FLV_H264VIDEOPACKET				= 7,
}FlvVideoTypeE;

typedef enum FlvPacketTypeE
{
	PTYPE_AUDIODATA			= 0x08,
	PTYPE_VIDEODATA			= 0x09,
	PTYPE_METADATA			= 0x12,
}FlvPacketTypeE;

typedef enum FlvAvcPacketTypeE
{
	AVCPTYPE_SEQHEADER		= 0,
	AVCPTYPE_NALU			= 1,
	AVCPTYPE_EOSEQ			= 2,
}FlvAvcPacketTypeE;

typedef enum FlvAacPacketTypeE
{
	AACPTYPE_SEQHEADER		= 0,
	AACPTYPE_RAW			= 1,
}FlvAacPacketTypeE;

typedef struct FLVVideoInfoS
{
	int32_t			eFrameType;
	int32_t			eCodecId;
} FLVVideoInfoS;

typedef struct FLVAudioInfoS
{
	int32_t			eCodecId;
	int32_t			nRate;
	int32_t			nSize;
	int32_t			eType ;// 0:mono; 1:stereo
}FLVAudioInfoS;

typedef struct AVCVideoPacketS
{
	int32_t			eAVCPacketType;
	int32_t			nCompositeTime;
}AVCVideoPacketS;

typedef struct MP3ConfigS
{
	uint8_t			version;
	uint8_t			layer;
	uint32_t		sampleRate;
	uint32_t		frameSize;
	uint32_t		samples;
	uint8_t			slotSize;
}MP3ConfigS;

typedef struct AACConfigS
{
	uint8_t			objtype;
	uint8_t			srindex;
	uint8_t			chconf;
	uint32_t		sampleRate;
	uint8_t			frameLengthFlag;
	uint8_t			dependsOnCoreCoder;
	uint8_t			extensionFlag;
	uint32_t		frameLength;
}AACConfigS;


/** Length of the Speex header identifier */
#define SPEEX_HEADER_STRING_LENGTH 8

/** Maximum number of characters for encoding the Speex version number in the header */
#define SPEEX_HEADER_VERSION_LENGTH 20

/** Speex header info for file-based formats */
typedef struct SpeexHeader {
	char speex_string[SPEEX_HEADER_STRING_LENGTH];   /**< Identifies a Speex bit-stream, always set to "Speex   " */
	char speex_version[SPEEX_HEADER_VERSION_LENGTH]; /**< Speex version */
	int32_t speex_version_id;       /**< Version for Speex (for checking compatibility) */
	int32_t header_size;            /**< Total size of the header ( sizeof(SpeexHeader) ) */
	int32_t rate;                   /**< Sampling rate used */
	int32_t mode;                   /**< Mode used (0 for narrowband, 1 for wideband) */
	int32_t mode_bitstream_version; /**< Version ID of the bit-stream */
	int32_t nb_channels;            /**< Number of channels encoded */
	int32_t bitrate;                /**< Bit-rate used */
	int32_t frame_size;             /**< Size of frames */
	int32_t vbr;                    /**< 1 for a VBR encoding, 0 otherwise */
	int32_t frames_per_packet;      /**< Number of frames stored per Ogg packet */
	int32_t extra_headers;          /**< Number of additional headers after the comments */
	int32_t reserved1;              /**< Reserved for future use, must be zero */
	int32_t reserved2;              /**< Reserved for future use, must be zero */
} SpeexHeader;

class FLVMeta;

#define URI_MAXLEN		1024

///////////////////////////////////////////////////////////////////////////////
// class : FLVUri
///////////////////////////////////////////////////////////////////////////////

class RTMP_API FLVUri
{
	typedef struct QueryArgPairS
	{
		LPCSTR		szArg;
		LPCSTR		szValue;
	}QueryArgPairS;
public:
	FLVUri();
	FLVUri(LPCSTR szUrl);
	~FLVUri();

	BCRESULT		Parse(LPCSTR szUrl, uint32_t nLength);
	inline LPCSTR	GetUri() const
	{
		return m_szUri;
	}
	LPCSTR			GetParam(LPCSTR szArg, BOOL bCaseSensitive = FALSE);

	FLVUri &operator = (const FLVUri &other);
protected:
public:
	char				m_szUrlBuf[URI_MAXLEN];
	char				m_szRecvBuf[URI_MAXLEN]; /*%< receive buffer */
	uint32_t			m_nRecvLen;	/*%< length recv'd */
	char			*	m_szUri;
	QueryArgPairS		m_arrayArgs[20];
	uint32_t			m_nArgCount;
};

///////////////////////////////////////////////////////////////////////////////
// class : FLVInfo
///////////////////////////////////////////////////////////////////////////////

class RTMP_API FLVInfo
{
	typedef struct bitstream_t
	{
		unsigned char *	bytes;
		size_t			length;
		size_t			byte;
		short			bit;
	} bitstream_t;
public:
	static FLVVideoInfoS	AnalyseVideo(uint8_t t);
	static FLVAudioInfoS	AnalyseAudio(uint8_t t);
	static AVCVideoPacketS	AnalyseAVC(uint32_t t);
	static MP3ConfigS		ParseMP3Info(LPVOID lpData, size_t nSize);
	static SpeexHeader		ParseSpeexHeader(LPVOID lpData, size_t nSize);
	static AACConfigS		ParseAACSeqHeader(LPVOID lpData, size_t nSize);
	static BCRESULT			ParseFileName(
								BCPString &strOut,
								LPCSTR szFlvName);
	static void				ParseH264SPS(
								BCIStream &refStream,
								FLVMeta &flvmetadata);
	static void				ReadFLVH264VideoPacket(
								BCFIStream &refStream,
								FLVMeta &flvmetadata);
private:
	static void				_ReadH264NALUnit(
								unsigned char *nalu,
								int length,
								FLVMeta &flvmetadata);
	static void				_ReadH264SPS(
								bitstream_t *bitstream,
								FLVMeta &flvmetadata);

	static uint32_t			_ReadCodedU(
								bitstream_t *bitstream,
								int nbits,
								const char *name);
	static uint32_t			_ReadCodedUE(
								bitstream_t *bitstream,
								const char *name);
	static int				_ReadCodedSE(
								bitstream_t *bitstream,
								const char *name);

	static int				_ReadBits(bitstream_t *bitstream, int nbits);
	static int				_ReadBit(bitstream_t *bitstream);
};

///////////////////////////////////////////////////////////////////////////////
// class : FLVMeta
///////////////////////////////////////////////////////////////////////////////

class RTMP_API FLVMeta
{
public:
	FLVMeta();
	~FLVMeta();

	BCRESULT		Read(BCIStream *pStream);
	BCRESULT		Write(BCOStream *pStream);
public:
	uint64_t		filesize;
	uint8_t			hasVideo;
	uint8_t			hasAudio;
	uint32_t		duration;
	uint32_t		videoDuration;
	uint16_t		height;
	uint16_t		width;
	uint8_t			videocodecid;
	uint32_t		videodatarate;
	uint16_t		framerate;
	uint8_t			audiocodecid;
	uint32_t		audiosamplerate;
	uint32_t		audiosamplesize;
	uint8_t			stereo;
	uint32_t		videotags;
	uint32_t		keyframes;
	uint32_t		totaltags;
	uint8_t			hasAudioSeqHeader;
	uint8_t			hasVideoSeqHeader;
};

#define SIZE_OF_INDEXMETA			53

///////////////////////////////////////////////////////////////////////////////
// class : IStreamHandler
///////////////////////////////////////////////////////////////////////////////

typedef enum PPacketReadyTypeE
{
	READYTYPE_PLAY_LIVE				= 0,
	READYTYPE_PLAY_RECORD			= 1,
	READYTYPE_PUBLISH_LIVE			= 2,
	READYTYPE_PUBLISH_RECORD		= 3,
}PPacketReadyTypeE;

enum{
	BUFFER_EVENT_FULL		= 0,
	BUFFER_EVENT_EMPTY		= 1,
	BUFFER_EVENT_DROP		= 2,
};

class RTMP_API IStreamHandler
{
public:
	virtual void	OnStreamPlay(BOOL /*bLive*/)				= 0;
	virtual void	OnStreamStop()								= 0;
	virtual void	OnStreamPacketReady(
						PPacket &/*refPacket*/,
						uint32_t /*eType*/)						= 0;
	virtual void	OnStreamNewSubscriber(
						Subscriber * /*pSuber*/)				= 0;
	virtual void	OnStreamNewPublisher(
						Publisher * /*pPuber*/)					= 0;
	virtual void	OnStreamPlayLiveInit()						= 0;
	virtual void	OnStreamPublishPacket(
						LPVOID /*pPacket*/,
						uint32_t /*eExtraInfo*/)				= 0;
	virtual void	OnStreamStopNotify()						= 0;
	virtual void 	OnStreamReleaseNotify()						= 0;
	virtual void	OnStreamBufferEvent(
						uint32_t /*eType*/,
						AVDropStatS &/*stats*/)					= 0;
	virtual void	OnStreamBufferOverflow(BCFObject *pInfo) {};
	virtual void	PostStreamEvent(BCEventItemS &ev)	= 0;
	virtual void	OnException(BCException &refExcept) {};
};


///////////////////////////////////////////////////////////////////////////////
// misc functions : 
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// End of namespace : FLVUtils
///////////////////////////////////////////////////////////////////////////////

} // End of namespace FLVUtils

#endif // RTMP_FLVUTILS_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : FLVUtils.h
///////////////////////////////////////////////////////////////////////////////
