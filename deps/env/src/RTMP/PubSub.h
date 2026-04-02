///////////////////////////////////////////////////////////////////////////////
// file : PubSub.h
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_PUBLISHER_H_INCLUDED__
#define RTMP_PUBLISHER_H_INCLUDED__

#include <BC/BCUserData.h>
#include <BC/BCFCodec.h>
#include <BC/BCOptions.h>
#include <BC/BCMap.h>
#include <RTMP/Exports.h>
#include <RTMP/PacketQueue.h>
#include <RTMP/IHandler.h>
#include <RTMP/TSGenerator.h>

namespace FLVUtils
{
	class IStreamHandler;
}
using namespace FLVUtils;

namespace BC
{
	class BCOptions;
	struct BCEventItemS;
}
using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

#define SENDTYPE_METADATA			0x00000001
#define SENDTYPE_AACSEQHDR			0x00000002
#define SENDTYPE_AVCSEQHDR			0x00000004
#define SENDTYPE_ALL				0x00000007
#define SENDTYPE_EMPTYAUDIO			0x00000008
#define SENDTYPE_COMMAND			0x00000010
#define SENDTYPE_USERCTRL			0x00000020

#define LVTYPE_LIVE					0x01
#define LVTYPE_VLIVE				0x02

#define PUBSUB_EVENT				2

enum{
	SUBEV_ON_LIVE_PLAY_PENDING			= 1000,
	SUBEV_ON_LIVE_PLAY_RESET			= 1001,
	SUBEV_ON_LIVE_PLAY_START			= 1002,
	SUBEV_ON_LIVE_PLAY_STOP				= 1003,
	SUBEV_ON_LIVE_PLAY_COMPLETE			= 1004,
	SUBEV_ON_LIVE_PLAY_UNPUBLISHNOTIFY	= 1005,
	SUBEV_ON_LIVE_PLAY_STREAMNOTFOUND	= 1006,
	PUBEV_ON_LIVE_PUBLISH_START			= 1007,
	PUBEV_ON_LIVE_PUBLISH_STOP			= 1008,
};

class PublishPipe;
class PublisherMgr;

///////////////////////////////////////////////////////////////////////////////
// class : Subscriber
///////////////////////////////////////////////////////////////////////////////

typedef uint32_t	SubStateE;

class Subscriber : public BCNodeList::Node
{
	//DECLARE_FIXED_ALLOC(Subscriber);

	friend class PublishPipe;
	friend class PublisherMgr;
public:
	Subscriber();
	virtual ~Subscriber();

	/**
	 * Below functions are used by NetStream
	 **/
	BCRESULT			Create(PublishPipe *pPipe, IStreamHandler *pHandler);
	void				Detach(Subscriber **ppSubscriber);
	BCRESULT			Play();
	void				Pause(bool bPause);
	PPacket			*	FeedPacket(PPacket *pPacket, uint32_t eExtraInfo);
	PPacket			*	SubscribePacket();
	void				Stop();
	void				StopFromPublisher();

	const BCPString	&	GetSubName() const;
	void				SetBufferLength(uint32_t nBufLen);
	uint32_t			GetBufferLength() const;
	void				SetBufferOverflow(uint32_t nBufLen);

	/**
	 * Below functions are used by PublishPipe
	 * PublishPipe must hold the lock of this subscriber
	 **/
	inline bool			IsInit() const { return m_bInit; };
	void				Init();
	void				NotifyPacket(PPacket *pPacket, uint32_t eExtraInfo);
	// Event handler
	void				OnConnectToPipe();
	void				OnDisconnectFromPipe();
protected:
private:
	DECLARE_NO_COPY_CLASS(Subscriber);
	BCSpinMutex				m_sLock;
	BCPString				m_strSubName;
	IStreamHandler		*	m_pHandler;
	PublishPipe			*	m_pPipe;
	uint32_t				m_nBufferLength;
	bool					m_bInit;
	SubStateE				m_nState;
	uint64_t				m_nCurrentIndex;
	PPacketQueue			m_lstAVPackets;
	uint32_t				m_nSendCount;
	bool					m_bSentAVCSeqHdr;
	bool					m_bPaused;
	bool					m_bNoCache;
	uint32_t				m_nBufferOverflow;
};

