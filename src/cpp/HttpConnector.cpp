///////////////////////////////////////////////////////////////////////////////
// file   : HttpConnector.cpp
// author : anto
//
// 通用 HTTP 客户端的实现。设计说明与生命周期契约见 HttpConnector.h 顶部注释。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// File-scope helpers
///////////////////////////////////////////////////////////////////////////////

namespace {

///////////////////////////////////////////////////////////////////////////////
// 响应头的硬上限
//
// maxResponseBytes 只盖报文体。头部这条路必须单独设防：llhttp 自己**不带任何
// 这类上限**（Node.js 是在它自己的 C++ 层另做的，--max-http-header-size 默认
// 16 KiB，覆盖 status line + 全部 header），所以只要不管，一个恶意或故障的
// 服务端可以：
//   * 无限重复 "X-Pad-N: AAAA...\r\n"          —— 实测 RSS 29 MB -> 1394 MB
//   * 发一条永不结束的 status reason（连 CR 都不发）—— 实测再涨到 2440 MB
//   * 发单条 8 MB 的头，maxResponseBytes=1024 也照收不误
// 三者都只有请求超时才能停下来，而在那之前内存已经没了。
//
// 取值比 Node.js 的 16 KiB 宽松一档（真实世界里带一堆 CDN 头 + 长 CSP 的响应
// 确实会超过 16 KiB），但每一项都有界：
const size_t kMaxHeaderTotalBytes  = 64 * 1024;  // reason + 所有头名头值之和
const size_t kMaxSingleHeaderBytes = 16 * 1024;  // 单条头（名 + 值）
const size_t kMaxHeaderCount       = 200;        // 头条数
const size_t kMaxReasonBytes       = 512;        // status line 的 reason-phrase

// 1xx 中间响应的连发上限。每收到一条 1xx 就把头部预算清零重来（否则一次
// 100-continue 就把最终响应的预算吃掉了），所以必须另外限一下条数，免得
// 服务端靠无限 1xx 把连接一直吊到超时。
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

// RFC 7230 的 token 字符集。方法名与头名都必须是 token，否则就是注入。
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

// 头值里不许出现 CR / LF / NUL —— 否则调用方可以往报文里塞任意行，
// 也就是经典的 HTTP header injection / request smuggling。
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

// 主机名 / 请求目标里同样不许出现空白与控制字符。
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

// 有报文体语义的方法即使 body 为空也要显式给 Content-Length: 0，
// 否则服务端在 "Connection: close" 下无从判断请求体是否已经结束。
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

} // namespace

///////////////////////////////////////////////////////////////////////////////
// HttpConnectorState —— 析构与排空通知
///////////////////////////////////////////////////////////////////////////////

HttpConnectorState::~HttpConnectorState()
{
    // 走到这里说明连接器、所有连接、所有 delete 任务都已经撒手，
    // 没有任何人还握着 logger_ctx，可以安全回收 appender。
    if (own_logger && logger_ctx)
    {
        void* ctx = logger_ctx;
        RemoveLogAppender(ctx);
    }
    delete log_sink;
}

namespace {

///////////////////////////////////////////////////////////////////////////////
// state->lock 的 RAII 包装 + "持锁时禁止打日志"的不变量
//
// ---------------------------------------------------------------------------
// 为什么这条不变量是硬性的
// ---------------------------------------------------------------------------
// BC 的日志器是**同步**的，而且带一把**全局非递归自旋锁**：
//
//     LogQ (Utils.cpp:311)
//       -> BCLogger::Log (BCLog.cpp:449)
//            BCSpinMutex::Owner lock(s_lock);      <-- 全局锁
//            ((BCLogBase*)logger_ctx)->Log(...)    <-- 持着它调 appender
//              -> HttpConnector::_LogCallback
//                   -> 业务的 IHttpConnectorHandler::OnLog
//
// 也就是说，业务的 OnLog 是在**持有 BCLogger 全局锁**的情况下被调用的。
// 于是只要本模块在持 state->lock 时打一条日志，就形成了
//
//     state->lock  ->  BCLogger::s_lock
//
// 而业务在 OnLog 里回头调 Request() / Close() 是反方向
//
//     BCLogger::s_lock  ->  state->lock
//
// 两条路跑在不同线程上就是 100% 必现的死锁 —— **业务自己一把锁都不需要有**，
// 反序的另一半就在 ttsignal 内部的日志器里。同线程更直接：在 OnLog 里调
// Request()，Request 再打一条日志就要重进那把非递归自旋锁，100% CPU 空转
// 且没有任何诊断信息。
//
// （早前一版把 HttpLogSink::lock 换成递归锁想解决这个，那是修错了环节：
//   反序发生在中间的 BCLogger 全局锁上，不在链条末端的 sink 锁上。）
//
// ---------------------------------------------------------------------------
// 怎么保证这条不变量不被后来人破坏
// ---------------------------------------------------------------------------
//   1. **本文件里一律用 HTTP_LOGQ，不准直接写 LogQ。** 可以直接 grep 验证：
//        grep -n 'LogQ(' src/cpp/HttpConnector.cpp
//      结果里除了 HTTP_LOGQ 的宏定义本身，不该出现裸的 LogQ。
//   2. HTTP_LOGQ 在持锁时 assert 失败（Debug 构建当场炸出调用栈）；
//      Release 构建则**丢掉这条日志**而不是去踩死锁 —— 少一条日志远好过挂死。
//   3. 所有对 state->lock 的加锁一律走 StateLock，由它维护 thread_local 深度。
//      直接写 std::lock_guard<std::recursive_mutex> 会绕过计数，同样可以 grep：
//        grep -n 'recursive_mutex> *lk' src/cpp/HttpConnector.cpp
//      应当只在 StateLock 与 DispatchGuard 内部出现。
///////////////////////////////////////////////////////////////////////////////

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
    // 供 condition_variable_any 使用。注意：wait 期间互斥量真的被放开了，
    // 而 t_state_lock_depth 仍然 > 0 —— 刻意的保守偏差，宁可多拦，
    // 反正等待期间本来也不该打日志。
    std::unique_lock<std::recursive_mutex>& Raw() { return lk_; }

private:
    DECLARE_NO_COPY_CLASS(StateLock);
    std::unique_lock<std::recursive_mutex> lk_;
};

