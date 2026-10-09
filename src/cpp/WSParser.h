///////////////////////////////////////////////////////////////////////////////
// file : WSParser.h
// author : zhoukai88@jd.com（原始实现）/ anto（移植进 ttsignal）
//
// RFC 6455 帧编解码。不碰任何 socket —— 收到的明文字节由调用方喂进
// ParseWSFrame，要发的帧经 OnWSWrite 交回调用方。
//
// 与从 jmp 拷进来的原版相比，本文件在移植时修了四处收包侧的缺陷（都有单测钉住，
// 见 src/cpp/tests/WSParser_test.cpp）：
//   1. **分片消息的中间帧原本被整段丢弃** —— 只有 FIN 帧才派发，前面的 payload
//      连累积都没有。对端一旦分片（大消息、流式生成的响应都会分片），业务收到的
//      就是残缺内容。现在按 opcode 0x0（continuation）累积到 frag_buffer_，
//      FIN 到达时一次性交付。
//   2. **零长度 payload 会取空 vector 的 &v[0]** —— 空 ping、空 close、空文本帧
//      都会命中，是 UB。现在零长度直接交付 NULL/0。
//   3. **payload 长度没有上界** —— 64 位长度字段照 resize()，对端发一个
//      0x7FFFFFFFFFFFFFFF 就是当场 bad_alloc / OOM。现在有 max_frame_bytes_。
//   4. **RSV 位与控制帧约束没校验** —— 未协商扩展时 RSV 必须为 0；控制帧必须
//      是 FIN 且 payload <= 125 字节。
///////////////////////////////////////////////////////////////////////////////
#ifndef WSPARSER_H_INCLUDED__
#define WSPARSER_H_INCLUDED__

#include <unordered_map>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <BC/BCStream.h>
#include <llhttp.h>
// HttpHeaderMap / GetLowerCaseHeaders / LLHTTPParser 已拆到独立文件，HTTP 客户端
// （HttpConnector）与 WebSocket 握手共用。三者都在全局命名空间里，下面 WS 里的
// 引用无需改写。
#include "LLHTTPParser.h"
#include "SMPacket.h"


using namespace BC;
using namespace SMP;

///////////////////////////////////////////////////////////////////////////////
// Namespace : WS
///////////////////////////////////////////////////////////////////////////////

namespace WS
{

///////////////////////////////////////////////////////////////////////////////
// Enum : WSOpCode
///////////////////////////////////////////////////////////////////////////////
typedef enum {
	WS_OP_CONTINUE			= 0x0, /* 0000 - continue frame */
	WS_OP_TEXT				= 0x1, /* 0001 - text frame */
	WS_OP_BINARY			= 0x2, /* 0010 - binary frame */
	WS_OP_CLOSE				= 0x8, /* 1000 - close frame */
	WS_OP_PING				= 0x9, /* 1001 - ping frame */
	WS_OP_PONG				= 0xA, /* 1010 - pong frame */
}WSOpCode;

// 单帧（以及分片消息重组后）payload 的默认上限。没有上界的话对端只要在 64 位
// 长度字段里填一个大数，我们就会照着 resize()。
#define WS_DEFAULT_MAX_FRAME_BYTES		(8u * 1024u * 1024u)
// 控制帧的 payload 上限，RFC 6455 5.5 规定死了
#define WS_MAX_CONTROL_PAYLOAD			125u

///////////////////////////////////////////////////////////////////////////////
// Class : WSFrameHeader
///////////////////////////////////////////////////////////////////////////////
class WSFrameHeader
{
public:
	WSFrameHeader(){}
	~WSFrameHeader(){}

	uint8_t		is_final = false;
	uint8_t		opcode = 0;
	uint8_t		has_mask = false;
	uint8_t		mask[4] = { 0, 0, 0, 0 };
	size_t		payload_size = 0;
};

///////////////////////////////////////////////////////////////////////////////
// Class : WSParser
///////////////////////////////////////////////////////////////////////////////

class WSParser
{
	typedef enum ParseStateE
	{
		PARSE_FIRST_2_BYTES		= 0,
		PARSE_PAYLOAD_SIZE		= 1,
		PARSE_MASK				= 2,
		PARSE_PAYLOAD			= 3,
	}ParseStateE;

public:
	WSParser();
	virtual ~WSParser() {}

