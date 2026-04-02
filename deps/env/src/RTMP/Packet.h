///////////////////////////////////////////////////////////////////////////////
// file : Packet.h
// author : antoniozhou.
///////////////////////////////////////////////////////////////////////////////


#ifndef RTMP_PACKET_H_INCLUDED__
#define RTMP_PACKET_H_INCLUDED__

#include <BC/BCFixedAlloc.h>
#include <RTMP/Exports.h>
#include <RTMP/AMF.h>
#include <BC/base/atomic_ref_count.h>


#define RTMP_DEFAULT_CHUNK_SIZE				128
#define MAX_CHUNK_HEADER_SIZE				18
#define RTMP_DEFAULT_CHUNK_BUFFER_SIZE		(MAX_CHUNK_HEADER_SIZE + RTMP_DEFAULT_CHUNK_SIZE)
#define RTMP_DEFAULT_SEND_BUFFER_SIZE		1024*16


//namespace AMFUtils
//{
//	class AMFVar;
//}

using namespace AMF;
using namespace AMFType;

typedef void (*PacketActionPtr)(void *arg);

///////////////////////////////////////////////////////////////////////////////
// Namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

namespace RTMP
{

typedef enum MTypeE
{
	// RTMP reserves message type IDs 1-7 for protocol control messages
	// Protocol messages with IDs 1 & 2 are
	// reserved for usage with RTM Chunk Stream protocol
	MTYPE_SETCHUNKSIZE			= 0x01,
	MTYPE_CHUNKABORT			= 0x02,
	// Protocol messages with IDs 3-6 are reserved for usage of RTMP.
	MTYPE_ACKWNDSIZE			= 0x03,
	MTYPE_USERCTRLEVENT			= 0x04,
	MTYPE_SETWNDSIZE			= 0x05,	// the servers downstream bw ? (rtmpy.org)
	MTYPE_SETBW					= 0x06, // the clients upstream bw ? (rtmpy.org)
	// Protocol message with ID 7 is used between edge server and origin server.
	MTYPE_INTERSERVER			= 0x07,
	// Audio message
	MTYPE_AUDIODATA				= 0x08,
	// Video message
	MTYPE_VIDEODATA				= 0x09,
	// Flex stream send
	MTYPE_FLEXSTREAMSEND		= 0x0F, // from wireshark
	// Flex shared-object
	MTYPE_FLEXSHAREDOBJECT		= 0x10, // from wireshark
	// Flex message
	MTYPE_FLEXMESSAGE			= 0x11, // from wireshark
	// Data message
	MTYPE_METADATA				= 0x12,
	// Shared object message
	MTYPE_SHAREDOBJECT			= 0x13,
	// RPC message
	MTYPE_COMMAND				= 0x14,
	// Aggregage message
	MTYPE_AGGREGATE				= 0x16,
	// Invalid message
	MTYPE_INVALID				= 0x89,
	// User defined message type - User notifier
	MTYPE_NOTIFIER				= 0x8A,
	// User defined message type - User task
	MTYPE_TASK					= 0x8B,
	// User defined message type - Virtual send
	MTYPE_DELAYSEND				= 0x8C,
	// User defined message type - wait signal
	MTYPE_SIGNALWAIT			= 0x8D,
	// User defined message type - Handshake step
	MTYPE_HSKREQUEST			= 0x8E,
	MTYPE_HSKRESPONSE			= 0x8F,
	MTYPE_HSKSENDACK			= 0x90,
}MTypeE;

#define MAX_RTMP_CHUNK_SIZE				65536

class Notifier;

typedef void (*LPFN_Notification)(Notifier &refNotifier);
typedef void (*LPFN_NotifierDtor)(Notifier &refNotifier);

#define PHDR_ABSTIME_MASK		0x00000001
#define PHDR_EXTTIME_MASK		0x00000002
#define PHDR_VKFRAME_MASK		0x00000004
#define PHDR_AKFRAME_MASK		0x00000008
#define PHDR_VSEQHDR_MASK		0x00000010
#define PHDR_ASEQHDR_MASK		0x00000020
#define PHDR_ADJUST_MASK		0x00000040

#define __PHDR_SET_MASK(__flags, _mask, _isset) \
	do                                          \
	{                                           \
		if (_isset) { (__flags) |= _mask;}      \
		else        { (__flags) &= ~_mask; }    \
	} while (0);

///////////////////////////////////////////////////////////////////////////////
// Class : PHeader
//       - Packet header object
///////////////////////////////////////////////////////////////////////////////

class RTMP_API PHeader
{
public:
	typedef enum HeaderMagicE
	{
		HEADER_INVALID			= -1,
		HEADER_NEW				= 0,
		HEADER_SAME_SOURCE		= 1,
		HEADER_TIMER_CHANGE		= 2,
		HEADER_CONTINUE			= 3,
	}HeaderMagicE;