// ⚠️ 本文件里打日志只准用这个宏，理由见上。
#define HTTP_LOGQ(...)                                                         \
    do {                                                                       \
        if (t_in_log_callback != 0)                                            \
        {                                                                      \
            /* 在 OnLog 回调栈里：BCLogger 的全局自旋锁正被本线程持有，       \
               再打一条就是重进非递归锁而空转挂死。直接丢掉。 */              \
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

///////////////////////////////////////////////////////////////////////////////
// 回调派发
//
// ⚠️ 业务回调（OnHttpResponse / OnHttpError / OnClosed）一律在**锁外**发出。
//
// 早前的实现是持锁发的，目的是关掉这个窗口：读出 handler 指针 -> 放锁 ->
// 放弃路径把 handler 置空、调用方随即释放它 -> 我们再去调，就是 UAF。
// 但"持锁发回调"把整个外部世界都拖进了本模块的锁里：日志（见上面那段）、
// 业务自己的锁、回调里重入 Request()/Close() 时的 TcpChannel::Open()……
// 每一条都是潜在的锁序反转，而且全都落在文档明确祝福的用法上。
//
// 改成"锁内登记、锁外执行、回来注销"：
//   * 登记时 callbacks_in_flight++（在锁内，与放弃路径互斥）；
//   * 放弃路径清空 handler 之后**等 callbacks_in_flight 归零**才返回，
//     所以调用方拿回控制权时，不可能还有回调正握着它的 handler 在跑；
//   * 回调本身在锁外执行，本模块的锁不再出现在任何业务栈帧的下方。
//
// t_dispatching 记录"本线程正在派发哪些 state 的回调"，用途有二：
//   * ~HttpConnector 靠它识别"业务在回调栈里析构连接器"，跳过注定等不到结果
//     的排空等待（那条连接要等本回调返回才会从 conns 里摘除）；
//   * 等 callbacks_in_flight 归零时把本线程自己那几层排除在外，否则自等。
// 用 vector 而不是单指针，是为了正确处理"连接器 A 的回调里又触发了 B 的回调"。
///////////////////////////////////////////////////////////////////////////////

thread_local std::vector<const HttpConnectorState*> t_dispatching;

///////////////////////////////////////////////////////////////////////////////
// "本线程正在 OnLog 回调里"标记
//
// _LogCallback 是被 BCLogger 持着它的**全局非递归自旋锁**调进来的。业务如果在
// OnLog 里回头调 Request()，Request 一路上会打日志（本模块自己的，以及
// TcpChannel::Open() 里那几条），再进那把锁就是 100% CPU 空转、没有任何诊断
// 信息的挂死。
//
// 本模块自己的日志可以靠这个标记压掉，**但 TcpChannel 的压不掉**（那是它直接
// 调的 LogQ，本模块管不着，而 TcpChannel 不在本任务的可改范围内）。所以正确的
// 做法不是"想办法让它能跑"，而是**当场拒绝**：把一个无法诊断的挂死换成一个
// 调用方看得见的错误码。契约里也明写了这条禁令。
///////////////////////////////////////////////////////////////////////////////

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

    // ⚠️ 必须在**已持有 state->lock** 的情况下调用
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

// 连接真正销毁之后的收尾：把它从 destroying 里摘掉，顺带判断要不要发 OnClosed。
//
// ⚠️ 摘除与"登记 OnClosed 在途"必须在**同一个临界区**里完成。分成两段的话中间
// 有一个窗口：析构那边已经看到两个集合都空了（于是返回、调用方随即释放它的
// handler），我们才刚要去派发 OnClosed —— 打在已经析构的 handler 上。
// TSan 实测抓到过：上一个用例栈上的 ConnHandler 已经析构，地址被下一个用例的
// ConnHandler 复用，OnClosed 打在了新对象上（ASan 看不见，因为是栈地址复用）。
// 把一条连接从 conns / destroying 里摘掉，并**在同一个临界区里**判定要不要发
// OnClosed。返回需要回调的 handler（已登记为在途；NULL 表示不用发）。
//
// ⚠️ 调用方必须把传进来的 guard 一直留到回调返回之后 —— 它就是"在途"的凭证。
// ⚠️ 摘除与登记必须原子。分成两段的话中间有个窗口：析构那边已经看到两个集合都
// 空了（于是返回，调用方随即释放它的 handler），我们才刚要去派发 OnClosed ——
// 打在已经析构的 handler 上。TSan 实测抓到过（ASan 看不见，因为是栈地址复用）。
// "内部对象全部销毁完了吗"——OnClosed 的前提，也是析构等待的谓词。
// ⚠️ 四项缺一不可，而且必须只有这一处定义：漏掉任何一项都表现为"析构提前
// 返回、调用方释放 handler、随后的回调打在野指针上"，偶发且极难复现。
// 调用方必须已持 state.lock。
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
    // 只用指针值，不解引用 —— 调用方可能已经（或即将）销毁它。
    // 一个指针只会在其中一个集合里，两边都擦一遍最省事。
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
        guard.ArmLocked();              // 与 erase 同一临界区，析构看得见
    }
    // ⚠️ 一定要 notify：否则析构可能白等满 drainTimeoutMs
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

// 一条池化连接彻底销毁完了。销账与"判定要不要发 OnClosed"必须在同一个临界
// 区里完成 —— 理由与 _DetachAndArmClosed 一字不差：中间若有并发析构看到全空
// 就返回、调用方释放 handler，我们随后就把 OnClosed 打在已释放的 handler 上。
//
// ⚠️ state 按**值**传，理由见下面 _NotifyClosedIfDrained 的注释。
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
            guard.ArmLocked();      // 与销账同一临界区，析构看得见
        }
        // ⚠️ 一定要 notify：否则析构可能白等满 drainTimeoutMs
        state->cv.notify_all();
    }
    if (handler)
    {
        handler->OnClosed();
    }
}

// 全部排空且已请求关闭时发一次 OnClosed。放弃等待之后不再发。
//
// ⚠️ state 按**值**传：业务完全可以在 OnClosed 里析构连接器，那会释放连接器
// 那一份引用；如果这里收的是引用（调用方传的往往正是成员 state_），引用在
// 回调返回后就悬垂了，而我们还要用它注销派发计数 —— 实测症状是"在已释放的
// recursive_mutex 上 unlock"，ASan/TSan 都看不见（pthread_mutex_unlock 在
// 未插桩的 libsystem 里），属于"sanitizer 干净"不能当证据的一类。
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