typedef TNodeList<Subscriber>		SubscriberList;

///////////////////////////////////////////////////////////////////////////////
// class : Publisher
///////////////////////////////////////////////////////////////////////////////

class RTMP_API Publisher 
	: public BCNodeList::Node
	, public BCUserData
{
	friend class PublishPipe;

	//DECLARE_FIXED_ALLOC(Publisher);
public:
	Publisher();
	virtual ~Publisher();

	BCRESULT			Create(
							PublishPipe *pPipe,
							IStreamHandler *pHandler);
	void				Detach(Publisher **ppPublisher);
	bool				Start();
	void				Publish(PPacket *pPacket);
	void				PublishAsync(PPacket *pPacket);
	void				Stop();
	LPCSTR				GetPubName() const;
	uint64_t			GetAbsTime() const;
	uint64_t			GetTotalTime() const;
	inline uint64_t		GetPubToken() const
	{
		return m_nToken;
	}
protected:
	void				_Cleanup();
	void				OnReleaseNotify();
	void				OnNewPublisher(Publisher *pPuber);
	void				OnNewSubscriber(Subscriber *pSuber);
	void				OnException(BCException &refExcept);
	// Event handler
	void				OnConnectToPipe();
	void				OnDisconnectFromPipe();
private:
	DECLARE_NO_COPY_CLASS(Publisher);
	PublishPipe			*	m_pPipe;
	uint64_t				m_nToken;
	IStreamHandler		*	m_pHandler;
};

typedef TNodeList<Publisher>	PublisherList;

///////////////////////////////////////////////////////////////////////////////
// class : PublishPipe
///////////////////////////////////////////////////////////////////////////////

typedef uint32_t		PubStateE;