	typedef enum ChunkDataTypeE
	{
		CHUNK_NONE			= 0,
		CHUNK_START			= 1,
		CHUNK_MIDDLE		= 2,
		CHUNK_END			= 3,
		CHUNK_SINGLE		= 4,
	}ChunkDataTypeE;
public:
	PHeader();
	PHeader(
		int32_t nChannelId,
		int32_t nTimestamp,
		int32_t nDataSize,
		int32_t eDataType,
		int32_t nStreamId);
	PHeader(const PHeader &sHeader);
	~PHeader();

	PHeader &operator = (const PHeader &sHeader);

	void			Reset();
	inline	bool	IsAbsTime() const	{ return ((m_nFlags&PHDR_ABSTIME_MASK) != 0); }
	inline	bool	IsExtTime() const	{ return ((m_nFlags&PHDR_EXTTIME_MASK) != 0); }
	inline	bool	IsVKFrame() const	{ return ((m_nFlags&PHDR_VKFRAME_MASK) != 0); }
	inline	bool	IsAKFrame() const	{ return ((m_nFlags&PHDR_AKFRAME_MASK) != 0); }
	inline	bool	IsVSeqHdr() const	{ return ((m_nFlags&PHDR_VSEQHDR_MASK) != 0); }
	inline	bool	IsASeqHdr() const	{ return ((m_nFlags&PHDR_ASEQHDR_MASK) != 0); }
	inline	bool	IsAdjust() const	{ return ((m_nFlags&PHDR_ADJUST_MASK) != 0); }
	inline	void	SetAbsTime(bool bValue) {__PHDR_SET_MASK(m_nFlags, PHDR_ABSTIME_MASK, bValue);}
	inline	void	SetExtTime(bool bValue) {__PHDR_SET_MASK(m_nFlags, PHDR_EXTTIME_MASK, bValue);}
	inline	void	SetVKFrame(bool bValue) {__PHDR_SET_MASK(m_nFlags, PHDR_VKFRAME_MASK, bValue);}
	inline	void	SetAKFrame(bool bValue) {__PHDR_SET_MASK(m_nFlags, PHDR_AKFRAME_MASK, bValue);}
	inline	void	SetVSeqHdr(bool bValue) {__PHDR_SET_MASK(m_nFlags, PHDR_VSEQHDR_MASK, bValue);}
	inline	void	SetASeqHdr(bool bValue) { __PHDR_SET_MASK(m_nFlags, PHDR_ASEQHDR_MASK, bValue); }
	inline	void	SetAdjust(bool bValue) { __PHDR_SET_MASK(m_nFlags, PHDR_ADJUST_MASK, bValue); }
	void			Dump() const;

public:
	uint32_t				m_nChannelId;
	uint32_t				m_nTimestamp;
	uint32_t				m_nDataSize;
	uint32_t				m_eDataType;
	uint32_t				m_nStreamId;
	uint32_t				m_eChunkDataType;
	uint32_t				m_nFinishedSize;
	uint32_t				m_nChunkSize;
	uint32_t				m_nFlags;
	// Below value are used to monitor live stream packet time
	uint64_t				m_nAbsTime;
	uint64_t				m_nTotalTime;
	int8_t					m_eLastChunkType;
};

///////////////////////////////////////////////////////////////////////////////
// class PPacket - Entire rtmp packet consist of packet header and packet body
///////////////////////////////////////////////////////////////////////////////

class RTMP_API PPacket : public BCNodeList::Node
{
	DECLARE_FIXED_ALLOC(PPacket);
public:
	PPacket();
	PPacket(const PPacket &sPacket, bool bRef = false);

	~PPacket();


