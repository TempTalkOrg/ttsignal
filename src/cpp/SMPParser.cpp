///////////////////////////////////////////////////////////////////////////////
// file : SMPParser.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "SMPParser.h"
#include <assert.h>
#ifdef OS_MAC
#include <malloc/malloc.h> // realloc
#else // OS_MAC
#include <malloc.h> // realloc
#endif // OS_MAC
#include <memory.h> // memcpy, memmove
#include <stdio.h> // printf




#define MY_UI8BEI(x) \
	(uint32_t)((x)[0])
#define MY_UI16BEI(x) \
	(uint32_t)((((uint32_t)((x)[0]) & 0xFF) << 8) + \
	         ((uint32_t)((x)[1]) & 0xFF))
#define MY_UI24BEI(x) \
	(uint32_t)((((uint32_t)((x)[0]) & 0xFF) << 16) + \
	        (((uint32_t)((x)[1]) & 0xFF) << 8) + \
	         ((uint32_t)((x)[2]) & 0xFF))
#define MY_UI32BEI(x) \
	(uint32_t)((((uint32_t)((x)[0]) & 0xFF) << 24) + \
	        (((uint32_t)((x)[1]) & 0xFF) << 16) + \
	        (((uint32_t)((x)[2]) & 0xFF) << 8) + \
	         ((uint32_t)((x)[3]) & 0xFF))
#define MY_UI64BEI(x) \
	(uint64_t)((((uint64_t)((x)[0]) & 0xFF) << 56) + \
	        (((uint64_t)((x)[1]) & 0xFF) << 48) + \
	        (((uint64_t)((x)[2]) & 0xFF) << 40) + \
            (((uint64_t)((x)[3]) & 0xFF) << 32) + \
            (((uint64_t)((x)[4]) & 0xFF) << 24) + \
            (((uint64_t)((x)[5]) & 0xFF) << 16) + \
            (((uint64_t)((x)[6]) & 0xFF) << 8) + \
	         ((uint64_t)((x)[7]) & 0xFF))

#define MY_UI8BEO(p, u) \
	((char *)p)[0] = (char)((u & 0x000000FF));
#define MY_UI16BEO(p, u) \
	((char *)p)[0] = (char)((u & 0x0000FF00) >> 8);\
	((char *)p)[1] = (char)((u & 0x000000FF));
#define MY_UI24BEO(p, u) \
	((char *)p)[0] = (char)((u & 0x00FF0000) >> 16);\
	((char *)p)[1] = (char)((u & 0x0000FF00) >> 8);\
	((char *)p)[2] = (char)((u & 0x000000FF));
#define MY_UI32BEO(p, u) \
	((char *)p)[0] = (char)((u & 0xFF000000) >> 24);\
	((char *)p)[1] = (char)((u & 0x00FF0000) >> 16);\
	((char *)p)[2] = (char)((u & 0x0000FF00) >> 8);\
	((char *)p)[3] = (char)((u & 0x000000FF));
#define MY_UI64BEO(p, u) \
	((char *)p)[0] = (char)((u & 0xFF00000000000000) >> 56);\
	((char *)p)[1] = (char)((u & 0x00FF000000000000) >> 48);\
	((char *)p)[2] = (char)((u & 0x0000FF0000000000) >> 40);\
	((char *)p)[3] = (char)((u & 0x000000FF00000000) >> 32);\
	((char *)p)[4] = (char)((u & 0x00000000FF000000) >> 24);\
	((char *)p)[5] = (char)((u & 0x0000000000FF0000) >> 16);\
	((char *)p)[6] = (char)((u & 0x000000000000FF00) >> 8);\
	((char *)p)[7] = (char)((u & 0x00000000000000FF));


#define DEFAULT_HEADER_SIZE     17

///////////////////////////////////////////////////////////////////////////////
// Namespace : SMP
///////////////////////////////////////////////////////////////////////////////

