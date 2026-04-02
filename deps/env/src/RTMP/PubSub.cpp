///////////////////////////////////////////////////////////////////////////////
// file : PubSub.cpp
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////


#include "Precompile.h"
#include <RTMP/FLVUtils.h>
#include <RTMP/PubSub.h>



///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

#define _set_state(inst, _state, _status)	\
	(inst)->_SetState(_state, __LINE__);(inst)->m_eCloseStatus = _status

///////////////////////////////////////////////////////////////////////////////
// class : Subscriber
///////////////////////////////////////////////////////////////////////////////

#define SUBSTATE_INIT				0x00000000
#define SUBSTATE_METASENT			0x00000001
#define SUBSTATE_AVCSEQHDRSENT		0x00000002
#define SUBSTATE_AACSEQHDRSENT		0x00000004
#define SUBSTATE_MEDIASENDING		0x00000008

#define DEFAULT_BUFFER_LENGTH		5000
#define MIN_BUFFER_LENGTH			2000
#define MAX_BUFFER_LENGTH			10000

//IMPLEMENT_FIXED_ALLOC(Subscriber, 16);

Subscriber::Subscriber()
	: m_pHandler(NULL)
	, m_pPipe(NULL)
	, m_nBufferLength(DEFAULT_BUFFER_LENGTH)
	, m_bInit(false)
	, m_nState(SUBSTATE_INIT)
	, m_nCurrentIndex(0)
	, m_nSendCount(0)
	, m_bSentAVCSeqHdr(false)
	, m_bPaused(false)
	, m_bNoCache(true)
	, m_nBufferOverflow(0)
{
	//
}

Subscriber::~Subscriber()
{
	Stop();
}

BCRESULT Subscriber::Create(
	PublishPipe *pPipe, 
	IStreamHandler *pHandler)
{
	BCRESULT result;

	ASSERT(pPipe);
	ASSERT(pHandler);

	result = m_lstAVPackets.Create();
	if (result != BC_R_SUCCESS)
	{
		return result;
	}
	pPipe->Attach(&m_pPipe);
	m_pHandler = pHandler;
	return BC_R_SUCCESS;
}

void Subscriber::Detach(Subscriber **ppSubscriber)
{
	ASSERT(ppSubscriber != NULL);
	ASSERT(*ppSubscriber == this);

	Stop();
	if (m_pPipe)
	{
		m_pPipe->Detach(&m_pPipe);
	}
	delete this;
	*ppSubscriber = NULL;
}

BCRESULT Subscriber::Play()
{
	if (m_pPipe)
	{
		m_pPipe->Add(this);
		return BC_R_SUCCESS;
	}
	return BC_R_FAILURE;
}

