///////////////////////////////////////////////////////////////////////////////
// file : IHandler.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_IHANDLER_H_INCLUDED__
#define RTMP_IHANDLER_H_INCLUDED__

#include <BC/BCList.h>
#include <BC/Utils.h>
#include <BC/BCMemPool.h>
#include <BC/BCBuffer.h>
#include <BC/BCException.h>
#include <BC/EventQueue.h>
#include <BC/BCFCodec.h>
#include <BC/BCSockAddr.h>
#include <RTMP/AMF.h>
#include <RTMP/FLVUtils.h>

using namespace BC;
using namespace AMF;

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

class MCommand;
class PPacket;
class IRPCStub;

enum{
	ACTIVECHECK_SENTPING		= 0,
	ACTIVECHECK_RECVPING		= 1,
	ACTIVECHECK_SENTPONG		= 2,
	ACTIVECHECK_RECVPONG		= 3,
};

///////////////////////////////////////////////////////////////////////////////
// class : IEventHandler
///////////////////////////////////////////////////////////////////////////////

class RTMP_API IEventHandler
{
public:
	virtual ~IEventHandler(){}
	virtual void	OnMgrMsg(uint32_t nMsg, int32_t wParam, void *lParam)	= 0;
};

#define MGRM_CONNCOUNT_CHANGED			1

///////////////////////////////////////////////////////////////////////////////
// Class : IRPCStub
///////////////////////////////////////////////////////////////////////////////

typedef void (*IRPCStubDtor)(IRPCStub &);
typedef IRPCStubDtor		LPFN_IRPCStubDtor;

class IRPCStub
{
public:
	IRPCStub(uint32_t nTransId) 
		: m_nTransId(nTransId)
		, m_lpfnDtor(NULL)
		, m_result(BC_R_SUCCESS)
	{
		memzero(m_szCmd, sizeof(m_szCmd));
		memzero(m_lParams, sizeof(m_lParams));
	}
	IRPCStub(const IRPCStub& other)
	{
		m_nTransId = other.m_nTransId;
		m_lpfnDtor = other.m_lpfnDtor;
		m_result = other.m_result;
		memcpy(m_szCmd, other.m_szCmd, sizeof(m_szCmd));
		memcpy(m_lParams, other.m_lParams, sizeof(m_lParams));
	}
	virtual ~IRPCStub()
	{
		//if (m_lpfnDtor)
		//{
		//	(m_lpfnDtor)(*this);
		//}		
	}
	virtual IRPCStub *Clone(){
		return new IRPCStub(*this);
	}
	void	SetCmdName(LPCSTR lpszCmdName) {
		strncpy(m_szCmd, lpszCmdName, sizeof(m_szCmd));
	}

	uint32_t				m_nTransId;
	LPFN_IRPCStubDtor		m_lpfnDtor;
	BCRESULT				m_result;
	char					m_szCmd[MAX_PATH];
	uint64_t				m_lParams[10];
	KBPool					m_sPool;
};

///////////////////////////////////////////////////////////////////////////////
// Class : NetConoHandler
///////////////////////////////////////////////////////////////////////////////

class NetConnHandler
{
public:
	NetConnHandler(){}
	virtual ~NetConnHandler(){}