// 池的分桶键。凡是"换一条连接就会变"的东西都必须拼进来 —— 池化最危险的错误
// 就是把一条连接交给一个不该走它的请求（发到另一个 host、或者绕过了本该绑的
// 那块网卡）。tls_cfg 也算：同一个 host 在不同 CA / SPKI pin 下的连接不能混用。
std::string _PoolKey(const TcpChannelConfig& cfg)
{
    std::string key = cfg.host;
    key += ":" + std::to_string((unsigned)cfg.port);
    key += cfg.tls ? ":tls" : ":tcp";
    key += ":p" + std::to_string((int)cfg.policy);
    key += ":i" + std::to_string((unsigned)cfg.ifIndex);
    key += ":n" + std::to_string((unsigned long long)cfg.androidNetHandle);
    // resolvedIp 非空时跳过 DNS 直连这个 IP，和"同名但解析到别处"不是一回事
    key += ":r" + cfg.resolvedIp;
    return key;
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// class : PooledChannel —— 池中一条空闲连接的所有者兼看守
//
// 一条连接躺在池里的时候是没有请求的：没有 HttpConnection，也就没人接它的
// 回调。可它随时会死（服务端有自己的 keep-alive 空闲超时），死了必须有人把它
// 从池里摘掉并回收 —— 这就是本类存在的全部理由。
//
// ⚠️ 生死由**两个互斥且必有其一**的回调界定，这是整个池化设计的支点：
//   * OnChannelClosed —— 连接死了。通道归我，连同本对象一起销毁。
//   * OnChannelDetached —— 连接被下一条请求 Adopt 走了。通道的所有权跟着
//     转移，本对象只销毁自己，**绝不能碰 channel_**。
//
// 两者不可能都发生：TcpChannel::Adopt 取号成功之后，OnChannelClosed 必定排在
// 接手那个 lambda 之后（见 _AcquirePendingTask 的注释），而 lambda 一旦换上
// 新 handler，后续回调就再也不会打到本对象了。
///////////////////////////////////////////////////////////////////////////////

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

    // 起空闲计时。⚠️ 必须在通道自己的队列线程上调用（归还连接的那个栈里）：
    // 定时器挂在通道的事件队列上，回调线程因此与 OnChannelClosed /
    // OnChannelDetached 完全一致，本对象的字段不需要额外加锁。
    void ArmIdleTimer(uint32_t idleMs)
    {
        if (idleMs == 0)
        {
            return;     // 不设本地超时，完全听服务端的
        }
        // 定时器回调里只推一把关闭，真正的回收仍然走 OnChannelClosed 那条
        // 唯一路径 —— 销毁时序已经在那里论证过一遍，不要再开第二条。
        channel_->ScheduleTask(idle_timer_, [this](int32_t) {
            idle_timer_ = 0;    // 已经触发，不必再取消
            channel_->Close();
        }, (uint64_t)idleMs * 1000);
    }

    // Park 状态的通道不会派发这两个回调（数据只会把通道标成不可复用）。
    void OnChannelReady() override {}
    void OnChannelData(const void*, size_t) override {}

    void OnChannelClosed(BCRESULT, const std::string&) override
    {
        _CancelIdleTimer();

        std::shared_ptr<HttpConnectorState> state = state_;

        if (state)
        {
            StateLock lk(*state);
            // ⚠️ 摘除与登记必须在同一个临界区：中间一旦松手，排空判定就会看到
            // "既不在 idle 里、也没算进 idle_destroying"的一条连接，于是认为
            // 池子空了。
            _RemoveFromPoolLocked(*state);
            state->idle_destroying++;
        }

        // ⚠️ 与 HttpConnection 同理：本回调跑在通道自己的队列线程上，那条线程
        // 此刻还停在 OnEventProcShutdown() 的栈帧里，不能就地 delete 通道。
        PooledChannel* self = this;
        Runtime::PostTask([self, state]() {
            delete self->channel_;
            delete self;
            // 销账与 OnClosed 的判定必须原子，理由见该函数的注释
            _FinishIdleDestroyAndMaybeNotify(state);
        });
    }

    void OnChannelDetached() override
    {
        // ⚠️ 取消计时器**必须**在这里做，而且必须赶在本对象销毁之前：定时器
        // 挂在通道的队列上，而通道此刻正活着转交给新主人。漏掉这一步，几十
        // 分钟后那个 lambda 会在已经 delete 的 PooledChannel 上回调，是一个
        // 潜伏期极长、几乎不可能复现的 UAF。
        _CancelIdleTimer();

        // 通道已经跟着 Adopt 转给新 handler 了。取走它的人**在持锁的时候就
        // 已经把本对象从 idle 里摘掉**（见 _TakeFromPool），所以这里不碰池子，
        // 也一个字都不能碰 channel_（除了上面那次 UnscheduleTask —— 那是在
        // 通道队列线程上、对本对象自己挂的任务做的取消）。
        PooledChannel* self = this;
        Runtime::PostTask([self]() { delete self; });
    }

private:
    DECLARE_NO_COPY_CLASS(PooledChannel);

    // 幂等。只在通道队列线程上调用。
    void _CancelIdleTimer()
    {
        if (idle_timer_ > 0)
        {
            channel_->UnscheduleTask(idle_timer_);
            idle_timer_ = 0;
        }
    }

    // 调用方必须已持 state.lock
    void _RemoveFromPoolLocked(HttpConnectorState& state)
    {
        std::map<std::string, std::vector<PooledChannel*> >::iterator it =
            state.idle.find(key_);
        if (it == state.idle.end())
        {
            return;     // 已经被取走或被关停流程摘走了
        }
        for (size_t i = 0; i < it->second.size(); i++)
        {
            if (it->second[i] == this)
            {
                it->second.erase(it->second.begin() + (long)i);
                break;
            }
        }
        // 空桶要连 key 一起删：关停时"池是否已经排空"就看 idle.empty()
        if (it->second.empty())
        {
            state.idle.erase(it);
        }
    }

    std::shared_ptr<HttpConnectorState> state_;
    TcpChannel*                         channel_;
    std::string                         key_;
    // 空闲计时器的 id，0 = 没挂。只由通道队列线程读写。
    int32_t                             idle_timer_ = 0;
};

namespace {

// 取一条可用的空闲连接。**调用方不持锁**，本函数自己取。
// 返回的 PooledChannel 已经从池里摘掉，调用方必须接着 Adopt 它的通道：
//   * Adopt 返回 SUCCESS —— 通道归新 handler，本对象会收到 OnChannelDetached
//     并自行销毁；
//   * Adopt 失败 —— 通道还归本对象，调用方调 Close() 让它走 OnChannelClosed
//     那条回收路径。
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

    // 后进先出：刚归还的那条离"服务端还没把它当空闲连接超时"最近。
    PooledChannel* taken = it->second.back();
    it->second.pop_back();
    if (it->second.empty())
    {
        state->idle.erase(it);
    }
    return taken;
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// class : HttpConnection —— 一次请求 = 一条连接
//
// 只在这个 .cpp 里可见。继承 LLHTTPParser 解析响应，继承 ITcpChannelHandler
// 接 TcpChannel 的三个回调。
//
// ⚠️ 生命周期（照抄 TcpChannel.h 的契约再往上叠一层）：
//   * Start() 返回非 BC_R_SUCCESS 时不会有任何回调，调用方可以立即 delete；
//   * 返回成功之后，唯一合法的销毁时机是 OnChannelClosed 之后，而且不能在
//     该回调里同步 delete —— 那时通道那条线程还停在 OnEventProcShutdown()
//     的栈帧里。所以统一 PostTask 到 Runtime 上去 delete。
///////////////////////////////////////////////////////////////////////////////

class HttpConnection
    : public LLHTTPParser
    , public ITcpChannelHandler
{
public:
    HttpConnection(const std::shared_ptr<HttpConnectorState>& state,
                   IHttpRequestHandler* handler,
                   void* loggerCtx,
                   size_t maxResponseBytes)
        // ⚠️ HTTP_RESPONSE 而不是默认的 HTTP_BOTH：客户端只应接受响应。
        // 留在 HTTP_BOTH 下，服务端回一段 "GET /evil HTTP/1.1\r\n..." 会被
        // 当成一条合法报文收下（status=0、body 照收），不报任何错。
        : LLHTTPParser(HTTP_RESPONSE)
        , state_(state)
        , handler_(handler)
        , logger_ctx_(loggerCtx)
        , max_response_bytes_(maxResponseBytes)
    {
        channel_ = new TcpChannel();
    }

    // 接管一条从池里捡来的、已经握完手的连接。通道的所有权随之转移过来：
    // 此后它和自己新建的那条没有任何区别，照常由 ~HttpConnection 销毁。
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
        // 析构期的 llhttp 回调派发问题由 ~LLHTTPParser 自己兜住（见
        // LLHTTPParser.cpp），派生类不需要、也不应该各自绕开。
        delete channel_;
        channel_ = NULL;
    }

