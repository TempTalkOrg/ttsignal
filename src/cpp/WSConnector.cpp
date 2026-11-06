






#include "StdAfx.h"
#include "WSConnector.h"











#include <cinttypes>        
#include <openssl/rand.h>   

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "BC/Utils.h"
#include "HttpConnector.h"  
#include "Runtime.h"
#include "Utils.h"






namespace WS
{





namespace {








const size_t kMaxHeaderTotalBytes  = 64 * 1024;
const size_t kMaxSingleHeaderBytes = 16 * 1024;
const size_t kMaxHeaderCount       = 200;



const uint32_t kMinCheckIntervalMs = 500;



const uint16_t kWSCloseNormal   = 1000;
const uint16_t kWSCloseProtocol = 1002;









std::string _ToLowerAscii(const std::string& s)
{
    std::string out(s);
    for (size_t i = 0; i < out.size(); i++)
    {
        char c = out[i];
        if (c >= 'A' && c <= 'Z')
        {
            out[i] = (char)(c - 'A' + 'a');
        }
    }
    return out;
}


bool _IsTokenChar(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
    {
        return true;
    }
    switch (c)
    {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

bool _IsToken(const std::string& s)
{
    if (s.empty())
    {
        return false;
    }
    for (size_t i = 0; i < s.size(); i++)
    {
        if (!_IsTokenChar(s[i]))
        {
            return false;
        }
    }
    return true;
}


bool _IsSafeHeaderValue(const std::string& s)
{
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '\r' || c == '\n' || c == '\0')
        {
            return false;
        }
    }
    return true;
}

uint64_t _NowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}





bool _HasToken(const std::string& listLower, const char* tokenLower)
{
    const std::string token(tokenLower);
    size_t pos = 0;
    while (pos <= listLower.size())
    {
        size_t comma = listLower.find(',', pos);
        std::string one = (comma == std::string::npos)
                              ? listLower.substr(pos)
                              : listLower.substr(pos, comma - pos);
        
        size_t b = one.find_first_not_of(" \t");
        size_t e = one.find_last_not_of(" \t");
        if (b != std::string::npos)
        {
            if (one.substr(b, e - b + 1) == token)
            {
                return true;
            }
        }
        if (comma == std::string::npos)
        {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

} 

































struct WSLogSink
{
    
    
    
    std::mutex           lock;
    IWSConnectorHandler* handler = NULL;
};

struct WSConnectorState
{
    std::recursive_mutex        lock;
    std::condition_variable_any cv;

    
    std::map<uint64_t, WSConnPtr> conns;

    IWSConnectorHandler*        handler         = NULL;
    bool                        closing         = false;
    bool                        closed_notified = false;
    
    bool                        abandoned       = false;
    
    uint32_t                    callbacks_in_flight = 0;

    void*                       logger_ctx      = NULL;
    bool                        own_logger      = false;
    WSLogSink*                  log_sink        = NULL;

    
    std::atomic<size_t>         total_allocated{0};
    std::atomic<size_t>         total_freed{0};
    std::atomic<size_t>         handshake_failed{0};
    std::atomic<size_t>         connect_timeout{0};
    std::atomic<size_t>         connect_refused{0};
    std::atomic<size_t>         connect_unreach{0};
    std::atomic<size_t>         connect_reset{0};
    std::atomic<size_t>         connect_canceled{0};
    std::atomic<size_t>         address_not_avail{0};
    std::atomic<size_t>         network_down{0};
    std::atomic<size_t>         idle_timeout{0};
    std::atomic<size_t>         user_closed{0};
    std::atomic<size_t>         dns_failed{0};
    std::atomic<size_t>         tls_verify_failed{0};
    std::atomic<size_t>         no_physical_iface{0};
    std::atomic<size_t>         pin_failed{0};
    std::atomic<size_t>         route_mismatch{0};
    std::atomic<size_t>         protocol_error{0};
    std::atomic<size_t>         unknown_closed{0};

    ~WSConnectorState()
    {
        
        if (own_logger && logger_ctx)
        {
            void* ctx = logger_ctx;
            RemoveLogAppender(ctx);
        }
        delete log_sink;
    }
};

namespace {


















thread_local uint32_t t_state_lock_depth = 0;


thread_local uint32_t t_in_log_callback  = 0;

thread_local std::vector<const WSConnectorState*> t_dispatching;

class StateLock
{
public:
    explicit StateLock(WSConnectorState& s) : lk_(s.lock)
    {
        ++t_state_lock_depth;
    }
    ~StateLock()
    {
        --t_state_lock_depth;
    }
    
    
    std::unique_lock<std::recursive_mutex>& Raw() { return lk_; }

private:
    DECLARE_NO_COPY_CLASS(StateLock);
    std::unique_lock<std::recursive_mutex> lk_;
};


#define WS_LOGQ(...)                                                           \
    do {                                                                       \
        if (t_in_log_callback != 0)                                            \
        {                                                                      \
              \
        }                                                                      \
        else if (t_state_lock_depth == 0)                                      \
        {                                                                      \
            LogQ(__VA_ARGS__);                                                 \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            assert(false && "持 state->lock 时禁止打日志：会与 BCLogger 的全局"  \
                            "自旋锁形成锁序反转");                              \
        }                                                                      \
    } while (0)

class LogCallbackScope
{
public:
    LogCallbackScope()  { ++t_in_log_callback; }
    ~LogCallbackScope() { --t_in_log_callback; }
private:
    DECLARE_NO_COPY_CLASS(LogCallbackScope);
};

size_t _DispatchDepthOn(const WSConnectorState* s)
{
    size_t n = 0;
    for (size_t i = 0; i < t_dispatching.size(); i++)
    {
        if (t_dispatching[i] == s)
        {
            n++;
        }
    }
    return n;
}

bool _IsDispatchingOn(const WSConnectorState* s)
{
    return _DispatchDepthOn(s) > 0;
}










class DispatchGuard
{
public:
    explicit DispatchGuard(const std::shared_ptr<WSConnectorState>& s)
        : state_(s)
    {
    }

    
    void ArmLocked()
    {
        state_->callbacks_in_flight++;
        t_dispatching.push_back(state_.get());
        armed_ = true;
    }

    ~DispatchGuard()
    {
        if (!armed_)
        {
            return;
        }
        t_dispatching.pop_back();
        StateLock lk(*state_);
        if (state_->callbacks_in_flight > 0)
        {
            state_->callbacks_in_flight--;
        }
        state_->cv.notify_all();
    }

private:
    DECLARE_NO_COPY_CLASS(DispatchGuard);
    std::shared_ptr<WSConnectorState> state_;
    bool                              armed_ = false;
};






void _NotifyClosedIfDrained(std::shared_ptr<WSConnectorState> state)
{
    if (!state)
    {
        return;
    }
    IWSConnectorHandler* handler = NULL;
    DispatchGuard        guard(state);
    {
        StateLock lk(*state);
        if (state->abandoned || !state->closing || state->closed_notified)
        {
            return;
        }
        if (!state->conns.empty())
        {
            return;
        }
        state->closed_notified = true;
        handler                = state->handler;
        if (!handler)
        {
            return;
        }
        guard.ArmLocked();
    }
    handler->OnClosed();
}







void _RemoveConnAndMaybeNotify(std::shared_ptr<WSConnectorState> state, uint64_t id)
{
    if (!state)
    {
        return;
    }
    
    
    WSConnPtr            gone;
    IWSConnectorHandler* handler = NULL;
    DispatchGuard        guard(state);
    {
        StateLock lk(*state);
        std::map<uint64_t, WSConnPtr>::iterator it = state->conns.find(id);
        if (it != state->conns.end())
        {
            gone = it->second;
            state->conns.erase(it);
            state->total_freed++;
        }
        if (!state->abandoned
            && state->closing
            && !state->closed_notified
            && state->conns.empty()
            && state->handler)
        {
            state->closed_notified = true;
            handler                = state->handler;
            guard.ArmLocked();      
        }
        
        state->cv.notify_all();
    }
    gone.reset();
    if (handler)
    {
        handler->OnClosed();
    }
}

} 





BCRESULT WSConnection::Config::Init(BCFObject* pConfig)
{
    if (!pConfig)
    {
        return BC_R_SUCCESS;
    }

    BCFVar* pVar;

    pVar = pConfig->Get("maxFrameBytes");
    if (IS_BCF_NUMBER(pVar))
    {
        
        
        
        
        uint64_t v = GET_BCF_INT(pVar);
        if (v > (uint64_t)UINT32_MAX)
        {
            bad_max_frame_bytes = v;
            v                   = (uint64_t)UINT32_MAX;
        }
        if (v > 0)
        {
            maxFrameBytes = (size_t)v;
        }
    }
    pVar = pConfig->Get("connectTimeoutMs");
    if (IS_BCF_NUMBER(pVar))
    {
        uint64_t v = GET_BCF_INT(pVar);
        if (v > 0)
        {
            connectTimeoutMs = (uint32_t)v;
        }
    }
    pVar = pConfig->Get("pingIntervalMs");
    if (IS_BCF_NUMBER(pVar))
    {
        pingIntervalMs = (uint32_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("idleTimeoutMs");
    if (IS_BCF_NUMBER(pVar))
    {
        idleTimeoutMs = (uint32_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("ifIndex");
    if (IS_BCF_NUMBER(pVar))
    {
        ifIndex = (uint32_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("androidNetHandle");
    if (IS_BCF_NUMBER(pVar))
    {
        androidNetHandle = (uint64_t)GET_BCF_INT(pVar);
    }
    
    pVar = pConfig->Get("caCerts");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR s = GET_BCF_STRING(pVar);
        tls_cfg.caCertsPem = s ? s : "";
    }
    pVar = pConfig->Get("spkiPin");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR s = GET_BCF_STRING(pVar);
        tls_cfg.spkiPin = s ? s : "";
    }
    pVar = pConfig->Get("insecureSkipVerify");
    if (IS_BCF_BOOL(pVar))
    {
        tls_cfg.insecureSkipVerify = (GET_BCF_BOOL(pVar) != 0);
    }
    
    
    pVar = pConfig->Get("clientCertFile");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR s = GET_BCF_STRING(pVar);
        tls_cfg.clientCertFile = s ? s : "";
    }
    pVar = pConfig->Get("clientKeyFile");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR s = GET_BCF_STRING(pVar);
        tls_cfg.clientKeyFile = s ? s : "";
    }
    pVar = pConfig->Get("clientKeyPassword");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR s = GET_BCF_STRING(pVar);
        tls_cfg.clientKeyPassword = s ? s : "";
    }
    
    pVar = pConfig->Get("dnsTimeoutMs");
    if (IS_BCF_NUMBER(pVar))
    {
        dns.timeoutMs = (uint32_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("dnsServers");
    if (IS_BCF_ARRAY(pVar))
    {
        BCFArray* pArray = (BCFArray*)pVar;
        dns.servers.clear();
        for (uint32_t i = 0; i < pArray->Size(); i++)
        {
            BCFVar* pItem = pArray->Get(i);
            if (IS_BCF_STRING(pItem))
            {
                LPCSTR s = GET_BCF_STRING(pItem);
                if (s && s[0])
                {
                    dns.servers.push_back(s);
                }
            }
        }
    }
    else if (IS_BCF_STRING(pVar))
    {
        
        LPCSTR      s = GET_BCF_STRING(pVar);
        std::string all(s ? s : "");
        dns.servers.clear();
        size_t      pos = 0;
        while (pos <= all.size() && !all.empty())
        {
            size_t      comma = all.find(',', pos);
            std::string one   = (comma == std::string::npos)
                                    ? all.substr(pos)
                                    : all.substr(pos, comma - pos);
            if (!one.empty())
            {
                dns.servers.push_back(one);
            }
            if (comma == std::string::npos)
            {
                break;
            }
            pos = comma + 1;
        }
    }
    return BC_R_SUCCESS;
}





bool WSConnection::ParseUrl(const std::string& url, WSUrlParts& out,
                            std::string& outErr)
{
    out = WSUrlParts();
    outErr.clear();

    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0)
    {
        outErr = "URL 缺少 scheme，期望 ws:// 或 wss://";
        return false;
    }
    std::string scheme = _ToLowerAscii(url.substr(0, schemeEnd));
    const char* httpScheme = NULL;
    if (scheme == "ws")
    {
        httpScheme = "http";
    }
    else if (scheme == "wss")
    {
        httpScheme = "https";
    }
    else
    {
        outErr = "不支持的 scheme \"" + scheme + "\"，只支持 ws / wss";
        return false;
    }

    
    
    
    
    
    
    
    
    
    
    
    
    std::string  rewritten = std::string(httpScheme) + url.substr(schemeEnd);
    HttpUrlParts hp;
    if (!HttpConnector::ParseUrl(rewritten, hp, outErr))
    {
        return false;
    }

    out.scheme     = scheme;
    out.host       = hp.host;
    out.hostHeader = hp.hostHeader;
    out.port       = hp.port;
    out.target     = hp.target;
    out.tls        = hp.tls;
    return true;
}

bool WSConnection::BuildHandshakeRequest(const WSUrlParts& parts,
                                         const std::string& secKey,
                                         const HttpHeaderMap& extraHeaders,
                                         std::string& out,
                                         std::string& outErr)
{
    out.clear();
    outErr.clear();

    if (parts.target.empty() || parts.hostHeader.empty())
    {
        outErr = "URL 解析结果不完整";
        return false;
    }
    if (secKey.empty())
    {
        outErr = "Sec-WebSocket-Key 为空";
        return false;
    }

    for (HttpHeaderMap::const_iterator it = extraHeaders.begin();
         it != extraHeaders.end(); ++it)
    {
        if (!_IsToken(it->first))
        {
            outErr = "头名 \"" + it->first + "\" 不是合法的 HTTP token";
            return false;
        }
        if (!_IsSafeHeaderValue(it->second))
        {
            outErr = "头 \"" + it->first + "\" 的取值含有 CR/LF/NUL，拒绝发送";
            return false;
        }
    }

    out  = "GET " + parts.target + " HTTP/1.1\r\n";
    out += "Host: " + parts.hostHeader + "\r\n";
    out += "Upgrade: websocket\r\n";
    out += "Connection: Upgrade\r\n";
    out += "Sec-WebSocket-Key: " + secKey + "\r\n";
    out += "Sec-WebSocket-Version: 13\r\n";

    bool hasUserAgent = false;
    for (HttpHeaderMap::const_iterator it = extraHeaders.begin();
         it != extraHeaders.end(); ++it)
    {
        if (_ToLowerAscii(it->first) == "user-agent")
        {
            hasUserAgent = true;
            break;
        }
    }
    if (!hasUserAgent)
    {
        out += "User-Agent: ttsignal/1.0\r\n";
    }

    for (HttpHeaderMap::const_iterator it = extraHeaders.begin();
         it != extraHeaders.end(); ++it)
    {
        std::string lower = _ToLowerAscii(it->first);
        
        
        if (lower == "host" || lower == "upgrade" || lower == "connection"
            || lower == "content-length" || lower == "transfer-encoding"
            || lower.compare(0, 14, "sec-websocket-") == 0)
        {
            continue;
        }
        out += it->first + ": " + it->second + "\r\n";
    }
    out += "\r\n";
    return true;
}

bool WSConnection::CheckHandshakeResponse(int status,
                                          const HttpHeaderMap& headers,
                                          const std::string& secKey,
                                          std::string& outErr)
{
    outErr.clear();

    if (status != 101)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "服务端没有切换协议：期望 101 Switching Protocols，实际 %d",
                 status);
        outErr = buf;
        return false;
    }
    
    HttpHeaderMap lower = GetLowerCaseHeaders(headers);

    HttpHeaderMap::const_iterator itUpgrade = lower.find("upgrade");
    if (itUpgrade == lower.end() || itUpgrade->second != "websocket")
    {
        outErr = "握手响应的 Upgrade 头不是 websocket";
        return false;
    }
    HttpHeaderMap::const_iterator itConn = lower.find("connection");
    
    if (itConn == lower.end() || !_HasToken(itConn->second, "upgrade"))
    {
        outErr = "握手响应的 Connection 头里没有 upgrade";
        return false;
    }
    HttpHeaderMap::const_iterator itAccept = lower.find("sec-websocket-accept");
    if (itAccept == lower.end())
    {
        outErr = "握手响应缺少 Sec-WebSocket-Accept";
        return false;
    }
    const std::string expect = WSParser::WSAcceptKey(secKey);
    if (expect.empty())
    {
        outErr = "无法计算 Sec-WebSocket-Accept 的期望值";
        return false;
    }
    if (itAccept->second != expect)
    {
        outErr = "Sec-WebSocket-Accept 不匹配，服务端可能不是 WebSocket 端点";
        return false;
    }
    return true;
}
















IMPLEMENT_FIXED_ALLOC(WSConnection, 32);

WSConnection::WSConnection()
    
    
    
    
    : LLHTTPParser(HTTP_RESPONSE)
    , connect_called_(false)
    , upgraded_(false)
    , user_closed_(false)
    , user_close_result_(BC_R_SUCCESS)
    , close_frame_sent_(false)
    , total_recv_bytes_(0)
    , total_send_bytes_(0)
    , total_recv_frames_(0)
    , total_send_frames_(0)
{
    
}

WSConnection::~WSConnection()
{
    
    
    
    if (channel_)
    {
        delete channel_;
        channel_ = NULL;
    }
    CloseDumpFile();
}

BCRESULT WSConnection::Create(
    const std::shared_ptr<WSConnectorState> &state,
    BCFObject *pConfig,
    const Config &defaults,
    uint64_t id,
    IWSConnectionHandler *pHandler)
{
    if (!state)
    {
        return BC_R_INVALIDARG;
    }
    state_      = state;
    id_         = id;
    handler_    = pHandler;
    logger_ctx_ = state->logger_ctx;
    config_     = defaults;
    config_.Init(pConfig);
    WSParser::SetMaxFrameBytes(config_.maxFrameBytes);

    channel_ = new TcpChannel();
    return BC_R_SUCCESS;
}

BCRESULT WSConnection::Connect(
    const std::string& url,
    uint32_t time_out_in_millisec)
{
    bool expected = false;
    if (!connect_called_.compare_exchange_strong(expected, true))
    {
        return BC_R_ALREADYRUNNING;
    }
    if (t_in_log_callback != 0)
    {
        
        
        
        
        connect_called_ = false;
        return BC_R_NOTIMPLEMENTED;
    }

    std::string err;
    if (!ParseUrl(url, parts_, err))
    {
        WS_LOGQ(logger_ctx_, _ERROR_, "[WSConnection#%" PRIu64 "] URL 解析失败：%s",
                id_, err.c_str());
        connect_called_ = false;
        return BC_R_INVALIDARG;
    }
    sec_key_ = WSParser::WSGenKey();
    if (!BuildHandshakeRequest(parts_, sec_key_, extra_headers_, req_text_, err))
    {
        WS_LOGQ(logger_ctx_, _ERROR_, "[WSConnection#%" PRIu64 "] 握手请求组装失败：%s",
                id_, err.c_str());
        connect_called_ = false;
        return BC_R_INVALIDARG;
    }
    url_ = url;

    TcpChannelConfig cfg;
    cfg.host             = parts_.host;
    cfg.port             = parts_.port;
    cfg.tls              = parts_.tls;
    cfg.policy           = config_.policy;
    cfg.ifIndex          = config_.ifIndex;
    cfg.androidNetHandle = config_.androidNetHandle;
    cfg.dns              = config_.dns;
    cfg.tls_cfg          = config_.tls_cfg;
    cfg.connectTimeoutMs = time_out_in_millisec ? time_out_in_millisec
                                                : config_.connectTimeoutMs;
    cfg.loggerCtx        = logger_ctx_;

    WS_LOGQ(logger_ctx_, _INFO_,
            "[WSConnection#%" PRIu64 "] 连接 %s（host=%s port=%u tls=%d timeout=%ums）",
            id_, url.c_str(), parts_.host.c_str(), (unsigned)parts_.port,
            parts_.tls ? 1 : 0, cfg.connectTimeoutMs);

    
    
    timeout_ms_           = cfg.connectTimeoutMs;
    start_ms_             = _NowMs();
    latest_net_action_ms_ = start_ms_;
    channel_opened_       = true;

    
    
    TcpChannel* ch = channel_;
    BCRESULT    r  = ch->Open(cfg, this);
    if (r != BC_R_SUCCESS)
    {
        
        channel_opened_ = false;
        connect_called_ = false;
        
        
        _SetLastErrorMessage(ch->LastOpenError());
        WS_LOGQ(logger_ctx_, _ERROR_,
                "[WSConnection#%" PRIu64 "] 连接发起失败 result=%d（%s:%u）：%s",
                id_, (int)r, parts_.host.c_str(), (unsigned)parts_.port,
                ch->LastOpenError().c_str());
        return r;
    }

    
    
    
    {
        StateLock lk(*state_);
        if (state_->closing && channel_)
        {
            channel_->Close();
        }
    }
    return BC_R_SUCCESS;
}

BCRESULT WSConnection::SetRequestHeader(const std::string& name,
                                        const std::string& value)
{
    if (connect_called_.load())
    {
        return BC_R_ALREADYRUNNING;
    }
    if (!_IsToken(name) || !_IsSafeHeaderValue(value))
    {
        return BC_R_INVALIDARG;
    }
    extra_headers_[name] = value;
    return BC_R_SUCCESS;
}

BCRESULT WSConnection::SendText(const std::string& text)
{
    BufferPtr payload(new BCBuffer);
    if (!text.empty())
    {
        payload->Write(text.data(), (uint32_t)text.size());
    }
    return _SendFrame(WS_OP_TEXT, payload);
}

BCRESULT WSConnection::SendData(LPCVOID data, size_t size)
{
    if (size > 0 && !data)
    {
        return BC_R_INVALIDARG;
    }
    BufferPtr payload(new BCBuffer);
    if (size > 0)
    {
        payload->Write(data, (uint32_t)size);
    }
    return _SendFrame(WS_OP_BINARY, payload);
}

BCRESULT WSConnection::SendPacket(SMPacketPtr pkt)
{
    if (!pkt || !pkt->origion_data)
    {
        return BC_R_INVALIDARG;
    }
    
    return _SendFrame(WS_OP_BINARY, BufferPtr(pkt->origion_data->RefClone()));
}

BCRESULT WSConnection::SendPing()
{
    BufferPtr  payload(new BCBuffer);
    BCBOStream writer(payload.get());
    const uint64_t now = _NowMs();

    writer.WriteUInt64BE(now);
    last_ping_ms_ = now;
    return _SendFrame(WS_OP_PING, payload);
}

void WSConnection::Close(BCRESULT result)
{
    if (result != BC_R_SUCCESS)
    {
        BCRESULT expected = BC_R_SUCCESS;
        user_close_result_.compare_exchange_strong(expected, result);
    }
    user_closed_ = true;
    if (upgraded_.load())
    {
        
        
        _SendCloseFrame(kWSCloseNormal);
    }
    StateLock lk(*state_);
    if (channel_)
    {
        
        
        
        
        channel_->Close();
    }
}

BCRESULT WSConnection::OpenDumpFile(const char* fileName)
{
    if (!fileName)
    {
        return BC_R_INVALIDARG;
    }
    std::lock_guard<std::mutex> lk(dump_lock_);
    if (dump_file_)
    {
        dump_file_->Close();
    }
    dump_file_.reset(new BCFOStream);
    if (!dump_file_->Open(fileName))
    {
        dump_file_.reset();
        return BC_R_FAILURE;
    }
    return BC_R_SUCCESS;
}

void WSConnection::CloseDumpFile()
{
    std::lock_guard<std::mutex> lk(dump_lock_);
    if (dump_file_)
    {
        dump_file_->Close();
        dump_file_.reset();
    }
}

IWSConnectionHandler* WSConnection::GetHandler() const
{
    StateLock lk(*state_);
    return handler_;
}

std::string WSConnection::PeerIp() const
{
    StateLock lk(*state_);
    return channel_ ? channel_->PeerIp() : peer_ip_;
}

uint32_t WSConnection::BoundIfIndex() const
{
    StateLock lk(*state_);
    return channel_ ? channel_->BoundIfIndex() : bound_ifindex_;
}

std::string WSConnection::PinMethod() const
{
    StateLock lk(*state_);
    return channel_ ? channel_->PinMethod() : pin_method_;
}

std::string WSConnection::LastErrorMessage() const
{
    StateLock lk(*state_);
    return last_error_message_;
}

void WSConnection::_SetLastErrorMessage(const std::string& message)
{
    
    
    StateLock lk(*state_);
    last_error_message_ = message;
}










void WSConnection::OnChannelReady()
{
    _TouchNetAction();

    BufferPtr buf(new BCBuffer);
    buf->Write(req_text_.data(), (uint32_t)req_text_.size());
    BCRESULT r = channel_->Send(buf);
    if (r != BC_R_SUCCESS)
    {
        _Fail(r, "WebSocket 握手请求发送失败");
        return;
    }
    total_send_bytes_ += req_text_.size();

    
    
    
    
    
    uint64_t elapsed = _NowMs() - start_ms_;
    uint64_t remain  = (timeout_ms_ > elapsed) ? (timeout_ms_ - elapsed) : 1;
    BCRESULT sr = channel_->ScheduleTask(hs_timer_, [this](int32_t) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "WebSocket 握手响应超时：整次连接超过 %u ms", timeout_ms_);
        _Fail(BC_R_CONNECT_TIMEOUT, msg);
    }, remain * 1000);
    if (sr != BC_R_SUCCESS)
    {
        
        
        _Fail(BC_R_UNEXPECTED,
              "握手超时定时器创建失败，无法保证连接不会永久挂起，主动终止");
    }
}

void WSConnection::OnChannelData(const void* data, size_t size)
{
    _TouchNetAction();
    total_recv_bytes_ += size;
    _WriteDump(data, size);

    if (delivered_)
    {
        return;     
    }

    if (upgraded_.load())
    {
        _FeedFrames(data, size);
        return;
    }

    
    
    
    
    
    
    
    size_t         consumed = 0;
    llhttp_errno_t err      = llhttp_execute(this, (const char*)data, size);
    if (err == HPE_OK)
    {
        consumed = size;
    }
    else
    {
        const char* pos = llhttp_get_error_pos(this);
        consumed = (pos && pos >= (const char*)data)
                       ? (size_t)(pos - (const char*)data) : 0;
        if (err == HPE_PAUSED_UPGRADE)
        {
            if (!upgraded_.load())
            {
                
                
                if (fail_result_ == BC_R_SUCCESS)
                {
                    _Fail(BC_R_UNEXPECTED, "服务端要求升级协议，但握手校验未通过");
                }
                else
                {
                    _Fail(fail_result_, fail_message_);
                }
                return;
            }
            llhttp_resume_after_upgrade(this);
        }
        else if (fail_result_ != BC_R_SUCCESS)
        {
            
            
            _Fail(fail_result_, fail_message_);
            return;
        }
        else
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "WebSocket 握手响应解析失败：%s（%s）",
                     llhttp_errno_name(err),
                     llhttp_get_error_reason(this) ? llhttp_get_error_reason(this)
                                                   : "");
            _Fail(BC_R_UNEXPECTEDTOKEN, msg);
            return;
        }
    }
    if (upgraded_.load() && size > consumed)
    {
        _FeedFrames((const uint8_t*)data + consumed, size - consumed);
    }
}