namespace SMP
{

//////////////////////////////////////////////////////////////////////////
/// class SMPHeader
//////////////////////////////////////////////////////////////////////////

SMPHeader::SMPHeader()
    : m_eDataType(0)
	, m_nDataSize(0)
	, m_nTimestamp(0)
	, m_nTransId(0)
	, m_nStreamId(0)
{
	//
}

SMPHeader::SMPHeader(
	uint8_t eDataType,
	uint32_t nDataSize,
	uint64_t nTimestamp,
	uint32_t nTransId,
	uint32_t nStreamId)
		: m_eDataType(eDataType)
		, m_nDataSize(nDataSize)
		, m_nTimestamp(nTimestamp)
		, m_nTransId(nTransId)
		, m_nStreamId(nStreamId)
{
	//
}

SMPHeader::SMPHeader(const SMPHeader &other)
{
	m_eDataType = other.m_eDataType;
	m_nDataSize = other.m_nDataSize;
	m_nTimestamp = other.m_nTimestamp;
	m_nTransId = other.m_nTransId;
	m_nStreamId = other.m_nStreamId;
}

SMPHeader::~SMPHeader()
{
	//
}

SMPHeader &SMPHeader::operator = (const SMPHeader &other)
{
	m_eDataType = other.m_eDataType;
	m_nDataSize = other.m_nDataSize;
	m_nTimestamp = other.m_nTimestamp;
	m_nTransId = other.m_nTransId;
	m_nStreamId = other.m_nStreamId;
	return *this;
}

void SMPHeader::Reset()
{
	m_eDataType = 0;
	m_nDataSize = 0;
	m_nTransId = 0;
	m_nTimestamp = 0;
	m_nStreamId = 0;
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPParser
///////////////////////////////////////////////////////////////////////////////

SMPParser::SMPParser()
    : m_pHandler(NULL)
    , data_buf_(NULL)
    , buf_size_(0)
    , data_size_(0)
    , parsed_size_(0)
	, parse_state_(PARSE_TYPE)
    , require_data_size_(DEFAULT_HEADER_SIZE)
	, pack_data_buf_(NULL)
	, pack_buf_size_(0)
{
}

SMPParser::~SMPParser() 
{ 
	Cleanup(); 
}

bool SMPParser::Create(ISMPacketHandler *pHandler, uint32_t stream_id) {
  assert(pHandler != NULL);
  m_pHandler = pHandler;
  parsed_header_.m_nStreamId = stream_id;

  return true;
}

void SMPParser::Initialize() {
  require_data_size_ = DEFAULT_HEADER_SIZE;
  data_size_ = 0;
}

void SMPParser::Parse(const void *data, size_t data_size) {
  bool bContinue = true;

  if (buf_size_ - data_size_ < data_size) {
    data_buf_ = (char *)realloc(data_buf_, data_size_ + data_size);
    buf_size_ = data_size_ + data_size;
  }
  memcpy(data_buf_ + data_size_, data, data_size);
  data_size_ += data_size;
  while (bContinue) {
    if (require_data_size_ > (data_size_ - parsed_size_)) {
      break;
    }
    switch (parse_state_) {
      case PARSE_TYPE:
        bContinue = _ParseHeader();
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
  if (data_size_ <= parsed_size_) {
    data_size_ = 0;
    parsed_size_ = 0;
  } else if (parsed_size_ > 0) {
    memmove(data_buf_, data_buf_ + parsed_size_, data_size_ - parsed_size_);
    data_size_ -= parsed_size_;
    parsed_size_ = 0;
  }
}

void SMPParser::PackData(
	uint8_t type,
	uint64_t timestamp,
	uint32_t transId,
	std::shared_ptr<BCBuffer> payload)
{
	int offset = 0;
	BCBuffer buffer;
	// Make a reference close, don't copy data
	payload->RefClone(&buffer);
	uint32_t payload_size = buffer.RemainingLength();
	if (pack_buf_size_ < DEFAULT_HEADER_SIZE + payload_size)
	{
		pack_buf_size_ = DEFAULT_HEADER_SIZE + payload_size;
		pack_data_buf_ = (char *)realloc(pack_data_buf_, pack_buf_size_);
	}
	MY_UI8BEO(pack_data_buf_ + offset, type);
	offset += 1;
	MY_UI32BEO(pack_data_buf_ + offset, payload_size);
	offset += 4;
	MY_UI64BEO(pack_data_buf_ + offset, timestamp);
	offset += 8;
	MY_UI32BEO(pack_data_buf_ + offset, transId);
	offset += 4;
	BCBIStream reader(&buffer);
	BCFBOStream sWriter(pack_data_buf_ + offset, payload_size);
	sWriter.WriteFrom(reader, payload_size);
	offset += payload_size;
	if (m_pHandler)
	{
		m_pHandler->OnDataPacked(pack_data_buf_, offset);
	}
}

void SMPParser::Cleanup() {
  free(data_buf_);
  data_buf_ = NULL;
  data_size_ = 0;
  buf_size_ = 0;
  parsed_size_ = 0;
  parse_state_ = PARSE_TYPE;
  free(pack_data_buf_);
  pack_data_buf_ = NULL;
  pack_buf_size_ = 0;
}

BCRESULT SMPParser::PackPacket(SMPacketPtr pkt)
{
	BCBuffer buffer;
	// Make a reference close, don't copy data
	pkt->origion_data->RefClone(&buffer);
	uint32_t payload_size = buffer.RemainingLength();
	if (!pkt->packed_jmp_data)
	{
		pkt->packed_jmp_data.reset(new BCBuffer);
	}
	else
	{
		pkt->packed_jmp_data->Reset();
	}
	BCBOStream sWriter(pkt->packed_jmp_data.get());
	sWriter.WriteUInt8(pkt->type);
	sWriter.WriteUInt32BE(payload_size);
	sWriter.WriteUInt64BE(pkt->timestamp);
	sWriter.WriteUInt32BE(pkt->trans_id);
	buffer.WriteTo(*pkt->packed_jmp_data);
	return BC_R_SUCCESS;
}

bool SMPParser::_RequireData(uint32_t nSize, ParseStateE eParseState) {
  // Change state needs previous process finished.
  if (require_data_size_ == 0) {
    parse_state_ = eParseState;
  }
  require_data_size_ += nSize;
  if (data_size_ - parsed_size_ >= require_data_size_) {
    return true;
  }
  return false;
}

bool SMPParser::_ParseHeader() {

  assert(require_data_size_ >= DEFAULT_HEADER_SIZE);
  // Parse packet type
  parsed_header_.m_eDataType = MY_UI8BEI(data_buf_ + parsed_size_);
  require_data_size_ -= 1;
  parsed_size_ += 1;
  // Parse payload size
  parsed_header_.m_nDataSize = MY_UI32BEI(data_buf_ + parsed_size_);
  require_data_size_ -= 4;
  parsed_size_ += 4;
  // Parse timestamp
  parsed_header_.m_nTimestamp = MY_UI64BEI(data_buf_ + parsed_size_);
  require_data_size_ -= 8;
  parsed_size_ += 8;
  // Parse transaction id
  parsed_header_.m_nTransId = MY_UI32BEI(data_buf_ + parsed_size_);
  require_data_size_ -= 4;
  parsed_size_ += 4;

  return _RequireData(parsed_header_.m_nDataSize, PARSE_PAYLOAD);
}

bool SMPParser::_ParsePayload() {
  char *payload = data_buf_ + parsed_size_;
  assert(require_data_size_ >= parsed_header_.m_nDataSize);
  require_data_size_ -= parsed_header_.m_nDataSize;

  // Notify chunk finished
  m_pHandler->OnPacketParsed(parsed_header_, payload, parsed_header_.m_nDataSize);

  parsed_size_ += parsed_header_.m_nDataSize;

  return _RequireData(DEFAULT_HEADER_SIZE, PARSE_TYPE);
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : SMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace SMP

///////////////////////////////////////////////////////////////////////////////
// End of file : SMPParser.cpp
///////////////////////////////////////////////////////////////////////////////
 