    // 参数已由 HttpConnector 校验完毕。返回非 BC_R_SUCCESS 时无任何回调。
    BCRESULT Start(const TcpChannelConfig& cfg,
                   const std::string& reqText,
                   uint32_t timeoutMs,
                   bool headRequest,
                   bool poolEnabled)
    {
        // ⚠️ 所有自身状态必须在 Open() 之前设好：Open() 内部 PostTask 之后，
        // 另一条 worker 线程可以立刻派发回调，OnChannelClosed 甚至可以先于
        // Open() 返回。
        req_text_     = reqText;
        timeout_ms_   = timeoutMs ? timeoutMs : 10000;
        head_request_ = headRequest;
        start_ms_     = _NowMs();
        pool_key_     = _PoolKey(cfg);
        pool_enabled_ = poolEnabled;

        // 捡来的连接跳过 DNS / connect / TLS 全套，直接接手。两条路的后续
        // 完全一致：都以 OnChannelReady 起手（Adopt 的契约就是这么定的）。
        if (reused_)
        {
            return channel_->Adopt(this);
        }
        return channel_->Open(cfg, this);
    }

    // Start() 同步失败时的现场描述，转发通道那份。必须在本对象销毁之前取走。
    const std::string& LastOpenError() const { return channel_->LastOpenError(); }

    // 接手池中连接失败（Adopt 同步返回错误）时调用：那条通道其实还归池里
    // 那个看守，本对象没拿到所有权，销毁时绝不能去 delete 它。
    void DisownChannel() { channel_ = NULL; }

    // 由 HttpConnector::Close() 调用，可能来自任意线程。
    // TcpChannel::Close() 本身线程安全且幂等。
    void Cancel()
    {
        // ⚠️ 连接归还池子之后 channel_ 会被置空（所有权转给了 PooledChannel）。
        // 置空与这里的读取用的是**同一把 state->lock**（Close() 的契约就是
        // 持锁遍历 conns 再调本函数），所以这个判空既挡住了空指针，也不存在
        // 中间窗口。漏掉它就是 SEGV in TcpChannel::Close —— 实测必现。
        if (channel_)
        {
            channel_->Close();
        }
    }

    // 由 ~HttpConnector 的放弃路径调用，**调用方必须已持有 state_->lock**。
    // 把请求级 handler 掐断：放弃之后调用方随时可能把它释放掉，再回调就是 UAF。
    // 与 _DeliverOnce 读 handler_ 用的是同一把锁，所以不存在中间窗口。
    void ClearHandlerLocked()
    {
        handler_ = NULL;
    }

private:
    DECLARE_NO_COPY_CLASS(HttpConnection);

    ///////////////////////////////////////////////////////////////////////
    // ITcpChannelHandler —— 全部跑在通道自己的事件循环线程上
    ///////////////////////////////////////////////////////////////////////

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