void Subscriber::Pause(bool bPause)
{
	BCSpinMutex::Owner lock(m_sLock);
	m_bPaused = bPause;
	if (bPause)
	{
		AVDropStatS stats;
		m_lstAVPackets.Align(stats);
		if ((stats.nAudioPacketCount || stats.nVideoPacketCount) && m_pHandler)
		{
			m_pHandler->OnStreamBufferEvent(BUFFER_EVENT_DROP, stats);
		}
	}
	else
	{
		PPacket *pPacket;

		if (!m_bPaused && !m_nSendCount && (pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
		{
			ScopedPointer<PPacket> pDtor(pPacket);
			{
				m_pHandler->OnStreamPacketReady(*pPacket, READYTYPE_PLAY_LIVE);
			}
			m_nSendCount++;
		}
	}
}

PPacket* Subscriber::FeedPacket(PPacket *pPacket, uint32_t eExtraInfo)
{
	PPacket* pRetPacket = NULL;

	BCSpinMutex::Owner lock(m_sLock);

	switch(eExtraInfo)
	{
	case SENDTYPE_METADATA:
		m_lstAVPackets.Cleanup();
	case SENDTYPE_AACSEQHDR:
	case SENDTYPE_AVCSEQHDR:
	case SENDTYPE_COMMAND:
	case SENDTYPE_USERCTRL:
		{
			PHeader &refHeader = *pPacket->m_pHeader;
			if (refHeader.m_eDataType == FLVUtils::FLV_TAG_TYPE_VIDEO)
			{
			//	if (!m_pDumpFD)
			//	{
			//		BCPString strName(m_strSubName.c_str());
			//		strName << "-subscriber-recv-reference-" << (double)bc_time_now();
			//		m_pDumpFD = ::fopen(strName.c_str(), "wb");
			//	}
			//	if (m_pDumpFD)
			//	{
			//		BCPString strMsg;
			//		strMsg.Format("packet:%p;    key frame:%d;    packet size:%d;    timestamp:%d;    seqheader:1\n",
			//			pPacket->packet.Get(), refHeader.IsVKFrame(),
			//			pPacket->packet->m_pPacket->m_sBody.UsedLength(),
			//			refHeader.m_nTimestamp);
			//		::fwrite(strMsg.c_str(), strMsg.length(), 1, m_pDumpFD);
			//	}
				m_bSentAVCSeqHdr = true;
			}

			//ScopedPointer<PPacket> pDtor(pPacket);
			m_nSendCount++;
			//{
			//	m_pHandler->OnStreamPacketReady(*pPacket, READYTYPE_PLAY_LIVE);
			//}
			pRetPacket = pPacket;
		}
		break;
	default:
		{
			uint32_t nBufferLength = 0;
			AVDropStatS stats;
			m_lstAVPackets.PushBack(pPacket);
			// Flush time out packets
			if (pPacket->m_pHeader->IsVKFrame() && m_lstAVPackets.Count() > 1)
			{
				m_lstAVPackets.FlushTimeout(m_nBufferLength, stats);
			}
			else if (!(AVINFO_HAS_VIDEO & m_lstAVPackets.GetFlags()))
			{
				m_lstAVPackets.FlushTimeout(m_nBufferLength, stats);
			}
			if ((stats.nAudioPacketCount || stats.nVideoPacketCount) && m_pHandler)
			{
				m_pHandler->OnStreamBufferEvent(BUFFER_EVENT_FULL, stats);
			}

			if (!m_bPaused && !m_nSendCount && (pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
			{
				//ScopedPointer<PPacket> pDtor(pPacket);
				m_nSendCount++;
				//{
				//	m_pHandler->OnStreamPacketReady(*pPacket, READYTYPE_PLAY_LIVE);
				//}
				pRetPacket = pPacket;
			}
			nBufferLength = m_lstAVPackets.GetDuration();
			if (m_nBufferOverflow > 0 && nBufferLength > m_nBufferOverflow && m_pHandler)
			{
				uint64_t audioDuration, videoDuration;

				m_lstAVPackets.GetDuration(videoDuration, audioDuration);
				BCFObject *pInfo = new BCFObject();
				pInfo->PutInt("audioBufferLength", audioDuration);
				pInfo->PutInt("videoBufferLength", videoDuration);
				pInfo->PutInt("bufferLength", nBufferLength);
				m_pHandler->OnStreamBufferOverflow(pInfo);
			}
		}
		break;
	}
	return pRetPacket;
}

PPacket* Subscriber::SubscribePacket()
{
	PPacket *pPacket;

	BCSpinMutex::Owner lock(m_sLock);
	if (m_nSendCount > 0)
	{
		m_nSendCount--;
	}
	if (!m_bPaused && !m_nSendCount && (pPacket = m_lstAVPackets.PopAndSendFront()) != NULL)
	{
		//ScopedPointer<PPacket> pDtor(pPacket);
		m_nSendCount++;
		//{
		//	m_pHandler->OnStreamPacketReady(*pPacket, READYTYPE_PLAY_LIVE);
		//}
		return pPacket;
	}
	return NULL;
}

void Subscriber::Stop()
{
	if (m_pPipe)
	{
		m_pPipe->Remove(this);
	}
}

void Subscriber::StopFromPublisher()
{
	if (m_pHandler)
	{
		m_pHandler->OnStreamStopNotify();
	}
}

const BCPString &Subscriber::GetSubName() const
{
	return m_strSubName;
}

void Subscriber::SetBufferLength(uint32_t nBufLen)
{
	if (nBufLen < MIN_BUFFER_LENGTH)
	{
		m_nBufferLength = MIN_BUFFER_LENGTH;
	}
	else if (nBufLen > MAX_BUFFER_LENGTH)
	{
		m_nBufferLength = MAX_BUFFER_LENGTH;
	}
	else
	{
		m_nBufferLength = nBufLen;
	}
}

uint32_t Subscriber::GetBufferLength() const
{
	return m_nBufferLength;
}

void Subscriber::SetBufferOverflow(uint32_t nBufLen)
{
	m_nBufferOverflow = nBufLen;
}

void Subscriber::Init()
{
	/**
	 * Caller must hold the lock of this subscriber
	 */
	m_pHandler->OnStreamPlayLiveInit();
	m_bInit = true;
}

void Subscriber::NotifyPacket(PPacket *pPacket, uint32_t eExtraInfo)
{
	/**
	 * Caller must hold the lock of this subscriber
	 */
	if (m_pHandler)
	{
		m_pHandler->OnStreamPublishPacket(pPacket, eExtraInfo);
	}
}

void Subscriber::OnConnectToPipe()
{
	if (m_pHandler)
	{
		BCEventItemS sEvent(MAKEEVENT(SUBEV_ON_LIVE_PLAY_START, 0, 0));
		m_pHandler->PostStreamEvent(sEvent);
	}
}

void Subscriber::OnDisconnectFromPipe()
{

}

///////////////////////////////////////////////////////////////////////////////
// Class : Publisher
///////////////////////////////////////////////////////////////////////////////

//IMPLEMENT_FIXED_ALLOC(Publisher, 16);

Publisher::Publisher()
	: m_pPipe(NULL)
	, m_nToken(0)
	, m_pHandler(NULL)
{
	//
}

Publisher::~Publisher()
{
	//
}

BCRESULT Publisher::Create(
	PublishPipe *pPipe,
	IStreamHandler *pHandler)
{
	if (!pPipe || !pHandler)
	{
		return BC_R_INVALIDARG;
	}

	m_pHandler = pHandler;
	pPipe->Attach(&m_pPipe);
	m_nToken = m_pPipe->AllocateToken();

	return BC_R_SUCCESS;
}

void Publisher::Detach(Publisher **ppPublisher)
{
	ASSERT(ppPublisher != NULL);
	ASSERT(*ppPublisher == this);

	Stop();
	_Cleanup();
	if (m_pPipe)
	{
		m_pPipe->Detach(&m_pPipe);
	}

	delete this;
	*ppPublisher = NULL;
}

bool Publisher::Start()
{
	if (m_pPipe)
	{
		m_pPipe->Add(this);
		return true;
	}
	return false;
}

#define USE_MULTI_PUBLISHER

void Publisher::Publish(PPacket *pPacket)
{
	ScopedPointer<PPacket> dtor(pPacket);
	if (m_pPipe && m_nToken == m_pPipe->GetPubToken())
	{
		m_pPipe->Publish(dtor.Release());
	}
}

void Publisher::PublishAsync(PPacket *pPacket)
{
	ScopedPointer<PPacket> dtor(pPacket);
	if (m_pPipe && m_nToken == m_pPipe->GetPubToken())
	{
		m_pPipe->PublishAsync(dtor.Release());
	}
}

void Publisher::Stop()
{
	if (m_pPipe)
	{
		m_pPipe->Remove(this);
	}
}

LPCSTR Publisher::GetPubName() const
{
	if (m_pPipe)
	{
		return m_pPipe->GetPubName();
	}
	return NULLPSTRING;
}

uint64_t Publisher::GetAbsTime() const
{
	if (m_pPipe)
	{
		return m_pPipe->GetAbsTime();
	}
	return 0;
}

uint64_t Publisher::GetTotalTime() const
{
	if (m_pPipe)
	{
		return m_pPipe->GetTotalTime();
	}
	return 0;
}

void Publisher::_Cleanup()
{
}

void Publisher::OnReleaseNotify()
{
	if (m_pHandler)
	{
		m_pHandler->OnStreamReleaseNotify();
	}
}

void Publisher::OnNewPublisher(Publisher *pPuber)
{
	if (m_pHandler)
	{
		m_pHandler->OnStreamNewPublisher(pPuber);
	}
}

void Publisher::OnNewSubscriber(Subscriber *pSuber)
{
	if (m_pHandler)
	{
		m_pHandler->OnStreamNewSubscriber(pSuber);
	}
}

void Publisher::OnException(BCException &refExcept)
{
	if (m_pHandler)
	{
		m_pHandler->OnException(refExcept);
	}
}

void Publisher::OnConnectToPipe()
{
	BCEventItemS sEvent(MAKEEVENT(PUBEV_ON_LIVE_PUBLISH_START, 0, 0));
	m_pHandler->PostStreamEvent(sEvent);
}

void Publisher::OnDisconnectFromPipe()
{

}

///////////////////////////////////////////////////////////////////////////////
// class : PublishPipe
///////////////////////////////////////////////////////////////////////////////

#define PUBSTATE_INIT				0x00000000
#define PUBSTATE_METARECV			0x00000001
#define PUBSTATE_AVCSEQHDRRECV		0x00000002
#define PUBSTATE_AACSEQHDRRECV		0x00000004
#define PUBSTATE_MEDIARECVING		0x00000008
#define PUBSTATE_AUDIOSTARTED		0x00000010
#define PUBSTATE_VIDEOSTARTED		0x00000020


#define PUBPIPE_MAGIC		BC_MAGIC('P', 'U', 'B', 'P')
#define VALID_PUBPIPE(t)	BC_MAGIC_VALID(t, PUBPIPE_MAGIC)

enum
{
	PUBSTATE_FREED			= 0,
	PUBSTATE_DEAD			= 1,
	PUBSTATE_WORKING		= 2,
	PUBSTATE_MAX			= 3,
};

//IMPLEMENT_FIXED_ALLOC(PublishPipe, 16);

PublishPipe::PublishPipe()
	: BCMagic(PUBPIPE_MAGIC)
	, m_eState(PUBSTATE_FREED)
	, m_eNewState(PUBSTATE_FREED)
	, m_nStateLineNo(0)
	, m_eCloseStatus(BC_R_SUCCESS)
	, m_eLiveType(LVTYPE_LIVE)
	, m_pPubMgr(NULL)
	, m_nState(PUBSTATE_INIT)
	, m_nPubToken(1)
	, m_nNextToken(1)
	, m_nRef(0)
	, m_nId(0)
	, m_bAVCache(true)
{
	//
}

PublishPipe::~PublishPipe()
{
	_Cleanup();
	m_sTSGenerator.Stop();
	BCEventFactory::_FlushEvents();
	BCEventFactory::Detach();
}

BCRESULT PublishPipe::Create(
	BCTaskMgr *pTaskMgr,
	PublisherMgr *pPubMgr,
	LPCSTR lpName,
	uint64_t nPipeId,
	uint8_t eLiveType)
{
	BCRESULT result;

	m_eLiveType			= eLiveType;
	m_pPubMgr			= pPubMgr;
	m_nId				= nPipeId;
	m_strPubName		= lpName;
	m_sPktAnalyzer.Create(lpName, this);
	m_nRef = 0;
	m_sTSGenerator.Create();
	result = BCEventFactory::Create(pTaskMgr, "PublishPipe", this);
	if (result == BC_R_SUCCESS)
	{
		m_eState = PUBSTATE_WORKING;
		m_eNewState = PUBSTATE_MAX;
	}

	return result;
}

void PublishPipe::ReleasePublishers()
{
	BCSpinMutex::Owner lock(m_sLock);

	_Inter_ReleasePublishers();
}

void PublishPipe::ReleaseSubscribers()
{
	BCSpinMutex::Owner lock(m_sLock);
	
	_Inter_ReleaseSubscribers();
}

bool PublishPipe::StartPublish()
{
	BCSpinMutex::Owner lock(m_sLock);
	return true;
}

/*
* We MUST NOT use link list to maintain publish packets, since packets will
* be reused, if a packet was recycled and reused immadiately, the content
* in the packet was updated, but the order number in subscribers subscribe
* list was not updated which cause the wrong packets to be sent.
*/

void PublishPipe::Publish(PPacket *pPacket)
{
	// Acquire publisher lock, don't allow any subscriber to
	// unregister itself from subscribers list
	BCSpinMutex::Owner lock(m_sLock);

	_Inter_Publish(pPacket);
}

void PublishPipe::PublishAsync(PPacket *pPacket)
{
	BCEventFactory::PostEvent(MAKEEVENT(PUB_PACKET, 0, 0), 
		pPacket, NULL, _PacketDtorCB);
}

const BCPString &PublishPipe::GetPubName() const
{
	return m_strPubName;
}

BCRESULT PublishPipe::Add(Subscriber *pSubscriber)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_lstSubscribers.IsExist(pSubscriber))
	{
		return BC_R_EXISTS;
	}
	m_lstSubscribers.PushBack(pSubscriber);
	pSubscriber->OnConnectToPipe();
	_Inter_PreSend(pSubscriber, SENDTYPE_ALL);
	OnNewSubscriber(pSubscriber);
	m_pPubMgr->OnPeerChanged(this, m_lstPublishers.Count(), false);
	return BC_R_SUCCESS;
}

bool PublishPipe::Remove(Subscriber *pSubscriber)
{
	BCSpinMutex::Owner lock(m_sLock);
	pSubscriber->RemoveFromList();
	pSubscriber->OnDisconnectFromPipe();
	m_pPubMgr->OnPeerChanged(this, m_lstPublishers.Count(), false);
	_CloseCheck();
	return true;
}

BCRESULT PublishPipe::Add(Publisher *pPublisher)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (m_lstPublishers.IsExist(pPublisher))
	{
		return BC_R_EXISTS;
	}
	m_lstPublishers.PushBack(pPublisher);
	pPublisher->OnConnectToPipe();
	OnNewPublisher(pPublisher);
	m_pPubMgr->OnPeerChanged(this, m_lstPublishers.Count(), true);
	return true;
}

bool PublishPipe::Remove(Publisher *pPublisher)
{
	BCSpinMutex::Owner lock(m_sLock);
	pPublisher->RemoveFromList();
	pPublisher->OnDisconnectFromPipe();
	m_pPubMgr->OnPeerChanged(this, m_lstPublishers.Count(), true);
	//if (!m_lstPublishers.Count() && m_eLiveType == LVTYPE_LIVE)
	//{
	//	Subscriber *pSuber, *pIterEnd;

	//	pSuber = m_lstSubscribers.Begin();
	//	pIterEnd = m_lstSubscribers.End();
	//	for(;pSuber != pIterEnd;pSuber = m_lstSubscribers.Next(pSuber))
	//	{
	//		pSuber->StopFromPublisher();
	//	}
	//}
	_CloseCheck();
	
	return true;
}

uint64_t PublishPipe::AllocateToken()
{
	BCSpinMutex::Owner lock(m_sLock);
	return m_nNextToken++;
}

uint64_t PublishPipe::GetPubToken()
{
	BCSpinMutex::Owner lock(m_sLock);
	return m_nPubToken;
}

BCRESULT PublishPipe::SetPubToken(uint64_t nPubToken)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (nPubToken >= m_nNextToken)
	{
		return BC_R_INVALIDARG;
	}
	m_nPubToken = nPubToken;
	_Cleanup();
	return BC_R_SUCCESS;
}