	inline bc_atomic_t IncRef()
	{
		return Base::AtomicRefCountIncN(&m_nRef, 1);
	}
	inline bc_atomic_t DecRef()
	{
		if (m_nRef == 0)
		{
			return 0;
		}
		return Base::AtomicRefCountIncN(&m_nRef, -1);
	}

	void			SplitFrom(
						PHeader &refHeader,
						BCBuffer &refBody,
						uint32_t nChunkSize);
	uint32_t		ChunkMessage(PHeader &refLastHeader);
	void			Reset(int32_t nReserveBufData = -1);
	void			Destroy();
	void			Dump();
	void			ParseHeader();
	PPacket		*	RefClone() const;
	PPacket		&	Clone(PPacket &destPacket) const;
public:

	typedef std::shared_ptr<PHeader>	PHeaderPtr;
	// Chunk Message Header Information
	PHeaderPtr				m_pHeader;
	// Packet data buffer
	BCBuffer				m_sBody;
	// User action
	PacketActionPtr			m_pAction;
	// User arguments
	void				*	m_pArgs;
	// Reference count
	bc_atomic_t				m_nRef;
	// User data
	void				*	m_pUserData;
protected:
	uint32_t	WriteContinueHeader();

private:
	PPacket & operator = (const PPacket &sPacket);
};

typedef TNodeList<PPacket>		PPacketList;

//////////////////////////////////////////////////////////////////////////
/// class MBase - Base class of message object after parse
//////////////////////////////////////////////////////////////////////////

class RTMP_API MBase
{
public:
	MBase(
		uint32_t nChannelId,
		uint32_t nTimestamp,
		uint32_t eMsgType,
		uint32_t nStreamId);
	virtual ~MBase();

	virtual bool Decode(PPacket &refInPacket)		= 0;
	virtual bool Encode(PPacket &refOutPacket)		= 0;
	virtual void Dump()								= 0;

	uint32_t			m_nChannelId;
	uint32_t			m_nTimestamp;
	uint32_t			m_nDataSize;
	const uint32_t		m_eDataType;
	uint32_t			m_nStreamId;
	bool				m_bAbsoluteTime;
private:
	DECLARE_NO_COPY_CLASS(MBase);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MCtrlChunkSize
///
/// Set Chunk Size (1)
/// Protocol control message 1, Set Chunk Size, is used to notify the
/// peer a new maximum chunk size to use.
//////////////////////////////////////////////////////////////////////////

class RTMP_API MCtrlChunkSize : public MBase
{
public:
	MCtrlChunkSize(uint32_t nChunkSize = RTMP_DEFAULT_CHUNK_SIZE);
	~MCtrlChunkSize();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

	uint32_t		m_nChunkSize;
private:
	DECLARE_NO_COPY_CLASS(MCtrlChunkSize);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MCtrlChunkAbort
///
/// Abort Message (2)
/// Protocol control message 2, Abort Message, is used to notify the peer
/// if it is waiting for chunks to complete a message, then to discard
/// the partially received message over a chunk stream and abort
/// processing of that message. The peer receives the chunk stream ID of
/// the message to be discarded as payload of this protocol message. This
/// message is sent when the sender has sent part of a message, but wants
/// to tell the receiver that the rest of the message will not be sent.
//////////////////////////////////////////////////////////////////////////

class RTMP_API MCtrlChunkAbort : public MBase
{
public:
	MCtrlChunkAbort();
	~MCtrlChunkAbort();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

	uint32_t		m_nChunkStreamId;
private:
	DECLARE_NO_COPY_CLASS(MCtrlChunkAbort);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MCtrlAckWndSize
///
/// Acknowledgement (3)
/// The client or the server sends the acknowledgment to the peer after
/// receiving bytes equal to the window size. The window size is the
/// maximum number of bytes that the sender sends without receiving
/// acknowledgment from the receiver. The server sends the window size to
/// the client after application connects. This message specifies the
/// sequence number, which is the number of the bytes received so far.
/// ���ݽ��շ������ж��Ƿ����ͷ��������ݽ���ȷ�ϵ���ֵ����Ϊ���ͷ�Ҫ���
/// �շ�ÿ�����ܵ�����һ�����ݵ�Ԫ֮��ʼ����һ���������ݺ�Ӧ�������ݽ�
/// ��ȷ����Ϣ�������ݷ��ͷ�δ�յ���ȷ����Ϣ�����ټ����������ݡ�
//////////////////////////////////////////////////////////////////////////

class RTMP_API MCtrlAckWndSize : public MBase
{
public:
	// For encode packet usage
	MCtrlAckWndSize(uint32_t nSegNumber = 0);
	~MCtrlAckWndSize();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

