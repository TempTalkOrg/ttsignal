///////////////////////////////////////////////////////////////////////////////
// file   : LLHTTPParser.h
// author : zhoukai88@jd.com（原始实现）/ anto（从 WSParser.h 拆出）
//
// llhttp 的薄封装。原先定义在 WSParser.h 的 namespace WS 里，只有 WebSocket
// 握手一个使用者；HTTP 客户端（HttpConnector）同样要用它解析响应报文，而
// WSParser.cpp 目前被排除在构建之外（见 src/CMakeLists.txt 的 REMOVE_ITEM），
// 直接引用会链接失败。因此拆成独立文件，并去掉 namespace WS 包裹 —— 一个通用
// 的 HTTP 报文解析器不该困在 WS 命名空间里。
//
// ⚠️ 析构不派发任何回调，派生类不需要为此做任何事。
//
// 曾经的 ~LLHTTPParser 会调 llhttp_finish(this)，而 llhttp 在
// HTTP_FINISH_SAFE_WITH_CB 状态下（响应体以连接关闭为终止、尚未收尾）会由此
// 补派发一次 on_message_complete。基类析构跑在所有派生类析构之后，那时虚函数
// 只会落到本类的纯虚声明上，直接 __cxa_pure_virtual 中止进程。现已在
// ~LLHTTPParser 里去掉该调用（理由见 LLHTTPParser.cpp）。
//
// 因此：**需要 EOF 收尾语义的派生类，必须在自己还活着的时候显式调用
// llhttp_finish()**，不要指望析构顺序。HttpConnection::_DeliverOnce() 是范例。
///////////////////////////////////////////////////////////////////////////////

#ifndef LLHTTPPARSER_H_INCLUDED__
#define LLHTTPPARSER_H_INCLUDED__

#include <map>
#include <string>

#include <BC/Config.h>
#include <llhttp.h>

typedef std::map<std::string, std::string> HttpHeaderMap;

// 键统一转小写；值除 Sec-WebSocket-Key / Sec-WebSocket-Accept 外也转小写
// （这两个头的值是 base64，大小写敏感）。语义与拆分前逐字一致。
HttpHeaderMap GetLowerCaseHeaders(const HttpHeaderMap& headers);

///////////////////////////////////////////////////////////////////////////////
// Class : LLHTTPParser
///////////////////////////////////////////////////////////////////////////////

class LLHTTPParser : public llhttp_t
{
public:
	// type 决定 llhttp 接受哪一类报文：
	//   HTTP_BOTH     —— 按第一段输入自动判断是请求还是响应（默认值，
	//                    WebSocket 握手两种都要，保持既有行为不变）
	//   HTTP_RESPONSE —— 只接受响应。**HTTP 客户端必须用这个**：留在
	//                    HTTP_BOTH 下，一个恶意/异常的服务端只要回一段
	//                    "GET /evil HTTP/1.1\r\n..."，客户端就会把它当成一条
	//                    合法报文收下（status 为 0、body 却照收），不报任何错。
	//   HTTP_REQUEST  —— 只接受请求（服务端侧）
	explicit LLHTTPParser(llhttp_type_t type = HTTP_BOTH);
	virtual ~LLHTTPParser();

	virtual int http_on_url(const char* at, size_t length)			= 0;
	virtual int http_on_status(const char* at, size_t length)		= 0;
	virtual int http_on_header_field(const char* at, size_t length) = 0;
	virtual int http_on_header_value(const char* at, size_t length) = 0;
	virtual int http_on_headers_complete()							= 0;
	virtual int http_on_message_complete()							= 0;
	// 响应体分片。WebSocket 握手不关心报文体，所以给了默认实现；HTTP 客户端
	// 覆盖它来累积 body 并施加 maxResponseBytes 上限。
	virtual int http_on_body(const char* at, size_t length)
	{
		(void)at;
		(void)length;
		return 0;
	}

	// HTTP request properties
	llhttp_settings_t	m_httpSettings;
	std::string			m_method;
	std::string			m_url;
	HttpHeaderMap		m_headers;
	std::string			m_currentField;
private:
	DECLARE_NO_COPY_CLASS(LLHTTPParser);
	void				_Initialize(llhttp_type_t type);
};

#endif // LLHTTPPARSER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : LLHTTPParser.h
///////////////////////////////////////////////////////////////////////////////