uint32_t PublishPipe::GetPublishersCount()
{
	BCSpinMutex::Owner lock(m_sLock);
	return m_lstPublishers.Count();
}

uint32_t PublishPipe::GetSubscribersCount()
{
	BCSpinMutex::Owner lock(m_sLock);
	return m_lstSubscribers.Count();
}

uint32_t PublishPipe::CreateTSSplitter(BCFObject *pConfig)
{
	BCSpinMutex::Owner lock(m_sLock);
	uint32_t nId = m_sTSGenerator.AddSplitter(pConfig, m_pPubMgr->GetHandler(), m_nId);
	if (nId)
	{
		TSSplitter *pSplitter = m_sTSGenerator.GetSplitterById(nId);
		if (pSplitter)
		{
			_Inter_PreSend(pSplitter, SENDTYPE_ALL);
		}
	}
	return nId;
}

BCRESULT PublishPipe::CloseTSGenerator(uint32_t nId)
{
	BCSpinMutex::Owner lock(m_sLock);
	return m_sTSGenerator.RemoveSplitter(nId);
}

BCRESULT PublishPipe::AddDataListener(IPacketHandler *pHandler, bool cocall)
{
	BCRESULT result;

	if (!pHandler || pHandler == this)
	{
		return BC_R_INVALIDARG;
	}
	else
	{
		BCSpinMutex::Owner lock(m_sLock);
		_Inter_PreSend(pHandler, SENDTYPE_ALL);
		result = IPacketSource::AddDataListener(pHandler, false);
	}
	if (result == BC_R_SUCCESS && cocall)
	{
		result = pHandler->AttachPacketSource(this, false);
	}
	return result;
}

BCRESULT PublishPipe::RemoveDataListener(IPacketHandler *pHandler, bool cocall)
{
	BCRESULT result;

	if (!pHandler || pHandler == this)
	{
		return BC_R_INVALIDARG;
	}
	else
	{
		BCSpinMutex::Owner lock(m_sLock);
		result = IPacketSource::RemoveDataListener(pHandler, false);
	}
	if (result == BC_R_SUCCESS && cocall)
	{
		pHandler->DetachPacketSource(false);
	}
	return result;
}

void PublishPipe::RemoveAllDataListeners()
{
	BCSpinMutex::Owner lock(m_sLock);
	IPacketSource::RemoveAllDataListeners();
}

BCRESULT PublishPipe::AttachPacketSource(IPacketSource *pSrc, bool cocall)
{
	BCRESULT result;

	if (!pSrc || pSrc == this)
	{
		return BC_R_INVALIDARG;
	}
	else if (cocall)
	{
		result = pSrc->AddDataListener(this, false);
	}
	else
	{
		result = BC_R_SUCCESS;
	}
	if (result == BC_R_SUCCESS)
	{
		BCSpinMutex::Owner lock(m_sLock);
		result = IPacketHandler::AttachPacketSource(pSrc, false);
	}
	return result;
}

IPacketSource *PublishPipe::DetachPacketSource(bool cocall)
{
	BCRESULT result;
	IPacketSource *pSrc = NULL;

	{
		BCSpinMutex::Owner lock(m_sLock);
		pSrc = IPacketHandler::DetachPacketSource(false);
	}
	if (cocall && pSrc)
	{
		result = pSrc->RemoveDataListener(this, false);
	}
	return pSrc;
}

