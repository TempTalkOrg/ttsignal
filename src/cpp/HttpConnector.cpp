





#include "StdAfx.h"
#include "HttpConnector.h"

#include "Runtime.h"
#include "Utils.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>





namespace {















const size_t kMaxHeaderTotalBytes  = 64 * 1024;  
const size_t kMaxSingleHeaderBytes = 16 * 1024;  
const size_t kMaxHeaderCount       = 200;        
const size_t kMaxReasonBytes       = 512;        




const uint32_t kMaxInterimResponses = 8;

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


bool _IsSafeRequestPart(const std::string& s)
{
    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20 || c == 0x7f)
        {
            return false;
        }
    }
    return true;
}

const std::string* _FindHeaderCI(const HttpHeaderMap& headers, const char* nameLower)
{
    for (HttpHeaderMap::const_iterator it = headers.begin(); it != headers.end(); ++it)
    {
        if (_ToLowerAscii(it->first) == nameLower)
        {
            return &it->second;
        }
    }
    return NULL;
}

uint64_t _NowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}



bool _MethodExpectsBody(const std::string& methodUpper)
{
    return methodUpper == "POST" || methodUpper == "PUT" || methodUpper == "PATCH";
}

std::string _ToUpperAscii(const std::string& s)
{
    std::string out(s);
    for (size_t i = 0; i < out.size(); i++)
    {
        char c = out[i];
        if (c >= 'a' && c <= 'z')
        {
            out[i] = (char)(c - 'a' + 'A');
        }
    }
    return out;
}

} 





HttpConnectorState::~HttpConnectorState()
{
    
    
    if (own_logger && logger_ctx)
    {
        void* ctx = logger_ctx;
        RemoveLogAppender(ctx);
    }
    delete log_sink;
}