void WSConnection::OnChannelClosed(BCRESULT result, const std::string& reason)
{
    _DeliverOnce(result, reason);

    TcpChannel* ch = NULL;
    {
        StateLock lk(*state_);
        ch       = channel_;
        
        if (ch)
        {
            peer_ip_        = ch->PeerIp();
            bound_ifindex_  = ch->BoundIfIndex();
            pin_method_     = ch->PinMethod();
        }
        channel_ = NULL;
    }

    
    
    
    
    
    
    std::shared_ptr<WSConnectorState> state = state_;
    const uint64_t                    id    = id_;
    Runtime::PostTask([ch, state, id]() {
        delete ch;
        _RemoveConnAndMaybeNotify(state, id);
    });
    
}





int WSConnection::OnWSWrite(std::shared_ptr<BCBuffer> data, void* user_data)
{
    UNUSED(user_data);

    if (!data)
    {
        return 0;
    }
    const size_t size = data->RemainingLength();
    if (_SendRawFrame(data) != BC_R_SUCCESS)
    {
        return 0;
    }
    return (int)size;
}

void WSConnection::OnRecvWSFrame(
    const WSFrameHeader& header, 
    const uint8_t *payload,
    size_t payload_len)
{
    total_recv_frames_++;

    switch (header.opcode)
    {
    case WS_OP_TEXT:
        {
            
            
            
            
            std::string text;
            if (payload && payload_len > 0)
            {
                text.assign((const char*)payload, payload_len);
            }
            IWSConnectionHandler* h = NULL;
            DispatchGuard         guard(state_);
            {
                StateLock lk(*state_);
                if (!handler_)
                {
                    return;
                }
                h = handler_;
                guard.ArmLocked();
            }
            h->OnRecvText(text.c_str());
        }
        break;
    case WS_OP_BINARY:
        {
            IWSConnectionHandler* h = NULL;
            DispatchGuard         guard(state_);
            {
                StateLock lk(*state_);
                if (!handler_)
                {
                    return;
                }
                h = handler_;
                guard.ArmLocked();
            }
            h->OnRecvData(payload, payload_len);
        }
        break;
    case WS_OP_PING:
        _SendPong(payload, payload_len);
        break;
    case WS_OP_PONG:
        
        if (last_ping_ms_ != 0)
        {
            WS_LOGQ(logger_ctx_, _DEBUG_,
                    "[WSConnection#%" PRIu64 "] pong rtt=%" PRIu64 "ms",
                    id_, _NowMs() - last_ping_ms_);
            last_ping_ms_ = 0;
        }
        break;
    case WS_OP_CLOSE:
        {
            
            
            
            
            
            uint16_t code = kWSCloseNormal;
            if (payload && payload_len >= 2)
            {
                code = (uint16_t)((payload[0] << 8) | payload[1]);
            }
            peer_closed_frame_ = true;
            close_code_        = code;
            WS_LOGQ(logger_ctx_, _INFO_,
                    "[WSConnection#%" PRIu64 "] 收到 close 帧 code=%u",
                    id_, (unsigned)code);
            _SendCloseFrame(code);
            
            if (channel_)
            {
                channel_->Close();
            }
        }
        break;
    default:
        break;
    }
}





