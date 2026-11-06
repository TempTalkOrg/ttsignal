






#include "StdAfx.h"
#include <inttypes.h> 
#include <openssl/rand.h> 
#include <openssl/sha.h>  
#include <BC/Base64.h>    
#include "BC/Utils.h"
#include "BC/BCException.h"
#include "WSParser.h"
#include "Runtime.h"
#include "Utils.h"







namespace WS
{






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
	
	size_t readSize = reader_.Read(data, 2);
	require_data_size_ -= readSize;
	
	parsed_header_.is_final = (data[0] & 0x80) ? 1 : 0;
	
	
	if (data[0] & 0x70)
	{
		PARSE_ERROR("Invalid websocket frame: RSV bits set but no extension negotiated");
	}
	tmp_opcode = (data[0] & 0x0F);

	
	if (tmp_opcode > WS_OP_PONG ||
		(tmp_opcode > WS_OP_BINARY && tmp_opcode < WS_OP_CLOSE))
	{
		PARSE_ERROR("Invalid websocket frame: reserved opcode");
	}

	const bool isControl = (tmp_opcode & 0x08) != 0;
	if (tmp_opcode == WS_OP_CONTINUE)
	{
		
		
		if (!in_fragment_)
		{
			PARSE_ERROR("Invalid websocket frame: continuation without a started message");
		}
		parsed_header_.opcode = frag_opcode_;
	}
	else if (isControl)
	{
		
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

	
	parsed_header_.has_mask = (data[1] & 0xFF) >> 7;
	parsed_header_.payload_size = (data[1] & 0x7F);

	
	
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
		
		
		
		
		
		
		
		
		
		
		if (tmp_opcode == WS_OP_CLOSE && parsed_header_.payload_size == 1)
		{
			PARSE_ERROR("Invalid websocket frame: close frame body must be empty or >= 2 bytes");
		}
	}

	
	if (parsed_header_.payload_size == 126)
	{
		

		return _RequireData(2, PARSE_PAYLOAD_SIZE);
	}
	else if (parsed_header_.payload_size == 127)
	{
		

		return _RequireData(8, PARSE_PAYLOAD_SIZE);
	}
	return _EnterPayloadStage();
}

bool WSParser::_ParsePayloadSize() 
{
	
	if (parsed_header_.payload_size == 126)
	{
		uint16_t size = 0;

		ASSERT(buffer_.RemainingLength() >= 2);
		

		size_t readSize = reader_.ReadUInt16BE(&size);
		require_data_size_ -= readSize;
		parsed_header_.payload_size = size;
	}
	else if (parsed_header_.payload_size == 127)
	{
		uint64_t size = 0;

		ASSERT(buffer_.RemainingLength() >= 8);
		

		size_t readSize = reader_.ReadUInt64BE(&size);
		require_data_size_ -= readSize;
		
		
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
	
	
	if (parsed_header_.payload_size > max_frame_bytes_)
	{
		PARSE_ERROR("Invalid websocket frame: payload length exceeds maxFrameBytes");
	}
	
	
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
		
		OnRecvWSFrame(parsed_header_, payload, payload_len);
		return;
	}
	if (parsed_header_.is_final && !in_fragment_)
	{
		
		OnRecvWSFrame(parsed_header_, payload, payload_len);
		return;
	}
	
	
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
		ret = 2; 
	else if (data_len < 65536u)
		ret = 4; 
	else if (data_len < 0xFFFFFFFFFFFFFFFF)
		ret = 10; 
	if (mask_data)
		ret += sizeof(uint32_t); 
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
		
		RAND_bytes(&header.mask[0], sizeof(header.mask));
	}

	
	
	writer.WriteUInt8((uint8_t)(1 << 7)|(uint8_t)(header.opcode & 0x0F));

	
	nByte = (uint8_t)((header.has_mask & 0x1) << 7); 

	
	if (data_len < 126u)
	{
		nByte |= data_len & 0x7F;
		writer.WriteUInt8(nByte);
	}
	
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

		
		writer.Write(header.mask, sizeof(header.mask));

		uint32_t nStartPos = rc->UsedLength();
		
		writer.WriteFrom(buffer, buffer.RemainingLength());
		
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

	
	writer.WriteFrom(buffer, buffer.RemainingLength());
	
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





} 