namespace {















































thread_local uint32_t t_state_lock_depth = 0;

class StateLock
{
public:
    explicit StateLock(HttpConnectorState& s) : lk_(s.lock)
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


#define HTTP_LOGQ(...)                                                         \
    do {                                                                       \
        if (t_in_log_callback != 0)                                            \
        {                                                                      \
              \
        }                                                                      \
        else if (t_state_lock_depth == 0)                                       \
        {                                                                      \
            LogQ(__VA_ARGS__);                                                 \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            assert(false && "持 state->lock 时禁止打日志：会与 BCLogger 的全局"   \
                            "自旋锁形成锁序反转");                               \
        }                                                                      \
    } while (0)

























thread_local std::vector<const HttpConnectorState*> t_dispatching;















thread_local uint32_t t_in_log_callback = 0;

class LogCallbackScope
{
public:
    LogCallbackScope()  { ++t_in_log_callback; }
    ~LogCallbackScope() { --t_in_log_callback; }
private:
    DECLARE_NO_COPY_CLASS(LogCallbackScope);
};

size_t _DispatchDepthOn(const HttpConnectorState* s)
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

bool _IsDispatchingOn(const HttpConnectorState* s)
{
    return _DispatchDepthOn(s) > 0;
}

class DispatchGuard
{
public:
    explicit DispatchGuard(const std::shared_ptr<HttpConnectorState>& s)
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
    std::shared_ptr<HttpConnectorState> state_;
    bool                                armed_ = false;
};



















bool _AllDrainedLocked(const HttpConnectorState& state)
{
    return state.conns.empty()
        && state.destroying.empty()
        && state.idle.empty()
        && state.idle_destroying == 0;
}

IHttpConnectorHandler* _DetachAndArmClosed(
    const std::shared_ptr<HttpConnectorState>& state,
    HttpConnection*                            gone,
    DispatchGuard&                             guard)
{
    IHttpConnectorHandler* handler = NULL;

    StateLock lk(*state);
    
    
    state->conns.erase(gone);
    state->destroying.erase(gone);
    if (!state->abandoned
        && state->closing
        && !state->closed_notified
        && _AllDrainedLocked(*state)
        && state->handler)
    {
        state->closed_notified = true;
        handler                = state->handler;
        guard.ArmLocked();              
    }
    
    state->cv.notify_all();
    return handler;
}

void _FinishDestroyAndMaybeNotify(std::shared_ptr<HttpConnectorState> state,
                                  HttpConnection*                     dead)
{
    if (!state)
    {
        return;
    }
    DispatchGuard          guard(state);
    IHttpConnectorHandler* handler = _DetachAndArmClosed(state, dead, guard);
    if (handler)
    {
        handler->OnClosed();
    }
}






void _FinishIdleDestroyAndMaybeNotify(std::shared_ptr<HttpConnectorState> state)
{
    if (!state)
    {
        return;
    }
    IHttpConnectorHandler* handler = NULL;
    DispatchGuard          guard(state);
    {
        StateLock lk(*state);
        if (state->idle_destroying > 0)
        {
            state->idle_destroying--;
        }
        if (!state->abandoned
            && state->closing
            && !state->closed_notified
            && _AllDrainedLocked(*state)
            && state->handler)
        {
            state->closed_notified = true;
            handler                = state->handler;
            guard.ArmLocked();      
        }
        
        state->cv.notify_all();
    }
    if (handler)
    {
        handler->OnClosed();
    }
}








void _NotifyClosedIfDrained(std::shared_ptr<HttpConnectorState> state)
{
    if (!state)
    {
        return;
    }
    IHttpConnectorHandler* handler = NULL;
    DispatchGuard          guard(state);
    {
        StateLock lk(*state);
        if (state->abandoned || !state->closing || state->closed_notified)
        {
            return;
        }
        if (!_AllDrainedLocked(*state))
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




std::string _PoolKey(const TcpChannelConfig& cfg)
{
    std::string key = cfg.host;
    key += ":" + std::to_string((unsigned)cfg.port);
    key += cfg.tls ? ":tls" : ":tcp";
    key += ":p" + std::to_string((int)cfg.policy);
    key += ":i" + std::to_string((unsigned)cfg.ifIndex);
    key += ":n" + std::to_string((unsigned long long)cfg.androidNetHandle);
    
    key += ":r" + cfg.resolvedIp;
    return key;
}

} 

























class PooledChannel : public ITcpChannelHandler
{
public:
    PooledChannel(const std::shared_ptr<HttpConnectorState>& state,
                  TcpChannel* channel, const std::string& key)
        : state_(state)
        , channel_(channel)
        , key_(key)
    {
    }

    TcpChannel* Channel() const { return channel_; }

    
    
    void HoldLocked() { held_ = true; }

    
    
    
    void Release()
    {
        Terminal pending = TERMINAL_NONE;
        {
            StateLock lk(*state_);
            held_   = false;
            pending = pending_;
        }
        if (pending != TERMINAL_NONE)
        {
            _ScheduleDestroy(pending);
        }
    }

    
    
    
    void ArmIdleTimer(uint32_t idleMs)
    {
        if (idleMs == 0)
        {
            return;     
        }
        
        
        channel_->ScheduleTask(idle_timer_, [this](int32_t) {
            idle_timer_ = 0;    
            channel_->Close();
        }, (uint64_t)idleMs * 1000);
    }

    
    void OnChannelReady() override {}
    void OnChannelData(const void*, size_t) override {}

    void OnChannelClosed(BCRESULT, const std::string&) override
    {
        _CancelIdleTimer();

        bool held = false;
        {
            StateLock lk(*state_);
            
            
            
            _RemoveFromPoolLocked(*state_);
            state_->idle_destroying++;
            held = held_;
            if (held)
            {
                pending_ = TERMINAL_CLOSED;     
            }
        }
        if (!held)
        {
            _ScheduleDestroy(TERMINAL_CLOSED);
        }
    }

    void OnChannelDetached() override
    {
        
        
        
        
        _CancelIdleTimer();

        
        
        
        
        bool held = false;
        {
            StateLock lk(*state_);
            held = held_;
            if (held)
            {
                pending_ = TERMINAL_DETACHED;   
            }
        }
        if (!held)
        {
            _ScheduleDestroy(TERMINAL_DETACHED);
        }
    }

private:
    DECLARE_NO_COPY_CLASS(PooledChannel);

    enum Terminal
    {
        TERMINAL_NONE     = 0,
        TERMINAL_CLOSED   = 1,  
        TERMINAL_DETACHED = 2,  
    };

    
    
    
    void _ScheduleDestroy(Terminal how)
    {
        PooledChannel*                      self  = this;
        std::shared_ptr<HttpConnectorState> state = state_;
        if (how == TERMINAL_CLOSED)
        {
            Runtime::PostTask([self, state]() {
                delete self->channel_;
                delete self;
                
                _FinishIdleDestroyAndMaybeNotify(state);
            });
        }
        else
        {
            Runtime::PostTask([self]() { delete self; });
        }
    }

    
    void _CancelIdleTimer()
    {
        if (idle_timer_ > 0)
        {
            channel_->UnscheduleTask(idle_timer_);
            idle_timer_ = 0;
        }
    }

    
    void _RemoveFromPoolLocked(HttpConnectorState& state)
    {
        std::map<std::string, std::vector<PooledChannel*> >::iterator it =
            state.idle.find(key_);
        if (it == state.idle.end())
        {
            return;     
        }
        for (size_t i = 0; i < it->second.size(); i++)
        {
            if (it->second[i] == this)
            {
                it->second.erase(it->second.begin() + (long)i);
                break;
            }
        }
        
        if (it->second.empty())
        {
            state.idle.erase(it);
        }
    }

    std::shared_ptr<HttpConnectorState> state_;
    TcpChannel*                         channel_;
    std::string                         key_;
    
    int32_t                             idle_timer_ = 0;
    
    bool                                held_       = false;
    Terminal                            pending_    = TERMINAL_NONE;
};

namespace {










PooledChannel* _TakeFromPool(const std::shared_ptr<HttpConnectorState>& state,
                             const std::string& key)
{
    StateLock lk(*state);

    std::map<std::string, std::vector<PooledChannel*> >::iterator it =
        state->idle.find(key);
    if (it == state->idle.end() || it->second.empty())
    {
        return NULL;
    }

    
    PooledChannel* taken = it->second.back();
    it->second.pop_back();
    taken->HoldLocked();
    if (it->second.empty())
    {
        state->idle.erase(it);
    }
    return taken;
}

} 














class HttpConnection
    : public LLHTTPParser
    , public ITcpChannelHandler
{
public:
    HttpConnection(const std::shared_ptr<HttpConnectorState>& state,
                   IHttpRequestHandler* handler,
                   void* loggerCtx,
                   size_t maxResponseBytes)
        
        
        
        : LLHTTPParser(HTTP_RESPONSE)
        , state_(state)
        , handler_(handler)
        , logger_ctx_(loggerCtx)
        , max_response_bytes_(maxResponseBytes)
    {
        channel_ = new TcpChannel();
    }

    
    
    HttpConnection(const std::shared_ptr<HttpConnectorState>& state,
                   IHttpRequestHandler* handler,
                   void* loggerCtx,
                   size_t maxResponseBytes,
                   TcpChannel* adopted)
        : LLHTTPParser(HTTP_RESPONSE)
        , state_(state)
        , handler_(handler)
        , logger_ctx_(loggerCtx)
        , max_response_bytes_(maxResponseBytes)
        , reused_(true)
    {
        channel_ = adopted;
    }

    ~HttpConnection() override
    {
        
        
        delete channel_;
        channel_ = NULL;
    }

    
    BCRESULT Start(const TcpChannelConfig& cfg,
                   const std::string& reqText,
                   uint32_t timeoutMs,
                   bool headRequest,
                   bool poolEnabled)
    {
        
        
        
        req_text_     = reqText;
        timeout_ms_   = timeoutMs ? timeoutMs : 10000;
        head_request_ = headRequest;
        start_ms_     = _NowMs();
        pool_key_     = _PoolKey(cfg);
        pool_enabled_ = poolEnabled;
        if (reused_)
        {
            cfg_ = cfg;     
        }

        
        
        if (reused_)
        {
            return channel_->Adopt(this);
        }
        return channel_->Open(cfg, this);
    }

    
    const std::string& LastOpenError() const { return channel_->LastOpenError(); }

    
    
    void DisownChannel() { channel_ = NULL; }

    
    
    void Cancel()
    {
        
        
        
        
        if (channel_)
        {
            channel_->Close();
        }
    }

    
    
    
    void ClearHandlerLocked()
    {
        handler_ = NULL;
    }

private:
    DECLARE_NO_COPY_CLASS(HttpConnection);

    
    
    

    void OnChannelReady() override
    {
        BufferPtr buf(new BCBuffer);
        buf->Write(req_text_.data(), (uint32_t)req_text_.size());
        BCRESULT r = channel_->Send(buf);
        if (r != BC_R_SUCCESS)
        {
            _Fail(r, "请求报文发送失败");
            return;
        }

        
        
        
        
        
        uint64_t elapsed = _NowMs() - start_ms_;
        uint64_t remain  = (timeout_ms_ > elapsed) ? (timeout_ms_ - elapsed) : 1;
        BCRESULT sr = channel_->ScheduleTask(resp_timer_, [this](int32_t) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "等待响应超时：整次请求超过 %u ms（已收到 %zu 字节响应体）",
                     timeout_ms_, body_.size());
            _Fail(BC_R_TIMEDOUT, msg);
        }, remain * 1000);
        if (sr != BC_R_SUCCESS)
        {
            
            
            
            _Fail(BC_R_UNEXPECTED,
                  "响应超时定时器创建失败，无法保证请求不会永久挂起，主动终止");
            return;
        }
    }