int WSConnection::http_on_url(const char* at, size_t length)
{
    UNUSED(at);
    UNUSED(length);
    return 0;       
}

int WSConnection::http_on_status(const char* at, size_t length)
{
    UNUSED(at);
    
    
    return _ChargeHeaderBytes(length) ? 0 : -1;
}

int WSConnection::http_on_header_field(const char* at, size_t length)
{
    if (last_was_value_ && !_CommitHeader())
    {
        return -1;
    }
    if (!_ChargeHeaderBytes(length) || !_CheckSingleHeaderSize(length))
    {
        return -1;
    }
    cur_field_.append(at, length);
    return 0;
}

int WSConnection::http_on_header_value(const char* at, size_t length)
{
    last_was_value_ = true;
    if (!_ChargeHeaderBytes(length) || !_CheckSingleHeaderSize(length))
    {
        return -1;
    }
    cur_value_.append(at, length);
    return 0;
}

int WSConnection::http_on_headers_complete()
{
    if (!_CommitHeader())
    {
        return -1;
    }
    
    hs_status_ = (int)this->status_code;

    std::string err;
    if (!CheckHandshakeResponse(hs_status_, hs_headers_, sec_key_, err))
    {
        state_->handshake_failed++;
        
        
        
        _Fail(BC_R_WS_HANDSHAKE_FAILED, err);
        return -1;
    }

    if (hs_timer_ > 0)
    {
        channel_->UnscheduleTask(hs_timer_);
        hs_timer_ = 0;
    }
    upgraded_ = true;
    WS_LOGQ(logger_ctx_, _INFO_,
            "[WSConnection#%" PRIu64 "] 握手成功 peer=%s ifIndex=%u pin=%s",
            id_, channel_->PeerIp().c_str(), channel_->BoundIfIndex(),
            channel_->PinMethod().empty() ? "(none)"
                                          : channel_->PinMethod().c_str());
    _DeliverConnectResult(BC_R_SUCCESS);

    
    uint32_t interval = 0;
    if (config_.pingIntervalMs > 0)
    {
        interval = config_.pingIntervalMs;
    }
    if (config_.idleTimeoutMs > 0 &&
        (interval == 0 || config_.idleTimeoutMs < interval))
    {
        interval = config_.idleTimeoutMs;
    }
    if (interval > 0)
    {
        if (interval < kMinCheckIntervalMs)
        {
            interval = kMinCheckIntervalMs;
        }
        
        
        last_ping_sent_ms_    = _NowMs();
        latest_net_action_ms_ = last_ping_sent_ms_;
        BCRESULT sr = channel_->ScheduleTask(check_timer_, [this](int32_t) {
            _OnActiveCheck();
        }, (uint64_t)interval * 1000, true);
        if (sr != BC_R_SUCCESS)
        {
            
            WS_LOGQ(logger_ctx_, _WARN_,
                    "[WSConnection#%" PRIu64 "] 保活定时器创建失败 result=%d，"
                    "本连接不做 ping 与空闲检查",
                    id_, (int)sr);
        }
    }
    return 0;
}