	uint32_t		m_nSequenceNumber;
private:
	DECLARE_NO_COPY_CLASS(MCtrlAckWndSize);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MCtrlUserCtrl
///
/// User Control Message (4)
/// The client or the server sends this message to notify the peer about
/// the user control events. This message carries Event type and Event
/// data.
//////////////////////////////////////////////////////////////////////////

typedef enum CtrlMsgTypeE
{
	// Event data : stream id - send from server
	MUCTRL_STREAM_BEGIN			= 0,
	// Event data : stream id - send from server
	MUCTRL_STREAM_EOF			= 1,
	// Event data : stream id - send from server
	MUCTRL_STREAM_DRY			= 2,
	// Event data : stream id;ext event data : buffer length - send from client
	MUCTRL_SET_BUFFER_LENGTH	= 3,
	// Event data : stream id - send from server
	MUCTRL_STREAM_IS_RECORDED	= 4,
	// Event data : server timestamp - send from server(need client response)
	MUCTRL_PING_REQUEST			= 6,
	// Event data : client timestamp - send from client(response server request)
	MUCTRL_PING_RESPONSE		= 7,
	// SWFVerification request
	// Event data :
	MUCTRL_SWFVERIFY_REQUEST	= 26,
	// SWFVerification response
	// Event data :
	MUCTRL_SWFVERIFY_RESPONSE	= 27,
}CtrlMsgTypeE;

class RTMP_API MCtrlUserCtrl : public MBase
{
public:
	// Default constructor, for decode command usage
	MCtrlUserCtrl();
	// For response usage
	MCtrlUserCtrl(
		uint32_t eEventType,
		uint32_t dwEventData,
		uint32_t dwEventDataEx = 0);
	~MCtrlUserCtrl();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

	uint16_t		m_eEventType;
	uint32_t		m_dwEventData;
	uint32_t		m_dwEventDataExt;	// Used only for CTRL_SET_BUFFER_LENGTH
	// message
private:
	DECLARE_NO_COPY_CLASS(MCtrlUserCtrl);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MCtrlSetWndSize
///
/// Window Acknowledgement Size (5)
/// The client or the server sends this message to inform the peer which
/// window size to use when sending acknowledgment. For example, a server
/// expects acknowledgment from the client every time the server sends
/// bytes equivalent to the window size. The server updates the client
/// about its window size after successful processing of a connect
/// request from the client.
/// ���ݷ��ͷ��������ý��շ���ȷ����ֵ�������ͷ�Ҫ����շ�ÿ�����ܵ�����
/// һ�����ݵ�Ԫ֮��ʼ��һ�����������ݺ�Ӧ�������ݽ���ȷ����Ϣ������
/// �ݷ��ͷ�δ�յ���ȷ����Ϣ�����ټ����������ݡ�
//////////////////////////////////////////////////////////////////////////

class RTMP_API MCtrlSetWndSize : public MBase
{
public:
	MCtrlSetWndSize(uint32_t nWindowSize = 1024*1024);
	~MCtrlSetWndSize();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

	uint32_t		m_nWindowSize;
private:
	DECLARE_NO_COPY_CLASS(MCtrlSetWndSize);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MCtrlSetBandWidth
///
/// Set Peer Bandwidth (6)
/// The client or the server sends this message to update the output
/// bandwidth of the peer. The output bandwidth value is the same as the
/// window size for the peer. The peer sends ��Window Acknowledgement
/// Size�� back if its present window size is different from the one
/// received in the message.
/// The sender can mark this message hard (0), soft (1), or dynamic (2)
/// using the Limit type field. In a hard (0) request, the peer must send
/// the data in the provided bandwidth. In a soft (1) request, the
/// bandwidth is at the discretion of the peer and the sender can limit
/// the bandwidth. In a dynamic (2) request, the bandwidth can be hard or
/// soft.
//////////////////////////////////////////////////////////////////////////

typedef enum MCtrlBWTypeE
{
	MCTRL_BW_HARD			= 0,
	MCTRL_BW_SOFT			= 1,
	MCTRL_BW_DYNAMIC		= 2,
}MCtrlBWTypeE;

class RTMP_API MCtrlSetBandWidth : public MBase
{
public:
	MCtrlSetBandWidth(
		uint32_t nBW = 512*1024,
		uint32_t eBWType = MCTRL_BW_DYNAMIC);
	~MCtrlSetBandWidth();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

