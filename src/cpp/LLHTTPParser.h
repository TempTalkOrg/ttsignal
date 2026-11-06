




















#ifndef LLHTTPPARSER_H_INCLUDED__
#define LLHTTPPARSER_H_INCLUDED__

#include <map>
#include <string>

#include <BC/Config.h>
#include <llhttp.h>

typedef std::map<std::string, std::string> HttpHeaderMap;



HttpHeaderMap GetLowerCaseHeaders(const HttpHeaderMap& headers);





class LLHTTPParser : public llhttp_t
{
public:
	
	
	
	
	
	
	
	
	explicit LLHTTPParser(llhttp_type_t type = HTTP_BOTH);
	virtual ~LLHTTPParser();

	virtual int http_on_url(const char* at, size_t length)			= 0;
	virtual int http_on_status(const char* at, size_t length)		= 0;
	virtual int http_on_header_field(const char* at, size_t length) = 0;
	virtual int http_on_header_value(const char* at, size_t length) = 0;
	virtual int http_on_headers_complete()							= 0;
	virtual int http_on_message_complete()							= 0;
	
	
	virtual int http_on_body(const char* at, size_t length)
	{
		(void)at;
		(void)length;
		return 0;
	}

	
	llhttp_settings_t	m_httpSettings;
	std::string			m_method;
	std::string			m_url;
	HttpHeaderMap		m_headers;
	std::string			m_currentField;
private:
	DECLARE_NO_COPY_CLASS(LLHTTPParser);
	void				_Initialize(llhttp_type_t type);
};

#endif 




