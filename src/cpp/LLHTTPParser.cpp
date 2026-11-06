







#include "LLHTTPParser.h"

#include <BC/BCPString.h>

using namespace BC;








static int handle_on_url(llhttp_t* parser, const char* at, size_t length)
{
	LLHTTPParser* pHandler = (LLHTTPParser*)parser;
	if (pHandler)
	{
		return pHandler->http_on_url(at, length);
	}
	return 0;
}

static int handle_on_status(llhttp_t* parser, const char* at, size_t length)
{
	LLHTTPParser* pHandler = (LLHTTPParser*)parser;
	if (pHandler)
	{
		return pHandler->http_on_status(at, length);
	}
	return 0;
}

static int handle_on_header_field(llhttp_t* parser, const char* at, size_t length)
{
	LLHTTPParser* pHandler = (LLHTTPParser*)parser;
	if (pHandler)
	{
		return pHandler->http_on_header_field(at, length);
	}
	return 0;
}

static int handle_on_header_value(llhttp_t* parser, const char* at, size_t length)
{
	LLHTTPParser* pHandler = (LLHTTPParser*)parser;
	if (pHandler)
	{
		return pHandler->http_on_header_value(at, length);
	}
	return 0;
}

static int handle_on_headers_complete(llhttp_t* parser)
{
	LLHTTPParser* pHandler = (LLHTTPParser*)parser;
	if (pHandler)
	{
		return pHandler->http_on_headers_complete();
	}
	return 0;
}

static int handle_on_message_complete(llhttp_t* parser)
{
	LLHTTPParser* pHandler = (LLHTTPParser*)parser;
	if (pHandler)
	{
		return pHandler->http_on_message_complete();
	}
	return 0;
}

static int handle_on_body(llhttp_t* parser, const char* at, size_t length)
{
	LLHTTPParser* pHandler = (LLHTTPParser*)parser;
	if (pHandler)
	{
		return pHandler->http_on_body(at, length);
	}
	return 0;
}

HttpHeaderMap GetLowerCaseHeaders(const HttpHeaderMap& headers)
{
	HttpHeaderMap outHeaders;
	for (auto &iter : headers)
	{
		BCPString strKey(iter.first.c_str(), iter.first.length());
		if (strKey.MakeLower() == "sec-websocket-key" ||
			strKey.MakeLower() == "sec-websocket-accept")
		{
			outHeaders[strKey.c_str()] = iter.second;
		}
		else
		{
			BCPString strValue(iter.second.c_str(), iter.second.length());
			outHeaders[strKey.c_str()] = strValue.MakeLower().c_str();
		}
	}
	return outHeaders;
}





LLHTTPParser::LLHTTPParser(llhttp_type_t type)
{
	_Initialize(type);
}

LLHTTPParser::~LLHTTPParser()
{
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	this->finish = HTTP_FINISH_SAFE;
}

void LLHTTPParser::_Initialize(llhttp_type_t type)
{
	
	llhttp_settings_init(&m_httpSettings);
	
	m_httpSettings.on_message_complete = handle_on_message_complete;
	m_httpSettings.on_url = handle_on_url;
	m_httpSettings.on_status = handle_on_status;
	m_httpSettings.on_header_field = handle_on_header_field;
	m_httpSettings.on_header_value = handle_on_header_value;
	m_httpSettings.on_headers_complete = handle_on_headers_complete;
	m_httpSettings.on_body = handle_on_body;

	



	llhttp_init(this, type, &m_httpSettings);
}