	uint32_t		m_nWindowSize;
	uint8_t			m_eLimitType;
private:
	DECLARE_NO_COPY_CLASS(MCtrlSetBandWidth);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MCtrlInterServer
///
/// Server Data Exchange Message (7)
/// Protocol message with ID
/// 7 is used between edge server and origin server.
//////////////////////////////////////////////////////////////////////////

class RTMP_API MCtrlInterServer : public MBase
{
public:
	MCtrlInterServer();
	~MCtrlInterServer();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

private:
	DECLARE_NO_COPY_CLASS(MCtrlInterServer);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MAudioData
///
/// Audio Data Message (8)
//////////////////////////////////////////////////////////////////////////

class RTMP_API MAudioData : public MBase
{
public:
	MAudioData();
	~MAudioData();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

private:
	DECLARE_NO_COPY_CLASS(MAudioData);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MVideoData
///
/// Video Data Message (9)
//////////////////////////////////////////////////////////////////////////

class RTMP_API MVideoData : public MBase
{
public:
	MVideoData();
	~MVideoData();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

private:
	DECLARE_NO_COPY_CLASS(MVideoData);
};

//////////////////////////////////////////////////////////////////////////
///
/// class MMetaData
///
/// Meta Data Message (18)
//////////////////////////////////////////////////////////////////////////

class RTMP_API MMetaData : public MBase
{
public:
	MMetaData();
	~MMetaData();

	bool Decode(PPacket &refInPacket);
	bool Encode(PPacket &refOutPacket);
	void Dump();

	BCVector<AMFVarWrapper *>		m_vecMetaData;
private:
	DECLARE_NO_COPY_CLASS(MMetaData);
	AMFCodecCtx		m_sCodecCtx;
};

//////////////////////////////////////////////////////////////////////////
///
/// class MSharedObject
///
/// Shared Object Message (19)
/*

Shared object message
A shared object is a Flash object (a collection of name value pairs)
that are in synchronization across multiple clients, instances, and
so on. The message types kMsgContainer=19 for AMF0 and
kMsgContainerEx=16 for AMF3 are reserved for shared object events.
Each message can contain multiple events.


+------+------+-------+-----+-----+------+-----+ - +-----+------+-----+
|Header|Shared|Current|Flags|Event|Event |Event|   |Event|Event |Event|
|      |Object|Version|     |Type |data  |data |   |Type |data  |data |
|      |Name  |       |     |     |length|     |   |     |length|     |
+------+------+-------+-----+-----+------+-----+ - +-----+------+-----+
       |                                                              |
       |<- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - >|
       |              AMF Shared Object Message body                  |

             Figure 1   The shared object message format

The following event types are supported:

+---------------+--------------------------------------------------+
|    Event      |                   Description                    |
+---------------+--------------------------------------------------+
| Use(=1)       | The client sends this event to inform the server |
|               | about the creation of a named shared object.     |
+---------------+--------------------------------------------------+
| Release(=2)   | The client sends this event to the server when   |
|               | the shared object is deleted on the client side. |
+---------------+--------------------------------------------------+
| Request Change| The client sends this event to request that the  |
| (=3)          | change the value associated with a named         |
|               | parameter of the shared object.                  |
+---------------+--------------------------------------------------+
| Change (=4)   | The server sends this event to notify all        |
|               | clients, except the client originating the       |
|               | request, of a change in the value of a named     |
|               | parameter.                                       |
+---------------+--------------------------------------------------+
| Success (=5)  | The server sends this event to the requesting    |
|               | client in response to RequestChange event if the |
|               | request is accepted.                             |
+---------------+--------------------------------------------------+
| SendMessage   | The client sends this event to the server to     |
| (=6)          | broadcast a message. On receiving this event,    |
|               | the server broadcasts a message to all the       |
|               | clients, including the sender.                   |
+---------------+--------------------------------------------------+
| Status (=7)   | The server sends this event to notify clients    |
|               | about error conditions.                          |
+---------------+--------------------------------------------------+
| Clear (=8)    | The server sends this event to the client to     |
|               | clear a shared object. The server also sends     |
|               | this event in response to Use event that the     |
|               | client sends on connect.                         |
+---------------+--------------------------------------------------+
| Remove (=9)   | The server sends this event to have the client   |
|               | delete a slot.                                   |
+---------------+--------------------------------------------------+
| Request Remove| The client sends this event to have the server   |
| (=10)         | delete a slot.                                   |
+---------------+--------------------------------------------------+
| Use Success   | The server sends this event to the client on a   |
| (=11)         | successful connection.                           |
+---------------+--------------------------------------------------+

*/
//////////////////////////////////////////////////////////////////////////

typedef enum SOMsgTypeE
{
	MSO_NONE = 0,
	MSO_SERVER_CONNECT,				// 01
	MSO_SERVER_DISCONNECT,			// 02
	MSO_SERVER_SET_ATTRIBUTE,		// 03
	MSO_CLIENT_UPDATE_DATA,			// 04
	MSO_CLIENT_UPDATE_ATTRIBUTE,	// 05
	MSO_SERVER_SEND_MESSAGE,		// 06
	MSO_CLIENT_STATUS,				// 07
	MSO_CLIENT_CLEAR_DATA,			// 08
	MSO_CLIENT_DELETE_DATA,			// 09
	MSO_SERVER_DELETE_ATTRIBUTE,	// 0A
	MSO_CLIENT_INITIAL_DATA,		// 0B
}SOMsgTypeE;

// Shared object event
class RTMP_API MSOEvent : public BCNodeList::Node
{
public:
	MSOEvent();
	MSOEvent(const MSOEvent &refEvent);
	~MSOEvent();