	virtual BCRESULT	OnConnect(BCRESULT result, LPVOID)				= 0;
	virtual	void		OnSockClose(
							BCSockAddrS *, 
							BCSockAddrS *, 
							BCRESULT)									= 0;
	virtual void		OnStart(LPVOID)									= 0;
	virtual void		OnCommand(MCommand *lpCmd, LPVOID)				= 0;
	virtual void		OnCmdSent(MCommand *lpCmd, LPVOID)				= 0;
	virtual void		OnPacket(PPacket *lpPacket, LPVOID)				= 0;
	virtual void		OnPacketSent(PPacket *, LPVOID)					= 0;
	virtual void		OnPacketSent(uint32_t, LPVOID)					= 0;
	virtual void		OnStop(
							BCRESULT, /*close status*/
							uint32_t, /*line number*/
							uint32_t, /*stream id*/
							uint32_t, /*exit code*/
							LPVOID)	= 0;
	virtual void		OnShutdown(LPVOID)								= 0;
	virtual void		OnException(BCException &, uint32_t, LPVOID)	= 0;
	virtual void		OnExecDone(IRPCStub *, LPVOID)					= 0;
	virtual void		OnRecvRawdata(BCBuffer *, LPVOID)				= 0;
	virtual void		OnSentRawdata(BCBuffer *, LPVOID)				= 0;
	virtual void		OnActiveCheck(uint32_t, uint32_t, LPVOID)		= 0;
	virtual void		OnActiveCheckTimeout(BCFObject *pInfo, LPVOID)	= 0;
	virtual void		OnBufferEvent(
							uint32_t, /*event type*/
							uint32_t, /*stream id*/
							LPVOID /*connection ptr*/)					= 0;
	virtual void		OnBufferOverflow(
							uint32_t, /*nStreamId*/
							BCFObject *, /*pInfo*/
							LPVOID /*connection ptr*/)					= 0;
	virtual void		OnSendBufferOverflow(
							BCFObject *, /*pInfo*/
							LPVOID /*connection ptr*/)					= 0;
	virtual void		OnUserCtrl(
							uint32_t, /*stream id*/
							uint32_t, /*event type*/
							uint32_t, /*event data extend*/
							LPVOID)										= 0;
	virtual void		OnAVSentDone(
							uint32_t, /* stream id */
							LPVOID /* connection ptr */)				= 0;
	virtual void		PostConnEvent(BCEventItemS &ev, LPVOID)			= 0;
	virtual void		OnSendCongestionStat(
							BCFObject *, /*pInfo*/
							LPVOID /*connection ptr*/)					= 0;
private:
	DECLARE_NO_COPY_CLASS(NetConnHandler);
};

///////////////////////////////////////////////////////////////////////////////
// Class : PublishHandler
///////////////////////////////////////////////////////////////////////////////

class PublishHandler
{
public:
	PublishHandler(){}
	virtual ~PublishHandler(){}

	virtual void		OnReady()								= 0;
	virtual void		OnStreamStart(LPVOID)					= 0;
	virtual void		OnStreamStop(LPVOID)					= 0;
	virtual void		OnMetaData(LPVOID, LPVOID)				= 0;
	virtual void		OnAACSeqHdr(LPVOID, LPVOID, uint32_t)	= 0;
	virtual void		OnAVCSeqHdr(LPVOID, LPVOID, uint32_t)	= 0;
	virtual void		OnAudioStart(LPVOID, LPVOID)			= 0;
	virtual void		OnVideoStart(LPVOID, LPVOID)			= 0;
	virtual void		OnPeerChanged(LPVOID, uint32_t, bool)	= 0;
	virtual void		OnException(BCException &, uint32_t)	= 0;
	virtual void		OnTSOpen(
							BCFObject *pInfo, 
							uint32_t nId,
							uint32_t nPipeId)					= 0;
	virtual void		OnTSFragment(
							BCFObject *pInfo,
							BCBuffer *pFragment,
							uint32_t nId,
							uint32_t nPipeId)					= 0;
	virtual void		OnTSClose(
							BCFObject *pInfo, 
							uint32_t nId,
							uint32_t nPipeId)					= 0;
};

///////////////////////////////////////////////////////////////////////////////
// Class : PAVQueueHandler
///////////////////////////////////////////////////////////////////////////////

class PAVQueueHandler
{
public:
	PAVQueueHandler(){}
	virtual ~PAVQueueHandler(){}

	virtual void		OnMetaData(AMFVarPtr &)					= 0;
	virtual void		OnCommonMetaData(
							PPacket *pPacket, 
							AMFVarPtr &, 
							AMFVarPtr&)							= 0;
	virtual void		OnMP3Audio(LPVOID, uint32_t)			= 0;
	virtual void		OnAACSeqHdr(LPVOID, uint32_t)			= 0;
	virtual void		OnAVCSeqHdr(LPVOID, uint32_t)			= 0;
	virtual void		OnAudioStart(AMFVarPtr &)				= 0;
	virtual void		OnVideoStart(AMFVarPtr &)				= 0;
	virtual void		OnAudioPacket(PPacket *)				= 0;
	virtual void		OnVideoPacket(PPacket *)				= 0;
	virtual void		OnException(BCException &refExcept)		= 0;
};

///////////////////////////////////////////////////////////////////////////////
// Class : IAVPacket
///////////////////////////////////////////////////////////////////////////////

class RTMP_API IAVPacket
{
public:
	IAVPacket(void *lpData) : m_pData(lpData), m_nId(0){}
	virtual ~IAVPacket(){}