int WSConnection::http_on_message_complete()
{
    return 0;
}





void WSConnection::_FeedFrames(const void* data, size_t size)
{
    
    
    
    try
    {
        WSParser::ParseWSFrame(data, size);
    }
    catch (BCException& e)
    {
        state_->protocol_error++;
        const std::string what = e.GetMsg().c_str();
        WS_LOGQ(logger_ctx_, _ERROR_,
                "[WSConnection#%" PRIu64 "] WebSocket 帧解析失败：%s",
                id_, what.c_str());
        
        IWSConnectionHandler* h = NULL;
        {
            DispatchGuard guard(state_);
            {
                StateLock lk(*state_);
                if (handler_)
                {
                    h = handler_;
                    guard.ArmLocked();
                }
            }
            if (h)
            {
                h->OnException(e);
            }
        }
        _SendCloseFrame(kWSCloseProtocol);
        _Fail(BC_R_UNEXPECTEDTOKEN, "WebSocket 帧解析失败：" + what);
    }
    catch (...)
    {
        state_->protocol_error++;
        WS_LOGQ(logger_ctx_, _ERROR_,
                "[WSConnection#%" PRIu64 "] WebSocket 帧解析时发生未知异常", id_);
        _Fail(BC_R_UNEXPECTED, "WebSocket 帧解析时发生未知异常");
    }
}