        // 通道自己的 connectTimeoutMs 只盖到 TLS 握手为止，进 READY 就取消了。
        // 剩下的"发出去了但服务端一直不回"必须由这里补上，否则一个黑洞服务端
        // 能把调用方吊死。定时器挂在通道自己的事件队列上：回调线程与
        // OnChannelData / OnChannelClosed 完全一致，不需要额外加锁；通道收尾
        // 时 Detach() 会顺手把它取消掉。
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
            // 定时器建不起来就没有任何东西能兜住"服务端不回话"这种情况了
            // （通道自己的 connectTimeoutMs 进 READY 就取消了）。与其让调用方
            // 永久挂着，不如当场失败。
            _Fail(BC_R_UNEXPECTED,
                  "响应超时定时器创建失败，无法保证请求不会永久挂起，主动终止");
            return;
        }
    }

    void OnChannelData(const void* data, size_t size) override
    {
        if (delivered_)
        {
            return;     // 已经收尾，多出来的字节直接丢掉
        }
        llhttp_errno_t err = llhttp_execute(this, (const char*)data, size);
        // HPE_PAUSED 是 http_on_message_complete 主动 pause 出来的。
        // ⚠️ 别指望用它来判断"报文收全了"：pause 发生在最后一个字节之后时，
        // llhttp_execute 照样返回 HPE_OK（实测如此）。收全与否只看
        // message_done_，那是 on_message_complete 亲手置的。
        if (err == HPE_OK || err == HPE_PAUSED)
        {
            if (message_done_ && !delivered_)
            {
                // 响应已经完整。连接还能用就别关，留给下一条请求。
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
            // http_on_body 的 maxResponseBytes 截断已经记过原因，别覆盖掉
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

    ///////////////////////////////////////////////////////////////////////
    // 内部
    ///////////////////////////////////////////////////////////////////////

    // 这条响应收完之后，连接还值不值得留给下一条请求。
    bool _ShouldKeepConnectionAlive() const
    {
        return pool_enabled_
            && keep_alive_          // 服务端没说要关，且报文边界是确定的
            && message_done_
            && fail_result_ == BC_R_SUCCESS
            && !delivered_;
    }

    // 响应已经完整、连接还活着：派发响应，把通道交给连接池，然后自毁。
    //
    // ⚠️ 只能从 OnChannelData 的栈里调用。本函数跑在通道自己的队列线程上，
    // 从这里到返回之间不可能有并发的通道回调，下面几步才不需要额外同步。
    void _FinishAndReturnChannelToPool()
    {
        TcpChannel* ch = channel_;

        // 先派发响应：_DeliverOnce 要读通道的 peer 信息、要取消响应定时器，
        // 都得赶在交出通道之前做。
        _DeliverOnce(BC_R_SUCCESS, std::string());

        // 交给看守。Park 之后通道就无主了（除了这个看守），它是池里那条
        // 连接唯一的所有者 —— 详见 PooledChannel 的注释。
        PooledChannel* pooled = new PooledChannel(state_, ch, pool_key_);
        ch->Park(pooled);

        bool     pooledOk  = false;
        uint32_t idleMs    = 0;
        {
            StateLock lk(*state_);
            // ⚠️ 置空必须在锁内：Close() 会持这把锁遍历 conns 并调 Cancel()，
            // 而本对象此刻还在 conns 里。锁外置空的话，那边读到的就是一个刚
            // 被清掉的 channel_（实测 SEGV in TcpChannel::Close）。
            channel_ = NULL;    // 所有权已转移，~HttpConnection 不再销毁它
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
            // 计时在入池之后才起：这样"空闲"从它真正可被取用的那一刻开始算。
            // 每次归还都是新起一轮，所以一条被持续复用的热连接不会被掐掉。
            pooled->ArmIdleTimer(idleMs);
        }
        if (!pooledOk)
        {
            // 池满了，或者连接器正在关停：这条连接没人要。交给 PooledChannel
            // 的正常回收路径（Close -> OnChannelClosed -> 连通道带看守一起销毁），
            // 不要在这里自己 delete —— 通道还没走完关闭流程。
            ch->Close();
        }

        _DetachAndScheduleDestroy();
    }

    // 从连接器的在途集合里摘除，并把自身的销毁排到 Runtime 上。
    // 走到这里就意味着本对象已经收尾，不会再有任何回调。
    void _DetachAndScheduleDestroy()
    {
        std::shared_ptr<HttpConnectorState> state = state_;
        if (state)
        {
            StateLock lk(*state);
            if (state->conns.erase(this) > 0)
            {
                // 挪进 destroying：已经收尾但还没真正销毁。放弃路径也要能
                // 遍历到它们，所以记的是集合而不是计数。
                state->destroying.insert(this);
            }
        }

        // ⚠️ 只有走到这里销毁才安全，而且不能在本回调里同步 delete —— 通道那条
        // 线程此刻还停在 OnEventProcShutdown() 的栈帧里。挪到 Runtime 的队列上。
        //
        // ⚠️ 捕获的是 shared_ptr 而不是裸的连接器指针。早前这里按值捕获
        // HttpConnector*，于是这条路径绕过了所有"掐断"机制：连接器可能在本任务
        // 跑起来之前就析构完了，任务再去碰它的锁就是 UAF（drainTimeoutMs 越小
        // 越容易命中，取 0 时每次都走这条路，压测 7/7 必崩）。
        HttpConnection* self = this;
        Runtime::PostTask([self, state]() {
            delete self;
            // 摘除与 OnClosed 的判定必须原子，理由见该函数的注释
            _FinishDestroyAndMaybeNotify(state, self);
        });
    }

    ///////////////////////////////////////////////////////////////////////
    // LLHTTPParser —— 同样跑在通道的事件循环线程上（由 llhttp_execute 驱动）
    ///////////////////////////////////////////////////////////////////////

    int http_on_url(const char* at, size_t length) override
    {
        (void)at;
        (void)length;
        return 0;       // 客户端只解析响应，不会有 request-line
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
        // llhttp 2.0.4 没有 llhttp_get_status_code()，status_code 是结构体的
        // 公开字段，直接读。
        status_ = (int)this->status_code;

        // ------------------------------------------------------------------
        // 1xx 是**中间**响应，不是最终响应。
        //
        // llhttp 会为它单独派发一次 headers_complete + message_complete，然后
        // 继续解析真正的最终响应。不区分的话，第一条 1xx 就会被当成最终结果
        // 交付、连接随即被拆掉，业务拿到的是 status=103 + 空 body 而且还是
        // "成功"。103 Early Hints 现在 Cloudflare / Fastly 都在发，100 Continue
        // 更是标准行为 —— 挂在 CDN 后面的接口会因此静默返回错误结果。
        //
        // 处理方式：把已经攒下的中间态整个丢掉，继续等下一条报文。
        // ------------------------------------------------------------------
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

        // ------------------------------------------------------------------
        // 明确没有报文体的响应，必须告诉 llhttp 别去等 body。
        //
        // llhttp 不知道我们发的是什么方法，对 HEAD 响应会老老实实等
        // Content-Length 声明的那么多字节，于是永远等不到 message_complete：
        // 服务端 keep-alive 就一路挂到 timeoutMs，关连接就报"响应不完整"。
        // 204 / 304 同理，而 304 回显 Content-Length 是 RFC 7232 明确允许的，
        // Apache 和多家 CDN 就这么做。
        //
        // 返回 1 是 llhttp 的 skip-body 约定。
        // ------------------------------------------------------------------
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
            return -1;      // 让 llhttp 停止解析
        }
        body_.append(at, length);
        return 0;
    }

    int http_on_message_complete() override
    {
        if (interim_)
        {
            // 刚刚那条是 1xx 中间响应，llhttp 会接着解析最终响应。
            // 绝不能在这里置 message_done_ / 关连接。
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
        // ⚠️ keep-alive 的判断必须就地做。llhttp_should_keep_alive 读的是刚解析
        // 完的这条报文的状态（HTTP 版本 + Connection 头 + body 是不是靠关连接
        // 断句），pause 之后就问不到了。它也是我们唯一的判据：HTTP/1.0 不带
        // Connection: keep-alive、以及服务端显式说 Connection: close，都在这里
        // 被算成 0。
        keep_alive_ = (llhttp_should_keep_alive(this) != 0);
        // 暂停解析：这条报文已经完整，暂停可以避免 llhttp 把粘在后面的字节
        // 当成新报文再触发一次本回调。
        llhttp_pause(this);
        // 收尾（关连接还是归还连接池）统一交给 OnChannelData 去做 —— 那里在
        // llhttp 的栈帧之外，动通道和派发业务回调都更踏实。
        //
        // delivered_ 为真说明现在是 _DeliverOnce 里的 llhttp_finish() 打回来
        // 的，那时通道已经在关，本来也轮不到我们推。
        return 0;
    }

    ///////////////////////////////////////////////////////////////////////
    // 内部
    ///////////////////////////////////////////////////////////////////////


    // 返回 false 表示撞了上限、已经记好失败原因，调用方应当立刻 return -1
    // 让 llhttp 停止解析。
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
            // 同名头按 RFC 7230 3.2.2 以逗号合并。⚠️ Set-Cookie 是那条规则的
            // 著名例外，合并之后拆不回来 —— 本模块不做 cookie 处理，限制已经
            // 写在 HttpResponse::headers 的注释里。
            it->second += ", ";
            it->second += cur_value_;
        }
        cur_field_.clear();
        cur_value_.clear();
        last_was_value_ = false;
        return true;
    }

    // 头部总预算。status reason 与所有头名头值都走这里扣账。
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

    // 单条头（名 + 值）的上限。总预算之外还要单独限一次，否则一条 60 KB 的
    // 头虽然进得了总预算，却会让下游拿到一个荒唐的取值。
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

    // 收到 1xx 中间响应之后清空中间态，让最终响应从干净的状态重新开始。
    // 头部预算一并归零 —— 否则一次 100-continue 就把最终响应的预算吃掉了；
    // 连发次数另有 kMaxInterimResponses 兜底。
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

    // 记下失败原因并推动通道关闭。真正的回调统一在 OnChannelClosed 里发。
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
            // 通道 Detach 时已经统一取消过，这里只是把 id 归位，失败无所谓。
            channel_->UnscheduleTask(resp_timer_);
            resp_timer_ = 0;
        }

        // 响应体以 EOF 结尾（无 Content-Length 且非 chunked）时，llhttp 只有
        // 被显式收尾才会派发 on_message_complete。"Connection: close" 下这是
        // 很常见的一类响应，不做这一步会把成功当成失败。
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

            // ⚠️ 锁内登记、**锁外**执行。放弃路径清空 handler 之后会等
            // callbacks_in_flight 归零才返回，所以调用方拿回控制权时不可能还有
            // 回调握着它的 handler；而回调本身跑在锁外，本模块的锁不会出现在
            // 业务栈帧的下方（否则业务打个日志、取个自己的锁都可能死锁）。
            IHttpRequestHandler* h = NULL;
            DispatchGuard        guard(state_);
            {
                StateLock lk(*state_);
                if (!handler_)
                {
                    return;     // 连接器已放弃等待，或本就没有 handler
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
                    // 头收全了、body 却要靠连接关闭来断句，而这次关闭又不干净。
                    // 这种响应天生可被截断（TLS 场景就是 truncation 攻击），
                    // 所以判失败而不是把半截 body 交上去。单独给一句说明，
                    // 免得排查的人只看到一个 result=24 摸不着头脑。
                    message += "；响应头已收全（status="
                             + std::to_string(status_)
                             + "），但报文体以连接关闭为终止，无法确认是否完整，"
                               "故判为失败。服务端应给 Content-Length 或用 chunked";
                }
            }
            else
            {
                // 通道"正常关闭"但报文没收全：对端在响应中途 FIN。
                result  = BC_R_UNEXPECTEDEND;
                message = "连接已关闭但 HTTP 响应不完整（对端在响应中途断开）";
            }
        }
        HTTP_LOGQ(logger_ctx_, _ERROR_, "[HttpConnector] 请求失败 result=%d: %s",
                  (int)result, message.c_str());

        // 同上：锁内登记，锁外执行
        IHttpRequestHandler* h = NULL;
        DispatchGuard        guard(state_);
        {
            StateLock lk(*state_);
            if (!handler_)
            {
                return;     // 连接器已放弃等待，或本就没有 handler
            }
            h = handler_;
            guard.ArmLocked();
        }
        h->OnHttpError(result, message);
    }