    void OnChannelData(const void* data, size_t size) override
    {
        if (delivered_)
        {
            return;     
        }
        llhttp_errno_t err = llhttp_execute(this, (const char*)data, size);
        
        
        
        
        if (err == HPE_OK || err == HPE_PAUSED)
        {
            if (message_done_ && !delivered_)
            {
                
                if (_ShouldKeepConnectionAlive())
                {
                    _FinishAndReturnChannelToPool();
                }
                else
                {
                    channel_->Close();
                }
            }
            return;
        }
        if (fail_result_ != BC_R_SUCCESS)
        {
            
            channel_->Close();
            return;
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "HTTP 响应解析失败：%s（%s）",
                 llhttp_errno_name(err),
                 llhttp_get_error_reason(this) ? llhttp_get_error_reason(this) : "");
        _Fail(BC_R_UNEXPECTEDTOKEN, msg);
    }

    void OnChannelClosed(BCRESULT result, const std::string& reason) override
    {
        _DeliverOnce(result, reason);
        _DetachAndScheduleDestroy();
    }

    
    
    
    
    
    
    
    
    
    
    
    
    void OnChannelAdoptFailed() override
    {
        HTTP_LOGQ(logger_ctx_, _INFO_,
             "[HttpConnector] 池中连接在接手前失效，改用新连接重试：%s:%u",
             cfg_.host.c_str(), (unsigned)cfg_.port);

        TcpChannel* fresh   = new TcpChannel();
        bool        closing = false;
        {
            
            
            StateLock lk(*state_);
            channel_ = fresh;
            closing  = state_->closing;
        }
        reused_ = false;

        
        
        uint64_t elapsed = _NowMs() - start_ms_;
        cfg_.connectTimeoutMs = (timeout_ms_ > elapsed)
                              ? (uint32_t)(timeout_ms_ - elapsed) : 1;

        
        
        std::shared_ptr<HttpConnectorState> state = state_;
        BCRESULT r = closing ? BC_R_SHUTTINGDOWN : fresh->Open(cfg_, this);
        if (r != BC_R_SUCCESS)
        {
            
            
            std::string reason = closing ? std::string("连接器正在关闭")
                                         : fresh->LastOpenError();
            if (fail_result_ == BC_R_SUCCESS)
            {
                fail_result_  = r;
                fail_message_ = "池中连接失效后新建连接失败：" + reason;
            }
            _DeliverOnce(r, reason);
            _DetachAndScheduleDestroy();
            return;
        }

        
        
        HttpConnection* self = this;
        StateLock lk(*state);
        if (state->closing && state->conns.count(self) > 0)
        {
            self->Cancel();
        }
    }

    
    
    

    
    bool _ShouldKeepConnectionAlive() const
    {
        return pool_enabled_
            && keep_alive_          
            && message_done_
            && fail_result_ == BC_R_SUCCESS
            && !delivered_;
    }

    
    
    
    
    void _FinishAndReturnChannelToPool()
    {
        TcpChannel* ch = channel_;

        
        
        _DeliverOnce(BC_R_SUCCESS, std::string());

        
        
        PooledChannel* pooled = new PooledChannel(state_, ch, pool_key_);
        ch->Park(pooled);

        bool     pooledOk  = false;
        uint32_t idleMs    = 0;
        {
            StateLock lk(*state_);
            
            
            
            channel_ = NULL;    
            if (!state_->closing && !state_->abandoned
                && state_->max_idle_per_key > 0)
            {
                std::map<std::string, std::vector<PooledChannel*> >::iterator it =
                    state_->idle.find(pool_key_);
                size_t cur = (it == state_->idle.end()) ? 0 : it->second.size();
                if (cur < state_->max_idle_per_key)
                {
                    state_->idle[pool_key_].push_back(pooled);
                    pooledOk = true;
                    idleMs   = state_->idle_timeout_ms;
                }
            }
        }
        if (pooledOk)
        {
            
            
            pooled->ArmIdleTimer(idleMs);
        }
        if (!pooledOk)
        {
            
            
            
            ch->Close();
        }

        _DetachAndScheduleDestroy();
    }

    
    
    void _DetachAndScheduleDestroy()
    {
        std::shared_ptr<HttpConnectorState> state = state_;
        if (state)
        {
            StateLock lk(*state);
            if (state->conns.erase(this) > 0)
            {
                
                
                state->destroying.insert(this);
            }
        }

        
        
        
        
        
        
        
        HttpConnection* self = this;
        Runtime::PostTask([self, state]() {
            delete self;
            
            _FinishDestroyAndMaybeNotify(state, self);
        });
    }

    
    
    

    int http_on_url(const char* at, size_t length) override
    {
        (void)at;
        (void)length;
        return 0;       
    }

    int http_on_status(const char* at, size_t length) override
    {
        if (!_ChargeHeaderBytes(length))
        {
            return -1;
        }
        if (reason_phrase_.size() + length > kMaxReasonBytes)
        {
            _FailHeaderLimit("status line 的 reason-phrase", kMaxReasonBytes, "字节");
            return -1;
        }
        reason_phrase_.append(at, length);
        return 0;
    }

    int http_on_header_field(const char* at, size_t length) override
    {
        if (last_was_value_ && !_CommitHeader())
        {
            return -1;
        }
        if (!_ChargeHeaderBytes(length)
            || !_CheckSingleHeaderSize(length))
        {
            return -1;
        }
        cur_field_.append(at, length);
        return 0;
    }

    int http_on_header_value(const char* at, size_t length) override
    {
        last_was_value_ = true;
        if (!_ChargeHeaderBytes(length)
            || !_CheckSingleHeaderSize(length))
        {
            return -1;
        }
        cur_value_.append(at, length);
        return 0;
    }

