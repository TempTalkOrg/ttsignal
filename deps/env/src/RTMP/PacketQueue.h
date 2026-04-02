///////////////////////////////////////////////////////////////////////////////
// file : PacketQueue.h
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_PACKETQUEUE_H_INCLUDED__
#define RTMP_PACKETQUEUE_H_INCLUDED__

#include <memory>
#include <BC/BCFCodec.h>
#include <RTMP/Packet.h>

namespace FLVUtils
{
class IStreamHandler;
}

using namespace FLVUtils;

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

class PAVQueueHandler;

///////////////////////////////////////////////////////////////////////////////
// class : PVideoQueue
///////////////////////////////////////////////////////////////////////////////

enum
{
	PKT_MISSING_TYPE_NONE		= 0,
	PKT_MISSING_TYPE_DROPPED	= 1,
	PKT_MISSING_TYPE_EXCEED		= 2,
};

#define AVINFO_HAS_AUDIO				0x000001
#define AVINFO_HAS_VIDEO				0x000002
#define AVINFO_HAS_AVCHDR				0x000004
#define AVINFO_HAS_AACHDR				0x000008
#define AVINFO_PARSE_MP3				0x000010
#define AVINFO_PARSE_SPEEX				0x000020
#define AVINFO_PARSE_DEVICE_SPECIFIC	0x000080
#define AVINFO_REPORT_NOAVCHDR			0x010000
#define AVINFO_REPORT_NOAACHDR			0x020000
#define AVINFO_ERROR_AUDIOUNSUPPORT		0x040000
#define AVINFO_ERROR_VIDEOUNSUPPORT		0x080000

enum
{
	FILTER_FLAG_NONE				= 0,
	FILTER_FLAG_DISABLEAUDIO		= 0x01,
	FILTER_FLAG_DISABLEVIDEO		= 0x02,
	FILTER_FLAG_LOGTIMESTAMP		= 0x04,
};

RTMPDLLEXPORT_DATA(extern const char *) SZ_absRecordTime;
RTMPDLLEXPORT_DATA(extern const char *) SZ_onMetaData;
RTMPDLLEXPORT_DATA(extern const char *) SZ_audiocodecid;
RTMPDLLEXPORT_DATA(extern const char *) SZ_videocodecid;
RTMPDLLEXPORT_DATA(extern const char *) SZ_width;
RTMPDLLEXPORT_DATA(extern const char *) SZ_height;
RTMPDLLEXPORT_DATA(extern const char *) SZ_audiosamplerate;
RTMPDLLEXPORT_DATA(extern const char *) SZ_framerate;
RTMPDLLEXPORT_DATA(extern const char *) SZ_stereo;
RTMPDLLEXPORT_DATA(extern const char *) SZ_audiosamplesize;


///////////////////////////////////////////////////////////////////////////////
// class : PacketAnalyzer
///////////////////////////////////////////////////////////////////////////////

class RTMP_API PacketAnalyzer
{
public:
	PacketAnalyzer();
	~PacketAnalyzer();

	BCRESULT			Create(
							LPCSTR lpszStreamName,
							PAVQueueHandler *pHandler);
	uint32_t			Analyze(PPacket *pPacket, uint32_t nLevel);
	inline PPacket	&	GetMetaData()
	{
		return m_sMetaData;
	}
	PPacket			*	GetVidSeqHdr(bool bClone = true);
	PPacket			*	GetAudSeqHdr(bool bClone = true);
	uint64_t			GetDuration() const;
	inline uint64_t		GetAbsRecordTime() const
	{
		return m_tmAbsRecordTime;
	}
	inline uint32_t		GetSampleRate() const
	{
		return m_nSampleRate;
	}
	inline uint32_t		GetSamplesPerFrame() const
	{
		return m_nSamplesPerFrame;
	}
	inline uint8_t		GetAVFlags()
	{
		return m_nAVFlags;
	}
	BCRESULT			CreateMetaData(PPacket &refPacket);
	void				Cleanup();
	void				SetAVFilter(uint8_t eFilterFlags);

	static bool			TrySetDataFrame(
							const PPacket &refPacket,
							PPacket &refDstPkt);
	static bool			IsMetaData(const PPacket &refPacket);
	static void			flv2annexb(LPVOID lpData, uint32_t nLen);
protected:
	void				_Reset();
private:
	DECLARE_NO_COPY_CLASS(PacketAnalyzer);
	PAVQueueHandler	*	m_pHandler;
	AMFVarPtr			m_pMetaObj;
	PPacket				m_sMetaData;
	PPacket				m_sAVCSeqHeader;
	PPacket				m_sAACSeqHeader;
	int64_t				m_nTotalTime;
	int64_t				m_nTotalAudTime;
	int64_t				m_nTotalVidTime;
	uint32_t			m_nFirstSeqNumber;
	uint32_t			m_nNextSeqNumber;
	bool				m_bNormalVidPkt;
	bool				m_bNormalAudPkt;
	uint64_t			m_tmAbsRecordTime;
	uint64_t			m_tmLatestKeyFrame;
	uint32_t			m_nAVFlags;
	uint32_t			m_nSampleRate;
	uint32_t			m_nSamplesPerFrame;
	uint64_t			m_nTotalAudPkt;
	uint8_t				m_eAVFilterFlags;
	uint32_t			m_nWidth;
	uint32_t			m_nHeight;
	std::string			m_strStreamName;
};