bool WSConnection::_CommitHeader()
{
    if (cur_field_.empty())
    {
        cur_field_.clear();
        cur_value_.clear();
        last_was_value_ = false;
        return true;
    }
    std::string key = _ToLowerAscii(cur_field_);
    HttpHeaderMap::iterator it = hs_headers_.find(key);
    if (it == hs_headers_.end())
    {
        if (hs_headers_.size() >= kMaxHeaderCount)
        {
            _Fail(BC_R_UNEXPECTEDTOKEN, "握手响应头条数超过上限");
            return false;
        }
        hs_headers_[key] = cur_value_;
    }
    else
    {
        
        it->second += ", ";
        it->second += cur_value_;
    }
    cur_field_.clear();
    cur_value_.clear();
    last_was_value_ = false;
    return true;
}

bool WSConnection::_ChargeHeaderBytes(size_t length)
{
    if (hs_header_bytes_ + length > kMaxHeaderTotalBytes)
    {
        _Fail(BC_R_UNEXPECTEDTOKEN,
              "握手响应头总大小超过上限（防止服务端用无限响应头撑爆内存）");
        return false;
    }
    hs_header_bytes_ += length;
    return true;
}

bool WSConnection::_CheckSingleHeaderSize(size_t incoming)
{
    if (cur_field_.size() + cur_value_.size() + incoming > kMaxSingleHeaderBytes)
    {
        _Fail(BC_R_UNEXPECTEDTOKEN, "握手响应里单条头超过上限");
        return false;
    }
    return true;
}

BCRESULT WSConnection::_SendRawFrame(BufferPtr frame)
{
    if (!frame)
    {
        return BC_R_INVALIDARG;
    }
    const size_t size = frame->RemainingLength();

    
    
    
    StateLock lk(*state_);
    if (!channel_)
    {
        return BC_R_NOTCONNECTED;
    }
    BCRESULT r = channel_->Send(frame);
    if (r == BC_R_SUCCESS)
    {
        total_send_bytes_  += size;
        total_send_frames_++;
    }
    return r;
}

