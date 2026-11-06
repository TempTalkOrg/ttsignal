


















#ifndef WSPARSER_H_INCLUDED__
#define WSPARSER_H_INCLUDED__

#include <unordered_map>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <BC/BCStream.h>
#include <llhttp.h>



#include "LLHTTPParser.h"
#include "SMPacket.h"


using namespace BC;
using namespace SMP;





namespace WS
{




typedef enum {
	WS_OP_CONTINUE			= 0x0, 
	WS_OP_TEXT				= 0x1, 
	WS_OP_BINARY			= 0x2, 
	WS_OP_CLOSE				= 0x8, 
	WS_OP_PING				= 0x9, 
	WS_OP_PONG				= 0xA, 
}WSOpCode;



#define WS_DEFAULT_MAX_FRAME_BYTES		(8u * 1024u * 1024u)

#define WS_MAX_CONTROL_PAYLOAD			125u




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

	
	
	void				ParseWSFrame(const void* data, size_t size);
	




	BCRESULT			WriteToWS(
							uint8_t type, 
							bool mask, 
							BufferPtr data, 
							void* user_data);
	void				Cleanup();

	
	
	
	
	
	
	
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
	
	
	
	
	
	
	static BufferPtr	PackPacket(
							SMPacketPtr pkt,
							uint8_t opcode,
							bool has_mask = false);

	
	
	
	static std::string	WSGenKey();
	
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
	
	bool			_ParseFirst2Bytes();
	bool			_ParsePayloadSize();
	bool			_ParseMask();
	bool			_ParsePayload();
	
	
	bool			_EnterPayloadStage();
	
	void			_DeliverPayload(const uint8_t* payload, size_t payload_len);
private:
	DECLARE_NO_COPY_CLASS(WSParser);
	BCBuffer					buffer_;
	BCBIStream					reader_;
	WSFrameHeader				parsed_header_;
	std::shared_ptr<BCBuffer>	parsed_frame_;
	
	ParseStateE					parse_state_ = PARSE_FIRST_2_BYTES;
	uint32_t					require_data_size_ = 0;
	std::vector<uint8_t>		flat_buffer_;
	size_t						max_frame_bytes_ = WS_DEFAULT_MAX_FRAME_BYTES;
	
	
	std::vector<uint8_t>		frag_buffer_;
	uint8_t						frag_opcode_ = 0;
	bool						in_fragment_ = false;
};





} 

#endif 