private:
    // 共享状态。它比连接器活得久，所有回连接器的销账动作都经由它。
    std::shared_ptr<HttpConnectorState> state_;
    // 请求级 handler，由 state_->lock 保护（放弃路径会在同一把锁下置空）
    IHttpRequestHandler *   handler_    = NULL;
    void                *   logger_ctx_ = NULL;
    size_t                  max_response_bytes_ = 0;

    TcpChannel          *   channel_    = NULL;
    std::string             req_text_;
    uint32_t                timeout_ms_ = 10000;
    uint64_t                start_ms_   = 0;
    int32_t                 resp_timer_ = 0;

    // 响应解析中间态
    std::string             cur_field_;
    std::string             cur_value_;
    bool                    last_was_value_ = false;
    std::string             reason_phrase_;
    HttpHeaderMap           headers_;
    std::string             body_;
    int                     status_       = 0;
    bool                    message_done_ = false;
    bool                    head_request_ = false;
    // 头部预算（reason + 所有头名头值），见 kMaxHeaderTotalBytes
    size_t                  header_bytes_ = 0;
    // 刚刚解析完的那条是否 1xx 中间响应
    bool                    interim_        = false;
    uint32_t                interim_count_  = 0;

    BCRESULT                fail_result_  = BC_R_SUCCESS;
    std::string             fail_message_;
    bool                    delivered_    = false;

    // ---- 连接复用 ----
    // 本条请求用的是从池里捡来的连接（Start 走 Adopt 而不是 Open）
    bool                    reused_       = false;
    // 连接器允许池化（maxIdleConnections > 0）
    bool                    pool_enabled_ = false;
    // 服务端这条响应之后还愿意留着连接，由 llhttp_should_keep_alive 判定
    bool                    keep_alive_   = false;
    // 归还时的分桶键，见 _PoolKey
    std::string             pool_key_;
};