BCRESULT WSConnection::_SendFrame(uint8_t opcode, BufferPtr payload)
{
    if (!payload)
    {
        return BC_R_INVALIDARG;
    }
    if (!upgraded_.load())
    {
        
        return BC_R_NOTCONNECTED;
    }
    const bool isControl = (opcode & 0x08) != 0;
    if (isControl && payload->RemainingLength() > WS_MAX_CONTROL_PAYLOAD)
    {
        return BC_R_INVALIDARG;
    }

    WSFrameHeader header;
    header.opcode       = opcode;
    header.is_final     = true;
    
    
    header.has_mask     = true;
    RAND_bytes(header.mask, sizeof(header.mask));
    header.payload_size = payload->RemainingLength();

    BufferPtr frame = WSParser::BuildFrame(header, payload.get());
    if (!frame)
    {
        return BC_R_FAILURE;
    }
    return _SendRawFrame(frame);
}

void WSConnection::_SendPong(const uint8_t* payload, size_t payload_len)
{
    BufferPtr buf(new BCBuffer);
    if (payload && payload_len > 0)
    {
        
        
        buf->Write(payload, (uint32_t)payload_len);
    }
    _SendFrame(WS_OP_PONG, buf);
}

void WSConnection::_SendCloseFrame(uint16_t code)
{
    bool expected = false;
    if (!close_frame_sent_.compare_exchange_strong(expected, true))
    {
        return;     
    }
    if (!upgraded_.load())
    {
        return;
    }
    BufferPtr  buf(new BCBuffer);
    BCBOStream writer(buf.get());
    writer.WriteUInt16BE(code);
    _SendFrame(WS_OP_CLOSE, buf);
}

void WSConnection::_Fail(BCRESULT result, const std::string& message)
{
    if (fail_result_ == BC_R_SUCCESS)
    {
        fail_result_  = result;
        fail_message_ = message;
    }
    
    
    if (channel_)
    {
        channel_->Close();
    }
}

void WSConnection::_DeliverConnectResult(BCRESULT result)
{
    if (connect_notified_)
    {
        return;
    }
    connect_notified_ = true;
    if (result == BC_R_SUCCESS)
    {
        connected_ok_ = true;
    }

    HttpHeaderMap         headers = hs_headers_;
    IWSConnectionHandler* h       = NULL;
    DispatchGuard         guard(state_);
    {
        
        
        
        
        StateLock lk(*state_);
        if (!handler_)
        {
            return;     
        }
        h = handler_;
        guard.ArmLocked();
    }
    h->OnConnectResult(result, headers);
}

void WSConnection::_DeliverOnce(BCRESULT closeResult, const std::string& closeReason)
{
    if (delivered_)
    {
        return;
    }
    delivered_ = true;

    
    if (hs_timer_ > 0 && channel_)
    {
        channel_->UnscheduleTask(hs_timer_);
        hs_timer_ = 0;
    }
    if (check_timer_ > 0 && channel_)
    {
        channel_->UnscheduleTask(check_timer_);
        check_timer_ = 0;
    }

    BCRESULT    result  = fail_result_;
    std::string message = fail_message_;
    if (result == BC_R_SUCCESS)
    {
        BCRESULT userResult = user_close_result_.load();
        if (userResult != BC_R_SUCCESS)
        {
            result  = userResult;
            message = bc_result2string(userResult);
        }
        else if (closeResult != BC_R_SUCCESS)
        {
            result  = closeResult;
            message = closeReason;
        }
    }
    _ClassifyClose(result);

    if (!connected_ok_)
    {
        
        
        if (result == BC_R_SUCCESS)
        {
            result  = BC_R_UNEXPECTEDEND;
            message = "连接在 WebSocket 握手完成前被对端关闭";
        }
        
        
        
        _SetLastErrorMessage(message);
        WS_LOGQ(logger_ctx_, _ERROR_,
                "[WSConnection#%" PRIu64 "] 连接失败 result=%d: %s",
                id_, (int)result, message.c_str());
        _DeliverConnectResult(result);
        return;
    }

    
    
    _SetLastErrorMessage(message);
    const std::string text = _CloseReasonText(result, message);
    WS_LOGQ(logger_ctx_, _INFO_,
            "[WSConnection#%" PRIu64 "] 已关闭：%s"
            "（收 %" PRIu64 " 帧 / %" PRIu64 " 字节，发 %" PRIu64 " 帧 / %" PRIu64 " 字节）",
            id_, text.c_str(), total_recv_frames_.load(), total_recv_bytes_.load(),
            total_send_frames_.load(), total_send_bytes_.load());

    IWSConnectionHandler* h = NULL;
    DispatchGuard         guard(state_);
    {
        StateLock lk(*state_);
        if (!handler_)
        {
            return;
        }
        h = handler_;
        guard.ArmLocked();
    }
    h->OnClosed(text.c_str());
}

std::string WSConnection::_CloseReasonText(BCRESULT result,
                                           const std::string& raw) const
{
    char buf[64];

    if (peer_closed_frame_)
    {
        snprintf(buf, sizeof(buf), "对端发起关闭握手（code=%u）",
                 (unsigned)close_code_);
        return buf;
    }
    if (result == BC_R_IDLE_TIMEOUT)
    {
        return "空闲超时";
    }
    if (result == BC_R_CONNECT_TIMEOUT)
    {
        return raw.empty() ? std::string("连接超时") : raw;
    }
    if (result == BC_R_SUCCESS)
    {
        return user_closed_.load() ? "调用方主动关闭" : "对端正常关闭";
    }
    snprintf(buf, sizeof(buf), "[code:%d]", (int)result);
    std::string text = raw.empty() ? std::string(bc_result2string(result)) : raw;
    text += buf;
    return text;
}

void WSConnection::_ClassifyClose(BCRESULT result)
{
    
    
    switch (result)
    {
    case BC_R_SUCCESS:
        if (user_closed_.load())
        {
            state_->user_closed++;
        }
        break;
    case BC_R_TIMEDOUT:
    case BC_R_CONNECT_TIMEOUT:
        state_->connect_timeout++;
        break;
    case BC_R_CONNREFUSED:
        state_->connect_refused++;
        break;
    case BC_R_HOSTUNREACH:
    case BC_R_NETUNREACH:
        state_->connect_unreach++;
        break;
    case BC_R_CONNECTIONRESET:
        state_->connect_reset++;
        break;
    case BC_R_CANCELED:
        state_->connect_canceled++;
        break;
    case BC_R_ADDRNOTAVAIL:
        state_->address_not_avail++;
        break;
    case BC_R_NETDOWN:
        state_->network_down++;
        break;
    case BC_R_IDLE_TIMEOUT:
        state_->idle_timeout++;
        break;
    case BC_R_DNS_FAILED:
        state_->dns_failed++;
        break;
    case BC_R_TLS_VERIFY_FAILED:
        state_->tls_verify_failed++;
        break;
    case BC_R_NO_PHYSICAL_INTERFACE:
        state_->no_physical_iface++;
        break;
    case BC_R_PIN_FAILED:
        state_->pin_failed++;
        break;
    case BC_R_ROUTE_MISMATCH:
        state_->route_mismatch++;
        break;
    default:
        state_->unknown_closed++;
        break;
    }
}

