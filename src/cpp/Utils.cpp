///////////////////////////////////////////////////////////////////////////////
// file : Utils.cpp
// author : anto.
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include <inttypes.h>
#include "Utils.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <BC/Utils.h>

#ifndef OS_WIN
#ifndef INADDR_NONE
#define INADDR_NONE 0xFFFFFFFF
#endif
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#else  // OS_WIN
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#define USE_GETHOSTBYNAME 1 /*because at least some Windows don't have getaddrinfo()*/
#pragma comment(lib, "ws2_32.lib")
// #pragma comment(lib,"event.lib")
#pragma comment(lib, "Iphlpapi.lib")

#define sleep(s) Sleep(s * 1000)
#endif  // OS_WIN

using namespace node;



namespace EngCallbacks {

    void* lock_create()
    {
        return new BCSpinMutex();
    }

    void lock_acquire(void* lock)
    {
        ((BCSpinMutex*)lock)->Lock();
    }

    void lock_release(void* lock)
    {
        ((BCSpinMutex*)lock)->Unlock();
    }

    void lock_destory(void* lock)
    {
        delete (BCSpinMutex*)lock;
    }

    void print_stack(const char* line, void* engine_user_data)
    {
#ifdef OS_WIN
        ShowTraceStack(line);
#endif // OS_WIN
    }

}

///////////////////////////////////////////////////////////////////////////////
// Namespace : node
///////////////////////////////////////////////////////////////////////////////

namespace node
{

BCStrPtrLen kConnect("connect");
BCStrPtrLen kConnected("connected");
BCStrPtrLen kInstCount("instanceCount");
BCStrPtrLen kENOMEM("ENOMEM");
BCStrPtrLen kEINITSTUB("EINITSTUB");
BCStrPtrLen kECONNECT("ECONNECT");
BCStrPtrLen kENOTCONNECTED("ENOTCONNECTED");
BCStrPtrLen kECLOSED("ECLOSED");
BCStrPtrLen kESTATE("ESTATE");
BCStrPtrLen kECONNECTING("ECONNECTING");
BCStrPtrLen kECONNECTED("ECONNECTED");
BCStrPtrLen kEINTERNAL("EINTERNAL");
BCStrPtrLen kEREJECTED("EREJECTED");
BCStrPtrLen kEMAXCONN("EMAXCONN");
BCStrPtrLen kEHANDSHAKE("EHANDSHAKE");
BCStrPtrLen kENOTFOUND("ENOTFOUND");
BCStrPtrLen kENOQUOTA("ENOQUOTA");
BCStrPtrLen kEEXISTS("EEXISTS");
BCStrPtrLen	kClosed("closed");
BCStrPtrLen	kPaused("paused");
BCStrPtrLen	kQuery("query");
BCStrPtrLen kQScheme(":scheme");
BCStrPtrLen kHttps("https");
BCStrPtrLen kHost("host");
BCStrPtrLen kQPath(":path");
BCStrPtrLen kProps("props");


///////////////////////////////////////////////////////////////////////////////
// global constant variables
///////////////////////////////////////////////////////////////////////////////


void SplitString(
	const ::std::string& str, 
	char delimiter,
	::std::vector< ::std::string>* dest) 
{
	::std::vector< ::std::string> parsed;
	::std::string::size_type pos = 0;
	while (true) 
	{
		const ::std::string::size_type colon = str.find(delimiter, pos);
		if (colon == ::std::string::npos) 
		{
			parsed.push_back(str.substr(pos));
			break;
		}
		else 
		{
			parsed.push_back(str.substr(pos, colon - pos));
			pos = colon + 1;
		}
	}
	dest->swap(parsed);
}


void
FormatLogTime(char* buf, size_t buf_len)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);

	struct tm tm;

#ifdef OS_WIN
	time_t t = tv.tv_sec;
#ifdef _USE_32BIT_TIME_T
	_localtime32_s(&tm, &t);
#else
	_localtime64_s(&tm, &t);
#endif

#else
	localtime_r(&tv.tv_sec, &tm);
#endif
	tm.tm_mon++;
	tm.tm_year += 1900;