///////////////////////////////////////////////////////////////////////////////
// class : HttpConnector —— 纯函数部分
///////////////////////////////////////////////////////////////////////////////

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

    // authority 到第一个 '/' '?' '#' 为止
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
        // userinfo 在 HTTP(S) 里早已废弃（RFC 7230 2.7.1），而且会让 Host 头与
        // SNI 的取值变得含糊，直接拒绝比猜一个语义更安全。
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
            // 裸写的 IPv6（多个冒号且没有方括号）会落到这里
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

    // Host 头：IPv6 保留方括号；端口等于 scheme 默认值时不写端口
    std::string hostForHeader = bracketed ? ("[" + hostPart + "]") : hostPart;
    if (port != defaultPort)
    {
        char portBuf[8];
        snprintf(portBuf, sizeof(portBuf), ":%u", (unsigned)port);
        hostForHeader += portBuf;
    }
    out.hostHeader = hostForHeader;

    // request-target：从 authority 之后一直到 '#'（fragment 不上线）
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

    // Host 允许业务覆盖（某些网关按 Host 分流），但默认取 URL 里的主机名而非
    // 解析出来的 IP —— 否则虚拟主机会返回错误内容。
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
    // HTTP/1.1 默认就是 keep-alive，想复用连接反而是什么都不发。禁用池化时
    // 才显式说 close —— 那既能让服务端立刻释放它那一侧，也让"不复用"这件事
    // 在抓包里一眼可辨。
    if (!keepAlive)
    {
        out += "Connection: close\r\n";
    }

    for (HttpHeaderMap::const_iterator it = req.headers.begin();
         it != req.headers.end(); ++it)
    {
        std::string lower = _ToLowerAscii(it->first);
        // 这三个由本函数权威决定，业务给的同名项一律忽略：让调用方同时控制
        // Content-Length 与 body 等于把请求走私的钥匙交出去。
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

///////////////////////////////////////////////////////////////////////////////
// class : HttpConnector
///////////////////////////////////////////////////////////////////////////////

HttpConnector::HttpConnector()
    : state_(std::make_shared<HttpConnectorState>())
{
    //
}

HttpConnector::~HttpConnector()
{
    Close();

    // 等到所有在途请求的 HttpConnection **析构完成**为止。只等回调发完是不够的：
    // 真正的 delete 是 PostTask 到 Runtime 上去做的。
    //
    // ⚠️ 这个等待带上限（drainTimeoutMs，默认 30 秒），不是无限等。原因见头文件
    // 契约第 5 条：Runtime::PostTask 在 Runtime 已经 Destroy() 之后会被**静默
    // 丢弃**（Runtime.cpp:129 的 if (s_pInstance)），调用方若把销毁顺序写反，
    // 那些 delete 任务永远不会跑，无限等就是永久死锁。
    // ⚠️ 自检：业务在回调栈里析构连接器（见头文件契约第 7 条）。
    // 那条连接要等本回调返回之后才会从 conns 里摘除，所以排空的谓词在本次
    // 析构期间**注定不可能成立** —— 只会白等满 drainTimeoutMs（默认 30 秒，
    // 上限 5 分钟），最后还是走放弃语义。既然结局注定，就别白等：直接把等待
    // 时长归零，并把现场打出来。
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
        // ⚠️ 谓词必须**同时**要求"没有在途回调"。只看两个集合是不够的：
        // 最后一条连接销毁时会在同一个临界区里把 OnClosed 登记为在途，
        // 若析构在那之后只检查集合就返回，调用方随即释放 handler，
        // 而 OnClosed 才刚要打出去（TSan 实测抓到）。
        // 本线程自己那几层要排除在外，否则自等（业务在回调栈里析构连接器）。
        const size_t mine = _DispatchDepthOn(state_.get());
        drained = state_->cv.wait_for(
            lk.Raw(), std::chrono::milliseconds(waitMs),
            [this, mine] {
                // ⚠️ 池子也必须排空（_AllDrainedLocked 把这一条算在内）。池里
                // 的连接虽然没有在途请求，它们回收完之后仍会去判定要不要发
                // OnClosed，那里要读 state->handler —— 析构一旦先返回，调用方
                // 就把 handler 释放了，那一下就是 UAF。上面的 Close() 已经把
                // 池里每条连接都推向关闭，这里等的是它们真正回收完。
                return _AllDrainedLocked(*state_)
                    && state_->callbacks_in_flight <= mine;
            });
        if (!drained)
        {
            // 放弃等待。共享状态本身会被残留连接与 delete 任务继续持有，所以
            // 它们照常销账、照常回收 appender，**不会碰任何已析构的东西**
            // —— 这正是把状态挪进 HttpConnectorState 的目的。
            //
            // 这里只需要把所有回调掐断：调用方一旦从析构返回，就可以释放它的
            // handler 了，之后任何回调都是 UAF。
            stranded          = state_->conns.size() + state_->destroying.size()
                              + state_->idle.size() + state_->idle_destroying;
            state_->abandoned = true;
            state_->handler   = NULL;   // 不再发 OnClosed
            for (std::set<HttpConnection*>::iterator it = state_->conns.begin();
                 it != state_->conns.end(); ++it)
            {
                // 与 _DeliverOnce 读 handler_ 用的是同一把锁，没有中间窗口
                (*it)->ClearHandlerLocked();
            }

            // ⚠️ 清空 handler 只挡住"还没开始的回调"。已经登记、正在锁外执行的
            // 那些还握着调用方的 handler 指针，必须等它们跑完才能让析构返回 ——
            // 否则调用方一释放 handler 就是 UAF。
            // 把本线程自己那几层排除在外（业务在回调栈里析构连接器时，
            // 在途计数里有一层就是它自己，等它归零等于自等）。
            state_->cv.wait(lk.Raw(), [this, mine] {
                return state_->callbacks_in_flight <= mine;
            });
        }
    }
    if (!drained && state_->log_sink)
    {
        // 日志回调也要断：它指向调用方的 IHttpConnectorHandler。
        // appender 本身留着不动 —— 残留连接和它们的 TcpChannel 还握着
        // logger_ctx 在打日志，等最后一个持有者撒手时由 ~HttpConnectorState
        // 统一回收。
        //
        // ⚠️ 刻意放在 state_->lock 的作用域**之外**：唯有如此，
        // state->lock -> sink->lock 这个嵌套才彻底不存在，业务在 OnLog 里回头
        // 调 Request() / Close()（sink->lock -> state->lock）也就不构成锁序反转。
        std::lock_guard<std::mutex> slk(state_->log_sink->lock);
        state_->log_sink->handler = NULL;
    }
    if (!drained)
    {
        // 措辞如实：残留连接稍后仍会自行销毁并回收共享状态，所以这不是"必然
        // 泄漏"，而是"本次析构没等到收尾"。真正会泄漏的只有"先 Runtime::
        // Destroy() 再析构本对象"那一种 —— 那时 delete 任务永远不会跑。
        HTTP_LOGQ(logger_ctx_, _ERROR_,
             "[HttpConnector] 析构等待 %u ms 仍未排空在途请求（残留 %zu 条，"
             "已掐断其全部回调），放弃等待。残留连接会自行收尾并回收共享状态；"
             "但若销毁顺序反了（先 Runtime::Destroy() 再析构 HttpConnector），"
             "那些收尾任务永远不会跑，就是真泄漏",
             drain_timeout_ms_, stranded);
    }

    logger_ctx_ = NULL;
    // state_ 在这里释放本对象那一份引用；appender 由 ~HttpConnectorState 回收
}