void WSConnection::_OnActiveCheck()
{
    const uint64_t now = _NowMs();

    if (config_.idleTimeoutMs > 0
        && now >= latest_net_action_ms_ + config_.idleTimeoutMs)
    {
        WS_LOGQ(logger_ctx_, _WARN_,
                "[WSConnection#%" PRIu64 "] 空闲超过 %u ms，主动关闭",
                id_, config_.idleTimeoutMs);
        Close(BC_R_IDLE_TIMEOUT);
        return;
    }
    if (config_.pingIntervalMs > 0
        && now >= last_ping_sent_ms_ + config_.pingIntervalMs)
    {
        last_ping_sent_ms_ = now;
        SendPing();
    }
}

void WSConnection::_TouchNetAction()
{
    latest_net_action_ms_ = _NowMs();
}

void WSConnection::_WriteDump(const void* data, size_t size)
{
    if (size == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lk(dump_lock_);
    if (dump_file_)
    {
        dump_file_->Write(data, (uint32_t)size);
        dump_file_->Flush();
    }
}

bool WSConnection::_CancelLocked()
{
    
    if (!channel_opened_)
    {
        
        
        
        return true;
    }
    if (channel_)
    {
        channel_->Close();
    }
    return false;
}

void WSConnection::ClearHandlerLocked()
{
    
    
    handler_ = NULL;
}





BCRESULT WSConnector::Config::Init(BCFObject* pConfig)
{
    if (!pConfig)
    {
        conn.policy = tt_vpn_policy_resolve(TT_VPN_POLICY_UNSET, 0, 0,
                                            tt_vpn_policy_platform_default(),
                                            NULL);
        return BC_R_SUCCESS;
    }

    BCFVar* pVar;

    
    
    
    
    
    TTVpnPolicy explicitPolicy = TT_VPN_POLICY_UNSET;
    bool        hasBypassVpn   = false;
    bool        bypassVpn      = false;

    pVar = pConfig->Get("bypassVpn");
    if (IS_BCF_BOOL(pVar))
    {
        bypassVpn    = (GET_BCF_BOOL(pVar) != 0);
        hasBypassVpn = true;
    }
    pVar = pConfig->Get("vpnPolicy");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR raw     = GET_BCF_STRING(pVar);
        explicitPolicy = tt_vpn_policy_from_string(raw);
        if (explicitPolicy == TT_VPN_POLICY_UNSET && raw && raw[0])
        {
            bad_policy_text = raw;
        }
    }
    int bothGiven = 0;
    conn.policy = tt_vpn_policy_resolve(explicitPolicy,
                                        hasBypassVpn ? 1 : 0,
                                        bypassVpn ? 1 : 0,
                                        tt_vpn_policy_platform_default(),
                                        &bothGiven);
    both_policy_given = (bothGiven != 0);

    
    
    
    pVar = pConfig->Get("logLevel");
    if (IS_BCF_NUMBER(pVar))
    {
        log_level = (int32_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("logFile");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR s = GET_BCF_STRING(pVar);
        log_file = s ? s : "";
    }

    
    
    
    pVar = pConfig->Get("drainTimeoutMs");
    if (IS_BCF_NUMBER(pVar))
    {
        
        
        
        
        const uint64_t kMaxDrainMs = 5 * 60 * 1000;
        uint64_t       v           = GET_BCF_INT(pVar);
        if (v > kMaxDrainMs)
        {
            bad_drain_timeout = v;
            v                 = kMaxDrainMs;
        }
        drain_timeout_ms = (uint32_t)v;
    }

    
    
    
    conn.Init(pConfig);
    return BC_R_SUCCESS;
}





WSConnector::WSConnector()
    : state_(std::make_shared<WSConnectorState>())
{
    
}

WSConnector::~WSConnector()
{
    Close();

    
    
    
    
    
    
    
    
    
    
    
    uint32_t waitMs = config_.drain_timeout_ms;
    if (_IsDispatchingOn(state_.get()))
    {
        WS_LOGQ(logger_ctx_, _ERROR_,
             "[WSConnector] **违约用法**：在回调（OnConnectResult / OnRecvText / "
             "OnRecvData / OnClosed / OnException）里析构 WSConnector。本次回调所属"
             "的那条连接要等回调返回之后才会收尾，所以析构根本等不到它 —— 白等满"
             "drainTimeoutMs（本次配置 %u ms）之后照样走放弃流程。因此这里直接跳过"
             "等待：**在途连接被静默丢弃、不再回调任何 handler**。正确做法是把析构"
             "挪出回调栈（例如投递到自己的事件循环里做）",
             config_.drain_timeout_ms);
        waitMs = 0;
    }

    bool   drained  = false;
    size_t stranded = 0;
    {
        StateLock lk(*state_);
        
        
        
        
        const size_t mine = _DispatchDepthOn(state_.get());
        drained = state_->cv.wait_for(
            lk.Raw(), std::chrono::milliseconds(waitMs),
            [this, mine] {
                return state_->conns.empty()
                    && state_->callbacks_in_flight <= mine;
            });
        if (!drained)
        {
            
            
            
            
            stranded          = state_->conns.size();
            state_->abandoned = true;
            state_->handler   = NULL;   
            for (std::map<uint64_t, WSConnPtr>::iterator it = state_->conns.begin();
                 it != state_->conns.end(); ++it)
            {
                
                it->second->ClearHandlerLocked();
            }

            
            
            state_->cv.wait(lk.Raw(), [this, mine] {
                return state_->callbacks_in_flight <= mine;
            });
        }
    }
    if (!drained && state_->log_sink)
    {
        
        
        
        
        
        
        
        std::lock_guard<std::mutex> slk(state_->log_sink->lock);
        state_->log_sink->handler = NULL;
    }
    if (!drained)
    {
        WS_LOGQ(logger_ctx_, _ERROR_,
             "[WSConnector] 析构等待 %u ms 仍未排空在途连接（残留 %zu 条，"
             "已掐断其全部回调），放弃等待。残留连接会自行收尾并回收共享状态；"
             "但若销毁顺序反了（先 Runtime::Destroy() 再析构 WSConnector），"
             "那些收尾任务永远不会跑，就是真泄漏",
             config_.drain_timeout_ms, stranded);
    }

    logger_ctx_ = NULL;
    
}

BCRESULT WSConnector::Create(BCFObject* pConfig, IWSConnectorHandler* pHandler)
{
    if (created_)
    {
        return BC_R_ALREADYRUNNING;
    }

    
    
    
    
    BCFObject emptyCfg;
    BCRESULT  rtResult = Runtime::Initialize(pConfig ? pConfig : &emptyCfg);
    if (rtResult != BC_R_SUCCESS)
    {
        return rtResult;
    }
    config_.Init(pConfig);
    state_->handler = pHandler;

    
    
    
    if (!config_.log_file.empty())
    {
        state_->logger_ctx = AddFileLogAppender(config_.log_file.c_str(),
                                                config_.log_level, true, true);
        state_->own_logger = (state_->logger_ctx != NULL);
    }
    else if (state_->handler)
    {
        
        
        state_->log_sink          = new WSLogSink();
        state_->log_sink->handler = state_->handler;
        state_->logger_ctx = AddExternalLogAppender(_LogCallback,
                                                    state_->log_sink,
                                                    config_.log_level, true);
        state_->own_logger = (state_->logger_ctx != NULL);
        if (!state_->own_logger)
        {
            delete state_->log_sink;
            state_->log_sink = NULL;
        }
    }
    logger_ctx_ = state_->logger_ctx;

    config_.conn.dns.policy    = config_.conn.policy;
    config_.conn.dns.loggerCtx = logger_ctx_;
    config_.conn.tls_cfg.loggerCtx = logger_ctx_;

    if (!config_.bad_policy_text.empty())
    {
        WS_LOGQ(logger_ctx_, _WARN_,
             "[WSConnector] 无法识别的 vpnPolicy=\"%s\"，已忽略；合法取值为 "
             "os / prefer-physical / force-physical。本次回落为 %s",
             config_.bad_policy_text.c_str(),
             tt_vpn_policy_to_string(config_.conn.policy));
    }
    if (config_.both_policy_given)
    {
        WS_LOGQ(logger_ctx_, _WARN_,
             "[WSConnector] 同时配置了 vpnPolicy 与已废弃的 bypassVpn，"
             "以 vpnPolicy=%s 为准，bypassVpn 被忽略",
             tt_vpn_policy_to_string(config_.conn.policy));
    }
    if (config_.conn.bad_max_frame_bytes != 0)
    {
        WS_LOGQ(logger_ctx_, _WARN_,
             "[WSConnector] maxFrameBytes=%llu 超出允许范围，已夹到 %u。"
             "帧解析内部用 uint32_t 记待收字节数，再大就会静默截断",
             (unsigned long long)config_.conn.bad_max_frame_bytes,
             (unsigned)UINT32_MAX);
        
        config_.conn.bad_max_frame_bytes = 0;
    }
    if (config_.bad_drain_timeout != 0)
    {
        WS_LOGQ(logger_ctx_, _WARN_,
             "[WSConnector] drainTimeoutMs=%llu 超出允许范围，已夹到 %u ms。"
             "负数会被读成一个极大的无符号值（-1 即 49.7 天），那等于事实上的"
             "永久阻塞",
             (unsigned long long)config_.bad_drain_timeout,
             config_.drain_timeout_ms);
    }
    if (config_.conn.tls_cfg.insecureSkipVerify)
    {
        WS_LOGQ(logger_ctx_, _WARN_,
             "[WSConnector] insecureSkipVerify=true：**不做任何证书校验**，"
             "只应出现在自签调试环境里");
    }

    WS_LOGQ(logger_ctx_, _INFO_,
         "[WSConnector] 已创建：vpnPolicy=%s dnsServers=%zu caCerts=%s "
         "spkiPin=%s maxFrameBytes=%zu pingIntervalMs=%u idleTimeoutMs=%u",
         tt_vpn_policy_to_string(config_.conn.policy),
         config_.conn.dns.servers.size(),
         config_.conn.tls_cfg.caCertsPem.empty() ? "(系统信任库)" : "(自定义)",
         config_.conn.tls_cfg.spkiPin.empty() ? "(none)" : "(已设置)",
         config_.conn.maxFrameBytes, config_.conn.pingIntervalMs,
         config_.conn.idleTimeoutMs);

    created_ = true;
    return BC_R_SUCCESS;
}

WSConnPtr WSConnector::CreateConnection(
    BCFObject *pConfig,
    IWSConnectionHandler *pHandler)
{
    if (!created_)
    {
        return WSConnPtr();
    }
    if (t_in_log_callback != 0)
    {
        
        return WSConnPtr();
    }

    WSConnPtr pConn(new WSConnection());
    uint64_t  id;
    {
        StateLock lk(*state_);
        if (state_->closing)
        {
            return WSConnPtr();
        }
        id = ++next_conn_id_;
        
        
        
        if (pConn->Create(state_, pConfig, config_.conn, id, pHandler)
            != BC_R_SUCCESS)
        {
            return WSConnPtr();
        }
        state_->conns[id] = pConn;
        state_->total_allocated++;
    }
    
    
    
    if (pConn->config_.bad_max_frame_bytes != 0)
    {
        WS_LOGQ(logger_ctx_, _WARN_,
             "[WSConnector] 连接 #%" PRIu64 " 的 maxFrameBytes=%llu 超出允许范围，"
             "已夹到 %u。帧解析内部用 uint32_t 记待收字节数，再大就会静默截断",
             id, (unsigned long long)pConn->config_.bad_max_frame_bytes,
             (unsigned)UINT32_MAX);
    }
    WS_LOGQ(logger_ctx_, _DEBUG_, "[WSConnector] 已创建连接 #%" PRIu64, id);
    return pConn;
}

void* WSConnector::GetLoggerCtx()
{
    return logger_ctx_;
}

BCRESULT WSConnector::OpenLogFile(LPCSTR lpszLogFilePath)
{
    if (!lpszLogFilePath || !lpszLogFilePath[0])
    {
        return BC_R_INVALIDARG;
    }
    
    
    
    
    
    
    if (state_->logger_ctx)
    {
        return BC_R_EXISTS;
    }
    void* ctx = AddFileLogAppender(lpszLogFilePath,
                                   config_.log_level, true, true);
    if (!ctx)
    {
        return BC_R_FAILURE;
    }
    state_->logger_ctx = ctx;
    state_->own_logger = true;
    logger_ctx_        = ctx;
    config_.log_file   = lpszLogFilePath;
    return BC_R_SUCCESS;
}

void WSConnector::Close()
{
    
    std::vector<WSConnPtr> dead;
    {
        StateLock lk(*state_);
        state_->closing = true;
        
        
        
        
        
        
        
        std::map<uint64_t, WSConnPtr>::iterator it = state_->conns.begin();
        while (it != state_->conns.end())
        {
            if (it->second->_CancelLocked())
            {
                
                
                dead.push_back(it->second);
                state_->total_freed++;
                it = state_->conns.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    dead.clear();
    _NotifyClosedIfDrained(state_);
}

BCRESULT WSConnector::GetStats(ConnStatsMap &stats)
{
    {
        StateLock lk(*state_);
        stats["active_conn_size"] = state_->conns.size();
    }
    stats["allocated_conn_size"]        = state_->total_allocated.load();
    stats["freed_conn_size"]            = state_->total_freed.load();
    stats["handshake_failed_size"]      = state_->handshake_failed.load();
    stats["connect_timeout_size"]       = state_->connect_timeout.load();
    stats["connect_refused_size"]       = state_->connect_refused.load();
    stats["connect_net_unreach_size"]   = state_->connect_unreach.load();
    stats["connect_reset_size"]         = state_->connect_reset.load();
    stats["connect_calceled_size"]      = state_->connect_canceled.load();
    stats["address_not_avail_size"]     = state_->address_not_avail.load();
    stats["network_down_size"]          = state_->network_down.load();
    stats["idle_timeout_size"]          = state_->idle_timeout.load();
    stats["user_closed_size"]           = state_->user_closed.load();
    stats["dns_failed_size"]            = state_->dns_failed.load();
    stats["tls_verify_failed_size"]     = state_->tls_verify_failed.load();
    
    stats["no_physical_iface_size"]     = state_->no_physical_iface.load();
    stats["pin_failed_size"]            = state_->pin_failed.load();
    stats["route_mismatch_size"]        = state_->route_mismatch.load();
    stats["protocol_error_size"]        = state_->protocol_error.load();
    stats["unknown_closed_size"]        = state_->unknown_closed.load();
    return BC_R_SUCCESS;
}

void WSConnector::_LogCallback(void* data, int level, LPCSTR msg)
{
    WSLogSink* sink = (WSLogSink*)data;
    if (!sink)
    {
        return;
    }
    
    
    
    
    
    
    
    std::lock_guard<std::mutex> lk(sink->lock);
    if (sink->handler)
    {
        LogCallbackScope scope;
        sink->handler->OnLog(level, msg);
    }
}





} 