class RTMP_API PublishPipe 
	: public BCEventFactory
	, public PAVQueueHandler
	, public IPacketHandler
	, public IPacketSource
	, public BCMagic
	, public BCNodeList::Node
{
	//DECLARE_FIXED_ALLOC(PublishPipe);

	friend class Subscriber;
	friend class PublisherMgr;

	typedef BCVector<IPacketHandler *>		PacketHandlerVector;

	enum { PUB_PACKET = 1, PUB_CLOSE = 2 };
public:
	PublishPipe();
	virtual ~PublishPipe();

	BCRESULT			Create(
							BCTaskMgr *pTaskMgr,
							PublisherMgr *pPubMgr,
							LPCSTR lpStreamName,
							uint64_t nPipeId,
							uint8_t eLiveType = LVTYPE_LIVE);
	void				ReleasePublishers();
	void				ReleaseSubscribers();
	bool				StartPublish();
	void				Publish(PPacket *pPacket);
	void				PublishAsync(PPacket *pPacket);
	BCRESULT			Add(Subscriber *pSubscriber);
	bool				Remove(Subscriber *pSubscriber);
	BCRESULT			Add(Publisher *pPublisher);
	bool				Remove(Publisher *pPublisher);
	const BCPString	&	GetPubName() const;
	inline uint64_t		GetAbsTime() const{
		return m_sPktAnalyzer.GetAbsRecordTime();
	}
	inline uint64_t		GetTotalTime() const{
		return m_sPktAnalyzer.GetDuration();
	}
	uint64_t			AllocateToken();
	uint64_t			GetPubToken();
	BCRESULT			SetPubToken(uint64_t nPubToken);
	bool				IsWorking() const;
	inline uint8_t		GetAVFlags()
	{
		return m_sPktAnalyzer.GetAVFlags();
	}
	uint32_t			GetPublishersCount();
	uint32_t			GetSubscribersCount();
	uint32_t			CreateTSSplitter(BCFObject *pConfig);
	BCRESULT			CloseTSGenerator(uint32_t nId);
	inline uint64_t		GetId() const
	{
		return m_nId;
	}
	inline uint32_t		GetRefCount() const
	{
		return m_nRef;
	}
	BCRESULT			AddDataListener(IPacketHandler *pHandler, bool cocall = true);
	BCRESULT			RemoveDataListener(IPacketHandler *pHandler, bool cocall = true);
	void				RemoveAllDataListeners();
	BCRESULT			AttachPacketSource(IPacketSource *pSrc, bool cocall = true);
	IPacketSource	*	DetachPacketSource(bool cocall = true);
	void				SetAVFilter(uint32_t eFilterFlags);
	void				Close();
	void				SetAVCache(bool bAVCache);
	void				Attach(PublishPipe **ppPipe);
	void				Detach(PublishPipe **ppPipe);
protected:
	inline void		_SetState(uint32_t eState, uint32_t nLineNumber)
	{
		m_eNewState = eState;
		m_nStateLineNo = nLineNumber;
	}
	void				_Inter_Publish(PPacket *pPacket);
	void				_Inter_PreSend(Subscriber *pSuber, uint32_t eSendType);
	void				_Inter_PreSend(IPacketHandler *pSuber, uint32_t eSendType);
	void				_Inter_PreSend(TSSplitter *pSplitter, uint32_t eSendType);
	void				_Inter_ReleasePublishers();
	void				_Inter_ReleaseSubscribers();
	// Override BCEventFactory interface
	bool				OnEventProcess(BCEventItemS &refEvent);
	static void			_PacketDtorCB(BCEventItemS &refEvent);
	void				_Cleanup();
	bool				_CloseCheck();

	void				OnMetaData(AMFVarPtr &pMetaData);
	void				OnCommonMetaData(
							PPacket *pPacket, 
							AMFVarPtr &pKey, 
							AMFVarPtr &pMetaData);
	void				OnMP3Audio(LPVOID lpData, uint32_t nSize);
	void				OnAACSeqHdr(LPVOID lpData, uint32_t nSize);
	void				OnAVCSeqHdr(LPVOID lpData, uint32_t nSize);
	void				OnAudioStart(AMFVarPtr &pMetaObj);
	void				OnVideoStart(AMFVarPtr &pMetaObj);
	void				OnAudioPacket(PPacket *pPacket);
	void				OnVideoPacket(PPacket *pPacket);
	void				OnRPCCall(PPacket *pPacket);
	void				OnUserCtrl(PPacket *pPacket);
	void				OnNewSubscriber(Subscriber *pSuber);
	void				OnNewPublisher(Publisher *pPuber);
	void				OnException(BCException &refExcept);
	void				HandlePacket(IAVPacket *pPacket);
	// Override BCEventFactory interfaces
	void				OnEventProcShutdown();
private:
	DECLARE_NO_COPY_CLASS(PublishPipe);
	BCSpinMutex				m_sLock;
	uint32_t				m_eState;
	uint32_t				m_eNewState;
	uint32_t				m_nStateLineNo;
	uint32_t				m_eCloseStatus;
	uint8_t					m_eLiveType;
	PublisherMgr		*	m_pPubMgr;
	BCPString				m_strPubName;
	SubscriberList			m_lstSubscribers;
	PacketAnalyzer			m_sPktAnalyzer;
	PubStateE				m_nState;
	PPacketList				m_lstInitPackets;
	uint64_t				m_nPubToken;
	uint64_t				m_nNextToken;
	PublisherList			m_lstPublishers;
	uint32_t				m_nRef; // Used by PublisherMgr
	uint64_t				m_nId;
	TSGenerator				m_sTSGenerator;
	bool					m_bAVCache;
};

typedef TNodeList<PublishPipe>		PublishPipeList;

///////////////////////////////////////////////////////////////////////////////
// class : PublishPipePtr
///////////////////////////////////////////////////////////////////////////////

class PublishPipePtr
{
public:
	PublishPipePtr(PublishPipe *pPipe);
	~PublishPipePtr();