#ifdef __APPLE__
	snprintf(buf, buf_len, "%4d/%02d/%02d %02d:%02d:%02d %06d",
		tm.tm_year, tm.tm_mon,
		tm.tm_mday, tm.tm_hour,
		tm.tm_min, tm.tm_sec, tv.tv_usec);
#else
	snprintf(buf, buf_len, "%4d/%02d/%02d %02d:%02d:%02d %06ld",
		tm.tm_year, tm.tm_mon,
		tm.tm_mday, tm.tm_hour,
		tm.tm_min, tm.tm_sec, tv.tv_usec);
#endif
}

uint32_t LogLevelToBCLogLevel(uint32_t level)
{
    switch (level)
    {
    case 'f':
        return _FATAL_;
    case 'e':
        return _ERROR_;
    case 'w':
        return _WARN_;
    case 'i':
        return _INFO_;
    case 'd':
        return _DEBUG_;
    case 't':
        return _FINEST_;
    default:
        return _INFO_;
    }
}

Json::Value StatFixedAlloc(uint32_t nFilter)
{
    BCFTableEntry* pIter, * pEnd;
    BCFVar* pVar;
    Json::Value retRoot(Json::objectValue);
    BCFObject* pStats = BCStatFixedAlloc(nFilter);

    if (pStats)
    {
        pIter = pStats->BeginEntry();
        pEnd = pStats->EndEntry();
        for (; pIter != pEnd; pIter = pStats->NextEntry(pIter))
        {
            pVar = pIter->GetValue();
            if (IS_BCF_NUMBER(pVar))
            {
                retRoot[pIter->GetKey()] = GET_BCF_DOUBLE(pVar);
            }
        }
        delete pStats;
    }
    return retRoot;
}

std::string StatFixedAllocToString(uint32_t nFilter)
{
    Json::Value retRoot = StatFixedAlloc(nFilter);
    Json::FastWriter writer;
    return writer.write(retRoot);
}

int32_t XQCLogLevelToBCLogLevel(int32_t lvl)
{
    int32_t level = _INFO_;
    switch (lvl)
    {
        case XQC_LOG_FATAL:
            level = _FATAL_;
            break;
        case XQC_LOG_ERROR:
            level = _ERROR_;
            break;
        case XQC_LOG_WARN:
            level = _WARN_;
            break;
        case XQC_LOG_REPORT:
        case XQC_LOG_STATS:
        case XQC_LOG_INFO:
            level = _INFO_;
            break;
        case XQC_LOG_DEBUG:
            level = _DEBUG_;
            break;
        default:
            level = _INFO_;
            break;
    }
    return level;
}

int32_t BCLogLevelToLogLevel(int32_t lvl)
{
    int32_t level = LOG_LEVEL_INFO;
    switch (lvl)
    {
        case _FATAL_:
            level = LOG_LEVEL_FATAL;
            break;
        case _ERROR_:
            level = LOG_LEVEL_ERROR;
            break;
        case _WARN_:
            level = LOG_LEVEL_WARN;
            break;
        case _INFO_:
            level = LOG_LEVEL_INFO;
            break;
        case _DEBUG_:
            level = LOG_LEVEL_DEBUG;
            break;
        default:
            level = LOG_LEVEL_INFO;
            break;
    }
    return level;
}

int32_t LogLevelToBCLogLevel(int32_t level)
{
    switch (level)
    {
        case LOG_LEVEL_FATAL:
            return _FATAL_;
        case LOG_LEVEL_ERROR:
            return _ERROR_;
        case LOG_LEVEL_WARN:
            return _WARN_;
        case LOG_LEVEL_INFO:
            return _INFO_;
        case LOG_LEVEL_DEBUG:
            return _DEBUG_;
    }
    return _INFO_;
}

void
LogQ(const void *logger_ctx, int32_t nLevel, const char* szFmtStr, ...)
{
	va_list args;
	char timestr[64];
	FormatLogTime(timestr, sizeof(timestr));
	BCPString strFmt;
	strFmt.Format("[%s] %s", timestr, szFmtStr);
	va_start(args, szFmtStr);
	LogCustomV(logger_ctx, nLevel, strFmt.c_str(), args);
	va_end(args);
}