	bool		Decode(BCIStream &refReader);
	bool		Encode(BCOStream &refWriter);

	MSOEvent *	Clone() const;
	MSOEvent &	operator = (const MSOEvent &refEvent);

	void		SetKey(LPCSTR szKey);
	LPCSTR		GetKey() const;
	AMFVarPtr	GetValue() const;
	void		SetValue(bool bValue);
	void		SetValue(double dbValue);
	void		SetValue(LPCSTR szValue);
	void		SetValue(AMFVar *pValue);
	// Only for add MSO_SERVER_SEND_MESSAGE events properties
	void		Put(AMFVarPtr &pValue);
	void		PutBool(bool bValue);
	void		PutDouble(double dbValue);
	void		PutString(LPCSTR szValue);
	// Only for add MSO_SERVER_SET_ATTRIBUTE & MSO_CLIENT_UPDATE_DATA
	// events name-value pairs
	AMFVarPtr	Get(LPCSTR szKey);
	void		Put(LPCSTR szKey, AMFVarPtr &pValue);
	void		PutBool(LPCSTR szKey, bool bValue);
	void		PutDouble(LPCSTR szKey, double dbValue);
	void		PutString(LPCSTR szKey, LPCSTR szValue);
	bool		IsContainsKey(LPCSTR key);

	void		Clear();

	uint8_t						m_eType;
	AMFVarPtr					m_pKey;
	AMFVarPtr					m_pValue;
	AMFVarWrapperList			m_lstVars;
};

typedef TNodeList<MSOEvent>		MSOEventList;

class RTMP_API MSharedObject : public MBase
{
public:
	MSharedObject(bool bFlexSO = false);
	~MSharedObject();

	bool		Decode(PPacket &refInPacket);
	bool		Encode(PPacket &refOutPacket);
	void		Dump();

	void		AddEvent(MSOEvent *pEvent);
	void		Clear();

	MSharedObject	*	Clone() const;
	MSharedObject	&	operator = (const MSharedObject &other);