///////////////////////////////////////////////////////////////////////////////
// class : PPacketQueue
///////////////////////////////////////////////////////////////////////////////

class RTMP_API PPacketQueue
{
	typedef struct SyncPairS{
		int64_t				ptsDiff;
		PPacket			*	pkt;
	}SyncPairS;
public:
	PPacketQueue();
	~PPacketQueue();

	BCRESULT			Create();
	uint32_t			PushBack(PPacket *pPacket);
	PPacket			*	PopAndSendFront();
	void				FlushTimeout(uint32_t nEpoch, AVDropStatS &refInfo);
	void				Align(AVDropStatS &refInfo);
	uint64_t			GetDuration();
	void				GetDuration(
							uint64_t &refVideoDuration, 
							uint64_t &refAudioDuration) const;
	size_t				Count() const { return m_lstPackets.Count(); }
	void				Cleanup();
	inline	void		SetParams(uint32_t nAVFlags)
	{ 
		m_nAVFlags = nAVFlags;
	}
	inline	uint8_t		GetFlags() const { return m_nAVFlags; }

protected:
	PPacket			*	_PopAndDropFront();
	void				_ReduceTotalTime(PPacket *pPacket);
	uint32_t			_RemoveOnce(AVDropStatS &refInfo);
	uint32_t			_RemoveOnceWithVideo(AVDropStatS &refInfo);
	void				_DropPacket(PPacket *pPacket, AVDropStatS &refInfo);
	static int			_QSortFunc(LPCVOID lpParam1, LPCVOID lpParam2);
private:
	DECLARE_NO_COPY_CLASS(PPacketQueue);
	uint32_t				m_nTotalTime;
	PPacketList				m_lstPackets;
	uint8_t					m_nAVFlags;
	//FILE				*	m_pDumpFD;
	uint64_t				m_nAudPktInput;
	int64_t					m_nVidTotalInput;
	int64_t					m_nVidTotalDropped;
	uint64_t				m_nAudPktDropped;
	uint64_t				m_nLastSentOrDroppedTimestamp;
	PPacketList				m_lstMustSendPackets;
	uint64_t				m_nAudPktOutput;
	int64_t					m_nVidTotalOutput;
	uint64_t				m_nAudPktCache;
	int64_t					m_nVidTotalCache;
	bool					m_bRecvVKFrame;
};

///////////////////////////////////////////////////////////////////////////////
// class : PPacketQueue
///////////////////////////////////////////////////////////////////////////////

class RTMP_API PPacketQueueSimple
{
	typedef struct SyncPairS{
		int64_t				ptsDiff;
		PPacket			*	pkt;
	}SyncPairS;
public:
	PPacketQueueSimple();
	~PPacketQueueSimple();

	BCRESULT			Create();
	uint32_t			PushBack(PPacket *pPacket);
	PPacket			*	PopAndSendFront();
	void				FlushTimeout(uint32_t nEpoch, AVDropStatS &refInfo);
	void				Align(AVDropStatS &refInfo);
	uint64_t			GetDuration() const;
	void				GetDuration(
							uint64_t &refVideoDuration, 
							uint64_t &refAudioDuration) const;
	size_t				Count() const { return m_lstPackets.Count(); }
	void				Cleanup();
	inline	void		SetParams(
							uint8_t nAVFlags, 
							uint32_t nSampleRate, 
							uint32_t nSamplesPerFrame) 
	{ 
		m_nAVFlags = nAVFlags;
		m_nSampleRate = nSampleRate;
		m_nSamplesPerFrame = nSamplesPerFrame;
	}
	inline	uint8_t		GetFlags() const { return m_nAVFlags; }

protected:
	PPacket			*	_PopAndDropFront();
	void				_ReduceTotalTime(PPacket *pPacket);
	uint32_t			_RemoveOnce(AVDropStatS &refInfo);
	uint32_t			_RemoveOnceWithVideo(AVDropStatS &refInfo);
	void				_DropPacket(PPacket *pPacket, AVDropStatS &refInfo);
	static int			_QSortFunc(LPCVOID lpParam1, LPCVOID lpParam2);
private:
	DECLARE_NO_COPY_CLASS(PPacketQueueSimple);
	uint32_t				m_nTotalTime;
	PPacketList				m_lstPackets;
	uint8_t					m_nAVFlags;
	//FILE				*	m_pDumpFD;
	uint64_t				m_nAudPktInput;
	int64_t					m_nVidTotalInput;
	int64_t					m_nVidTotalDropped;
	uint64_t				m_nAudPktDropped;
	uint32_t				m_nSampleRate;
	uint32_t				m_nSamplesPerFrame;
	uint64_t				m_nLastSentOrDroppedTimestamp;
	PPacketList				m_lstMustSendPackets;
	uint64_t				m_nAudPktOutput;
	int64_t					m_nVidTotalOutput;
	uint64_t				m_nAudPktCache;
	int64_t					m_nVidTotalCache;
	// debug
	uint64_t				m_nFeedPackets;
	uint64_t				m_nSentPackets;
	uint64_t				m_nDroppedPackets;
};

