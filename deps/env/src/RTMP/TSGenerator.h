
///////////////////////////////////////////////////////////////////////////////
// File : TSGenerator.h
///////////////////////////////////////////////////////////////////////////////

#ifndef TSGENERATOR_H_INCLUDED__
#define TSGENERATOR_H_INCLUDED__

#include <RTMP/Exports.h>
#include <BC/BCPArray.h>
#include <BC/BCNodeList.h>

namespace FLVUtils
{
	typedef struct FLVAudioInfoS	FLVAudioInfoS;
}
using namespace FLVUtils;



///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

typedef struct mpegts_frame_t	mpegts_frame_t;
class TSFragConfig;
class PublishHandler;

///////////////////////////////////////////////////////////////////////////////
// class : HLSConfig
///////////////////////////////////////////////////////////////////////////////

class HLSConfig
{
public:
	HLSConfig();
	HLSConfig(const HLSConfig &other);
	~HLSConfig();

	HLSConfig &operator = (const HLSConfig &other);

	uint64_t		fragLen;
	int64_t			maxFragLen;
	int64_t			sync;
	int64_t			muxDelay;
	uint64_t		audioBufferSize;
	uint64_t		maxAudioDelay;
	uint32_t		slicing;
	uint32_t		winfrags;
	uint32_t		naming;
	uint32_t		granularity;
	uint64_t		playLen;

	BCRESULT		Init(BCFObject *pConfig);
private:

};

///////////////////////////////////////////////////////////////////////////////
// Class : TSFragConfig
///////////////////////////////////////////////////////////////////////////////

class TSFragConfig
{
public:
	TSFragConfig() : id(0), key_id(0), duration(0), active(0), discont(0){}
	~TSFragConfig(){}

	uint64_t                            id;
	uint64_t                            key_id;
	double                              duration;
	unsigned                            active : 1;
	unsigned                            discont : 1; /* before */

	void 		Reset() 
	{
		id = 0;
		key_id = 0;
		duration = 0;
		active = 0;
		discont = 0;
	}
};

///////////////////////////////////////////////////////////////////////////////
// class : TSSplitter
///////////////////////////////////////////////////////////////////////////////

class RTMP_API TSSplitter : public BCNodeList::Node
{
	DECLARE_FIXED_ALLOC(TSSplitter);
public:
	TSSplitter();
	~TSSplitter();

	BCRESULT			Create(
							BCFObject *pConfig,
							uint32_t nId,
							PublishHandler *pHandler, 
							uint32_t nPipeId);
	void				OnMP3Audio(LPVOID lpData, uint32_t nSize);
	void				OnAACSeqHdr(LPVOID lpData, uint32_t nSize);
	void				OnAVCSeqHdr(LPVOID lpData, uint32_t nSize);
	void				OnAudioStart(uint32_t nAVFlags);
	void				OnVideoStart(uint32_t nAVFlags);
	void				OnAudioPacket(PPacket *pPacket);
	void				OnVideoPacket(PPacket *pPacket);
	void				Close();
	void				Stop();

	inline uint32_t		GetId() const{
		return m_nId;
	}
protected:
	void				_UpdateFragment(uint64_t ts, bool boundary, uint32_t flush_rate);
	int32_t				_InitFragment(TSFragConfig *pFrag);
	int32_t				_FlushAudio();
	TSFragConfig	*	_GetFrag(uint32_t n);
	void				_NextFrag();
	int32_t				_CloseFrag(TSFragConfig *f);
	int32_t				_OpenFrag(uint64_t ts, int32_t discont);
	uint64_t			_GetFragId(uint64_t ts);
	int32_t				_ParseSpsPps(LPVOID lpData, uint32_t nSize, BCByteArray &outBA);
	int32_t				_WriteMpegtsFrame(mpegts_frame_t *f, BCBuffer &inBuffer);
private:
	DECLARE_NO_COPY_CLASS(TSSplitter);
	KBPool					m_sPool;
	uint32_t				m_nId;
	PublishHandler		*	m_pHandler;
	uint32_t				m_nPipeId;
	BCByteArray				m_sAACSeqHeader;
	BCByteArray				m_sAVCSeqHeader;
	bool					m_bAudioBaseSet;
	uint64_t				m_nAudioBaseTime;
	bool					m_bVideoBaseSet;
	uint64_t				m_nVideoBaseTime;
	HLSConfig				m_sConfig;
	uint32_t				m_nSegCount;
	uint64_t				audio_pts;
	uint8_t					m_nAVFlags;
	BCBuffer			*	m_pAFrames;
	uint32_t				m_nVideoCC;
	bool					m_bAVCSeqHdrSent;
	bool					m_bWorking;
	uint8_t					m_eAudioFormat;
	AACConfigS				m_sAACConfig;
	MP3ConfigS				m_sMP3Config;
	uint64_t				m_tmAudFramePTS;
	uint32_t				m_nAudioCC;
	uint64_t				m_nAudFrameCount;
	uint32_t				m_nSampleRate;
	uint64_t				m_tmAudFrameBase;
	uint32_t				m_nSamplesPerFrame;
	uint64_t				frag;
	uint64_t				frag_ts;
	uint32_t				nfrags;
	TSFragConfig		*	frags;
	bool					opened;
	uint32_t				avc_nal_bytes;
	// For debug
	//BCFOStream				m_sRecorder;
};

typedef TNodeList<TSSplitter>	TSSplitterList;

///////////////////////////////////////////////////////////////////////////////
// class : TSGenerator
///////////////////////////////////////////////////////////////////////////////

class TSGenerator
{
public:
	TSGenerator();
	~TSGenerator();

	BCRESULT			Create();
	uint32_t			AddSplitter(
							BCFObject *pConfig, 
							PublishHandler *pHandler, 
							uint32_t nPipeId);
	BCRESULT			RemoveSplitter(uint32_t nId);
	uint32_t			GetSplitterCount() const;
	TSSplitter		*	GetSplitterById(uint32_t nId);
	void				OnMP3Audio(LPVOID lpData, uint32_t nSize);
	void				OnAACSeqHdr(LPVOID lpData, uint32_t nSize);
	void				OnAVCSeqHdr(LPVOID lpData, uint32_t nSize);
	void				OnAudioStart(uint32_t nAVFlags);
	void				OnVideoStart(uint32_t nAVFlags);
	void				OnAudioPacket(PPacket *pPacket);
	void				OnVideoPacket(PPacket *pPacket);
	void				Stop();

protected:
private:
	DECLARE_NO_COPY_CLASS(TSGenerator);
	TSSplitterList			m_lstWriters;
	uint32_t				m_nNextWriterId;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // TSGENERATOR_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : TSGenerator.h
///////////////////////////////////////////////////////////////////////////////