    int http_on_headers_complete() override
    {
        if (!_CommitHeader())
        {
            return -1;
        }
        
        
        status_ = (int)this->status_code;

        
        
        
        
        
        
        
        
        
        
        
        if (status_ >= 100 && status_ < 200)
        {
            if (++interim_count_ > kMaxInterimResponses)
            {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "服务端连续发了超过 %u 条 1xx 中间响应，判为异常",
                         (unsigned)kMaxInterimResponses);
                _Fail(BC_R_UNEXPECTEDTOKEN, msg);
                return -1;
            }
            interim_ = true;
            _ResetMessageState();
            return 0;
        }

        
        
        
        
        
        
        
        
        
        
        
        if (head_request_ || status_ == 204 || status_ == 304)
        {
            return 1;
        }
        return 0;
    }

    int http_on_body(const char* at, size_t length) override
    {
        if (body_.size() + length > max_response_bytes_)
        {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "响应体超过 maxResponseBytes 上限（%zu 字节），已中断",
                     max_response_bytes_);
            _Fail(BC_R_RESPONSE_TOO_LARGE, msg);
            return -1;      
        }
        body_.append(at, length);
        return 0;
    }

    int http_on_message_complete() override
    {
        if (interim_)
        {
            
            
            interim_ = false;
            return 0;
        }
        if (!_CommitHeader())
        {
            return -1;
        }
        if (status_ == 0)
        {
            status_ = (int)this->status_code;
        }
        message_done_ = true;
        
        
        
        
        
        keep_alive_ = (llhttp_should_keep_alive(this) != 0);
        
        
        llhttp_pause(this);
        
        
        
        
        
        return 0;
    }

    
    
    


    
    
    bool _CommitHeader()
    {
        if (cur_field_.empty())
        {
            cur_field_.clear();
            cur_value_.clear();
            last_was_value_ = false;
            return true;
        }
        std::string key = _ToLowerAscii(cur_field_);
        HttpHeaderMap::iterator it = headers_.find(key);
        if (it == headers_.end())
        {
            if (headers_.size() >= kMaxHeaderCount)
            {
                _FailHeaderLimit("响应头条数", kMaxHeaderCount, "条");
                return false;
            }
            headers_[key] = cur_value_;
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

    
    bool _ChargeHeaderBytes(size_t length)
    {
        if (header_bytes_ + length > kMaxHeaderTotalBytes)
        {
            _FailHeaderLimit("响应头总大小", kMaxHeaderTotalBytes, "字节");
            return false;
        }
        header_bytes_ += length;
        return true;
    }

    
    
    bool _CheckSingleHeaderSize(size_t incoming)
    {
        if (cur_field_.size() + cur_value_.size() + incoming
            > kMaxSingleHeaderBytes)
        {
            _FailHeaderLimit("单条响应头", kMaxSingleHeaderBytes, "字节");
            return false;
        }
        return true;
    }

    void _FailHeaderLimit(const char* what, size_t limit, const char* unit)
    {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "%s超过上限（%zu %s），已中断。"
                 "这类上限独立于 maxResponseBytes，用于防止服务端用无限响应头撑爆内存",
                 what, limit, unit);
        _Fail(BC_R_RESPONSE_TOO_LARGE, msg);
    }

    
    
    
    void _ResetMessageState()
    {
        cur_field_.clear();
        cur_value_.clear();
        last_was_value_ = false;
        reason_phrase_.clear();
        headers_.clear();
        body_.clear();
        status_       = 0;
        header_bytes_ = 0;
    }

    
    void _Fail(BCRESULT result, const std::string& message)
    {
        if (fail_result_ == BC_R_SUCCESS)
        {
            fail_result_  = result;
            fail_message_ = message;
        }
        channel_->Close();
    }

    void _DeliverOnce(BCRESULT closeResult, const std::string& closeReason)
    {
        if (delivered_)
        {
            return;
        }
        delivered_ = true;

        if (resp_timer_ > 0)
        {
            
            channel_->UnscheduleTask(resp_timer_);
            resp_timer_ = 0;
        }

        
        
        
        if (!message_done_
            && fail_result_ == BC_R_SUCCESS
            && closeResult == BC_R_SUCCESS)
        {
            llhttp_finish(this);
        }

        if (message_done_)
        {
            HTTP_LOGQ(logger_ctx_, _INFO_,
                 "[HttpConnector] 收到响应 status=%d body=%zu 字节 peer=%s "
                 "ifIndex=%u pin=%s",
                 status_, body_.size(), channel_->PeerIp().c_str(),
                 channel_->BoundIfIndex(),
                 channel_->PinMethod().empty() ? "(none)"
                                               : channel_->PinMethod().c_str());

            HttpResponse resp;
            resp.status       = status_;
            resp.reason       = reason_phrase_;
            resp.headers      = headers_;
            resp.body         = body_;
            resp.peerIp       = channel_->PeerIp();
            resp.boundIfIndex = channel_->BoundIfIndex();
            resp.pinMethod    = channel_->PinMethod();

            
            
            
            
            IHttpRequestHandler* h = NULL;
            DispatchGuard        guard(state_);
            {
                StateLock lk(*state_);
                if (!handler_)
                {
                    return;     
                }
                h = handler_;
                guard.ArmLocked();
            }
            h->OnHttpResponse(resp);
            return;
        }

        BCRESULT    result = fail_result_;
        std::string message = fail_message_;
        if (result == BC_R_SUCCESS)
        {
            if (closeResult != BC_R_SUCCESS)
            {
                result  = closeResult;
                message = closeReason;
                if (status_ != 0)
                {
                    
                    
                    
                    
                    message += "；响应头已收全（status="
                             + std::to_string(status_)
                             + "），但报文体以连接关闭为终止，无法确认是否完整，"
                               "故判为失败。服务端应给 Content-Length 或用 chunked";
                }
            }
            else
            {
                
                result  = BC_R_UNEXPECTEDEND;
                message = "连接已关闭但 HTTP 响应不完整（对端在响应中途断开）";
            }
        }
        HTTP_LOGQ(logger_ctx_, _ERROR_, "[HttpConnector] 请求失败 result=%d: %s",
                  (int)result, message.c_str());

        
        IHttpRequestHandler* h = NULL;
        DispatchGuard        guard(state_);
        {
            StateLock lk(*state_);
            if (!handler_)
            {
                return;     
            }
            h = handler_;
            guard.ArmLocked();
        }
        h->OnHttpError(result, message);
    }

private:
    
    std::shared_ptr<HttpConnectorState> state_;
    
    IHttpRequestHandler *   handler_    = NULL;
    void                *   logger_ctx_ = NULL;
    size_t                  max_response_bytes_ = 0;

    TcpChannel          *   channel_    = NULL;
    std::string             req_text_;
    
    TcpChannelConfig        cfg_;
    uint32_t                timeout_ms_ = 10000;
    uint64_t                start_ms_   = 0;
    int32_t                 resp_timer_ = 0;

    
    std::string             cur_field_;
    std::string             cur_value_;
    bool                    last_was_value_ = false;
    std::string             reason_phrase_;
    HttpHeaderMap           headers_;
    std::string             body_;
    int                     status_       = 0;
    bool                    message_done_ = false;
    bool                    head_request_ = false;
    
    size_t                  header_bytes_ = 0;
    
    bool                    interim_        = false;
    uint32_t                interim_count_  = 0;

    BCRESULT                fail_result_  = BC_R_SUCCESS;
    std::string             fail_message_;
    bool                    delivered_    = false;

    
    
    bool                    reused_       = false;
    
    bool                    pool_enabled_ = false;
    
    bool                    keep_alive_   = false;
    
    std::string             pool_key_;
};