	AMFVarPtr				m_strSOName;
	char					m_szPath[MAX_PATH];
	uint32_t				m_nVersion;
	bool					m_bPersistent;
	MSOEventList			m_lstEvents;
	uint32_t				m_nOwnerId;
	bool					m_bFlexSO;
protected:
private:
	AMFCodecCtx				m_sCodecCtx;
};

//////////////////////////////////////////////////////////////////////////
///
/// class MCommand
///
/// Command Message (20)
/// Command messages carry the AMF-encoded commands between the client
/// and the server. These messages have been assigned message type value
/// of 20 for AMF0 encoding and message type value of 17 for AMF3
/// encoding. These messages are sent to perform some operations like
/// connect, createStream, publish, play, pause on the peer. Command
/// messages like onstatus, result etc. are used to inform the sender
/// about the status of the requested commands. A command message
/// consists of command name, transaction ID, and command object that
/// contains related parameters. A client or a server can request Remote
/// Procedure Calls (RPC) over streams that are communicated using the
/// command messages to the peer.
//////////////////////////////////////////////////////////////////////////

typedef enum MCmdTypeE
{
	MCMD_NULL					= 0,
	MCMD_CONNECT				= 1,
	MCMD_CALL					= 2,
	MCMD_CREATESTREAM			= 3,
	MCMD_GETSTREAMLENGTH		= 4,
	MCMD_CLOSE					= 5,
	MCMD_PLAY					= 6,
	MCMD_PLAY2					= 7,
	MCMD_DELETESTREAM			= 8,
	MCMD_CLOSESTREAM			= 9,
	MCMD_RELEASESTREAM			= 10,
	MCMD_RECEIVEAUDIO			= 11,
	MCMD_RECEIVEVIDEO			= 12,
	MCMD_FCPUBLISH				= 13,
	MCMD_PUBLISH				= 14,
	MCMD_FCUNPUBLISH			= 15,
	MCMD_SEEK					= 16,
	MCMD_PAUSE					= 17,
	MCMD_PAUSERAW				= 18,
	MCMD_DISCONNECT				= 19,
	MCMD_ONSTATUS				= 20,
	MCMD_RESULT					= 21,
	MCMD_ERROR					= 22,
	MCMD_WARNING				= 23,
	MCMD_ADMINSVR				= 24,
	MCMD_UNPUBLISH				= 25,
}MCmdTypeE;

///////////////////////////////////////////////////////////////////////////////
// class : MCommand
///////////////////////////////////////////////////////////////////////////////

#define CMDNAME_INDEX		0
#define TRANSID_INDEX		1
#define PROPERTY_INDEX		2
#define OPTION_INDEX		3

class MCommand;

class RTMP_API MCommand : public MBase
{
	DECLARE_FIXED_ALLOC(MCommand);
public:
	static const char	*_result;
	static const char	*_error;
	static const char	*onStatus;
	static const char	*s_szLevel;
	static const char	*s_szCode;
	static const char	*s_szDescription;
	static const char	*s_szResult;
	static const char	*s_szError;
	static const char	*s_szStatus;
public:
	MCommand(bool bFlex = false);
	MCommand(const MBase &refMsg);
	MCommand(const MCommand &other);
	MCommand(uint32_t nChannelId, uint32_t nTimestamp, uint32_t nStreamId);
	virtual ~MCommand();

	bool			Decode(PPacket &refInPacket);
	bool			Encode(PPacket &refOutPacket);
	bool			Decode(BCBuffer &sInBuffer);
	bool			Encode(BCBuffer &sOutBuffer);
	void			Dump();

	void			SetCmdName(const char *szValue);
	uint32_t		GetCmdType() const;
	const BCPString GetCmdName() const;
	void			SetTransId(uint32_t nTransId);
	uint32_t		GetTransId() const;
	void			SetClientId(const char *szClientId);
	LPCSTR			GetClientId() const;
	// Add properties if command properties is AMF0Object, which will
	// create AMF0Object type command object automatically
	void			AddBoolProperty(const char *szProp, bool bValue);
	void			AddDoubleProperty(const char *szProp, double dblValue);
	void			AddStringProperty(const char *szProp, const char *szValue);
	void			AddVarProperty(const char *szOption, const AMF::AMFVarPtr & pVar);
	void			AddProperty(const AMF::AMFVarPtr & pVar);
	AMFVarPtr		GetPropertyByKey(const char *szProp);
	AMFVarPtr	&	GetProperty();
	AMFVarPtr		SetProperty(const AMF::AMFVarPtr &props);
	// Add options if response is AMF0Object, which will create AMF0Object type
	// response automatically
	void			AddBoolOption(const char *szOption, bool bValue);
	void			AddDoubleOption(const char *szOption, double dblValue);
	void			AddStringOption(const char *szOption, const char *szValue);
	void			AddVarOption(const char *szOption, const AMF::AMFVarPtr & pVar);
	AMFVarPtr		GetOptionByKey(const char *szOptionKey);
	const BCPString GetResponseInfo(const char *szKey);
	AMFVarPtr		GetResponse();
	// Set response type as AMF* type if response type is not AMF0Object
	void			SetBoolResponse(bool bValue);
	void			SetDoubleResponse(double dblValue);
	void			SetStringResponse(const char *szValue);
	void			SetUndefinedResponse();
	void			SetVarResponse(const AMF::AMFVarPtr & pResp);
	void			AppendNullResponse();
	void			AppendBoolResponse(bool bValue);
	void			AppendDoubleResponse(double dblValue);
	void			AppendStringResponse(const char *szValue);
	void			AppendResponse(const AMF::AMFVarPtr & pValue);