	// 协议违规一律 throw BCException，由调用方决定怎么收场（WSConnection 会转成
	// OnException + 关连接）。
	void				ParseWSFrame(const void* data, size_t size);
	/*
	 * A server must not mask any frames that it sends to the client.
	 * 反过来，**客户端发出的每一帧都必须掩码**（RFC 6455 5.1），否则合规的
	 * 服务端会以 1002 关连接。
	 */
	BCRESULT			WriteToWS(
							uint8_t type, 
							bool mask, 
							BufferPtr data, 
							void* user_data);
	void				Cleanup();

	// 0 表示不改。收包侧的上限，发包侧不限制（发多大由业务自己负责）。
	//
	// ⚠️ 夹在 UINT32_MAX：_RequireData 的形参是 uint32_t，而 payload_size 是
	// size_t，_EnterPayloadStage 会把后者强转传进去。上限一旦超过 4 GiB，
	// 那次强转就是静默截断，require_data_size_ 与真实 payload_size 对不上，
	// 解析状态机的行为不可预测。这里是不变量的持有者，兜最后一道；业务配置
	// 入口（WSConnection::Config::Init）另有一道带告警的 clamp。
	void				SetMaxFrameBytes(size_t nBytes)
	{
		if (nBytes > 0)
		{
			max_frame_bytes_ = (nBytes > (size_t)UINT32_MAX)
								? (size_t)UINT32_MAX : nBytes;
		}
	}
	size_t				MaxFrameBytes() const { return max_frame_bytes_; }

	static size_t		CalculateFrameHeaderSize(bool mask_data, size_t data_len);
	static BufferPtr	MaskBuffer(BufferPtr origion, MaskBufferPtr mask_key);
	static BufferPtr	BuildFrame(WSFrameHeader& header, BCBuffer* pData);
	// 把 SMPacket 的 origion_data 封成一帧。
	//
	// ⚠️ 签名与 jmp 的 PackPacket(JMPacketPtr, bool) 不同：SMPacket 没有 JMPacket
	// 的 ws_type / packed_ws_data / mask / mask_key 那几个字段（它是 SMP/QUIC 那条
	// 路径的包，不该为 WebSocket 往里加字段），所以 opcode 改由调用方传入、结果
	// 直接返回，而不是写回 pkt。pkt 为空或没有 origion_data 时返回空指针。
	static BufferPtr	PackPacket(
							SMPacketPtr pkt,
							uint8_t opcode,
							bool has_mask = false);

	// 握手用的两个纯函数（原先在 jmp 的 Utils.cpp 里，ttsignal 的 Utils 没有，
	// 放这里顺带可单测）。
	// 随机 16 字节的 base64，即 Sec-WebSocket-Key。
	static std::string	WSGenKey();
	// RFC 6455 4.2.2：base64(SHA1(key + GUID))，即 Sec-WebSocket-Accept 的期望值。
	static std::string	WSAcceptKey(const std::string& client_key);

protected:

	virtual int	    OnWSWrite(
						std::shared_ptr<BCBuffer> data, 
						void* user_data)						= 0;
    virtual void    OnRecvWSFrame(
						const WSFrameHeader &header, 
						const uint8_t* payload,
						size_t payload_len)						= 0;

	bool			_RequireData(
						uint32_t nSize,
						ParseStateE eParseState);
	// Message header receive
	bool			_ParseFirst2Bytes();
	bool			_ParsePayloadSize();
	bool			_ParseMask();
	bool			_ParsePayload();
	// payload_size 定下来之后统一走这里：查上限、查控制帧约束，然后进
	// PARSE_MASK / PARSE_PAYLOAD。
	bool			_EnterPayloadStage();
	// 把一帧的 payload 交付出去，负责分片重组
	void			_DeliverPayload(const uint8_t* payload, size_t payload_len);
private:
	DECLARE_NO_COPY_CLASS(WSParser);
	BCBuffer					buffer_;
	BCBIStream					reader_;
	WSFrameHeader				parsed_header_;
	std::shared_ptr<BCBuffer>	parsed_frame_;
	// Asynch state
	ParseStateE					parse_state_ = PARSE_FIRST_2_BYTES;
	uint32_t					require_data_size_ = 0;
	std::vector<uint8_t>		flat_buffer_;
	size_t						max_frame_bytes_ = WS_DEFAULT_MAX_FRAME_BYTES;
	// 分片重组：in_fragment_ 为真表示收到过非 FIN 的数据帧，正在等
	// continuation。控制帧允许插在分片消息中间，不影响这两个字段。
	std::vector<uint8_t>		frag_buffer_;
	uint8_t						frag_opcode_ = 0;
	bool						in_fragment_ = false;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : WS
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : WS

#endif // WSPARSER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : WSParser.h
///////////////////////////////////////////////////////////////////////////////
