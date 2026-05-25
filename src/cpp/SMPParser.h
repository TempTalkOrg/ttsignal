///////////////////////////////////////////////////////////////////////////////
// file : SMPParser.h
// author : anto
///////////////////////////////////////////////////////////////////////////////

#ifndef SMPPARSER_H_INCLUDED__
#define SMPPARSER_H_INCLUDED__

#include <inttypes.h>
#include <BC/BCBuffer.h>
#include "SMPacket.h"


using namespace BC;


#ifndef DECLARE_NO_COPY_CLASS
#define DECLARE_NO_COPY_CLASS(classname)        \
	private:                                    \
	classname(const classname&);            \
	classname& operator=(const classname&);
#endif

///////////////////////////////////////////////////////////////////////////////
// Namespace : SMP
///////////////////////////////////////////////////////////////////////////////

namespace SMP
{

typedef enum SMP_PTYPE
{
	SMP_TYPE_UNKNOWN			= 0x00,
	SMP_TYPE_COMMAND			= 0x01,
	SMP_TYPE_MESSAGE			= 0x02,
	SMP_TYPE_USER_CONTROL		= 0x03,
	SMP_TYPE_PING				= 0x04,
	SMP_TYPE_PONG				= 0x05,
}SMP_PTYPE;

///////////////////////////////////////////////////////////////////////////////
// Class : SMPHeader
//       - Packet header object
///////////////////////////////////////////////////////////////////////////////

class SMPHeader
{
public:
	SMPHeader();
	SMPHeader(
		uint8_t eDataType,
		uint32_t nDataSize,
		uint64_t nTimestamp,
		uint32_t nTransId,
		uint32_t nStreamId);
	SMPHeader(const SMPHeader &sHeader);
	~SMPHeader();

	SMPHeader &operator = (const SMPHeader &sHeader);
	
	void			Reset();
public:
	uint8_t					m_eDataType;
	uint32_t				m_nDataSize;
	uint64_t				m_nTimestamp;
	uint32_t				m_nTransId;
	uint32_t 				m_nStreamId;
};

///////////////////////////////////////////////////////////////////////////////
// class : ISMPacketHandler
///////////////////////////////////////////////////////////////////////////////

class ISMPacketHandler
{
	friend class SMPParser;
public:
	ISMPacketHandler(){};
	virtual ~ISMPacketHandler(){};

protected:
	virtual void OnPacketParsed(
					const SMPHeader &refHeader, 
					const char *payload,
					size_t payload_size)				= 0;
	virtual void OnDataPacked(void* data, size_t size) {};
private:
	DECLARE_NO_COPY_CLASS(ISMPacketHandler);
};

///////////////////////////////////////////////////////////////////////////////
// class : SMPParser
///////////////////////////////////////////////////////////////////////////////

class SMPParser
{
public:
	typedef enum ParseStateE
	{
		PARSE_TYPE				= 0,
		PARSE_PAYLOAD_SIZE		= 1,
		PARSE_TIMESTAMP			= 2,
		PARSE_STREAM_ID			= 3,
		PARSE_PAYLOAD			= 4,
	}ParseStateE;
public:
	SMPParser();
	virtual ~SMPParser();

	bool				Create(ISMPacketHandler *pHandler, uint32_t stream_id);
	void				Initialize();
	void				Parse(const void *data, size_t data_size);
	void				PackData(
							uint8_t type,
							uint64_t timestamp,
							uint32_t transId,
							std::shared_ptr<BCBuffer> payload);
	void				Cleanup();

	static BCRESULT		PackPacket(SMPacketPtr pkt);

protected:
	bool			_RequireData(
						uint32_t nSize,
						ParseStateE eParseState);
	// Message header receive
	bool			_ParseHeader();
	bool			_ParsePayload();
private:
	DECLARE_NO_COPY_CLASS(SMPParser);
	ISMPacketHandler	*	m_pHandler;
	char                *   data_buf_;
	size_t                  buf_size_;
	size_t                  data_size_;
	size_t                  parsed_size_;
	SMPHeader				parsed_header_;
	// Asynch state
	ParseStateE				parse_state_;
	uint32_t				require_data_size_;
	// pack data
	char                *   pack_data_buf_;
	size_t                  pack_buf_size_;
};

///////////////////////////////////////////////////////////////////////////////
// End of namespace : SMP
///////////////////////////////////////////////////////////////////////////////

} // End of namespace SMP

#endif // SMPPARSER_H_INCLUDED__