bool HttpConnector::ParseUrl(const std::string& url, HttpUrlParts& out,
                             std::string& outErr)
{
    out    = HttpUrlParts();
    outErr.clear();

    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0)
    {
        outErr = "URL 缺少 scheme，期望 http:// 或 https://";
        return false;
    }
    std::string scheme = _ToLowerAscii(url.substr(0, schemeEnd));
    uint16_t    defaultPort = 0;
    if (scheme == "http")
    {
        defaultPort = 80;
        out.tls     = false;
    }
    else if (scheme == "https")
    {
        defaultPort = 443;
        out.tls     = true;
    }
    else
    {
        outErr = "不支持的 scheme \"" + scheme + "\"，只支持 http / https";
        return false;
    }
    out.scheme = scheme;

    
    size_t authStart = schemeEnd + 3;
    size_t authEnd   = url.size();
    for (size_t i = authStart; i < url.size(); i++)
    {
        char c = url[i];
        if (c == '/' || c == '?' || c == '#')
        {
            authEnd = i;
            break;
        }
    }
    std::string authority = url.substr(authStart, authEnd - authStart);
    if (authority.empty())
    {
        outErr = "URL 缺少主机名";
        return false;
    }
    if (authority.find('@') != std::string::npos)
    {
        
        
        outErr = "URL 里的 userinfo（user:pass@host）不受支持";
        return false;
    }

    std::string hostPart, portPart;
    bool        bracketed = false;
    if (authority[0] == '[')
    {
        size_t rb = authority.find(']');
        if (rb == std::string::npos || rb == 1)
        {
            outErr = "IPv6 字面量格式错误，期望 [addr] 或 [addr]:port";
            return false;
        }
        bracketed = true;
        hostPart  = authority.substr(1, rb - 1);
        if (rb + 1 < authority.size())
        {
            if (authority[rb + 1] != ':')
            {
                outErr = "IPv6 字面量的方括号之后只能跟 :port";
                return false;
            }
            portPart = authority.substr(rb + 2);
        }
    }
    else
    {
        size_t colon = authority.find(':');
        if (colon == std::string::npos)
        {
            hostPart = authority;
        }
        else
        {
            hostPart = authority.substr(0, colon);
            portPart = authority.substr(colon + 1);
        }
        if (hostPart.empty() || portPart.find(':') != std::string::npos)
        {
            
            outErr = "主机名非法；IPv6 字面量必须写成 [addr] 或 [addr]:port";
            return false;
        }
    }
    if (hostPart.empty() || !_IsSafeRequestPart(hostPart))
    {
        outErr = "主机名为空或含有非法字符";
        return false;
    }
    out.host = hostPart;

    uint16_t port = defaultPort;
    if (!portPart.empty())
    {
        if (portPart.size() > 5)
        {
            outErr = "端口号非法：\"" + portPart + "\"";
            return false;
        }
        unsigned long v = 0;
        for (size_t i = 0; i < portPart.size(); i++)
        {
            if (portPart[i] < '0' || portPart[i] > '9')
            {
                outErr = "端口号非法：\"" + portPart + "\"";
                return false;
            }
            v = v * 10 + (unsigned long)(portPart[i] - '0');
        }
        if (v == 0 || v > 65535)
        {
            outErr = "端口号超出范围：\"" + portPart + "\"";
            return false;
        }
        port = (uint16_t)v;
    }
    out.port = port;

    
    std::string hostForHeader = bracketed ? ("[" + hostPart + "]") : hostPart;
    if (port != defaultPort)
    {
        char portBuf[8];
        snprintf(portBuf, sizeof(portBuf), ":%u", (unsigned)port);
        hostForHeader += portBuf;
    }
    out.hostHeader = hostForHeader;

    
    std::string target;
    if (authEnd < url.size() && url[authEnd] != '#')
    {
        size_t hash = url.find('#', authEnd);
        target = (hash == std::string::npos) ? url.substr(authEnd)
                                             : url.substr(authEnd, hash - authEnd);
    }
    if (target.empty())
    {
        target = "/";
    }
    else if (target[0] == '?')
    {
        target = "/" + target;
    }
    if (!_IsSafeRequestPart(target))
    {
        outErr = "URL 的路径部分含有空白或控制字符，请先做 percent-encoding";
        return false;
    }
    out.target = target;

    return true;
}

bool HttpConnector::BuildRequestText(const HttpRequest& req,
                                     const HttpUrlParts& parts,
                                     std::string& out,
                                     std::string& outErr,
                                     bool keepAlive)
{
    out.clear();
    outErr.clear();

    std::string method = req.method.empty() ? std::string("GET") : req.method;
    if (!_IsToken(method))
    {
        outErr = "method \"" + method + "\" 不是合法的 HTTP token";
        return false;
    }
    if (parts.target.empty() || !_IsSafeRequestPart(parts.target))
    {
        outErr = "request-target 非法";
        return false;
    }

    for (HttpHeaderMap::const_iterator it = req.headers.begin();
         it != req.headers.end(); ++it)
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

    
    
    std::string hostValue = parts.hostHeader;
    const std::string* pHost = _FindHeaderCI(req.headers, "host");
    if (pHost)
    {
        hostValue = *pHost;
    }
    if (hostValue.empty() || !_IsSafeHeaderValue(hostValue))
    {
        outErr = "Host 头为空或含有非法字符";
        return false;
    }

    std::string methodUpper = _ToUpperAscii(method);
    bool needContentLength =
        !req.body.empty() || _MethodExpectsBody(methodUpper);

    out  = method + " " + parts.target + " HTTP/1.1\r\n";
    out += "Host: " + hostValue + "\r\n";
    if (!_FindHeaderCI(req.headers, "user-agent"))
    {
        out += "User-Agent: ttsignal/1.0\r\n";
    }
    if (needContentLength)
    {
        out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    }
    
    
    
    if (!keepAlive)
    {
        out += "Connection: close\r\n";
    }

    for (HttpHeaderMap::const_iterator it = req.headers.begin();
         it != req.headers.end(); ++it)
    {
        std::string lower = _ToLowerAscii(it->first);
        
        
        if (lower == "host" || lower == "content-length" || lower == "connection"
            || lower == "transfer-encoding")
        {
            continue;
        }
        out += it->first + ": " + it->second + "\r\n";
    }
    out += "\r\n";
    out += req.body;
    return true;
}