BCRESULT TextToSockaddr(
    int type, const char *addr_text,
    unsigned int port,
    struct sockaddr *saddr,
    socklen_t *saddr_len) 
{
  struct addrinfo hints, *res = NULL;
  BCRESULT result = BC_R_FAILURE;
  std::string port_str = std::to_string(port);

  if (type == AF_INET6) {
    memset(saddr, 0, sizeof(struct sockaddr_in6));
    struct sockaddr_in6 *addr_v6 = (struct sockaddr_in6 *)(saddr);
    if (inet_pton(type, addr_text, &(addr_v6->sin6_addr.s6_addr))) {
      addr_v6->sin6_family = type;
      addr_v6->sin6_port = htons(port);
    } else {
      int e;

      memset(&hints, 0, sizeof(hints));
      hints.ai_flags = AI_NUMERICSERV;
      hints.ai_family = AF_INET6;
      e = getaddrinfo(addr_text, port_str.c_str(), &hints, &res);
      if (e != 0) {
        switch (e) {
        case EAI_AGAIN:
          result = BC_R_NETDOWN;
          break;
        case EAI_SERVICE:
        case EAI_FAIL:
        case EAI_NODATA:
          result = BC_R_NETUNREACH;
          break;
        case EAI_FAMILY:
          result = BC_R_ADDRNOTAVAIL;
          break;
        default:
          result = BC_R_FAILURE;
          break;
        }
        goto end;
      }
      if (res->ai_addrlen > sizeof(*addr_v6)) {
        result = BC_R_ADDRNOTAVAIL;
        goto end;
      }
      memcpy(addr_v6, res->ai_addr, res->ai_addrlen);
    }
    *saddr_len = sizeof(struct sockaddr_in6);
    result = BC_R_SUCCESS;
  } else {
    memset(saddr, 0, sizeof(struct sockaddr_in));
    struct sockaddr_in *addr_v4 = (struct sockaddr_in *)(saddr);
    if (inet_pton(type, addr_text, &(addr_v4->sin_addr.s_addr))) {
      addr_v4->sin_family = type;
      addr_v4->sin_port = htons(port);
    } else {
      int e;

      memset(&hints, 0, sizeof(hints));
      hints.ai_flags = AI_NUMERICSERV;
      hints.ai_family = AF_INET;
      e = getaddrinfo(addr_text, port_str.c_str(), &hints, &res);
      if (e != 0) {
        switch (e) {
        case EAI_AGAIN:
          result = BC_R_NETDOWN;
          break;
        case EAI_SERVICE:
        case EAI_FAIL:
        case EAI_NODATA:
          result = BC_R_NETUNREACH;
          break;
        case EAI_FAMILY:
          result = BC_R_ADDRNOTAVAIL;
          break;
        default:
          result = BC_R_FAILURE;
          break;
        }
        goto end;
      }
      if (res->ai_addrlen > sizeof(*addr_v4)) {
        result = BC_R_ADDRNOTAVAIL;
        goto end;
      }
      memcpy(addr_v4, res->ai_addr, res->ai_addrlen);
    }
    *saddr_len = sizeof(struct sockaddr_in);
    result = BC_R_SUCCESS;
  }

end:
  if (res) {
	  freeaddrinfo(res);
  }

  return result;
}

bool ParseJsonFromBuffer(BCBuffer& buffer, Json::Value& outValue)
{
    std::string strJson;
    buffer.ToString(strJson);
    Json::Reader reader;

#ifdef _WIN32
    //BCPString strOEM;
    //BCUtf8ToOEM(strJson.c_str(), strOEM);
    //return reader.parse(strOEM.c_str(), outValue);
    return reader.parse(strJson, outValue);
#else // !_WIN32
    return reader.parse(strJson, outValue);
#endif // _WIN32
}


///////////////////////////////////////////////////////////////////////////////
// function utilities
///////////////////////////////////////////////////////////////////////////////