	PublishPipe * operator->() {
		return m_pPipe;
	}
	PublishPipe *	Get() {
		return m_pPipe;
	}
	PublishPipe *	Release() {
		PublishPipe *pPipe = m_pPipe;
		m_pPipe = NULL;
		return pPipe;
	}
private:
	DECLARE_NO_COPY_CLASS(PublishPipePtr);
	PublishPipe			*	m_pPipe;
};

///////////////////////////////////////////////////////////////////////////////
// class : PublisherMgr
///////////////////////////////////////////////////////////////////////////////

typedef void (*PubStrmNameEnumPtr)(void *pArg, LPCSTR szName);
typedef PubStrmNameEnumPtr	LPFN_PubStrmNameEnumPtr;

class RTMP_API PublisherMgr : public BCOptionListener
{
	friend class PublishPipe;
public:
	PublisherMgr();
	~PublisherMgr();

	BCRESULT		Create(uint32_t nId);
	void			SetHandler(PublishHandler *pHandler);
	inline PublishHandler * GetHandler() const
	{
		return m_pHandler;
	}
	bool			IsStreamExist(LPCSTR lpName);
	BCRESULT		CreatePipe(
						LPCSTR lpName, 
						PublishPipe **ppPipe);
	BCRESULT		AttachPipe(PublishPipe *pSrcPipe, PublishPipe **ppDstPipe);
	bool			DetachPipe(PublishPipe **ppPipe);
#if 0
	BCRESULT		AttachPipe(
						LPCSTR lpName, 
						PublishPipe **ppPipe,
						uint8_t nMask = 0);
	// if bEnd equal to true, only decreate reference count is reference 
	// count of pipe == 1
	uint32_t		EnumPubStrmName(
						LPFN_PubStrmNameEnumPtr pFunc,
						void *pArg);
	BCRESULT		EnumPubStrmName(
						KBPool &refPool,
						LPCSTR *&lpszArray,
						uint32_t &refSize);
	uint32_t		GetLivePipesCount();
	uint32_t		GetVirtualLivePipesCount();
#endif // 0

	void			OnPipeClosed(PublishPipe *pPipe);
protected:
	void			OnOptionChanged(const BCPString &strKey, BCFVar *pValue);
	void			OnPeerChanged(PublishPipe *pPipe, uint32_t nCount, bool bPublisher);
private:
	BCSpinMutex			m_sLock;
	BCTaskMgr		*	m_pTaskMgr;
	uint32_t			m_nId;
	BCOptions		*	m_pOptions;
	char				m_szOptKey[MAX_PATH];
	BCHashTable			m_htPipes;
	SubscriberList		m_lstSubscribers;
	PublishHandler	*	m_pHandler;
	uint64_t			m_nNextId;
	PublishPipeList		m_lstWaitToDetach;
};

///////////////////////////////////////////////////////////////////////////////
// class : PublishCenter
///////////////////////////////////////////////////////////////////////////////

class RTMP_API PublishCenter
{
public:
	PublishCenter();
	~PublishCenter();

	static uint32_t			CreatePubMgr();
	static PublisherMgr	*	GetPubMgr(uint32_t nPublishId = 0);
	static void				PutPubMgr(uint32_t nPublishId);
#if 0
	static uint32_t			GetLivePipesCount();
	static uint32_t			GetVirtualLivePipesCount();
	static bool				IsStreamExist(LPCSTR lpName);
#endif // 0
	static uint32_t			GetLivePipesQuota();
	static uint32_t			GetVirtualLivePipesQuota();
	static void				SetLivePipesQuota(uint32_t nQuota);
	static void				SetVirtualLivePipesQuota(uint32_t nQuota);

protected:
private:
	DECLARE_NO_COPY_CLASS(PublishCenter);
	typedef BCMap<uint32_t, PublisherMgr *>	PubMgrMap;
	static BCSpinMutex		m_sLock;
	static PubMgrMap		m_mapPubMgrs;
	static uint32_t			m_nNextProbId;
	static PublisherMgr	*	m_pDefPubMgr;
	static uint32_t			m_nLiveQuota;
	static uint32_t			m_nVirtualLiveQuota;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_PUBLISHER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : PubSub.h
///////////////////////////////////////////////////////////////////////////////