void PublishPipe::Close()
{
	PostEvent(MAKEEVENT(PUB_CLOSE, 0, 0));
}

void PublishPipe::SetAVCache(bool bAVCache)
{
	BCSpinMutex::Owner lock(m_sLock);
	m_bAVCache = bAVCache;
}

void PublishPipe::Attach(PublishPipe **ppPipe)
{
	ASSERT(m_pPubMgr && ppPipe);
	m_pPubMgr->AttachPipe(this, ppPipe);
}

void PublishPipe::Detach(PublishPipe **ppPipe)
{
	ASSERT(m_pPubMgr && ppPipe && *ppPipe);
	m_pPubMgr->DetachPipe(ppPipe);
}

void PublishPipe::_Inter_Publish(PPacket *pPacket)
{
	ScopedPointer<PPacket> dtor(pPacket);

	// Queue packet
	switch (pPacket->m_pHeader->m_eDataType)
	{
	case MTYPE_METADATA:
	case MTYPE_AUDIODATA:
	case MTYPE_VIDEODATA:
		{
			// Notify data handlers
			PPacket *pDupPacket = new PPacket(*pPacket);
			pDupPacket->ParseHeader();
			SendPacketToListeners(pDupPacket);
			// Analyze after notify data handlers to avoid data usage conflict
			m_sPktAnalyzer.Analyze(dtor.Release(), 1);
		}
		break;
	case MTYPE_USERCTRLEVENT:
		OnUserCtrl(dtor.Release());
		break;
	case MTYPE_COMMAND:
		OnRPCCall(dtor.Release());
		break;
	}
}

void PublishPipe::_Inter_PreSend(Subscriber *pSuber, uint32_t eSendType)
{
	PPacket *pPacket;

	// Metadata
	if (eSendType & SENDTYPE_METADATA)
	{
#if 0
		// Debug
		LogInfo(_LOCAL_, "____Type : %2d; metadata timestamp : %6d; "
			"datasize : %6d; absolutetime : %d; total time : %I64d.\n ",
			pPacket->m_pHeader->m_eDataType,
			pPacket->m_pHeader->m_nTimestamp,
			pPacket->m_pHeader->m_nDataSize,
			pPacket->m_pHeader->m_bAbsoluteTime,
			m_lstAVPackets.GetDuration());
#endif // 0
		ScopedPointer<PPacket> dtor(new PPacket());
		BCRESULT result = m_sPktAnalyzer.CreateMetaData(*dtor.Get());
		if (BC_R_SUCCESS == result)
		{
			pSuber->NotifyPacket(dtor.Release(), SENDTYPE_METADATA);
		}
	}
	// AVC Sequence header
	if (eSendType & SENDTYPE_AVCSEQHDR)
	{
		pPacket = m_sPktAnalyzer.GetVidSeqHdr();
		if (pPacket)
		{
#if 0
			// Debug
			LogInfo(_LOCAL_, "____Type : %2d; videoseq timestamp : %6d; "
				"datasize : %6d; absolutetime : %d; total audio : %I64d.\n",
				pPacket->m_pHeader->m_eDataType,
				pPacket->m_pHeader->m_nTimestamp,
				pPacket->m_pHeader->m_nDataSize,
				pPacket->m_pHeader->m_bAbsoluteTime,
				m_lstAVPackets.GetDuration());
#endif // 0
			pSuber->NotifyPacket(pPacket, SENDTYPE_AVCSEQHDR);
		}
	}
	// AAC Sequence header
	if (eSendType & SENDTYPE_AACSEQHDR)
	{
		pPacket = m_sPktAnalyzer.GetAudSeqHdr();
		if (pPacket)
		{
#if 0
			// Debug
			LogInfo(_LOCAL_, "____Type : %2d; audioseq timestamp : %6d; "
				"datasize : %6d; absolutetime : %d; total audio : %I64d.\n",
				pPacket->m_pHeader->m_eDataType,
				pPacket->m_pHeader->m_nTimestamp,
				pPacket->m_pHeader->m_nDataSize,
				pPacket->m_pHeader->m_bAbsoluteTime,
				m_lstAVPackets.GetDuration());
#endif // 0
			pSuber->NotifyPacket(pPacket, SENDTYPE_AACSEQHDR);
		}
	}
}

void PublishPipe::_Inter_PreSend(IPacketHandler *pHandler, uint32_t eSendType)
{
	PPacket *pPacket;

	// Metadata
	if (eSendType & SENDTYPE_METADATA)
	{
		ScopedPointer<PPacket> pkt(new PPacket());
		BCRESULT result = m_sPktAnalyzer.CreateMetaData(*pkt.Get());
		if (BC_R_SUCCESS == result)
		{
			pHandler->HandlePacket(new RTMPPacket(pkt.Release()));
		}
	}
	// AVC Sequence header
	if (eSendType & SENDTYPE_AVCSEQHDR)
	{
		pPacket = m_sPktAnalyzer.GetVidSeqHdr();
		if (pPacket)
		{
			pHandler->HandlePacket(new RTMPPacket(pPacket));
		}
	}
	// AAC Sequence header
	if (eSendType & SENDTYPE_AACSEQHDR)
	{
		pPacket = m_sPktAnalyzer.GetAudSeqHdr();
		if (pPacket)
		{
			pHandler->HandlePacket(new RTMPPacket(pPacket));
		}
	}
	if (m_bAVCache && m_lstInitPackets.Count() > 0)
	{
		PPacket *pIter, *pEnd;

		pIter = m_lstInitPackets.Begin();
		pEnd = m_lstInitPackets.End();
		for (; pIter != pEnd; pIter = m_lstInitPackets.Next(pIter))
		{
			ScopedPointer<PPacket> pkt(new PPacket(*pIter, true));
			pHandler->HandlePacket(new RTMPPacket(pkt.Release()));
		}
	}
}

void PublishPipe::_Inter_PreSend(TSSplitter *pSplitter, uint32_t eSendType)
{
	PPacket *pPacket;

	// AVC Sequence header
	if (eSendType & SENDTYPE_AVCSEQHDR)
	{
		pSplitter->OnVideoStart(m_sPktAnalyzer.GetAVFlags());
		pPacket = m_sPktAnalyzer.GetVidSeqHdr(false);
		if (pPacket)
		{
			LPBYTE lpData = (LPBYTE)pPacket->m_sBody.Base();
			uint32_t nSize = pPacket->m_sBody.UsedLength();
			pSplitter->OnAVCSeqHdr(lpData, nSize);
		}
	}
	// AAC Sequence header
	if (eSendType & SENDTYPE_AACSEQHDR)
	{
		pSplitter->OnAudioStart(m_sPktAnalyzer.GetAVFlags());
		pPacket = m_sPktAnalyzer.GetAudSeqHdr(false);
		if (pPacket)
		{
			LPBYTE lpData = (LPBYTE)pPacket->m_sBody.Base();
			uint32_t nSize = pPacket->m_sBody.UsedLength();
			pSplitter->OnAACSeqHdr(lpData, nSize);
		}
	}
	if (m_bAVCache && m_lstInitPackets.Count() > 0)
	{
		PPacket *pIter, *pEnd;

		pIter = m_lstInitPackets.Begin();
		pEnd = m_lstInitPackets.End();
		for (; pIter != pEnd; pIter = m_lstInitPackets.Next(pIter))
		{
			switch (pIter->m_pHeader->m_eDataType)
			{
			case MTYPE_AUDIODATA:
				pSplitter->OnAudioPacket(new PPacket(*pIter));
				break;
			case MTYPE_VIDEODATA:
				pSplitter->OnVideoPacket(new PPacket(*pIter));
				break;
			default:
				break;
			}
		}
	}
}