	void			Clear();
	void			Reset();

	MCommand	*	Clone() const;
	MCommand	&	operator=(const MCommand &other);

	// Get command var
	AMFVarPtr		GetVar(uint32_t nIndex) const;
	uint32_t		GetVarCount() const;

	// Get command arguments
	/*
	 * index starts from 1
	 */
	AMFVarPtr		GetArguments(uint32_t nIndex) const;
	uint32_t		GetArgumentsCount() const;

	// Static methods
	static uint32_t GetCmdType(LPCSTR pCmdStr);

    inline void		SetEncoding(long eEncoding)
	{
		m_sCtx.SetEncoding(eEncoding);
	}
	inline long		GetEncoding() const
	{
		return m_sCtx.GetEncoding();
	}

	uint32_t				m_eCmdType;
	/*
	 * Index 0 - AMFString : command name string
	 * Index 1 - AMFDouble : command transaction id value
	 * Index 2 - AMF0Object(or AMFNull) : command property information object
	 * Index 3 - AMF0Object(or AMFNull) : command response information object
	 */
	LPCSTR						m_strCmdName;
	AMF::AMFVarWrapperList		m_lstVars;
	uint32_t					m_nOwnerId;
	uint32_t					m_nCmdId;
	char						m_szClientId[33];
protected:
	// Automatically called when set command name, which inticated
	// this command instance will be a send command
	void			_Initialize();
	AMFVarPtr		_EnsureItemByType(uint32_t nIndex, uint32_t eType);
private:
	bool					m_bInit;
	bool					m_bFlex;
    AMFCodecCtx				m_sCtx;
};

//////////////////////////////////////////////////////////////////////////
/// class MAggregate
//////////////////////////////////////////////////////////////////////////

class RTMP_API MAggregate : public MBase
{
public:
	MAggregate();
	~MAggregate();

	bool			Decode(PPacket &refInPacket);
	bool			Encode(PPacket &refOutPacket);
	BCRESULT		Append(PPacket &refAVPacket);
	void			Reset();
	void			Dump();

private:
	DECLARE_NO_COPY_CLASS(MAggregate);
	BCBuffer				m_sBody;
};

//////////////////////////////////////////////////////////////////////////
/// class MCodec - rtmp message codec
//////////////////////////////////////////////////////////////////////////

class RTMP_API MCodec
{
public:
	MCodec();
	~MCodec();

	bool				Decode(MBase &sOutMsg, PPacket &sInMsg);
	bool				Encode(PPacket &sOutMsg, MBase &sInMsg);
	inline void			SetEncoding(uint32_t eEncoding)
	{
		m_eObjectEncoding = eEncoding;
	}
	inline uint32_t		GetEncoding() const
	{
		return m_eObjectEncoding;
	}
private:
	DECLARE_NO_COPY_CLASS(MCodec);
	uint8_t				m_eObjectEncoding;
};

//////////////////////////////////////////////////////////////////////////
/// struct : DropInfoS - Drop stats
//////////////////////////////////////////////////////////////////////////

typedef struct AVDropStatS{
	uint32_t		nAudioPacketCount;
	uint32_t		nVideoPacketCount;
	uint64_t		nLastDropTotalTime;
	uint64_t		nFirstTotalTime;
	AVDropStatS() : nAudioPacketCount(0), nVideoPacketCount(0)
		, nLastDropTotalTime(0), nFirstTotalTime(0){}
}AVDropStatS;

///////////////////////////////////////////////////////////////////////////////
// namespace scope functions
///////////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
// End of namespace : RTMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace RTMP

#endif // RTMP_PACKET_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : Packet.h
///////////////////////////////////////////////////////////////////////////////
