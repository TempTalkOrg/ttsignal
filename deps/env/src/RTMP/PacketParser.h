///////////////////////////////////////////////////////////////////////////////
// file : PacketParser.h
// author : antoniozhou
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_PACKETPARSER_H_INCLUDED__
#define RTMP_PACKETPARSER_H_INCLUDED__

#include <list>
#include <BC/BCMap.h>
#include <RTMP/Packet.h>
#include <RTMP/RTMPUrl.h>

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

class SEIDelayAppender;

typedef enum EDelayTag {
	DTPushEncodePre			= 1,
	DTPushEncodeAfter		= 2,
	DTPushSend				= 3,
	DTMediaSvrRecv			= 4,
	DTMediaSvrSend			= 5,
	DTMcuRecv				= 6,
	DTMcuSend				= 7,
	DTRTP2RTMPRecv			= 8,
	DTRTP2RTMPend			= 9,
	DTCDNOriginRecv			= 10,
	DTCDNOriginSend			= 11,
	DTCDNTranscodeRecv		= 12,
	DTCDNTranscodeSend		= 13,
	DTCDNL1Recv				= 14,
	DTCDNL1Send				= 15,
	DTCDNL2Recv				= 16,
	DTCDNL2Send				= 17,
	DTPlayerRecv			= 18,
	DTPlayerDecodePre		= 19,
	DTPlayerDecodeAfter		= 20,
	DTFMSRecv				= 21,
	DTFMSSend				= 22,
	DTVersion				= 255,
}EDelayTag;

///////////////////////////////////////////////////////////////////////////////
// class : IChunkHandler
///////////////////////////////////////////////////////////////////////////////

class IChunkHandler
{
	friend class PacketParser;
public:
	IChunkHandler(){};
	virtual ~IChunkHandler(){};

protected:
	virtual void OnChunkParsed(
					const PHeader &refHeader, 
					BCRegionS &refRegion) = 0;
private:
	DECLARE_NO_COPY_CLASS(IChunkHandler);
};

///////////////////////////////////////////////////////////////////////////////
// class : PacketParser
///////////////////////////////////////////////////////////////////////////////

class PacketParser
{
public:
	typedef enum HeaderMagicE
	{
		HEADER_NEW				= 0,
		HEADER_SAME_SOURCE		= 1,
		HEADER_TIMER_CHANGE		= 2,
		HEADER_CONTINUE			= 3,
		HEADER_INVALID			= 4,
	}HeaderMagicE;

	typedef enum NetIoStateE
	{
		HANDSHAKE_VERSION				= 0,
		HANDSHAKE_REQUEST				= 1,
		HANDSHAKE_SENDACK				= 2,
		HANDSHAKE_RESPONSE				= 3,
		HANDSHAKE_RECVACK				= 4,
		RECV_BASE_CHUNK_HEADER_BYTE		= 5,
		RECV_EXT_CHANNEL_ID				= 6,
		RECV_CHUNK_HEADER				= 7,
		RECV_EXT_TIMESTAMP				= 8,
		RECV_CHUNK_DATA					= 9,
		SEND_CHUNK_DATA					= 10,
	}NetIoStateE;
public:
	PacketParser();
	virtual ~PacketParser();

	BCRESULT		Create(IChunkHandler *pHandler);
	void			Initialize();
	void			Parse();
	BCBuffer	*	GetRecvBuf();
	void			SetChunkSize(uint32_t nSize);
	uint32_t		GetChunkSize() const;
	void			Cleanup();
	
	static bool		TrySetDataFrame(
						const PPacket &refPacket,
						PPacket &refDstPkt);
protected:
	BOOL			_RequireData(
						uint32_t nSize,
						uint32_t nForward,
						NetIoStateE eRecvState);
	// Message header receive
	BOOL			_RequireBaseChunkHeader();
	BOOL			_RequireChunkHeader();
	BOOL			_RequireExtTimestamp();
	BOOL			_RequireExtTimestamp(PHeader &refChunkHdr);
	BOOL			_RequireChunkData(PHeader &sChunkHdr);
	BOOL			_ParseBaseChunkHeader();
	BOOL			_ParseExtChannelId();
	BOOL			_ParseChunkHeader();
	BOOL			_ParseExtTimestamp();
	BOOL			_FinishChunk();
private:
	DECLARE_NO_COPY_CLASS(PacketParser);
	typedef BCMap<uint32_t, PHeader>	ChunkHdrMap;
	ChunkHdrMap				m_mapRecvChunkHdr;
	IChunkHandler		*	m_pHandler;
	BCBuffer				m_sRecvBuffer;
	BCBIStream				m_sReader;
	// Asynch state
	NetIoStateE				m_eRecvState;
	uint32_t				m_nRequireDataSize;
	uint32_t				m_nChunkDataLen;

	// RTMP specific variables
	uint32_t				m_nChannelId;
	uint32_t				m_eChunkType;
	// The chunk size is maintained independently for each direction.
	uint32_t				m_nChunkSize;

	// Buffer to store chunk data
	BCRegionS				m_sChunk;
	BCRegionS				m_sRegion;
};

///////////////////////////////////////////////////////////////////////////////
// class : Notifier
///////////////////////////////////////////////////////////////////////////////