BCRESULT HttpConnector::Create(BCFObject* pConfig, IHttpConnectorHandler* handler)
{
    if (created_)
    {
        return BC_R_ALREADYRUNNING;
    }
    state_->handler = handler;

    // --------------------------------------------------------------
    // 日志。必须最先装好，后面的告警才有地方落。
    // --------------------------------------------------------------
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
        // 交给 appender 的是 HttpLogSink 而不是 this，理由见 HttpConnector.h
        // 里 HttpLogSink 的注释。appender 与 sink 的所有权都在共享状态上，
        // 由 ~HttpConnectorState 统一回收。
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

    // --------------------------------------------------------------
    // vpnPolicy（新键）与 bypassVpn（旧键，已废弃）合并，读法与
    // SMPConnector::Config::Init 完全一致。
    // --------------------------------------------------------------
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

    // --------------------------------------------------------------
    // DNS
    // --------------------------------------------------------------
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
            // 逗号分隔的写法，命令行工具用着方便
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

    // --------------------------------------------------------------
    // TLS
    // --------------------------------------------------------------
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
            // 每个目标（host+port+tls+网卡策略）最多囤几条空闲连接。
            // 0 = 关掉复用，退回"一次请求一条连接"的老行为。上限压在 64：
            // 池里的每条都是一个真实 fd，配大了只是把 fd 囤在自己手里。
            const uint64_t kMaxIdle = 64;
            uint64_t       v        = GET_BCF_INT(pVar);
            if (v > kMaxIdle) v = kMaxIdle;
            state_->max_idle_per_key = (size_t)v;
        }
        pVar = pConfig->Get("idleTimeoutMs");
        if (IS_BCF_NUMBER(pVar))
        {
            // 同 drainTimeoutMs 那个坑：GET_BCF_INT 是 uint64_t，传 -1 会变成
            // 49.7 天，超过 2^32 的值会被静默截断。夹到 24 小时，越界告警。
            // 0 是合法取值：不设本地超时，完全听服务端的。
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
            // ⚠️ GET_BCF_INT 返回 uint64_t，直接窄化成 uint32_t 会出两种事故：
            // 传 -1 变成 0xFFFFFFFF ≈ 49.7 天，正好把"带上限"想避免的事实上
            // 永久阻塞重新引回来；超过 2^32 的值则被静默截断成一个毫不相干的
            // 数字。这里夹到 5 分钟，越界就告警。
            // 0 是合法取值：不等，直接掐断在途连接的回调走人。
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
        // ⚠️ 在 IHttpConnectorHandler::OnLog 里调 Request() 是契约明令禁止的：
        // OnLog 是被 BCLogger 持着全局非递归自旋锁调进来的，而发起请求的路上
        // TcpChannel::Open() 一定会打日志，再进那把锁就是无诊断的 100% CPU
        // 空转挂死。这里当场拒绝，把挂死换成一个调用方看得见的错误码。
        // （注意：Close() 不打日志，在 OnLog 里调它是安全的。）
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

    // 池化开着的话请求头就不发 Connection: close —— 发了等于告诉服务端这条
    // 连接用完即弃，复用也就无从谈起。
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

    // 先看池里有没有现成的连接。取到的话本次请求省掉 TCP 握手 + TLS 握手
    // （实测 280ms -> 95ms）。取不到、或者取到的那条已经不能用了，就照常新建。
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
                // 关停途中还借出去就没人收得回来了，原地让它走回收路径。
                borrowed->Channel()->Close();
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

    // llhttp 不知道我们发的是什么方法，HEAD 响应必须显式告诉它"没有 body"，
    // 否则它会一直等 Content-Length 声明的那些字节（详见 http_on_headers_complete）
    bool isHead = (_ToUpperAscii(req.method.empty() ? std::string("GET")
                                                    : req.method) == "HEAD");
    BCRESULT r = conn->Start(cfg, reqText, req.timeoutMs, isHead, pool_enabled);

    if (r != BC_R_SUCCESS && borrowed)
    {
        // 捡来的那条连接在接手之前就失效了。服务端的 keep-alive 空闲超时是
        // 日常而不是异常，这个失败不该让调用方看见 —— 丢掉它，用一条新连接
        // 重来一次。不重试的话，复用带来的就是一串偶发、无从复现的请求失败。
        HTTP_LOGQ(logger_ctx_, _INFO_,
             "[HttpConnector] 池中连接已失效（result=%d），改用新连接重试：%s:%u",
             (int)r, parts.host.c_str(), (unsigned)parts.port);

        conn->DisownChannel();      // 通道仍归 borrowed 的看守，别跟着 conn 走
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
        // 让它走 PooledChannel 的正常回收路径（OnChannelClosed 里连通道带
        // 看守一起销毁）
        borrowed->Channel()->Close();
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
        // TcpChannel::Open 返回非成功时保证不会有任何回调，这条连接是惰性的，
        // 摘除之后销毁是安全的。
        //
        // ⚠️ 摘除与"判定要不要发 OnClosed"必须在同一个临界区里完成，否则与
        // _DetachAndArmClosed 注释里那条（TSan 抓到的）bug 完全同构：中间若有
        // 并发析构看到两个集合都空就返回、调用方释放 handler，我们随后就把
        // OnClosed 打在已释放的 handler 上（此时 abandoned 仍为 false、
        // state->handler 仍非空，一个都挡不住）。
        DispatchGuard          guard(state_);
        IHttpConnectorHandler* closedHandler =
            _DetachAndArmClosed(state_, conn, guard);
        // ⚠️ 必须在 delete conn **之前**取走：文案存在通道里，跟着 conn 一起没。
        const std::string openErr = conn->LastOpenError();
        if (outErrMessage) *outErrMessage = openErr;
        // 已经摘除，此后没人能再拿到它，锁外销毁安全（也不能在锁内销毁：
        // ~TcpChannel 有可能打日志，那就踩了"持锁禁止打日志"的不变量）
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

    // Start() 是在锁外跑的（Open 里有网卡探测，不该压着连接器的锁）。这段时间
    // 里 Close() 可能已经取过快照并对这条连接调过 Cancel() —— 那时通道还没建
    // 队列，Cancel 是空操作。所以补一次复核，否则这条请求会一直挂到超时才收场。
    //
    // ⚠️ 必须持锁访问 conn：Start() 成功之后回调随时可能到，conn 可能已经走完
    // OnChannelClosed 并被销毁。而"从 conns 摘除"严格早于销毁、且要拿同一把锁，
    // 所以"持锁 + 仍在 conns 里"就等价于"对象还活着"。
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
    // ⚠️ 只影响此后入池的连接。已经躺在池里的那些各自的定时器早就挂好了，
    // 要在这里重挂就得跨线程去碰它们的通道队列 —— 换来的收益远不值那份复杂度
    // （这本来就是一道兜底）。契约写在头文件里。
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
        // ⚠️ Cancel() 必须在锁内调：连接从 conns 摘除也要拿这把锁，而真正的
        // delete 严格排在摘除之后，所以"持锁 + 仍在 conns 里"就等价于"对象还
        // 活着"。先取快照再到锁外遍历的话，快照里的指针可能已经被销毁。
        //
        // Cancel() 只会走到 TcpChannel::Close()，那条路径不会回头来拿本锁
        // （OnChannelClosed 是从通道自己的事件循环线程上另外发出来的），
        // 因此锁内调用不存在死锁。
        //
        // ⚠️ 跨模块假设：这里依赖 **TcpChannel::Close() 不打日志**。它一旦开始
        // 打日志，本处就变成"持 state->lock 打日志"，与 BCLogger 的全局自旋锁
        // 形成锁序反转（见本文件顶部那段）。HTTP_LOGQ 的断言拦不住它 ——
        // 那是 TcpChannel 自己直接调的 LogQ。改动 TcpChannel::Close 时请复核这里。
        for (std::set<HttpConnection*>::iterator it = state_->conns.begin();
             it != state_->conns.end(); ++it)
        {
            (*it)->Cancel();
        }

        // 池里躺着的是真实的 socket，不关就是 fd 泄漏。这里只推一把关闭，
        // **不**从 idle 里摘 —— 摘除统一由 PooledChannel::OnChannelClosed 做，
        // 那样"idle 为空"才真的等于"池里的连接都回收干净了"，OnClosed 的判定
        // （见 _DetachAndArmClosed）也才站得住。
        //
        // 与上面 Cancel() 同理，这里依赖 TcpChannel::Close() 不打日志。
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
    // handler 由 sink->lock 保护：析构放弃等待时会把它置空，而那一刻残留连接
    // 可能正在别的线程上打日志。
    //
    // ⚠️ 本回调是被 BCLogger 持着它的**全局非递归自旋锁**调进来的。因此业务
    // 在 OnLog 里回调本模块任何会打日志的 API（Request / Close / 析构）都会
    // 重进那把锁而空转挂死 —— 这是 BC 日志器的结构约束，本模块修不了，只能
    // 写进头文件契约。
    std::lock_guard<std::mutex> lk(sink->lock);
    if (sink->handler)
    {
        LogCallbackScope scope;
        sink->handler->OnLog(level, msg);
    }
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