bool PublishPipe::OnEventProcess(BCEventItemS &refEvent)
{
	BCSpinMutex::Owner lock(m_sLock);
	if (_CloseCheck())
	{
		return true;
	}
	switch(EVENTMAJOR(refEvent.eType))
	{
	case PUB_PACKET:
		if (m_eState >= PUBSTATE_WORKING && refEvent.wParam)
		{
			_Inter_Publish((PPacket *)refEvent.wParam);
			refEvent.wParam = 0;// Avoid twice delete
		}
		break;
	case PUB_CLOSE:
		{
			_set_state(this, PUBSTATE_FREED, BC_R_SUCCESS);
		}
		break;
	default:
		BCDefEventProc(refEvent);
		break;
	}
	(void)_CloseCheck();
	return true;
}

void PublishPipe::_PacketDtorCB(BCEventItemS &refEvent)
{
	if (refEvent.wParam)
	{
		delete (PPacket *)refEvent.wParam;
		refEvent.wParam = 0;
	}
}

void PublishPipe::_Cleanup()
{
	PPacket *pPkt;

	while((pPkt = m_lstInitPackets.PopFront()) != NULL)
	{
		BC_SAFE_DELETE_PTR(pPkt);
	}
}

bool PublishPipe::_CloseCheck()
{
	if (m_eState <= m_eNewState)
	{
		return false;
	}

	if (m_eState == PUBSTATE_WORKING)
	{
		if (m_lstPublishers.Count() > 0)
		{
			_Inter_ReleasePublishers();
			return true;
		}
		if (m_lstSubscribers.Count() > 0)
		{
			_Inter_ReleaseSubscribers();
			return true;
		}
		m_eState = PUBSTATE_DEAD;
	}

	if (m_eState == PUBSTATE_DEAD)
	{
		if (m_nCtrls > 0)
		{
			return true;
		}
		_Cleanup();
		BCEventFactory::Detach();
		m_eState = PUBSTATE_FREED;
	}

	return true;
}

void PublishPipe::OnMetaData(AMFVarPtr &pMetaData)
{
	Subscriber *pSuber, *pIterEnd;
	PPacket sPacket;
	m_sPktAnalyzer.CreateMetaData(sPacket);

	// Clean up last movie cached data
	_Cleanup();
	m_nState |= PUBSTATE_METARECV;
	if (m_pPubMgr && m_pPubMgr->GetHandler())
	{
		m_pPubMgr->GetHandler()->OnMetaData(this, new AMFVarWrapper(pMetaData));
	}
	// Notify subscribers
	pSuber = m_lstSubscribers.Begin();
	pIterEnd = m_lstSubscribers.End();
	for(;pSuber != pIterEnd;pSuber = m_lstSubscribers.Next(pSuber))
	{
		pSuber->NotifyPacket(new PPacket(sPacket, true), SENDTYPE_METADATA);
	}
}

void PublishPipe::OnCommonMetaData(
	PPacket *pPacket, 
	AMFVarPtr &pKey, 
	AMFVarPtr &pMetaData)
{
	Subscriber *pSuber, *pSuberEnd;
	ScopedPointer<PPacket> dtor(pPacket);

	// Send packet to subscribers
	if (m_lstSubscribers.Count() > 0)
	{
		// Notify subscribers for new packet incoming
		pSuber = m_lstSubscribers.Begin();
		pSuberEnd = m_lstSubscribers.End();
		for (; pSuber != pSuberEnd; pSuber = m_lstSubscribers.Next(pSuber))
		{
			pSuber->NotifyPacket(new PPacket(*dtor.Get(), true), 0);
		}
	}
}

void PublishPipe::OnMP3Audio(LPVOID lpData, uint32_t nSize)
{
	m_sTSGenerator.OnMP3Audio(lpData, nSize);
}

void PublishPipe::OnAACSeqHdr(LPVOID lpData, uint32_t nSize)
{
	Subscriber *pSuber, *pIterEnd;

	m_nState |= PUBSTATE_AACSEQHDRRECV;
	if (m_pPubMgr && m_pPubMgr->GetHandler())
	{
		m_pPubMgr->GetHandler()->OnAACSeqHdr(this, lpData, nSize);
	}

	PPacket *pPacket = m_sPktAnalyzer.GetAudSeqHdr();
	if (pPacket)
	{
		ScopedPointer<PPacket> dtor(pPacket);
		// Notify subscribers
		pSuber = m_lstSubscribers.Begin();
		pIterEnd = m_lstSubscribers.End();
		for(;pSuber != pIterEnd;pSuber = m_lstSubscribers.Next(pSuber))
		{
			pSuber->NotifyPacket(new PPacket(*pPacket, true), SENDTYPE_AACSEQHDR);
		}
	}
	m_sTSGenerator.OnAACSeqHdr(lpData, nSize);
}

void PublishPipe::OnAVCSeqHdr( LPVOID lpData, uint32_t nSize )
{
	Subscriber *pSuber, *pIterEnd;

	m_nState |= PUBSTATE_AVCSEQHDRRECV;
	if (m_pPubMgr && m_pPubMgr->GetHandler())
	{
		m_pPubMgr->GetHandler()->OnAVCSeqHdr(this, lpData, nSize);
	}

	PPacket* pPacket = m_sPktAnalyzer.GetVidSeqHdr();
	if (pPacket)
	{
		ScopedPointer<PPacket> dtor(pPacket);
		// Notify subscribers
		pSuber = m_lstSubscribers.Begin();
		pIterEnd = m_lstSubscribers.End();
		for(;pSuber != pIterEnd;pSuber = m_lstSubscribers.Next(pSuber))
		{
			pSuber->NotifyPacket(new PPacket(*pPacket, true), SENDTYPE_AVCSEQHDR);
		}
	}
	m_sTSGenerator.OnAVCSeqHdr(lpData, nSize);
}

void PublishPipe::OnAudioStart(AMFVarPtr &pMetaObj)
{
	m_nState |= PUBSTATE_AUDIOSTARTED;
	if (m_pPubMgr && m_pPubMgr->GetHandler())
	{
		m_pPubMgr->GetHandler()->OnAudioStart(this, new AMFVarWrapper(pMetaObj));
	}
	m_sTSGenerator.OnAudioStart(m_sPktAnalyzer.GetAVFlags());
}

void PublishPipe::OnVideoStart(AMFVarPtr &pMetaObj)
{
	m_nState |= PUBSTATE_VIDEOSTARTED;
	if (m_pPubMgr && m_pPubMgr->GetHandler())
	{
		m_pPubMgr->GetHandler()->OnVideoStart(this, new AMFVarWrapper(pMetaObj));
	}
	m_sTSGenerator.OnVideoStart(m_sPktAnalyzer.GetAVFlags());
}