/**
 * @brief 检查字符是否属于 encodeURI 规范中不应被编码的保留字符或未保留字符。
 * * 不编码的字符集 (RFC 2396/3986 的子集)：
 * 字母数字 (A-Z, a-z, 0-9)
 * - _ . ! ~ * ' ( )
 * ; , / ? : @ & = + $ #
 * * @param c 待检查的字符。
 * @return int 1 表示不编码，0 表示需要编码。
 */
static int is_uri_safe(char c) {
    // 字母数字 (A-Z, a-z, 0-9)
    if (isalnum(c)) {
        return 1;
    }
    
    // 未保留字符 (Unreserved Marks)
    if (c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || 
        c == '\'' || c == '(' || c == ')') {
        return 1;
    }
    
    // 常见的保留字符 (Reserved/Sub-delimiters)，encodeURI不编码它们
    if (c == ';' || c == ',' || c == '/' || c == '?' || c == ':' || c == '@' || 
        c == '&' || c == '=' || c == '+' || c == '$' || c == '#') {
        return 1;
    }
    
    return 0;
}

/**
 * @brief 对 URI 字符串执行 EncodeURI 百分号编码。
 * * 该函数假定输入字符串是有效的 UTF-8 编码。
 * * @param uri_string 待编码的原始 URI 字符串 (UTF-8)。
 * @return std::string 编码后的新字符串。
 */
std::string EncodeURI(const std::string &uri_string) {
    if (uri_string.length() == 0) {
        return uri_string;
    }

    // 估算最大所需内存：每个字节最多被编码成 %HH (3 字节)
    // 尽管我们知道只有非 ASCII/非安全字符才被编码，但这是安全的上界。
    size_t max_len = uri_string.length() * 3 + 1; 
    
    // 分配输出缓冲区
    KBPool pool;
    char* encoded_string = (char*)pool.Alloc(max_len);
    if (encoded_string == NULL) {
        return NULL;
    }

    char* p_out = encoded_string; // 指向输出缓冲区当前位置的指针
    const unsigned char* p_in = (const unsigned char*)uri_string.c_str(); // 指向输入字符串的指针

    while (*p_in != '\0') {
        unsigned char byte = *p_in;

        // 1. ASCII 字符检查
        if (byte <= 0x7F) {
            // 是单字节 ASCII 字符
            
            // 检查是否为 URI 安全字符
            if (is_uri_safe((char)byte)) {
                // 安全字符：直接复制
                *p_out++ = (char)byte;
            } else {
                // 不安全字符（如空格 0x20）：百分号编码
                sprintf(p_out, "%%%02X", byte);
                p_out += 3;
            }
            p_in++;
        } else {
            // 2. 非 ASCII 字符 (UTF-8 多字节序列)
            // 所有非 ASCII 字节都必须进行百分号编码。
            
            // 将当前字节编码为 %HH
            sprintf(p_out, "%%%02X", byte);
            p_out += 3;
            p_in++;
            
            // 检查后续字节是否属于同一个 UTF-8 序列的继续字节 (0x80 到 0xBF)
            // 这里我们只需要知道：所有非 ASCII 字节（多字节序列的起始字节和继续字节）都必须被编码。
            // 完整的 UTF-8 序列遍历逻辑在这里是为了确保每个字节都被正确编码。
            
            // 简化逻辑：只要是多字节序列的一部分，就编码，直到遇到下一个 ASCII 字符或序列的开始
            // 因为输入字符串假设是有效的 UTF-8，我们只需对每个非 ASCII 字节进行 %HH 编码
        }
    }

    *p_out = '\0'; // 终止字符串
    
    std::string ret(encoded_string);
    return ret;
}

/**
 * @brief 将两位十六进制字符转换为对应的整数值 (0-255)。
 * @param hex_char 两位十六进制字符中的一个 (0-9, A-F, a-f)。
 * @return int 对应的整数值，如果不是有效十六进制字符则返回 -1。
 */