HttpConnector::HttpConnector()
    : state_(std::make_shared<HttpConnectorState>())
{
    
}

HttpConnector::~HttpConnector()
{
    Close();

    
    
    
    
    
    
    
    
    
    
    
    
    uint32_t waitMs = drain_timeout_ms_;
    if (_IsDispatchingOn(state_.get()))
    {
        HTTP_LOGQ(logger_ctx_, _ERROR_,
             "[HttpConnector] **违约用法**：在回调（OnHttpResponse / OnHttpError / "
             "OnClosed）里析构 HttpConnector。本次回调所属的那条连接要等回调返回"
             "之后才会收尾，所以析构根本等不到它 —— 白等满 drainTimeoutMs"
             "（本次配置 %u ms）之后照样走放弃流程。因此这里直接跳过等待："
             "**在途请求被静默丢弃、不再回调任何 handler**。"
             "正确做法是把析构挪出回调栈（例如投递到自己的事件循环里做）",
             drain_timeout_ms_);
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
                
                
                
                
                
                return _AllDrainedLocked(*state_)
                    && state_->callbacks_in_flight <= mine;
            });
        if (!drained)
        {
            
            
            
            
            
            
            stranded          = state_->conns.size() + state_->destroying.size()
                              + state_->idle.size() + state_->idle_destroying;
            state_->abandoned = true;
            state_->handler   = NULL;   
            for (std::set<HttpConnection*>::iterator it = state_->conns.begin();
                 it != state_->conns.end(); ++it)
            {
                
                (*it)->ClearHandlerLocked();
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
        
        
        
        HTTP_LOGQ(logger_ctx_, _ERROR_,
             "[HttpConnector] 析构等待 %u ms 仍未排空在途请求（残留 %zu 条，"
             "已掐断其全部回调），放弃等待。残留连接会自行收尾并回收共享状态；"
             "但若销毁顺序反了（先 Runtime::Destroy() 再析构 HttpConnector），"
             "那些收尾任务永远不会跑，就是真泄漏",
             drain_timeout_ms_, stranded);
    }

    logger_ctx_ = NULL;
    
}