void PublishPipe::OnAudioPacket(PPacket *pPacket)
{
	Subscriber* pSuber, * pSuberEnd;
	ScopedPointer<PPacket> dtor(pPacket);

	if (m_sTSGenerator.GetSplitterCount() > 0)
	{
		// Make a new packet copy to avoid conflict when ts generation
		PPacket sTSUse(*pPacket);
		m_sTSGenerator.OnAudioPacket(&sTSUse);
	}

	// We MUST always update the cache list
	if (m_lstInitPackets.Count() > 0)
	{
		// Cache latest frame sequence
		m_lstInitPackets.PushBack(new PPacket(*dtor.Get(), true));
	}

	// Send packet to subscribers
	if (m_lstSubscribers.Count() > 0)
	{
		uint8_t nAVFlags = m_sPktAnalyzer.GetAVFlags();
		// Notify subscribers for new packet incoming
		pSuber = m_lstSubscribers.Begin();
		pSuberEnd = m_lstSubscribers.End();
		for (;pSuber != pSuberEnd;pSuber = m_lstSubscribers.Next(pSuber))
		{
			if (pSuber->IsInit())
			{
				pSuber->NotifyPacket(new PPacket(*dtor.Get(), true), 0);
			}
			else if (!(AVINFO_HAS_VIDEO & nAVFlags))
			{
				pSuber->Init();
				pSuber->NotifyPacket(new PPacket(*dtor.Get(), true), 0);
			}
		}
	}
}

void PublishPipe::OnVideoPacket(PPacket *pPacket)
{
	Subscriber *pSuber, *pSuberEnd;
	PHeader& refHeader = *pPacket->m_pHeader;
	ScopedPointer<PPacket> dtor(pPacket);

	if (m_sTSGenerator.GetSplitterCount() > 0)
	{
		// Make a new packet copy to avoid conflict when ts generation
		PPacket sTSUse(*pPacket);
		m_sTSGenerator.OnVideoPacket(&sTSUse);
	}

	if (m_bAVCache)
	{
		// We MUST always update the cache list
		// Clear old packets
		if (refHeader.IsVKFrame())
		{
			_Cleanup();
			// Cache latest frame sequence
			m_lstInitPackets.PushBack(new PPacket(*dtor.Get(), true));
		}
		else if (m_lstInitPackets.Count() > 0)
		{
			// Cache latest frame sequence
			m_lstInitPackets.PushBack(new PPacket(*dtor.Get(), true));
		}
	}

	// Send packet to subscribers
	if (m_lstSubscribers.Count() > 0)
	{	
		// Notify subscribers for new packet incoming
		pSuber = m_lstSubscribers.Begin();
		pSuberEnd = m_lstSubscribers.End();
		for (;pSuber != pSuberEnd;pSuber = m_lstSubscribers.Next(pSuber))
		{
			if (!pSuber->IsInit())
			{
				if (m_bAVCache)
				{
					PPacket *pIter, *pEnd;

					pSuber->Init();
					pIter = m_lstInitPackets.Begin();
					pEnd = m_lstInitPackets.End();
					for (; pIter != pEnd; pIter = m_lstInitPackets.Next(pIter))
					{
						//if (refHeader.m_eDataType == MTYPE_VIDEODATA)
						//{
							pSuber->NotifyPacket(new PPacket(*pIter, true), 0);
						//}
					}
				} 
				else if (refHeader.IsVKFrame())
				{
					pSuber->Init();
					pSuber->NotifyPacket(new PPacket(*dtor.Get(), true), 0);
				}
			}
			else
			{
				pSuber->NotifyPacket(new PPacket(*dtor.Get(), true), 0);
			}
		}
	}
}

void PublishPipe::OnRPCCall(PPacket *pPacket)
{
	ScopedPointer<PPacket> dtor(pPacket);
	// Send packet to subscribers
	if (m_lstSubscribers.Count() > 0)
	{
		Subscriber *pSuber, *pSuberEnd;
		// Notify subscribers for new packet incoming
		pSuber = m_lstSubscribers.Begin();
		pSuberEnd = m_lstSubscribers.End();
		for (;pSuber != pSuberEnd;pSuber = m_lstSubscribers.Next(pSuber))
		{
			pSuber->NotifyPacket(new PPacket(*dtor.Get(), true), SENDTYPE_COMMAND);
		}
	}
}

void PublishPipe::OnUserCtrl(PPacket *pPacket)
{
	ScopedPointer<PPacket> dtor(pPacket);
	// Send packet to subscribers
	if (m_lstSubscribers.Count() > 0)
	{
		Subscriber *pSuber, *pSuberEnd;
		// Notify subscribers for new packet incoming
		pSuber = m_lstSubscribers.Begin();
		pSuberEnd = m_lstSubscribers.End();
		for (;pSuber != pSuberEnd;pSuber = m_lstSubscribers.Next(pSuber))
		{
			pSuber->NotifyPacket(new PPacket(*dtor.Get(), true), SENDTYPE_USERCTRL);
		}
	}
}

void PublishPipe::OnNewSubscriber(Subscriber *pSubscriber)
{
	Publisher *pIter, *pEnd;

	pIter = m_lstPublishers.Begin();
	pEnd = m_lstPublishers.End();
	for (;pIter != pEnd;pIter = m_lstPublishers.Next(pIter))
	{
		pIter->OnNewSubscriber(pSubscriber);
	}
}

void PublishPipe::OnNewPublisher(Publisher *pPublisher)
{
	Publisher *pIter, *pEnd;

	pIter = m_lstPublishers.Begin();
	pEnd = m_lstPublishers.End();
	for (;pIter != pEnd;pIter = m_lstPublishers.Next(pIter))
	{
		pIter->OnNewPublisher(pPublisher);
	}
}

void PublishPipe::OnException(BCException &refExcept)
{
	if (m_pPubMgr && m_pPubMgr->GetHandler())
	{
		m_pPubMgr->GetHandler()->OnException(refExcept, m_nId);
	}
}

void PublishPipe::HandlePacket(IAVPacket *pPacket)
{
	if (pPacket && (pPacket->GetType() == AVPKT_RTMPPACKET))
	{
		PublishAsync(pPacket->Release<PPacket>());
	}
	BC_SAFE_DELETE_PTR(pPacket);
}

void PublishPipe::OnEventProcShutdown()
{
	if (m_pPubMgr)
	{
		m_pPubMgr->OnPipeClosed(this);
	}
}

bool PublishPipe::IsWorking() const
{
	return m_eState >= PUBSTATE_WORKING;
}

void PublishPipe::_Inter_ReleasePublishers()
{
	Publisher *pPuber, *pPuberEnd;
	pPuber = m_lstPublishers.Begin();
	pPuberEnd = m_lstPublishers.End();
	for (; pPuber != pPuberEnd; pPuber = m_lstPublishers.Next(pPuber))
	{
		pPuber->OnReleaseNotify();
	}
}

void PublishPipe::_Inter_ReleaseSubscribers()
{
	Subscriber *pSuber, *pIterEnd;

	pSuber = m_lstSubscribers.Begin();
	pIterEnd = m_lstSubscribers.End();
	for (; pSuber != pIterEnd; pSuber = m_lstSubscribers.Next(pSuber))
	{
		pSuber->StopFromPublisher();
	}
}

void PublishPipe::SetAVFilter(uint32_t eFilterFlags)
{
	BCSpinMutex::Owner lock(m_sLock);
	m_sPktAnalyzer.SetAVFilter(eFilterFlags);
}

///////////////////////////////////////////////////////////////////////////////
// class : PublishPipePtr
///////////////////////////////////////////////////////////////////////////////

PublishPipePtr::PublishPipePtr(PublishPipe *pPipe)
	: m_pPipe(pPipe)
{
}