	virtual uint32_t		GetType()							= 0;
	virtual IAVPacket	*	Clone()								= 0;
	template<class T>	
	inline			T *	Cast(){
		return reinterpret_cast<T*>(m_pData);
	}
	template<class T>
	inline			T *	Release(){
		void *pData = m_pData;
		m_pData = NULL;
		return reinterpret_cast<T*>(pData);
	}

	void			*	m_pData;
	uint64_t			m_nId;
};

typedef enum IAVPacketType
{
	AVPKT_UNKNOWN			= 0,
	AVPKT_RTMPPACKET		= 1,
	AVPKT_RAWVIDEO			= 2,
	AVPKT_RAWAUDIO			= 3,
	AVPKT_EVENT				= 4,
	AVPKT_RAWFRAME			= 5,
}IAVPacketType;

//////////////////////////////////////////////////////////////////////////
/// class : RTMPPacket
//////////////////////////////////////////////////////////////////////////

class RTMP_API RTMPPacket : public IAVPacket
{
public:
	RTMPPacket(PPacket *pPkt) : IAVPacket(pPkt){}
	~RTMPPacket(){
		if (m_pData)
		{
			delete (PPacket *)m_pData;
			m_pData = NULL;
		}
	}
	uint32_t		GetType(){
		return AVPKT_RTMPPACKET;
	}
	IAVPacket	*	Clone(){
		return new RTMPPacket(new PPacket(*(PPacket *)m_pData));
	}
private:
	DECLARE_NO_COPY_CLASS(RTMPPacket);
};

//////////////////////////////////////////////////////////////////////////
/// class : RAWEvent
//////////////////////////////////////////////////////////////////////////

class RAWEvent : public IAVPacket
{
public:
	RAWEvent();
	RAWEvent(const BCEventItemS &refEvent);
	~RAWEvent();
	uint32_t		GetType(){
		return AVPKT_EVENT;
	}
	BCRESULT		Create();
	IAVPacket	*	Clone();

	BCEventItemS		m_sEvent;
private:
	DECLARE_NO_COPY_CLASS(RAWEvent);
};

//////////////////////////////////////////////////////////////////////////
/// class : RAWFrame
//////////////////////////////////////////////////////////////////////////

class RAWFrame : public IAVPacket
{
public:
	enum
	{
		BC_PIX_YUV420P			= 0,
		BC_PIX_YUVA420P			= 1,
	};
	enum
	{
		BC_SAMPLE_S16P			= 0,
	};
public:
	RAWFrame();
	RAWFrame(const RAWFrame &other);
	~RAWFrame();
	uint32_t		GetType(){
		return AVPKT_RAWFRAME;
	}
	BCRESULT			Create(
							uint8_t **data, 
							int *linesize, 
							int format, 
							int width, 
							int height);
	BCRESULT			Create(
							uint8_t **data, 
							int *linesize, 
							int format, 
							int channels, 
							int sampleRate, 
							int samples);
	IAVPacket		*	Clone();
	RAWFrame &			operator = (const RAWFrame &other);
	void				Reset();

	KBPool			pool;
	uint32_t		type;

#define BC_NUM_DATA_POINTERS 8
	/**
	* pointer to the picture/channel planes.
	* This might be different from the first allocated byte
	*
	* Some decoders access areas outside 0,0 - width,height, please
	* see avcodec_align_dimensions2(). Some filters and swscale can read
	* up to 16 bytes beyond the planes, if these filters are to be used,
	* then 16 extra bytes must be allocated.
	*
	* NOTE: Except for hwaccel formats, pointers not needed by the format
	* MUST be set to NULL.
	*/
	uint8_t *data[BC_NUM_DATA_POINTERS];

	/**
	* For video, size in bytes of each picture line.
	* For audio, size in bytes of each plane.
	*
	* For audio, only linesize[0] may be set. For planar audio, each channel
	* plane must be the same size.
	*
	* For video the linesizes should be multiples of the CPUs alignment
	* preference, this is 16 or 32 for modern desktop CPUs.
	* Some code requires such alignment other code can be slower without
	* correct alignment, for yet other it makes no difference.
	*
	* @note The linesize may be larger than the size of usable data -- there
	* may be extra padding present for performance reasons.
	*/
	int linesize[BC_NUM_DATA_POINTERS];