///////////////////////////////////////////////////////////////////////////////
// class : IPPacketSenderHandler
///////////////////////////////////////////////////////////////////////////////

class RTMP_API IPPacketSenderHandler
{
public:
	virtual void	OnPacketReady(
						uint32_t /*nStreamId*/, 
						PPacket * /*refPacket*/)				= 0;
	virtual void	OnBufferEvent(
						uint32_t /*nStreamId*/, 
						uint32_t /*eType*/,
						AVDropStatS &/*stats*/)					= 0;
	virtual void	OnBufferOverflow(
						uint32_t /*nStreamId*/,
						BCFObject* /*nDuration*/)				= 0;
};

///////////////////////////////////////////////////////////////////////////////
// class : PPacketSender
///////////////////////////////////////////////////////////////////////////////

class RTMP_API PPacketSender
{
public:
	///////////////////////////////////////////////////////////////////////////////
	// class : Config
	///////////////////////////////////////////////////////////////////////////////

	class Config
	{
	public:
		Config() : streamId(0), flags(0), bufferLength(30), sampleRate(2000)
			, samplesPerFrame(0), bufferOverflow(0), flyPackets(1)
		{
		}

		Config(const Config &other)
		{
			operator=(other);
		}

		~Config()
		{
		}

		Config & operator=(const Config &other)
		{
			streamId = other.streamId;
			flags = other.flags;
			bufferLength = other.bufferLength;
			sampleRate = other.sampleRate;
			samplesPerFrame = other.samplesPerFrame;
			bufferOverflow = other.bufferOverflow;
			flyPackets = other.flyPackets;
			return *this;
		}

		uint32_t			streamId;
		uint32_t			flags;
		uint32_t			bufferLength; // seconds
		uint32_t			sampleRate;// milliseconds
		uint32_t			samplesPerFrame;// 0-99
		uint32_t			bufferOverflow;// milliseconds
		uint32_t			flyPackets;

		BCRESULT		Init(BCFObject *pConfig)
		{
			BCFVar *pVar;

			pVar = pConfig->Get("streamId");
			if (IS_BCF_NUMBER(pVar))
			{
				streamId = GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("flags");
			if (IS_BCF_NUMBER(pVar))
			{
				flags = GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("bufferLength");
			if (IS_BCF_NUMBER(pVar))
			{
				bufferLength = GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("sampleRate");
			if (IS_BCF_NUMBER(pVar))
			{
				sampleRate = GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("samplesPerFrame");
			if (IS_BCF_NUMBER(pVar))
			{
				samplesPerFrame = GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("bufferOverflow");
			if (IS_BCF_NUMBER(pVar))
			{
				bufferOverflow = GET_BCF_INT(pVar);
			}
			pVar = pConfig->Get("flyPackets");
			if (IS_BCF_NUMBER(pVar))
			{
				flyPackets = GET_BCF_INT(pVar);
			}
			return BC_R_SUCCESS;
		}

	private:

	};
public:
	PPacketSender();
	~PPacketSender();

	BCRESULT			Create(
							IPPacketSenderHandler *pHandler, 
							PPacketSender::Config &refConfig);
	BCRESULT			Reconfig(BCFObject *pConfig);
	uint32_t			GetBufferLength() const;
	void				Pause(bool bPause);
	void				PauseWithoutCounter(bool bPause);
	void				FeedPacket(PPacket *pPacket);
	void				FeedPacketWithoutCounter(PPacket *pPacket);
	void				SendPacket();
	void				SendPacketWithoutCounter();
private:
	DECLARE_NO_COPY_CLASS(PPacketSender);
	Config						m_sConfig;
	IPPacketSenderHandler	*	m_pHandler;
	PPacketQueueSimple			m_lstAVPackets;
	bool						m_bPaused;
	uint64_t					m_nSendCount;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_PACKETQUEUE_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : PacketQueue.h
///////////////////////////////////////////////////////////////////////////////