class Notifier
{
public:
	Notifier();
	Notifier(
		uint32_t eType,
		uint32_t nId,
		LPFN_Notification pAction,
		void *wParam,
		uint64_t lParam = 0,
		LPFN_NotifierDtor cbDestroy = NULL);
	virtual ~Notifier();

	uint32_t				m_eType;
	PHeader					m_sHeader;
	LPFN_Notification		m_pAction;
	void				*	m_wParam;
	uint64_t				m_lParam;
	LPFN_NotifierDtor		m_cbDestroy;

	Notifier &	operator = (const Notifier &refOther);
	void		Init(
					uint32_t eType,
					uint32_t nStreamId,
					LPFN_Notification pAction,
					void *wParam,
					uint64_t lParam = 0,
					LPFN_NotifierDtor cbDestroy = NULL);
	void		Init(
					uint32_t eType,
					uint32_t nStreamId,
					PHeader &refHeader,
					LPFN_Notification pAction,
					void *wParam,
					uint64_t lParam = 0,
					LPFN_NotifierDtor cbDestroy = NULL);
	void		Notify();
	void		Destroy();
};

///////////////////////////////////////////////////////////////////////////////
// class : MsgItemPacketizer
///////////////////////////////////////////////////////////////////////////////

class MsgItemPacketizer
{
public:
	MsgItemPacketizer() {}
	~MsgItemPacketizer(){}

	virtual BCRESULT	Packetize(PPacket& refPacket, BCBuffer& refBuffer) = 0;
};

///////////////////////////////////////////////////////////////////////////////
// class : MsgItem
///////////////////////////////////////////////////////////////////////////////

class MsgItem : public BCNodeList::Node
{
	DECLARE_FIXED_ALLOC(MsgItem);
public:
	MsgItem(uint32_t eType = MTYPE_INVALID);
	~MsgItem();

	uint32_t				m_eType;
	BCBuffer			*	m_pBuffer;
	Notifier			*	m_pNotifier;
	PPacket				*	m_pPacket;
	MsgItemPacketizer	*	m_pPacketizer;
};

typedef TNodeList<MsgItem>		SendItemList;

///////////////////////////////////////////////////////////////////////////////
// class : MsgBuffer
///////////////////////////////////////////////////////////////////////////////

class MsgQueue
{
public:
	MsgQueue();
	virtual ~MsgQueue();

	BCRESULT		Append(BCBuffer &refBody, uint32_t eType);
	BCRESULT		Append(
						PHeader &refHeader,
						BCBuffer &refBody,
						MsgItemPacketizer* pPacketizer = NULL);
	BCRESULT		Append(Notifier *pNotifier);
	MsgItem *		PopFront(SEIDelayAppender *pSeiAppender/* = NULL*/);
	uint32_t		RemoveNotifierById(uint32_t nId);
	void			Cleanup();
	BOOL			IsEmpty() const;
	uint32_t		Count() const;
	void			SetChunkSize(uint32_t nSize);
	uint32_t		GetChunkSize() const;
protected:
	void			_WriteFirstHeader(
						PHeader &refLastHeader,
						PHeader &refHeader,
						BCBuffer &refBuffer);
	void			_WriteContHeader(
						PHeader &refLastHeader,
						BCBuffer &refBuffer);
private:
	DECLARE_NO_COPY_CLASS(MsgQueue);
	typedef BCMap<uint32_t, PHeader>	ChunkHdrMap;
	ChunkHdrMap				m_mapSendChunkHdr;
	SendItemList			m_lstItems;
	uint32_t				m_nChunkSize;
	BOOL					m_bInitialized;
	uint64_t				m_tmBaseOfAllChannels;
};

///////////////////////////////////////////////////////////////////////////////
// class : TimstampChecker
///////////////////////////////////////////////////////////////////////////////

class TimstampChecker
{
public:
	TimstampChecker();
	~TimstampChecker();

	BCRESULT	Create(bool bRtmpProtocol, ClientOS eClientOS, uint64_t nVer);
	void		ResetTimestamp(PHeader &refHeader);

	bool				m_bRtmpProtocol;
	ClientOS			m_eClientOS;
	uint64_t			m_nSdkVer;
	bool				m_bAllChannelRebase;
	uint64_t			m_tmBaseOfAVChannels;
	bool				m_bVideoRebase;
	uint64_t			m_tmLastVideoPacket;
	bool				m_bAudioRebase;
	uint64_t			m_tmLastAudioPacket;
};

///////////////////////////////////////////////////////////////////////////////
// class : SEIDelayAppender
///////////////////////////////////////////////////////////////////////////////

class SEIDelayAppender
{
	class NTPItem
	{
	public:
		uint8_t		type;
		uint32_t	ip;
		uint64_t	timestamp;
	};

	typedef std::list<NTPItem>	NTPItemList;
public:
	SEIDelayAppender();
	~SEIDelayAppender();

	BCRESULT	Create(uint8_t type, LPCSTR ip);
	BCRESULT	ProcessPacket(PPacket &refPacket);

private:
	DECLARE_NO_COPY_CLASS(SEIDelayAppender);

	BCRESULT	_ProcessSEI(PHeader &refHeader, BCBuffer& refBody);
	uint32_t	_ParseAndEncodeSEI(
					const void* data, 
					size_t data_size,
					BCOStream& refWriter);

	uint8_t				m_eType;
	uint32_t			m_ip;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_PACKETPARSER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : PacketParser.h
///////////////////////////////////////////////////////////////////////////////