PublishPipePtr::~PublishPipePtr()
{
	if (m_pPipe)
	{
		m_pPipe->Detach(&m_pPipe);
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : PublisherMgr
///////////////////////////////////////////////////////////////////////////////

enum
{
	PUBMGR_STATE_INIT		= 0,
	PUBMGR_STATE_RUNNING	= 1,
};

PublisherMgr::PublisherMgr()
	: m_pTaskMgr(NULL)
	, m_nId(0)
	, m_pOptions(NULL)
	, m_pHandler(NULL)
	, m_nNextId(1)
{
	memzero(m_szOptKey, sizeof(m_szOptKey));
}

PublisherMgr::~PublisherMgr()
{
	//
}

BCRESULT PublisherMgr::Create(uint32_t nId)
{
	BCRESULT result;

	m_nId = nId;

	m_pTaskMgr = new BCTaskMgr();
	if (!m_pTaskMgr)
	{
		return BC_R_NOMEMORY;
	}
	result = m_pTaskMgr->Create(2, 0);

	return result;
}

void PublisherMgr::SetHandler(PublishHandler *pHandler)
{
	ASSERT(pHandler);

	m_sLock.Lock();
	m_pHandler = pHandler;
	m_sLock.Unlock();
	//m_pHandler->OnReady(); // already sync emit ready event
}

bool PublisherMgr::IsStreamExist(LPCSTR lpName)
{
	BCPString strStrmName;
	BCRESULT result = FLVInfo::ParseFileName(strStrmName, lpName);
	if (BC_R_SUCCESS != result)
	{
		return false;
	}
	BCSpinMutex::Owner lock(m_sLock);
	return m_htPipes.Get(strStrmName, HASH_KEY_STRING) != NULL;
}

BCRESULT PublisherMgr::CreatePipe(
	LPCSTR lpStreamName, 
	PublishPipe **ppPipe)
{

	if (!lpStreamName || !ppPipe || *ppPipe)
	{
		return BC_R_INVALIDARG;
	}
	else
	{
		BCRESULT result;
		PublishPipe *pPipe = NULL;
		BCPString strStrmName;
		result = FLVInfo::ParseFileName(strStrmName, lpStreamName);
		if (BC_R_SUCCESS != result)
		{
			return result;
		}
		pPipe = new PublishPipe();
		if (!pPipe)
		{
			return BC_R_NOMEMORY;
		}
		BCSpinMutex::Owner lock(m_sLock);
		result = pPipe->Create(m_pTaskMgr, this, strStrmName, m_nNextId++);
		if (BC_R_SUCCESS != result)
		{
			delete pPipe;
			return result;
		}
		*ppPipe = pPipe;
		pPipe->m_nRef++;
	}
	return BC_R_SUCCESS;
}

BCRESULT PublisherMgr::AttachPipe(PublishPipe *pSrcPipe, PublishPipe **ppDstPipe)
{
	ASSERT(pSrcPipe && ppDstPipe);

	m_sLock.Lock();
	ASSERT(pSrcPipe->m_nRef > 0);
	pSrcPipe->m_nRef++;
	*ppDstPipe = pSrcPipe;
	m_sLock.Unlock();
	return true;
}

bool PublisherMgr::DetachPipe(PublishPipe **ppPipe)
{
	ASSERT(ppPipe && *ppPipe);

	PublishPipe *pPipe = *ppPipe;

	*ppPipe = NULL;
	m_sLock.Lock();	
	ASSERT(pPipe->m_nRef > 0);
	pPipe->m_nRef--;
	if (pPipe->m_nRef > 0)
	{
		m_sLock.Unlock();
		return false;
	}
	//const BCPString &strPubName = pPipe->m_strPubName;
	//PublishPipe *pOldPipe = (PublishPipe *)m_htPipes.Get(
	//	strPubName.c_str(), strPubName.size());
	//ASSERT(pOldPipe == pPipe);
	//if (pOldPipe == pPipe)
	//{
	//	m_htPipes.Set(strPubName.c_str(), strPubName.size(), NULL);
	//	pPipe->RemoveFromList();
	//	m_lstWaitToDetach.PushBack(pPipe);
	//	pPipe->Close();
	//}
	pPipe->Close();
	m_sLock.Unlock();
	return true;
}

#if 0
BCRESULT PublisherMgr::AttachPipe(
	LPCSTR lpStreamName, 
	PublishPipe **ppPipe,
	uint8_t nLiveMask)
{
	BCRESULT result;
	SubscriberList lstWaitSubers;
	Subscriber *pWaitSuber;
	PublishPipe *pPipe = NULL;

	if (!lpStreamName || !ppPipe || *ppPipe)
	{
		return BC_R_INVALIDARG;
	}
	else
	{
		BCPString strStrmName;
		result = FLVInfo::ParseFileName(strStrmName, lpStreamName);
		if (BC_R_SUCCESS != result)
		{
			return result;
		}
		BCSpinMutex::Owner lock(m_sLock);
		pPipe = (PublishPipe *)m_htPipes.Get(strStrmName, HASH_KEY_STRING);
		if (!pPipe)
		{
			if (!nLiveMask)
			{
				*ppPipe = NULL;
				return BC_R_NOTFOUND;
			}		
			pPipe = new PublishPipe();
			if (!pPipe)
			{
				return BC_R_NOMEMORY;
			}
			result = pPipe->Create(m_pTaskMgr, this, strStrmName, m_nNextId++, nLiveMask);
			if (BC_R_SUCCESS != result)
			{
				delete pPipe;
				return result;
			}
			Subscriber *pIter, *pIterEnd, *pNext;

			pIter = m_lstSubscribers.Begin();
			pIterEnd = m_lstSubscribers.End();
			for (;pIter != pIterEnd;)
			{
				if (pIter->m_strSubName == strStrmName)
				{
					pNext = m_lstSubscribers.Next(pIter);
					pIter->RemoveFromList();
					lstWaitSubers.PushBack(pIter);
					pIter = pNext;
				}
				else
				{
					pIter = m_lstSubscribers.Next(pIter);
				}
			}
			m_htPipes.Set(strStrmName, strStrmName.length(), pPipe);
			if (m_pHandler)
			{
				// Increase reference
				pPipe->m_nRef++;
				m_pHandler->OnStreamStart(pPipe);
			}
		}
		*ppPipe = pPipe;
		pPipe->m_nRef++;
	}
	// Outside of lock-scope
	while((pWaitSuber = lstWaitSubers.PopFront()) != NULL)
	{
		pPipe->Add(pWaitSuber);
	}
	return BC_R_SUCCESS;
}

uint32_t PublisherMgr::EnumPubStrmName(
	LPFN_PubStrmNameEnumPtr pFunc,
	void *pArg)
{
	BCHashTable::HashIndexType *pHashIndex;
	uint32_t nCounter = 0;

	if (pFunc == NULL)
	{
		return 0;
	}

	BCSpinMutex::Owner lock(m_sLock);

	for (pHashIndex = m_htPipes.First(NULLPOOL); 
		 pHashIndex != NULL; 
		 pHashIndex = m_htPipes.Next(pHashIndex))
	{
		(pFunc)(pArg, (LPCSTR)pHashIndex->pSelf->pKey);
		nCounter++;
	}

	return nCounter;
}

BCRESULT PublisherMgr::EnumPubStrmName(
	KBPool &refPool,
	LPCSTR *&lpszArray,
	uint32_t &refSize)
{
	BCHashTable::HashIndexType *pHashIndex;
	uint32_t i = 0;

	BCSpinMutex::Owner lock(m_sLock);

	refSize = m_htPipes.Count();
	if (!refSize)
	{
		lpszArray = NULL;
		return BC_R_SUCCESS;
	}
	lpszArray = (LPCSTR *)refPool.Calloc(sizeof(LPCSTR)*refSize);
	if (!lpszArray)
	{
		return BC_R_NOMEMORY;
	}
	for (pHashIndex = m_htPipes.First(NULLPOOL); 
		pHashIndex != NULL; 
		pHashIndex = m_htPipes.Next(pHashIndex))
	{
		lpszArray[i++] = refPool.Strndup(
			(LPCSTR)pHashIndex->pSelf->pKey,
			pHashIndex->pSelf->nKeyLen);
	}

	return BC_R_SUCCESS;
}

uint32_t PublisherMgr::GetLivePipesCount()
{
	BCHashTable::HashIndexType *pHashIndex;
	uint32_t nSize = 0;
	PublishPipe *pPipe = NULL;

	BCSpinMutex::Owner lock(m_sLock);

	nSize = m_htPipes.Count();
	if (!nSize)
	{
		return 0;
	}
	nSize = 0;
	for (pHashIndex = m_htPipes.First(NULLPOOL); 
		pHashIndex != NULL; 
		pHashIndex = m_htPipes.Next(pHashIndex))
	{
		pPipe = (PublishPipe *)pHashIndex->pSelf->pValue;
		if (pPipe->m_eLiveType == LVTYPE_LIVE)
		{
			nSize++;
		}
	}

	return nSize;
}

uint32_t PublisherMgr::GetVirtualLivePipesCount()
{
	BCHashTable::HashIndexType *pHashIndex;
	uint32_t nSize;
	PublishPipe *pPipe = NULL;

	BCSpinMutex::Owner lock(m_sLock);

	nSize = m_htPipes.Count();
	if (!nSize)
	{
		return 0;
	}
	nSize = 0;
	for (pHashIndex = m_htPipes.First(NULLPOOL); 
		pHashIndex != NULL; 
		pHashIndex = m_htPipes.Next(pHashIndex))
	{
		pPipe = (PublishPipe *)pHashIndex->pSelf->pValue;
		if (pPipe->m_eLiveType == LVTYPE_VLIVE)
		{
			nSize++;
		}
	}

	return nSize;
}
#endif // 0

void PublisherMgr::OnPipeClosed(PublishPipe *pPipe)
{
	{
		BCSpinMutex::Owner lock(m_sLock);
		if (m_lstWaitToDetach.IsExist(pPipe))
		{
			pPipe->RemoveFromList();
		}
	}
	if (m_pHandler)
	{
		m_pHandler->OnStreamStop(pPipe);
	}
	else
	{
		delete pPipe;
	}
}

void PublisherMgr::OnOptionChanged(const BCPString &strKey, BCFVar *pValue)
{
	ScopedPointer<BCFVar> valuep(pValue);
	if (strKey == m_szOptKey)
	{
	}
}

void PublisherMgr::OnPeerChanged( 
	PublishPipe *pPipe, 
	uint32_t nCount, 
	bool bPublisher )
{
	if (m_pHandler)
	{
		m_pHandler->OnPeerChanged(pPipe, nCount, bPublisher);
	}
}

///////////////////////////////////////////////////////////////////////////////
// class : PublishCenter
///////////////////////////////////////////////////////////////////////////////

BCSpinMutex					PublishCenter::m_sLock;
PublishCenter::PubMgrMap	PublishCenter::m_mapPubMgrs;
uint32_t					PublishCenter::m_nNextProbId = 1;
PublisherMgr			*	PublishCenter::m_pDefPubMgr = NULL;
uint32_t					PublishCenter::m_nLiveQuota = 5;
uint32_t					PublishCenter::m_nVirtualLiveQuota = 5;

PublishCenter::PublishCenter()
{
	//
}

PublishCenter::~PublishCenter()
{
	//
}

uint32_t PublishCenter::CreatePubMgr()
{
	uint32_t nPublishId;
	PublisherMgr *pPubMgr = NULL;

	BCSpinMutex::Owner lock(m_sLock);
	nPublishId = m_nNextProbId++;
	pPubMgr = new PublisherMgr();
	if (pPubMgr)
	{
		BCRESULT result;

		result = pPubMgr->Create(nPublishId);
		if (result != BC_R_SUCCESS)
		{
			delete pPubMgr;
			return 0;
		}
		m_mapPubMgrs[nPublishId] = pPubMgr;
		return nPublishId;
	}
	return 0;
}

PublisherMgr *PublishCenter::GetPubMgr(uint32_t nPublishId)
{
	PublisherMgr *pPubMgr = NULL;

	BCSpinMutex::Owner lock(m_sLock);
	if (nPublishId == 0)
	{
		if (m_pDefPubMgr)
		{
			return m_pDefPubMgr;
		}
		pPubMgr = new PublisherMgr();
		if (pPubMgr)
		{
			BCRESULT result;

			result = pPubMgr->Create(0);
			if (result != BC_R_SUCCESS)
			{
				delete pPubMgr;
				return NULL;
			}
			m_mapPubMgrs[0] = pPubMgr;
			m_pDefPubMgr = pPubMgr;
		}
	}
	else
	{
		PubMgrMap::iterator iter, iterEnd;
		iterEnd = m_mapPubMgrs.end();
		iter = m_mapPubMgrs.find(nPublishId);
		if (iter != iterEnd)
		{
			pPubMgr = iter->second;
			ASSERT(pPubMgr);
		}
		else
		{
			pPubMgr = NULL;
		}
	}
	return pPubMgr;
}

void PublishCenter::PutPubMgr(uint32_t nPublishId)
{
	PubMgrMap::iterator iter, iterEnd;

	BCSpinMutex::Owner lock(m_sLock);
	ASSERT(nPublishId > 0);
	iterEnd = m_mapPubMgrs.end();
	iter = m_mapPubMgrs.find(nPublishId);
	ASSERT(iter != iterEnd);
	delete iter->second;
	m_mapPubMgrs.erase(iter);
	if (m_mapPubMgrs.size() == 0)
	{
		m_mapPubMgrs.clear();
	}
}

#if 0
uint32_t PublishCenter::GetLivePipesCount()
{
	PubMgrMap::iterator iter, iterEnd;
	uint32_t nSize = 0;

	BCSpinMutex::Owner lock(m_sLock);
	iter = m_mapPubMgrs.begin();
	iterEnd = m_mapPubMgrs.end();
	for (;iter != iterEnd;iter++)
	{
		nSize += iter->second->GetLivePipesCount();
	}
	return nSize;
}

uint32_t PublishCenter::GetVirtualLivePipesCount()
{
	PubMgrMap::iterator iter, iterEnd;
	uint32_t nSize = 0;

	BCSpinMutex::Owner lock(m_sLock);
	iter = m_mapPubMgrs.begin();
	iterEnd = m_mapPubMgrs.end();
	for (;iter != iterEnd;iter++)
	{
		nSize += iter->second->GetVirtualLivePipesCount();
	}
	return nSize;
}

bool PublishCenter::IsStreamExist(LPCSTR lpName)
{
	PubMgrMap::iterator iter, iterEnd;

	BCSpinMutex::Owner lock(m_sLock);
	iter = m_mapPubMgrs.begin();
	iterEnd = m_mapPubMgrs.end();
	for (;iter != iterEnd;iter++)
	{
		if (iter->second->IsStreamExist(lpName))
		{
			return true;
		}
	}
	return false;
}
#endif // 0

uint32_t PublishCenter::GetLivePipesQuota()
{
	return INFINITE;
}

uint32_t PublishCenter::GetVirtualLivePipesQuota()
{
	return INFINITE;
}

void PublishCenter::SetLivePipesQuota( uint32_t nQuota )
{
	BCSpinMutex::Owner lock(m_sLock);
	m_nLiveQuota = nQuota;
}

void PublishCenter::SetVirtualLivePipesQuota( uint32_t nQuota )
{
	BCSpinMutex::Owner lock(m_sLock);
	m_nVirtualLiveQuota = nQuota;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

///////////////////////////////////////////////////////////////////////////////
// End of file : PubSub.cpp
///////////////////////////////////////////////////////////////////////////////