static int hex_to_int(char hex_char) {
    if (hex_char >= '0' && hex_char <= '9') {
        return hex_char - '0';
    } else if (hex_char >= 'a' && hex_char <= 'f') {
        return hex_char - 'a' + 10;
    } else if (hex_char >= 'A' && hex_char <= 'F') {
        return hex_char - 'A' + 10;
    }
    return -1;
}

/**
 * @brief 对 URI 字符串执行 decodeURI 百分号解码。
 * * 注意：此函数执行字节解码，但**不进行**严格的 UTF-8 有效性检查。
 * * 仅执行百分号解码，返回解码后的字节序列。
 * @param encoded_string 待解码的 URI 字符串。
 * @return std::string 解码后的新字符串（UTF-8 字节序列）。
 */
std::string DecodeURI(const std::string& encoded_string) {
    if (encoded_string.length() == 0) {
        return encoded_string;
    }

    // 最大长度：由于解码是 3 字节 -> 1 字节，所以最大长度不会超过原始长度。
    size_t max_len = encoded_string.length() + 1; 
    
    KBPool pool;
    // 分配输出缓冲区
    char* decoded_string = (char*)pool.Alloc(max_len);
    if (decoded_string == NULL) {
        return NULL;
    }

    char* p_out = decoded_string; // 输出指针
    const char* p_in = encoded_string.c_str(); // 输入指针

    while (*p_in != '\0') {
        if (*p_in == '%') {
            // 检查后面是否有两位十六进制字符
            if (p_in[1] != '\0' && p_in[2] != '\0') {
                int high = hex_to_int(p_in[1]);
                int low = hex_to_int(p_in[2]);

                if (high != -1 && low != -1) {
                    // 成功解码 %HH 为单个字节
                    unsigned char decoded_byte = (unsigned char)(high * 16 + low);
                    
                    // 检查特殊情况：保留的 ASCII 字符（如 / ? &）
                    // 在 decodeURI 中，只有 '%' 符号被特殊处理并解码，
                    // 而 / ? : @ & = + $ # 这些字符在 encodeURI 时未被编码，因此这里无需特殊处理。
                    
                    // 所有的 %HH 序列，无论其值如何，都应被解码。
                    *p_out++ = (char)decoded_byte;
                    
                    p_in += 3; // 跳过 %HH
                    continue;
                }
            }
            
            // 如果 % 后面不是有效的两位十六进制，或者字符串结束，
            // 则该 % 不应被解码，而是直接被复制。
            // 这是一个与某些 JavaScript 环境行为保持一致的做法，即保留格式错误的编码。
            // 
            // 规范说明：如果 %HH 格式不正确，则不解码，直接复制 %.
            *p_out++ = *p_in++;
            
        } else {
            // 非 % 字符：直接复制
            *p_out++ = *p_in++;
        }
    }

    *p_out = '\0'; // 终止字符串
    
    // 重新调整内存大小以节省空间
    std::string ret(decoded_string);
    return ret;
}

const char* GetSDKVersion() 
{
    static char ver[20] = {0};
    if (ver[0] == 0) 
    {
        const char* d = __DATE__; // "Mmm dd yyyy"
        int y = (d[7]-'0')*1000+(d[8]-'0')*100+(d[9]-'0')*10+(d[10]-'0');
        int m = (d[0]=='J'&&d[1]=='a')?1 :d[0]=='F'?2
              : (d[0]=='M'&&d[2]=='r')?3 :(d[0]=='A'&&d[1]=='p')?4
              :  d[0]=='M'?5 :(d[0]=='J'&&d[2]=='n')?6
              :  d[0]=='J'?7 : d[0]=='A'?8 :d[0]=='S'?9
              :  d[0]=='O'?10: d[0]=='N'?11:12;
        int dy = (d[4]==' '?0:(d[4]-'0')*10)+(d[5]-'0');
        static BCSpinMutex mutex;
        mutex.Lock();
        snprintf(ver, sizeof(ver), "1.0.0.%04d%02d%02d", y, m, dy);
        mutex.Unlock();
    }
    return ver;
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : node
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : node

///////////////////////////////////////////////////////////////////////////////
// End of file : Utils.cpp
///////////////////////////////////////////////////////////////////////////////
