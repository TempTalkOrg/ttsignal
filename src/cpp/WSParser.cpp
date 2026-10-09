
///////////////////////////////////////////////////////////////////////////////
// file : WSParser.cpp
// author : zhoukai88@jd.com（原始实现）/ anto（移植进 ttsignal）
//
// 设计说明与移植时修掉的四处收包侧缺陷见 WSParser.h 顶部注释。
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include <inttypes.h> // for PRI macros
#include <openssl/rand.h> // for RAND_bytes
#include <openssl/sha.h>  // for SHA1
#include <BC/Base64.h>    // for base64_encode
#include "BC/Utils.h"
#include "BC/BCException.h"
#include "WSParser.h"
#include "Runtime.h"
#include "Utils.h"



///////////////////////////////////////////////////////////////////////////////
// Namespace : WS
///////////////////////////////////////////////////////////////////////////////

namespace WS
{

///////////////////////////////////////////////////////////////////////////////
// File-scope macros & utilities
///////////////////////////////////////////////////////////////////////////////

// RFC 6455 1.3 定的魔术字符串，算 Sec-WebSocket-Accept 用
#define WS_HANDSHAKE_GUID		"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define PARSE_ERROR(_reason)										\
	{																\
		LogDebug(_LOCAL_, "%s; ConsumedLength:%" _U32BITARG_			\
			"; remainingLength:%" _U32BITARG_						\
			"; payloadSize:%" _U64BITARG_							\
			"; has_mask:%" _U32BITARG_,								\
			(_reason),												\
			buffer_.ConsumedLength(),								\
			buffer_.RemainingLength(),								\
			(uint64_t)parsed_header_.payload_size,					\
			(uint32_t)parsed_header_.has_mask);						\
		buffer_.Rewind();											\
		LogBuffer(_LOCAL_, &buffer_);								\
		throw BCException(__FUNCTION__, (_reason));					\
	}

// llhttp 回调桩、GetLowerCaseHeaders、LLHTTPParser 的实现已搬到
// LLHTTPParser.cpp（HTTP 客户端也要用，且 WSParser.cpp 当前被排除在构建之外）。

///////////////////////////////////////////////////////////////////////////////
// class : WSParser
///////////////////////////////////////////////////////////////////////////////

WSParser::WSParser()
	: reader_(&buffer_)
	, require_data_size_(0)
{
	_RequireData(2, PARSE_FIRST_2_BYTES);
}

BCRESULT WSParser::WriteToWS(
	uint8_t type, 
	bool mask, 
	BufferPtr data, 
	void* user_data)
{
	if (type != WS_OP_TEXT && type != WS_OP_BINARY)
	{
		return BC_R_INVALIDARG;
	}
	if (!data)
	{
		return BC_R_INVALIDARG;
	}
	WSFrameHeader header;
	header.opcode = type;
	if (mask)
	{
		header.has_mask = true;
		RAND_bytes(header.mask, sizeof(header.mask));
	}
	else
	{
		header.has_mask = false;
	}
	header.is_final = true;
	header.payload_size = data->RemainingLength();
	auto rc = BuildFrame(header, data.get());
	if (!rc)
	{
		return BC_R_FAILURE;
	}
	OnWSWrite(rc, user_data);
	// 原版无论成功失败都 return BC_R_FAILURE，调用方一判返回值就永远是"失败"。
	return BC_R_SUCCESS;
}

void WSParser::ParseWSFrame(const void* data, size_t size)
{
	bool bContinue = true;

	buffer_.Write(data, size);
	while (bContinue) 
	{
		if (require_data_size_ > reader_.RemainingLength()) 
		{
			break;
		}
		switch (parse_state_) {
		case PARSE_FIRST_2_BYTES:
			bContinue = _ParseFirst2Bytes();
			break;
		case PARSE_PAYLOAD_SIZE:
			bContinue = _ParsePayloadSize();
			break;
		case PARSE_MASK:
			bContinue = _ParseMask();
			break;
		case PARSE_PAYLOAD:
			bContinue = _ParsePayload();
			break;
		default:
			// LogError(_LOCAL_, "Invalid parse state[%"_U32BITARG_"]",
			// parse_state_);
			break;
		}
	}
	if (buffer_.RemainingLength() == 0)
	{
		buffer_.Reset();
	}
	else
	{
		buffer_.RemoveConsumed();
	}
}

void WSParser::Cleanup() 
{
	buffer_.Reset();
	parse_state_ = PARSE_FIRST_2_BYTES;
	require_data_size_ = 0;
	_RequireData(2, PARSE_FIRST_2_BYTES);
	flat_buffer_.clear();
	frag_buffer_.clear();
	frag_opcode_ = 0;
	in_fragment_ = false;
}

bool WSParser::_RequireData(uint32_t nSize, ParseStateE eParseState) 
{
	// Change state needs previous process finished.
	if (require_data_size_ == 0) 
	{
		parse_state_ = eParseState;
	}
	require_data_size_ += nSize;
	if (buffer_.RemainingLength() >= require_data_size_) 
	{
		return true;
	}
	return false;
}

bool WSParser::_ParseFirst2Bytes() 
{
	uint8_t data[2], tmp_opcode;
	ASSERT(require_data_size_ >= 2);
	// Parse packet type
	size_t readSize = reader_.Read(data, 2);
	require_data_size_ -= readSize;
	/* 1st byte */
	parsed_header_.is_final = (data[0] & 0x80) ? 1 : 0;
	// RSV1..RSV3 必须为 0 —— 我们不协商任何扩展（permessage-deflate 之类），
	// 非零意味着对端在用我们不认识的帧格式，继续解析只会得到垃圾。
	if (data[0] & 0x70)
	{
		PARSE_ERROR("Invalid websocket frame: RSV bits set but no extension negotiated");
	}
	tmp_opcode = (data[0] & 0x0F);

	/* invalid websocket packet must return error */
	if (tmp_opcode > WS_OP_PONG ||
		(tmp_opcode > WS_OP_BINARY && tmp_opcode < WS_OP_CLOSE))
	{
		PARSE_ERROR("Invalid websocket frame: reserved opcode");
	}

	const bool isControl = (tmp_opcode & 0x08) != 0;
	if (tmp_opcode == WS_OP_CONTINUE)
	{
		// continuation 帧的真实 opcode 在分片消息的第一帧里，这里补回来，
		// 交付时业务看到的才是 TEXT / BINARY。
		if (!in_fragment_)
		{
			PARSE_ERROR("Invalid websocket frame: continuation without a started message");
		}
		parsed_header_.opcode = frag_opcode_;
	}
	else if (isControl)
	{
		// 控制帧可以插在分片消息中间，不影响 in_fragment_ / frag_opcode_
		parsed_header_.opcode = tmp_opcode;
	}
	else
	{
		if (in_fragment_)
		{
			PARSE_ERROR("Invalid websocket frame: new data frame while a fragmented message is open");
		}
		parsed_header_.opcode = tmp_opcode;
	}

	/* 2nd byte */
	parsed_header_.has_mask = (data[1] & 0xFF) >> 7;
	parsed_header_.payload_size = (data[1] & 0x7F);

	// RFC 6455 5.5：控制帧必须是 FIN，且 payload 不超过 125 字节
	// （payload_size 此刻还是 7 位原值，126/127 就已经越界了）。
	if (isControl)
	{
		if (!parsed_header_.is_final)
		{
			PARSE_ERROR("Invalid websocket frame: fragmented control frame");
		}
		if (parsed_header_.payload_size > WS_MAX_CONTROL_PAYLOAD)
		{
			PARSE_ERROR("Invalid websocket frame: control frame payload exceeds 125 bytes");
		}
		// RFC 6455 5.5.1：close 帧的 body 要么**为空**，要么**至少 2 字节**
		// （2 字节关闭码 + 可选的 UTF-8 原因）。长度为 1 是畸形帧。
		//
		// ⚠️ 不判掉的话它会被当成"没带关闭码"一路交付上去，上层
		// `if (payload_len >= 2)` 取不到码就回落成 1000 —— 一个畸形帧被报成
		// "干净的正常关闭"。这里判掉之后走的是 WSConnection::_FeedFrames 的
		// 异常路径，回给对端的是 1002 Protocol Error。
		//
		// 此刻 payload_size 还是 7 位原值，而控制帧的 126/127 上面已经拦掉了，
		// 所以这个值就是真实长度，可以直接比。
		if (tmp_opcode == WS_OP_CLOSE && parsed_header_.payload_size == 1)
		{
			PARSE_ERROR("Invalid websocket frame: close frame body must be empty or >= 2 bytes");
		}
	}

	/* determine payload length */
	if (parsed_header_.payload_size == 126)
	{
		/* If 126, the following 2 bytes interpreted as a
			  16-bit unsigned integer are the payload length. */
		return _RequireData(2, PARSE_PAYLOAD_SIZE);
	}
	else if (parsed_header_.payload_size == 127)
	{
		/* If 127, the following 8 bytes interpreted as a 64-bit unsigned integer (the
			 most significant bit MUST be 0) are the payload length */
		return _RequireData(8, PARSE_PAYLOAD_SIZE);
	}
	return _EnterPayloadStage();
}

bool WSParser::_ParsePayloadSize() 
{
	/* determine payload length */
	if (parsed_header_.payload_size == 126)
	{
		uint16_t size = 0;

		ASSERT(buffer_.RemainingLength() >= 2);
		/* If 126, the following 2 bytes interpreted as a
			  16-bit unsigned integer are the payload length. */
		size_t readSize = reader_.ReadUInt16BE(&size);
		require_data_size_ -= readSize;
		parsed_header_.payload_size = size;
	}
	else if (parsed_header_.payload_size == 127)
	{
		uint64_t size = 0;

		ASSERT(buffer_.RemainingLength() >= 8);
		/* If 127, the following 8 bytes interpreted as a 64-bit unsigned integer (the
			 most significant bit MUST be 0) are the payload length */
		size_t readSize = reader_.ReadUInt64BE(&size);
		require_data_size_ -= readSize;
		// RFC 6455 5.2：最高位必须为 0。而且 size_t 在 32 位平台上装不下 64 位
		// 长度，先在 uint64_t 上判上限再赋值。
		if ((size >> 63) != 0 || size > (uint64_t)max_frame_bytes_)
		{
			parsed_header_.payload_size = 0;
			PARSE_ERROR("Invalid websocket frame: payload length exceeds maxFrameBytes");
		}
		parsed_header_.payload_size = (size_t)size;
	}

	return _EnterPayloadStage();
}

bool WSParser::_EnterPayloadStage()
{
	// 上限必须在 _RequireData 之前查：require_data_size_ 一旦加上一个天文数字，
	// 后面 flat_buffer_.resize() 就直接 OOM 了。
	if (parsed_header_.payload_size > max_frame_bytes_)
	{
		PARSE_ERROR("Invalid websocket frame: payload length exceeds maxFrameBytes");
	}
	// 分片消息重组之后的总长也要受同一个上限约束，否则对端只要把大消息切成
	// 若干个合法小帧就能绕过限制。
	const bool isControl = (parsed_header_.opcode & 0x08) != 0;
	if (!isControl &&
		frag_buffer_.size() + parsed_header_.payload_size > max_frame_bytes_)
	{
		PARSE_ERROR("Invalid websocket frame: reassembled message exceeds maxFrameBytes");
	}
	if (parsed_header_.has_mask)
	{
		return _RequireData(4, PARSE_MASK);
	}
	return _RequireData((uint32_t)parsed_header_.payload_size, PARSE_PAYLOAD);
}

bool WSParser::_ParseMask() {
	ASSERT(require_data_size_ >= 4);
	ASSERT(buffer_.RemainingLength() >= 4);
	size_t readSize = reader_.Read(parsed_header_.mask, 4);
	require_data_size_ -= readSize;

	return _RequireData((uint32_t)parsed_header_.payload_size, PARSE_PAYLOAD);
}

bool WSParser::_ParsePayload() {
	if (require_data_size_ < parsed_header_.payload_size)
	{
		buffer_.Rewind();
		LogBuffer(_LOCAL_, &buffer_);
		throw BCException(__FUNCTION__, "Invalid websocket frame");
	}
	if (parsed_header_.payload_size == 0)
	{
		// 空 payload（空 ping / 空 close / 空文本帧都合法）。原版会取
		// &flat_buffer_[0]，flat_buffer_ 为空时那是 UB。
		_DeliverPayload(NULL, 0);
		return _RequireData(2, PARSE_FIRST_2_BYTES);
	}
	if (flat_buffer_.size() < parsed_header_.payload_size)
	{
		flat_buffer_.resize(parsed_header_.payload_size);
	}
	BCFBOStream sWriter(&flat_buffer_[0], parsed_header_.payload_size);
	size_t readSize = sWriter.WriteFrom(buffer_, parsed_header_.payload_size);
	require_data_size_ -= readSize;
	uint8_t* data = &flat_buffer_[0];
	/* unmask data */
	if (parsed_header_.has_mask)
	{
		size_t i;
		for (i = 0u; i < readSize; ++i)
			data[i] ^= parsed_header_.mask[i % sizeof(parsed_header_.mask)];
	}
	_DeliverPayload(data, parsed_header_.payload_size);

	return _RequireData(2, PARSE_FIRST_2_BYTES);
}

void WSParser::_DeliverPayload(const uint8_t* payload, size_t payload_len)
{
	const bool isControl = (parsed_header_.opcode & 0x08) != 0;

	if (isControl)
	{
		// 控制帧从不分片，直接交付
		OnRecvWSFrame(parsed_header_, payload, payload_len);
		return;
	}
	if (parsed_header_.is_final && !in_fragment_)
	{
		// 单帧消息：不必经 frag_buffer_ 中转
		OnRecvWSFrame(parsed_header_, payload, payload_len);
		return;
	}
	// 分片消息。原版在这里什么都不做（非 FIN 帧的 payload 被整段丢弃），
	// 于是分片消息的前半截永远送不到业务手上。
	if (payload && payload_len > 0)
	{
		frag_buffer_.insert(frag_buffer_.end(), payload, payload + payload_len);
	}
	if (!parsed_header_.is_final)
	{
		in_fragment_ = true;
		frag_opcode_ = parsed_header_.opcode;
		return;
	}
	WSFrameHeader joined = parsed_header_;
	joined.payload_size = frag_buffer_.size();
	in_fragment_ = false;
	frag_opcode_ = 0;
	OnRecvWSFrame(joined,
		frag_buffer_.empty() ? NULL : &frag_buffer_[0],
		frag_buffer_.size());
	frag_buffer_.clear();
}

size_t WSParser::CalculateFrameHeaderSize(bool mask_data, size_t data_len)
{
	int ret = 0;
	if (data_len < 126u)
		ret = 2; /* header 2 bytes */
	else if (data_len < 65536u)
		ret = 4; /* for extra 2-bytes for payload length */
	else if (data_len < 0xFFFFFFFFFFFFFFFF)
		ret = 10; /* for extra 8-bytes for payload length */
	if (mask_data)
		ret += sizeof(uint32_t); /* for mask */
	return ret;
}

std::shared_ptr<BCBuffer> WSParser::BuildFrame(WSFrameHeader &header, BCBuffer *pData)
{
	BCBuffer buffer;
	std::shared_ptr<BCBuffer> rc(new BCBuffer);
	BCBOStream writer(rc.get());
	size_t data_len = 0L;
	uint8_t nByte;

	pData->RefClone(&buffer);
	data_len = buffer.RemainingLength();
	if (header.has_mask && (header.mask[0] == 0))
	{
		/* generate mask, if we are a client */
		RAND_bytes(&header.mask[0], sizeof(header.mask));
	}

	/* 1st byte */
	/* final flag | 3 bits reserved for negotiation of protocol */
	writer.WriteUInt8((uint8_t)(1 << 7)|(uint8_t)(header.opcode & 0x0F));

	/* 2nd byte */
	nByte = (uint8_t)((header.has_mask & 0x1) << 7); /* masking bit */

	/* payload length */
	if (data_len < 126u)
	{
		nByte |= data_len & 0x7F;
		writer.WriteUInt8(nByte);
	}
	/* 3rd byte & 4th bytes - extended payload length */
	else if (data_len < 65536u)
	{
		nByte |= 126u & 0x7F;
		writer.WriteUInt8(nByte);
		writer.WriteUInt16BE(data_len);
	}
	else if (data_len < 0xFFFFFFFFFFFFFFFF)
	{
		nByte |= 127u & 0x7F;
		writer.WriteUInt8(nByte);
		writer.WriteUInt64BE(data_len);
	}
	else
	{
		LogError(_LOCAL_, "Data too large for websocket frame");
		return NULL;
	}

	if (header.has_mask)
	{
		size_t idx = 0u;
		uint8_t* data;
		uint32_t nReadSize = 0;

		/* copy masking key into ws header */
		writer.Write(header.mask, sizeof(header.mask));

		uint32_t nStartPos = rc->UsedLength();
		/* write payload directly */
		writer.WriteFrom(buffer, buffer.RemainingLength());
		/* mask payload */
		rc->Rewind(nStartPos);
		while ((data = (uint8_t*)rc->ReadBlock(INFINITE, nReadSize)) && nReadSize > 0)
		{
			for (uint32_t i = 0; i < nReadSize; i++, idx++)
			{
				data[i] ^= header.mask[idx % sizeof(header.mask)];
			}
		}
		rc->Rewind(0);
	}
	else
	{
		/* write payload directly */
		writer.WriteFrom(buffer, buffer.RemainingLength());
	}
	return rc;
}

BufferPtr WSParser::MaskBuffer(BufferPtr origion, MaskBufferPtr mask_key)
{
	if (!mask_key || !origion || !mask_key->size())
	{
		return BufferPtr(origion->Clone());
	}
	BCBuffer buffer;
	std::shared_ptr<BCBuffer> rc(new BCBuffer);
	BCBOStream writer(rc.get());

	origion->RefClone(&buffer);
	size_t idx = 0u, mask_size = mask_key->size();
	uint8_t* data, *mask = &(*mask_key)[0];
	uint32_t nReadSize = 0;

	/* write payload directly */
	writer.WriteFrom(buffer, buffer.RemainingLength());
	/* mask payload */
	while ((data = (uint8_t*)rc->ReadBlock(INFINITE, nReadSize)) && nReadSize > 0)
	{
		for (uint32_t i = 0; i < nReadSize; i++, idx++)
		{
			data[i] ^= mask[idx % mask_size];
		}
	}
	rc->Rewind(0);
	return rc;
}

BufferPtr WSParser::PackPacket(SMPacketPtr pkt, uint8_t opcode, bool has_mask)
{
	if (!pkt || !pkt->origion_data)
	{
		return BufferPtr();
	}
	WSFrameHeader header;
	header.opcode = opcode;
	header.has_mask = has_mask;
	header.is_final = true;
	header.payload_size = pkt->origion_data->RemainingLength();
	return BuildFrame(header, pkt->origion_data.get());
}

std::string WSParser::WSGenKey()
{
	// 16 字节随机数的 base64 —— 24 字节输出，留够余量
	char	ws_key[64] = { 0 };
	uint8_t	buf[16];

	RAND_bytes(buf, sizeof(buf));
	int len = base64_encode(ws_key, buf, (int32_t)sizeof(buf));
	if (len <= 0)
	{
		return std::string();
	}
	return std::string(ws_key, (size_t)len);
}

std::string WSParser::WSAcceptKey(const std::string& client_key)
{
	// base64(SHA1(key + GUID))：SHA1 输出 20 字节，base64 之后 28 字节
	char		ws_key[64] = { 0 };
	uint8_t		output[SHA_DIGEST_LENGTH];
	std::string	sha1str = client_key + WS_HANDSHAKE_GUID;

	SHA1((const uint8_t*)sha1str.c_str(), sha1str.length(), output);
	int len = base64_encode(ws_key, output, (int32_t)SHA_DIGEST_LENGTH);
	if (len <= 0)
	{
		return std::string();
	}
	return std::string(ws_key, (size_t)len);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : WS
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : WS

///////////////////////////////////////////////////////////////////////////////
// End of file : WSParser.cpp
///////////////////////////////////////////////////////////////////////////////