BCRESULT HttpConnector::Create(BCFObject* pConfig, IHttpConnectorHandler* handler)
{
    if (created_)
    {
        return BC_R_ALREADYRUNNING;
    }
    state_->handler = handler;

    
    
    
    int32_t logLevel = _INFO_;
    LPCSTR  logFile  = NULL;
    if (pConfig)
    {
        BCFVar* pVar = pConfig->Get("logLevel");
        if (IS_BCF_NUMBER(pVar))
        {
            logLevel = (int32_t)GET_BCF_INT(pVar);
        }
        pVar = pConfig->Get("logFile");
        if (IS_BCF_STRING(pVar))
        {
            logFile = GET_BCF_STRING(pVar);
        }
    }
    if (logFile && logFile[0])
    {
        state_->logger_ctx = AddFileLogAppender(logFile, logLevel, true, true);
        state_->own_logger = (state_->logger_ctx != NULL);
    }
    else if (state_->handler)
    {
        
        
        
        state_->log_sink          = new HttpLogSink();
        state_->log_sink->handler = state_->handler;
        state_->logger_ctx = AddExternalLogAppender(_LogCallback,
                                                    state_->log_sink,
                                                    logLevel, true);
        state_->own_logger = (state_->logger_ctx != NULL);
        if (!state_->own_logger)
        {
            delete state_->log_sink;
            state_->log_sink = NULL;
        }
    }
    logger_ctx_ = state_->logger_ctx;

    
    
    
    
    TTVpnPolicy explicitPolicy = TT_VPN_POLICY_UNSET;
    bool        hasBypassVpn   = false;
    bool        bypassVpn      = false;
    std::string badPolicyText;
    if (pConfig)
    {
        BCFVar* pVar = pConfig->Get("bypassVpn");
        if (IS_BCF_BOOL(pVar))
        {
            bypassVpn    = (GET_BCF_BOOL(pVar) != 0);
            hasBypassVpn = true;
        }
        pVar = pConfig->Get("vpnPolicy");
        if (IS_BCF_STRING(pVar))
        {
            LPCSTR raw = GET_BCF_STRING(pVar);
            explicitPolicy = tt_vpn_policy_from_string(raw);
            if (explicitPolicy == TT_VPN_POLICY_UNSET && raw && raw[0])
            {
                badPolicyText = raw;
            }
        }
    }
    int bothGiven = 0;
    policy_ = tt_vpn_policy_resolve(explicitPolicy,
                                    hasBypassVpn ? 1 : 0,
                                    bypassVpn ? 1 : 0,
                                    tt_vpn_policy_platform_default(),
                                    &bothGiven);
    if (!badPolicyText.empty())
    {
        HTTP_LOGQ(logger_ctx_, _WARN_,
             "[HttpConnector] 无法识别的 vpnPolicy=\"%s\"，已忽略；合法取值为 "
             "os / prefer-physical / force-physical。本次回落为 %s",
             badPolicyText.c_str(), tt_vpn_policy_to_string(policy_));
    }
    if (bothGiven)
    {
        HTTP_LOGQ(logger_ctx_, _WARN_,
             "[HttpConnector] 同时配置了 vpnPolicy 与已废弃的 bypassVpn，"
             "以 vpnPolicy=%s 为准，bypassVpn 被忽略",
             tt_vpn_policy_to_string(policy_));
    }

    
    
    
    dns_cfg_.policy    = policy_;
    dns_cfg_.loggerCtx = logger_ctx_;
    if (pConfig)
    {
        BCFVar* pVar = pConfig->Get("androidNetHandle");
        if (IS_BCF_NUMBER(pVar))
        {
            android_net_handle_ = (uint64_t)GET_BCF_INT(pVar);
            dns_cfg_.androidNetHandle = android_net_handle_;
        }
        pVar = pConfig->Get("dnsTimeoutMs");
        if (IS_BCF_NUMBER(pVar))
        {
            dns_cfg_.timeoutMs = (uint32_t)GET_BCF_INT(pVar);
        }
        pVar = pConfig->Get("dnsServers");
        if (IS_BCF_ARRAY(pVar))
        {
            BCFArray* pArray = (BCFArray*)pVar;
            for (uint32_t i = 0; i < pArray->Size(); i++)
            {
                BCFVar* pItem = pArray->Get(i);
                if (IS_BCF_STRING(pItem))
                {
                    LPCSTR s = GET_BCF_STRING(pItem);
                    if (s && s[0])
                    {
                        dns_cfg_.servers.push_back(s);
                    }
                }
            }
        }
        else if (IS_BCF_STRING(pVar))
        {
            
            LPCSTR      s = GET_BCF_STRING(pVar);
            std::string all(s ? s : "");
            size_t      pos = 0;
            while (pos <= all.size() && !all.empty())
            {
                size_t comma = all.find(',', pos);
                std::string one = (comma == std::string::npos)
                                      ? all.substr(pos)
                                      : all.substr(pos, comma - pos);
                if (!one.empty())
                {
                    dns_cfg_.servers.push_back(one);
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                pos = comma + 1;
            }
        }
    }

    
    
    
    tls_cfg_.loggerCtx = logger_ctx_;
    if (pConfig)
    {
        BCFVar* pVar = pConfig->Get("caCerts");
        if (IS_BCF_STRING(pVar))
        {
            LPCSTR s = GET_BCF_STRING(pVar);
            tls_cfg_.caCertsPem = s ? s : "";
        }
        pVar = pConfig->Get("spkiPin");
        if (IS_BCF_STRING(pVar))
        {
            LPCSTR s = GET_BCF_STRING(pVar);
            tls_cfg_.spkiPin = s ? s : "";
        }
        pVar = pConfig->Get("insecureSkipVerify");
        if (IS_BCF_BOOL(pVar))
        {
            tls_cfg_.insecureSkipVerify = (GET_BCF_BOOL(pVar) != 0);
        }
        pVar = pConfig->Get("maxResponseBytes");
        if (IS_BCF_NUMBER(pVar))
        {
            uint64_t v = GET_BCF_INT(pVar);
            if (v > 0)
            {
                max_response_bytes_ = (size_t)v;
            }
        }
        pVar = pConfig->Get("maxIdleConnections");
        if (IS_BCF_NUMBER(pVar))
        {
            
            
            
            const uint64_t kMaxIdle = 64;
            uint64_t       v        = GET_BCF_INT(pVar);
            if (v > kMaxIdle) v = kMaxIdle;
            state_->max_idle_per_key = (size_t)v;
        }
        pVar = pConfig->Get("idleTimeoutMs");
        if (IS_BCF_NUMBER(pVar))
        {
            
            
            
            const uint64_t kMaxIdleMs = 24ull * 60 * 60 * 1000;
            uint64_t       v          = GET_BCF_INT(pVar);
            if (v > kMaxIdleMs)
            {
                bad_idle_timeout_ = v;
                v                 = kMaxIdleMs;
            }
            state_->idle_timeout_ms = (uint32_t)v;
        }
        pVar = pConfig->Get("drainTimeoutMs");
        if (IS_BCF_NUMBER(pVar))
        {
            
            
            
            
            
            const uint64_t kMaxDrainMs = 5 * 60 * 1000;
            uint64_t       v           = GET_BCF_INT(pVar);
            if (v > kMaxDrainMs)
            {
                bad_drain_timeout_ = v;
                v                  = kMaxDrainMs;
            }
            drain_timeout_ms_ = (uint32_t)v;
        }
    }
    if (bad_idle_timeout_ != 0)
    {
        HTTP_LOGQ(logger_ctx_, _WARN_,
             "[HttpConnector] idleTimeoutMs=%llu 超出允许范围，已夹到 %u ms",
             (unsigned long long)bad_idle_timeout_, IdleTimeoutMs());
    }
    if (bad_drain_timeout_ != 0)
    {
        HTTP_LOGQ(logger_ctx_, _WARN_,
             "[HttpConnector] drainTimeoutMs=%llu 超出允许范围，已夹到 %u ms。"
             "负数会被读成一个极大的无符号值（-1 即 49.7 天），那等于事实上的"
             "永久阻塞",
             (unsigned long long)bad_drain_timeout_, drain_timeout_ms_);
    }
    if (tls_cfg_.insecureSkipVerify)
    {
        HTTP_LOGQ(logger_ctx_, _WARN_,
             "[HttpConnector] insecureSkipVerify=true：**不做任何证书校验**，"
             "只应出现在自签调试环境里");
    }

    HTTP_LOGQ(logger_ctx_, _INFO_,
         "[HttpConnector] 已创建：vpnPolicy=%s dnsServers=%zu caCerts=%s "
         "spkiPin=%s maxResponseBytes=%zu",
         tt_vpn_policy_to_string(policy_), dns_cfg_.servers.size(),
         tls_cfg_.caCertsPem.empty() ? "(系统信任库)" : "(自定义)",
         tls_cfg_.spkiPin.empty() ? "(none)" : "(已设置)",
         max_response_bytes_);

    created_ = true;
    return BC_R_SUCCESS;
}

BCRESULT HttpConnector::Request(const HttpRequest& req,
                                IHttpRequestHandler* handler,
                                std::string* outErrMessage)
{
    if (outErrMessage) outErrMessage->clear();

    if (!created_)
    {
        return BC_R_NOTCONNECTED;
    }
    if (!handler)
    {
        return BC_R_INVALIDARG;
    }
    if (t_in_log_callback != 0)
    {
        
        
        
        
        
        return BC_R_NOTIMPLEMENTED;
    }

    HttpUrlParts parts;
    std::string  err;
    if (!ParseUrl(req.url, parts, err))
    {
        if (outErrMessage) *outErrMessage = "URL 解析失败：" + err;
        HTTP_LOGQ(logger_ctx_, _ERROR_, "[HttpConnector] URL 解析失败：%s", err.c_str());
        return BC_R_INVALIDARG;
    }

    
    
    bool pool_enabled = false;
    {
        StateLock lk(*state_);
        pool_enabled = (state_->max_idle_per_key > 0);
    }

    std::string reqText;
    if (!BuildRequestText(req, parts, reqText, err, pool_enabled))
    {
        if (outErrMessage) *outErrMessage = "请求组装失败：" + err;
        HTTP_LOGQ(logger_ctx_, _ERROR_, "[HttpConnector] 请求组装失败：%s", err.c_str());
        return BC_R_INVALIDARG;
    }

    TcpChannelConfig cfg;
    cfg.host             = parts.host;
    cfg.port             = parts.port;
    cfg.tls              = parts.tls;
    cfg.resolvedIp       = req.resolvedIp;
    cfg.policy           = policy_;
    cfg.androidNetHandle = android_net_handle_;
    cfg.dns              = dns_cfg_;
    cfg.tls_cfg          = tls_cfg_;
    cfg.connectTimeoutMs = req.timeoutMs ? req.timeoutMs : 10000;
    cfg.loggerCtx        = logger_ctx_;

    
    
    const std::string poolKey  = _PoolKey(cfg);
    PooledChannel*    borrowed = NULL;
    if (pool_enabled)
    {
        borrowed = _TakeFromPool(state_, poolKey);
    }

    HttpConnection* conn = NULL;
    {
        StateLock lk(*state_);
        if (state_->closing)
        {
            if (borrowed)
            {
                
                borrowed->Channel()->Close();
                borrowed->Release();
            }
            return BC_R_SHUTTINGDOWN;
        }
        conn = borrowed
             ? new HttpConnection(state_, handler, logger_ctx_,
                                  max_response_bytes_, borrowed->Channel())
             : new HttpConnection(state_, handler, logger_ctx_,
                                  max_response_bytes_);
        state_->conns.insert(conn);
    }

    HTTP_LOGQ(logger_ctx_, _INFO_,
         "[HttpConnector] %s %s%s（host=%s port=%u tls=%d timeout=%ums）",
         req.method.c_str(), parts.hostHeader.c_str(), parts.target.c_str(),
         parts.host.c_str(), (unsigned)parts.port, parts.tls ? 1 : 0,
         cfg.connectTimeoutMs);

    
    
    bool isHead = (_ToUpperAscii(req.method.empty() ? std::string("GET")
                                                    : req.method) == "HEAD");
    BCRESULT r = conn->Start(cfg, reqText, req.timeoutMs, isHead, pool_enabled);

    if (r == BC_R_SUCCESS && borrowed)
    {
        
        
        borrowed->Release();
        borrowed = NULL;
    }

    if (r != BC_R_SUCCESS && borrowed)
    {
        
        
        
        HTTP_LOGQ(logger_ctx_, _INFO_,
             "[HttpConnector] 池中连接已失效（result=%d），改用新连接重试：%s:%u",
             (int)r, parts.host.c_str(), (unsigned)parts.port);

        conn->DisownChannel();      
        {
            DispatchGuard          guard(state_);
            IHttpConnectorHandler* closedHandler =
                _DetachAndArmClosed(state_, conn, guard);
            delete conn;
            conn = NULL;
            if (closedHandler)
            {
                closedHandler->OnClosed();
            }
        }
        
        
        borrowed->Channel()->Close();
        borrowed->Release();
        borrowed = NULL;

        {
            StateLock lk(*state_);
            if (state_->closing)
            {
                return BC_R_SHUTTINGDOWN;
            }
            conn = new HttpConnection(state_, handler, logger_ctx_,
                                      max_response_bytes_);
            state_->conns.insert(conn);
        }
        r = conn->Start(cfg, reqText, req.timeoutMs, isHead, pool_enabled);
    }

    if (r != BC_R_SUCCESS)
    {
        
        
        
        
        
        
        
        
        DispatchGuard          guard(state_);
        IHttpConnectorHandler* closedHandler =
            _DetachAndArmClosed(state_, conn, guard);
        
        const std::string openErr = conn->LastOpenError();
        if (outErrMessage) *outErrMessage = openErr;
        
        
        delete conn;
        HTTP_LOGQ(logger_ctx_, _ERROR_,
             "[HttpConnector] 连接发起失败 result=%d（%s:%u）：%s",
             (int)r, parts.host.c_str(), (unsigned)parts.port, openErr.c_str());
        if (closedHandler)
        {
            closedHandler->OnClosed();
        }
        return r;
    }

    
    
    
    
    
    
    
    {
        StateLock lk(*state_);
        if (state_->closing && state_->conns.count(conn) > 0)
        {
            conn->Cancel();
        }
    }
    return BC_R_SUCCESS;
}

size_t HttpConnector::MaxIdleConnections() const
{
    StateLock lk(*state_);
    return state_->max_idle_per_key;
}

uint32_t HttpConnector::IdleTimeoutMs() const
{
    StateLock lk(*state_);
    return state_->idle_timeout_ms;
}

void HttpConnector::SetIdleTimeoutMs(uint32_t ms)
{
    const uint32_t kMaxIdleMs = 24u * 60u * 60u * 1000u;
    uint32_t       clamped    = (ms > kMaxIdleMs) ? kMaxIdleMs : ms;
    {
        StateLock lk(*state_);
        state_->idle_timeout_ms = clamped;
    }
    
    
    
    if (ms > kMaxIdleMs)
    {
        HTTP_LOGQ(logger_ctx_, _WARN_,
             "[HttpConnector] SetIdleTimeoutMs(%u) 超出允许范围，已夹到 %u ms",
             ms, clamped);
    }
}

void HttpConnector::Close()
{
    {
        StateLock lk(*state_);
        state_->closing = true;
        
        
        
        
        
        
        
        
        
        
        
        
        for (std::set<HttpConnection*>::iterator it = state_->conns.begin();
             it != state_->conns.end(); ++it)
        {
            (*it)->Cancel();
        }

        
        
        
        
        
        
        for (std::map<std::string, std::vector<PooledChannel*> >::iterator it =
                 state_->idle.begin(); it != state_->idle.end(); ++it)
        {
            for (size_t i = 0; i < it->second.size(); i++)
            {
                it->second[i]->Channel()->Close();
            }
        }
    }
    _NotifyClosedIfDrained(state_);
}

void HttpConnector::_LogCallback(void* data, int level, LPCSTR msg)
{
    HttpLogSink* sink = (HttpLogSink*)data;
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