	/**
	* pointers to the data planes/channels.
	*
	* For video, this should simply point to data[].
	*
	* For planar audio, each channel has a separate data pointer, and
	* linesize[0] contains the size of each channel buffer.
	* For packed audio, there is just one data pointer, and linesize[0]
	* contains the total size of the buffer for all channels.
	*
	* Note: Both data and extended_data should always be set in a valid frame,
	* but for planar audio with more channels that can fit in data,
	* extended_data must be used in order to access all channels.
	*/
	uint8_t **extended_data;

	/**
	* width and height of the video frame
	*/
	int width, height;

	/**
	* number of audio samples (per channel) described by this frame
	*/
	int nb_samples;

	/**
	* format of the frame, -1 if unknown or unset
	* Values correspond to enum AVPixelFormat for video frames,
	* enum AVSampleFormat for audio)
	*/
	int format;

	/**
	* 1 -> keyframe, 0-> not
	*/
	int key_frame;

	/**
	* Presentation timestamp in time_base units (time when frame should be shown to user).
	*/
	int64_t pts;

	/**
	* DTS copied from the AVPacket that triggered returning this frame. (if frame threading isn't used)
	* This is also the Presentation time of this AVFrame calculated from
	* only AVPacket.dts values without pts values.
	*/
	int64_t pkt_dts;

	/**
	* quality (between 1 (good) and FF_LAMBDA_MAX (bad))
	*/
	int quality;

	/**
	* for some private data of the user
	*/
	void *opaque;

	/**
	* Sample rate of the audio data.
	*/
	int sample_rate;

	/**
	* Channel layout of the audio data.
	*/
	uint64_t channel_layout;

	/**
	* Frame flags, a combination of @ref lavu_frame_flags
	*/
	int flags;

	/**
	* number of audio channels, only used for audio.
	* Code outside libavutil should access this field using:
	* av_frame_get_channels(frame)
	* - encoding: unused
	* - decoding: Read by user.
	*/
	int channels;
private:
	
};

class IPacketHandler;

///////////////////////////////////////////////////////////////////////////////
// Class : IPacketSource
///////////////////////////////////////////////////////////////////////////////

class RTMP_API IPacketSource
{
public:
	typedef BCList<IPacketHandler *>		PacketHandlerQueue;
public:
	IPacketSource();
	virtual ~IPacketSource();

	virtual BCRESULT	AddDataListener(IPacketHandler *pHandler, bool cocall = true);
	virtual BCRESULT	RemoveDataListener(IPacketHandler *pHandler, bool cocall = true);
	virtual void		RemoveAllDataListeners();
	virtual void		PostSourceEvent(const BCEventItemS &refEvent) {}
protected:
	virtual void		SendPacketToListeners(IAVPacket *pPkt);
	virtual void		SendPacketToListeners(PPacket *pPkt);
	// Event handler
	virtual void		OnAddDataListener(IPacketHandler *pHandler) {}
	virtual void		OnRemoveDataListener(IPacketHandler *pHandler){}
	BCSpinMutex					m_sListenersLock;
	PacketHandlerQueue			m_vecDataListeners;
private:
	DECLARE_NO_COPY_CLASS(IPacketSource);
};

///////////////////////////////////////////////////////////////////////////////
// Class : IPacketHandler
///////////////////////////////////////////////////////////////////////////////

class RTMP_API IPacketHandler
{
public:
	IPacketHandler();
	virtual ~IPacketHandler();

	virtual void		HandlePacket(IAVPacket *lpPacket) = 0;
	virtual BCRESULT	AttachPacketSource(IPacketSource *pSrc, bool cocall = true);
	virtual IPacketSource	*	DetachPacketSource(bool cocall = true);
	inline bool			IsPacketSourceAttached()
	{
		return m_pPacketSource != NULL;
	}
protected:
	virtual BCRESULT	PostListenerEvent(const BCEventItemS &refEvent);
private:
	DECLARE_NO_COPY_CLASS(IPacketHandler);
	BCSpinMutex					m_sSourceLock;
	IPacketSource			*	m_pPacketSource;
};

///////////////////////////////////////////////////////////////////////////////
// Class : IHandlerGetter
///////////////////////////////////////////////////////////////////////////////

class RTMP_API IHandlerGetter
{
public:
	IHandlerGetter(){}
	virtual ~IHandlerGetter(){}

	virtual IPacketHandler		*	GetHandler()					= 0;
private:
	DECLARE_NO_COPY_CLASS(IHandlerGetter);
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_IHANDLER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : IHandler.h
///////////////////////////////////////////////////////////////////////////////
