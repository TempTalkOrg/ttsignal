///////////////////////////////////////////////////////////////////////////////
// file   : LLHTTPParser.cpp
// author : zhoukai88@jd.com（原始实现）/ anto（从 WSParser.cpp 拆出）
//
// 实现整段搬自 WSParser.cpp，只做了两处改动：
//   1. 去掉 namespace WS 包裹；
//   2. 补上 on_body 回调的转发桩（原实现只挂了六个回调，收不到报文体）。
///////////////////////////////////////////////////////////////////////////////

#include "LLHTTPParser.h"

#include <BC/BCPString.h>

using namespace BC;

///////////////////////////////////////////////////////////////////////////////
// llhttp 回调桩：把 C 回调转发到虚函数上
//
// llhttp_t 是 LLHTTPParser 的第一个（也是唯一的）基类，所以 llhttp 回传的
// parser 指针强转回 LLHTTPParser* 是安全的。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// class : LLHTTPParser
///////////////////////////////////////////////////////////////////////////////

LLHTTPParser::LLHTTPParser(llhttp_type_t type)
{
	_Initialize(type);
}

LLHTTPParser::~LLHTTPParser()
{
	// ⚠️ 这里刻意**不**调 llhttp_finish()，原来是调的，那是个会中止进程的坑。
	//
	// llhttp_finish() 唯一的作用就是在 HTTP_FINISH_SAFE_WITH_CB 状态下补派发
	// 一次 on_message_complete（deps/llhttp/src/api.c:37-58）。它不释放任何
	// 资源 —— llhttp_t 是纯值语义的结构体，没有堆内存，所以不调它没有任何损失。
	//
	// 而基类析构跑在所有派生类析构**之后**：那一刻派生对象已经没了，虚函数只
	// 会落到本类的纯虚声明上，直接 __cxa_pure_virtual 中止进程。触发条件很实在
	// —— 响应体以连接关闭为终止（既没有 Content-Length 也不是 chunked），
	// 报文还没收尾时对象就被销毁（超时、对端断开、调用方主动关闭都算）。
	// 见 tests/HttpConnector_integration_test.cpp 的 case O。
	//
	// 结论：基类析构里派发回调永远不可能安全，而这次派发本身又没有收益。
	// 需要 EOF 收尾语义的派生类，应当在**自己还活着**的时候显式调用
	// llhttp_finish()（HttpConnection::_DeliverOnce 就是这么做的），而不是
	// 指望析构顺序。
	//
	// finish 仍然复位一次：万一将来有人在本函数后面补别的清理逻辑，也不会
	// 再踩到同一个坑。
	this->finish = HTTP_FINISH_SAFE;
}

void LLHTTPParser::_Initialize(llhttp_type_t type)
{
	/* Initialize user callbacks and settings */
	llhttp_settings_init(&m_httpSettings);
	/* Set user callback */
	m_httpSettings.on_message_complete = handle_on_message_complete;
	m_httpSettings.on_url = handle_on_url;
	m_httpSettings.on_status = handle_on_status;
	m_httpSettings.on_header_field = handle_on_header_field;
	m_httpSettings.on_header_value = handle_on_header_value;
	m_httpSettings.on_headers_complete = handle_on_headers_complete;
	m_httpSettings.on_body = handle_on_body;

	/* type 由调用方决定，默认仍是 HTTP_BOTH（自动在 HTTP_REQUEST / HTTP_RESPONSE
	 * 之间二选一）——WebSocket 握手两种都要，保持既有行为。只解析响应的一侧
	 * （HTTP 客户端）必须显式传 HTTP_RESPONSE，理由见头文件里的注释。
	 */
	llhttp_init(this, type, &m_httpSettings);
}

///////////////////////////////////////////////////////////////////////////////
// End of file : LLHTTPParser.cpp
///////////////////////////////////////////////////////////////////////////////
