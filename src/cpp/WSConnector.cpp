
///////////////////////////////////////////////////////////////////////////////
// file : WSConnector.cpp
// author : zhoukai88@jd.com（原始实现）/ anto（移植进 ttsignal，接 TcpChannel）
//
// 设计说明、线程模型与生命周期契约见 WSConnector.h 顶部注释。
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "WSConnector.h"

// PRI 宏必须走 <cinttypes>，不能用 <inttypes.h>。
//
// 老 glibc（Linux aarch64 交叉工具链 gcc 13.3 的 sysroot 就是）严格照 ISO C99
// 办事，PRI 系列被 `#if !defined __cplusplus || defined __STDC_FORMAT_MACROS`
// 挡住 —— C++ 里不显式请求就一个都不定义，于是
//     "[WSConnection#%" PRIu64 "] ..."
// 里的 PRIu64 是个裸标识符，报 expected ')' before 'PRIu64'。
// macOS 的 libc++ 和较新 glibc 无条件定义，所以只有 arm64 那一趟会挂。
// <cinttypes> 是标准解法：libstdc++ 内部自己 define/undef __STDC_FORMAT_MACROS，
// C++11 起保证 PRI 宏一定可用。
#include <cinttypes>        // for PRI macros
#include <openssl/rand.h>   // for RAND_bytes

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "BC/Utils.h"
#include "HttpConnector.h"  // 复用 ParseUrl（ws/wss 的 URL 语法与 http/https 相同）
#include "Runtime.h"
#include "Utils.h"


///////////////////////////////////////////////////////////////////////////////
// Namespace : WS
///////////////////////////////////////////////////////////////////////////////

namespace WS
{

///////////////////////////////////////////////////////////////////////////////
// File-scope helpers
///////////////////////////////////////////////////////////////////////////////

namespace {

///////////////////////////////////////////////////////////////////////////////
// 握手响应头的硬上限。
//
// 与 HttpConnector.cpp 顶部那组同样的理由：llhttp 自己**不带任何这类上限**，
// 一个恶意或故障的服务端可以靠无限重复响应头把内存撑爆，而握手阶段唯一会让它
// 停下来的是 connectTimeoutMs —— 在那之前内存已经没了。取值与 HTTP 侧一致。
///////////////////////////////////////////////////////////////////////////////
const size_t kMaxHeaderTotalBytes  = 64 * 1024;
const size_t kMaxSingleHeaderBytes = 16 * 1024;
const size_t kMaxHeaderCount       = 200;

// 定时器最小间隔。ping / 空闲检查合用一个 ticker，间隔取两者里较小的那个，
// 但不允许比这个还密 —— 否则一个 idleTimeoutMs=1 的配置就是空转 CPU。
const uint32_t kMinCheckIntervalMs = 500;

// WebSocket 关闭码。1000 = Normal Closure，1002 = Protocol Error，
// 1009 = Message Too Big（RFC 6455 7.4.1）。
const uint16_t kWSCloseNormal   = 1000;
const uint16_t kWSCloseProtocol = 1002;

///////////////////////////////////////////////////////////////////////////////
// 下面四个小工具与 HttpConnector.cpp 里的同名函数逐字相同。
//
// 没有抽成公共头文件，是因为它们在 HttpConnector.cpp 的匿名命名空间里（那份实现
// 已经过 review，本任务不改它）。真要合并应该单开一个 HttpText.h，那是另一次
// 独立的重构 —— 这里先复制，并把这条说明留在原地。
///////////////////////////////////////////////////////////////////////////////

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

// RFC 7230 的 token 字符集。头名必须是 token，否则就是注入。
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

// 头值里不许出现 CR / LF / NUL，否则调用方可以往报文里塞任意行
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

// "Connection: keep-alive, Upgrade" 这种逗号列表里找一个 token。
//
// jmp 原版是 headers["connection"] == "upgrade" 的全等比较，nginx / gorilla /
// 各家 CDN 回的 "Upgrade, keep-alive" 一律判失败 —— 也就是握手永远不成功。
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
        // trim
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

} // namespace

///////////////////////////////////////////////////////////////////////////////
// struct : WSConnectorState —— 连接器的共享状态，**比连接器本身活得久**
//
// 为什么必须独立出来（与 HttpConnectorState 同构，理由见那边的长注释）：析构
// 放弃等待时 WSConnector 会先于残留的 WSConnection 消失，而那些连接稍后还要
// 回来销账（从 conns 里摘除、通知 cv、判断要不要发 OnClosed）。只要这些字段还
// 长在 WSConnector 身上，任何"回来销账"的动作都是在碰已析构的对象。
//
// 与 HTTP 侧的一处不同：这里的 conns 存的是 **WSConnPtr（强引用）**，而不是裸
// 指针 + 单独的 destroying 集合。原因是 WSConnPtr 会交到业务手上，业务随时可能
// 放手，只有连接器一直持有强引用才能保证"通道还活着时对象不会被销毁"。
// 摘除时机因此严格定在"OnChannelClosed 之后、TcpChannel 已销毁"那一刻
// （WSConnection::OnChannelClosed 里那个 Runtime 任务），于是
// conns.empty() 就等价于"所有通道都收尾了"。
//
// ⚠️ 摘除动作与"连接持有 state 的 shared_ptr"之间构成一个**临时环**
// （state -> WSConnPtr -> state）。环在连接收尾时由那个 Runtime 任务打破。
// 若 Runtime 先于连接器 Destroy()，任务不会跑，环就留着 —— 那是头文件契约
// 第 6 条明写的错误销毁顺序，日志里会有对应的 _ERROR_。
//
// ---------------------------------------------------------------------------
// 锁
// ---------------------------------------------------------------------------
// 一把递归锁保护下面所有非 atomic 字段。业务回调一律在**锁外**发出，递归性
// 纯属保守（万一哪条路径被漏掉时不至于硬死锁）。
//
// ⚠️ **持这把锁时禁止打日志**。BC 的日志器带一把全局非递归自旋锁，且持着它回调
// 业务的 OnLog；持本锁打日志就构成 state->lock -> BCLogger 全局锁，与"业务在
// OnLog 里调 CreateConnection()"正好反向，两线程 100% 死锁。保障手段见下面
// StateLock / WS_LOGQ。
///////////////////////////////////////////////////////////////////////////////

struct WSLogSink
{
    // 保护 handler：放弃路径会把它置空，而那一刻别的线程可能正在打日志。
    // 普通 mutex 而非递归锁 —— 理由见 HttpConnector.h 里 HttpLogSink 的注释
    // （"在 OnLog 里析构连接器"那条路无论如何都挂，递归化只是换一种自锁）。
    std::mutex           lock;
    IWSConnectorHandler* handler = NULL;
};

struct WSConnectorState
{
    std::recursive_mutex        lock;
    std::condition_variable_any cv;

    // 还没走完收尾的连接。强引用，见上面的说明。
    std::map<uint64_t, WSConnPtr> conns;

    IWSConnectorHandler*        handler         = NULL;
    bool                        closing         = false;
    bool                        closed_notified = false;
    // 连接器已放弃等待。此后**任何** handler 都不再回调
    bool                        abandoned       = false;
    // 正在**锁外**执行的业务回调条数
    uint32_t                    callbacks_in_flight = 0;

    void*                       logger_ctx      = NULL;
    bool                        own_logger      = false;
    WSLogSink*                  log_sink        = NULL;

    // stats。只增不减，用 atomic 免得为了几个计数器去争那把锁。
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
        // 走到这里说明连接器与所有连接都已经撒手，没有任何人还握着 logger_ctx
        if (own_logger && logger_ctx)
        {
            void* ctx = logger_ctx;
            RemoveLogAppender(ctx);
        }
        delete log_sink;
    }
};

namespace {

///////////////////////////////////////////////////////////////////////////////
// state->lock 的 RAII 包装 + "持锁时禁止打日志"的不变量
//
// 与 HttpConnector.cpp 顶部那一整段是同一件事，理由不重复抄；这里只记两条可以
// 直接 grep 验证的硬规则：
//
//   1. **本文件里一律用 WS_LOGQ，不准直接写 LogQ。**
//        grep -n 'LogQ(' src/cpp/WSConnector.cpp
//      结果里除了 WS_LOGQ 的宏定义本身，不该出现裸的 LogQ。
//   2. 所有对 state->lock 的加锁一律走 StateLock，由它维护 thread_local 深度。
//        grep -n 'recursive_mutex> *lk' src/cpp/WSConnector.cpp
//      应当只在 StateLock 与 DispatchGuard 内部出现。
//
// WS_LOGQ 在持锁时 assert 失败（Debug 构建当场炸出调用栈）；Release 构建则丢掉
// 这条日志而不是去踩死锁 —— 少一条日志远好过挂死。
///////////////////////////////////////////////////////////////////////////////

thread_local uint32_t t_state_lock_depth = 0;
// 本线程正在 IWSConnectorHandler::OnLog 的栈帧里（被 BCLogger 持着全局非递归
// 自旋锁调进来的），此时本模块再打日志就是重进那把锁
thread_local uint32_t t_in_log_callback  = 0;
// 本线程正在派发哪些 state 的业务回调
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
    // 供 condition_variable_any 使用。wait 期间互斥量真的被放开了，而
    // t_state_lock_depth 仍然 > 0 —— 刻意的保守偏差，等待期间本来也不该打日志。
    std::unique_lock<std::recursive_mutex>& Raw() { return lk_; }

private:
    DECLARE_NO_COPY_CLASS(StateLock);
    std::unique_lock<std::recursive_mutex> lk_;
};

// ⚠️ 本文件里打日志只准用这个宏，理由见上。
#define WS_LOGQ(...)                                                           \
    do {                                                                       \
        if (t_in_log_callback != 0)                                            \
        {                                                                      \
            /* 在 OnLog 回调栈里：BCLogger 的全局自旋锁正被本线程持有，       \
               再打一条就是重进非递归锁而空转挂死。直接丢掉。 */              \
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

///////////////////////////////////////////////////////////////////////////////
// 回调派发：锁内登记、锁外执行、回来注销。
//
// 登记时 callbacks_in_flight++（在锁内，与放弃路径互斥）；放弃路径清空 handler
// 之后**等 callbacks_in_flight 归零**才返回，所以调用方拿回控制权时不可能还有
// 回调正握着它的 handler；而回调本身在锁外执行，本模块的锁不再出现在任何业务
// 栈帧的下方。
///////////////////////////////////////////////////////////////////////////////

class DispatchGuard
{
public:
    explicit DispatchGuard(const std::shared_ptr<WSConnectorState>& s)
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
    std::shared_ptr<WSConnectorState> state_;
    bool                              armed_ = false;
};

// 全部排空且已请求关闭时发一次 OnClosed。放弃等待之后不再发。
//
// ⚠️ state 按**值**传：业务完全可以在 OnClosed 里析构连接器，那会释放连接器
// 那一份引用；如果这里收的是引用（调用方传的往往正是成员 state_），引用在回调
// 返回后就悬垂了，而我们还要用它注销派发计数。
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

// 一条连接彻底收尾（OnChannelClosed 已发、TcpChannel 已销毁）之后的销账。
//
// ⚠️ 摘除与"登记 OnClosed 在途"必须在**同一个临界区**里完成。分成两段的话中间
// 有一个窗口：析构那边已经看到 conns 空了（于是返回、调用方随即释放它的
// handler），我们才刚要去派发 OnClosed —— 打在已经析构的 handler 上。
// 这条 bug 在 HTTP 侧被 TSan 实测抓到过（ASan 看不见，因为是栈地址复用）。
void _RemoveConnAndMaybeNotify(std::shared_ptr<WSConnectorState> state, uint64_t id)
{
    if (!state)
    {
        return;
    }
    // ⚠️ 摘出来的强引用留到锁外再释放：~WSConnection 会碰到别的东西（例如
    // dump 文件），不该在持锁时跑。
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
            guard.ArmLocked();      // 与 erase 同一临界区，析构看得见
        }
        // ⚠️ 一定要 notify：否则析构可能白等满 drainTimeoutMs
        state->cv.notify_all();
    }
    gone.reset();
    if (handler)
    {
        handler->OnClosed();
    }
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// class : WSConnection::Config
///////////////////////////////////////////////////////////////////////////////

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
        // ⚠️ 夹在 UINT32_MAX：WSParser::_RequireData 的形参是 uint32_t，上限超过
        // 4 GiB 之后那次强转就是静默截断，解析状态机行为不可预测。越界值记进
        // bad_max_frame_bytes，由 WSConnector::Create / CreateConnection 打告警
        // （这里是纯配置解析，没有 logger ctx，也不该在这一层打日志）。
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
    // TLS
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
    // 双向 TLS。键名从 jmp 的 certificate_file / private_key_file /
    // private_key_password 改成 TlsConfig 的字段名，语义不变。
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
    // DNS
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
        // 逗号分隔的写法，命令行工具用着方便
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

///////////////////////////////////////////////////////////////////////////////
// class : WSConnection —— 纯函数部分
///////////////////////////////////////////////////////////////////////////////

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

    // ------------------------------------------------------------------
    // 剩下的部分直接转交 HttpConnector::ParseUrl。
    //
    // RFC 6455 3 定义的 ws-URI / wss-URI 语法就是照抄 http-URI，只换了 scheme 名
    // 与默认端口（80 / 443，与 http / https 一致），所以把 scheme 换掉之后两者
    // 逐字相同。这样做而不是再写一份解析，是为了不让"IPv6 方括号、userinfo 拒绝、
    // 端口范围、#fragment 剥离"这些边界各自演化 —— 那份实现有单测钉住。
    //
    // ⚠️ 绝不能用 HTTPProtocol::ParseAddrFromUrl：它内部同步调 DNSGetAddrInfo，
    // 既阻塞调用线程，又绕开了 DnsResolver 那条不走 VPN 的路径（TUN 全局模式下
    // 会拿到 fake-IP）。
    // ------------------------------------------------------------------
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
        // 这几个由本函数权威决定，业务给的同名项一律忽略 —— 让调用方能覆盖
        // Sec-WebSocket-Key 或 Host 等于把握手校验的钥匙交出去。
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
    // 键统一小写；值也转小写，但 sec-websocket-accept 保持原样（base64 大小写敏感）
    HttpHeaderMap lower = GetLowerCaseHeaders(headers);

    HttpHeaderMap::const_iterator itUpgrade = lower.find("upgrade");
    if (itUpgrade == lower.end() || itUpgrade->second != "websocket")
    {
        outErr = "握手响应的 Upgrade 头不是 websocket";
        return false;
    }
    HttpHeaderMap::const_iterator itConn = lower.find("connection");
    // "Connection: keep-alive, Upgrade" 是合法的，必须按 token 列表匹配
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

///////////////////////////////////////////////////////////////////////////////
// class : WSConnection
//
// 继承 LLHTTPParser 解析握手响应，继承 WSParser 解析 / 组装帧，继承
// ITcpChannelHandler 接 TcpChannel 的三个回调。
//
// ⚠️ 生命周期（照抄 TcpChannel.h 的契约再往上叠一层）：
//   * Connect() 返回非 BC_R_SUCCESS 时不会有任何回调；
//   * 返回成功之后，唯一合法的销毁 TcpChannel 的时机是 OnChannelClosed 之后，
//     而且不能在该回调里同步 delete —— 那时通道那条线程还停在
//     OnEventProcShutdown() 的栈帧里。所以统一 PostTask 到 Runtime 上去 delete。
//   * 连接器一直持有本对象的强引用，直到那个 Runtime 任务跑完为止，所以业务
//     什么时候丢掉自己那份 WSConnPtr 都不会踩到"销毁一条还活着的通道"。
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(WSConnection, 32);

WSConnection::WSConnection()
    // ⚠️ HTTP_RESPONSE 而不是默认的 HTTP_BOTH：WebSocket 客户端只可能收到响应。
    // 留在 HTTP_BOTH 下，服务端回一段 "GET /evil HTTP/1.1\r\n..." 会被当成一条
    // 合法报文收下（status_code 为 0），虽然随后 CheckHandshakeResponse 一定会
    // 判失败，但错误信息会变成"期望 101，实际 0"这种误导性的描述。
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
    //
}

WSConnection::~WSConnection()
{
    // channel_ 非空只有一种情况：从没 Open 过（或 Open 失败）。按 TcpChannel.h
    // 契约第 1 条，那时对象可以立即销毁。Open 成功过的通道一律由
    // OnChannelClosed 里那个 Runtime 任务销毁，走到这里时 channel_ 已经是空的。
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
        // ⚠️ 在 IWSConnectorHandler::OnLog 里发起连接是契约明令禁止的：OnLog 是被
        // BCLogger 持着全局非递归自旋锁调进来的，而 TcpChannel::Open() 一定会打
        // 日志，再进那把锁就是无诊断的 100% CPU 空转挂死。这里当场拒绝，把挂死
        // 换成一个调用方看得见的错误码。
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

    // ⚠️ 所有自身状态必须在 Open() 之前设好：Open() 内部 PostTask 之后，另一条
    // worker 线程可以立刻派发回调，OnChannelClosed 甚至可以先于 Open() 返回。
    timeout_ms_           = cfg.connectTimeoutMs;
    start_ms_             = _NowMs();
    latest_net_action_ms_ = start_ms_;
    channel_opened_       = true;

    // ⚠️ Open() 会打日志（还会做网卡探测），所以**不能**压着 state->lock 调。
    // 这一步之前没有任何回调，channel_ 不会被并发改动，可以直接取。
    TcpChannel* ch = channel_;
    BCRESULT    r  = ch->Open(cfg, this);
    if (r != BC_R_SUCCESS)
    {
        // Open 返回非成功时保证不会有任何回调，成员仍然只有本线程在碰
        channel_opened_ = false;
        connect_called_ = false;
        // 同步失败走不到 _DeliverOnce，现场描述得在这里补进 LastErrorMessage()
        // —— 否则 force-physical 拿不到网卡时业务只看得到一个 64。
        _SetLastErrorMessage(ch->LastOpenError());
        WS_LOGQ(logger_ctx_, _ERROR_,
                "[WSConnection#%" PRIu64 "] 连接发起失败 result=%d（%s:%u）：%s",
                id_, (int)r, parts_.host.c_str(), (unsigned)parts_.port,
                ch->LastOpenError().c_str());
        return r;
    }

    // Open() 是在锁外跑的，这段时间里连接器可能已经 Close() 过并遍历完了 conns
    // （那时本连接的通道还没建队列，Cancel 是空操作）。补一次复核，否则这条连接
    // 会一直挂到超时才收场。
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
    // SMPacket 没有 ws_type 字段，统一按 binary 帧发（见 WSParser::PackPacket）
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
        // 尽力发一个 close 帧再断。Send 与 Close 都是投递到通道自己那条事件
        // 队列上的，FIFO，所以 close 帧一定排在 FIN 之前。
        _SendCloseFrame(kWSCloseNormal);
    }
    StateLock lk(*state_);
    if (channel_)
    {
        // ⚠️ 跨模块假设：**TcpChannel::Close() 不打日志**。它一旦开始打日志，
        // 这里就变成"持 state->lock 打日志"，与 BCLogger 的全局自旋锁形成锁序
        // 反转（见本文件顶部那段）。WS_LOGQ 的断言拦不住它 —— 那是 TcpChannel
        // 自己直接调的 LogQ。改动 TcpChannel::Close 时请复核这里。
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
    // ⚠️ 锁内只做一次赋值，**不打日志**（持 state->lock 打日志会与 BCLogger 的
    // 全局非递归自旋锁构成锁序反转，见本文件顶部那段）。
    StateLock lk(*state_);
    last_error_message_ = message;
}

///////////////////////////////////////////////////////////////////////////////
// class : WSConnection —— ITcpChannelHandler
//
// 三个回调全部跑在通道自己的事件循环线程上。**在这条线程上 channel_ 一定非空**
// （只有 OnChannelClosed 会把它置空，而那是通道发出的最后一个回调），所以这些
// 函数以及它们调起的定时器回调里可以直接用 channel_，不必加锁。
// 从**其它线程**（业务线程）碰 channel_ 的地方一律要走 StateLock。
///////////////////////////////////////////////////////////////////////////////

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

    // 通道自己的 connectTimeoutMs 只盖到 TLS 握手为止，进 READY 就取消了。
    // 剩下的"请求发出去了但服务端一直不回 101"必须由这里补上，否则一个黑洞
    // 服务端能把调用方吊死。定时器挂在通道自己的事件队列上：回调线程与
    // OnChannelData / OnChannelClosed 完全一致，不需要额外加锁；通道收尾时
    // Detach() 会顺手把它取消掉。
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
        // 定时器建不起来就没有任何东西能兜住"服务端不回 101"了。与其让调用方
        // 永久挂着，不如当场失败。
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
        return;     // 已经收尾，多出来的字节直接丢掉
    }

    if (upgraded_.load())
    {
        _FeedFrames(data, size);
        return;
    }

    // ------------------------------------------------------------------
    // 还在握手：把字节喂给 llhttp。
    //
    // 101 响应会让 llhttp 置 upgrade 标志并以 HPE_PAUSED_UPGRADE 停下来，
    // 停下的位置就是"响应头之后的第一个字节"—— 那之后的内容已经是 WS 帧了，
    // 必须接着喂给 ParseWSFrame（服务端完全可以把 101 和第一帧粘在一个包里）。
    // ------------------------------------------------------------------
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
                // llhttp 认为这是一次协议升级，但我们的握手校验没通过 ——
                // http_on_headers_complete 已经记好原因了。
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
            // 握手校验失败时我们从回调里返回 -1，llhttp 会以 HPE_CB_* 报错。
            // 原因已经记过，别覆盖成"解析失败"。
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
        // 提前把诊断信息拷下来 —— 通道马上就要销毁了
        if (ch)
        {
            peer_ip_        = ch->PeerIp();
            bound_ifindex_  = ch->BoundIfIndex();
            pin_method_     = ch->PinMethod();
        }
        channel_ = NULL;
    }

    // ⚠️ 只有走到这里销毁通道才安全，而且不能在本回调里同步 delete —— 通道那条
    // 线程此刻还停在 OnEventProcShutdown() 的栈帧里。挪到 Runtime 的队列上。
    //
    // ⚠️ lambda 里只捕获裸通道指针 + 共享状态 + id，**不捕获 this**：那个任务
    // 里的 _RemoveConnAndMaybeNotify 会释放连接器持有的最后一份强引用，本对象
    // 很可能就在那一刻析构。捕获 this 的话，析构之后还要用它就是 UAF。
    std::shared_ptr<WSConnectorState> state = state_;
    const uint64_t                    id    = id_;
    Runtime::PostTask([ch, state, id]() {
        delete ch;
        _RemoveConnAndMaybeNotify(state, id);
    });
    // ⚠️ 到这里为止不要再碰任何成员：上面那个任务随时可能跑起来并销毁本对象。
}

///////////////////////////////////////////////////////////////////////////////
// class : WSConnection —— WSParser 出入口
///////////////////////////////////////////////////////////////////////////////

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
            // IWSConnectionHandler::OnRecvText 收的是 NUL 结尾的 C 字符串
            // （签名与 jmp 一致，绑定层可以直接 NewStringUTF），所以这里必须
            // 拷一份并补 NUL —— WS 帧的 payload 本身不带结束符。
            // ⚠️ 文本帧里内嵌 NUL 会造成截断，契约里写明了。
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
        // 一次 RTT 采样。last_ping_ms_ 只在通道那条线程上读写。
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
            // 对端发起关闭握手：回一个同码的 close 帧，然后关通道。
            //
            // payload_len 只可能是 0 或 >= 2 —— RFC 6455 5.5.1 不允许长度 1，
            // WSParser 在帧头阶段就把它判成协议违规了（走 _FeedFrames 的异常
            // 路径回 1002）。所以下面"取不到码就用 1000"只会命中真正的空 body。
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
            // ⚠️ 这里直接用 channel_ 是安全的：本函数跑在通道的事件循环线程上。
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

///////////////////////////////////////////////////////////////////////////////
// class : WSConnection —— LLHTTPParser（同样跑在通道的事件循环线程上）
///////////////////////////////////////////////////////////////////////////////

int WSConnection::http_on_url(const char* at, size_t length)
{
    UNUSED(at);
    UNUSED(length);
    return 0;       // 客户端只解析响应，不会有 request-line
}

int WSConnection::http_on_status(const char* at, size_t length)
{
    UNUSED(at);
    // reason-phrase 我们不用，但长度要计入预算 —— 否则服务端发一条永不结束的
    // reason 就能撑爆内存（HttpConnector.cpp 顶部那段有实测数据）。
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
    // llhttp 2.0.4 没有 llhttp_get_status_code()，status_code 是结构体的公开字段
    hs_status_ = (int)this->status_code;

    std::string err;
    if (!CheckHandshakeResponse(hs_status_, hs_headers_, sec_key_, err))
    {
        state_->handshake_failed++;
        // 统一用 BC_R_WS_HANDSHAKE_FAILED —— "服务端回了 4xx"和"这压根不是个
        // WebSocket 端点"都属于握手没成功，具体原因在 err 里（会随
        // OnConnectResult 之前的那条 _ERROR_ 日志一起落盘）。
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

    // ping / 空闲检查合用一个 ticker，握手成功之后才有意义
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
        // 从"现在"开始算间隔。不置的话 last_ping_sent_ms_ 是 0，第一次 tick
        // （可能只隔了 idleTimeoutMs）就会立刻发一个 ping。
        last_ping_sent_ms_    = _NowMs();
        latest_net_action_ms_ = last_ping_sent_ms_;
        BCRESULT sr = channel_->ScheduleTask(check_timer_, [this](int32_t) {
            _OnActiveCheck();
        }, (uint64_t)interval * 1000, true);
        if (sr != BC_R_SUCCESS)
        {
            // 不致命：只是没有 ping / 空闲检查了，连接本身照常可用。
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

///////////////////////////////////////////////////////////////////////////////
// class : WSConnection —— 内部
///////////////////////////////////////////////////////////////////////////////

void WSConnection::_FeedFrames(const void* data, size_t size)
{
    // ParseWSFrame 在遇到协议违规时 throw BCException（RSV 位、保留 opcode、
    // 超大帧、分片状态机违规……）。这里是唯一的捕获点：转成 OnException +
    // 关连接，绝不能让异常穿过 TcpChannel 的事件循环。
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
        // 先把 OnException 发出去（锁外），再走关闭流程
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
        // 同名头按 RFC 7230 3.2.2 以逗号合并
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

    // ⚠️ 持锁读 channel_：本函数可以从任意业务线程被调到，而 OnChannelClosed
    // 会在通道那条线程上把 channel_ 摘走。TcpChannel::Send 自身不打日志，
    // 所以锁内调用不违反"持锁禁止打日志"的不变量（同 Close() 那条注释）。
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
        // 握手没完成之前 WS 帧发不出去（对端还在等/正在回 101）
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
    // ⚠️ RFC 6455 5.1：**客户端发出的每一帧都必须掩码**。jmp 原版走的是
    // PackPacket(pkt) 的 has_mask 默认值 false，合规服务端会以 1002 关连接。
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
        // 对端 ping 的 payload 必须原样回显（RFC 6455 5.5.3）。长度已由解析侧
        // 限在 125 字节以内。
        buf->Write(payload, (uint32_t)payload_len);
    }
    _SendFrame(WS_OP_PONG, buf);
}

void WSConnection::_SendCloseFrame(uint16_t code)
{
    bool expected = false;
    if (!close_frame_sent_.compare_exchange_strong(expected, true))
    {
        return;     // 一条连接只发一个 close 帧
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
    // ⚠️ 本函数只在通道那条线程上被调用（TcpChannel 回调 / 挂在通道上的定时器），
    // 那里 channel_ 一定非空。仍然判一次空，免得将来有人从别处调过来。
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
        // ⚠️ 锁内登记、**锁外**执行。放弃路径清空 handler 之后会等
        // callbacks_in_flight 归零才返回，所以调用方拿回控制权时不可能还有回调
        // 握着它的 handler；而回调本身跑在锁外，本模块的锁不会出现在业务栈帧
        // 的下方（否则业务打个日志、取个自己的锁都可能死锁）。
        StateLock lk(*state_);
        if (!handler_)
        {
            return;     // 连接器已放弃等待，或本就没有 handler
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

    // 通道 Detach 时已经统一取消过所有定时器，这里只是把 id 归位，失败无所谓
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
        // 握手还没成功过 —— 这次收尾就是"连接失败"。按契约此时**只发
        // OnConnectResult，不发 OnClosed**。
        if (result == BC_R_SUCCESS)
        {
            result  = BC_R_UNEXPECTEDEND;
            message = "连接在 WebSocket 握手完成前被对端关闭";
        }
        // ⚠️ 必须排在 _DeliverConnectResult **之前**：OnConnectResult 的签名里没有
        // message 形参，绑定层只能在回调里回头调 LastErrorMessage() 取这段文案。
        // 顺序反了的话，业务拿到的永远是空串（然后只剩一个错误码可看）。
        _SetLastErrorMessage(message);
        WS_LOGQ(logger_ctx_, _ERROR_,
                "[WSConnection#%" PRIu64 "] 连接失败 result=%d: %s",
                id_, (int)result, message.c_str());
        _DeliverConnectResult(result);
        return;
    }

    // 握手成功过的连接：OnClosed 收的是 _CloseReasonText 归纳过的短句，原始文案
    // 同样留一份给 LastErrorMessage()（"连上之后为什么断"也要可诊断）。
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
    // force-physical 那三个码必须能单独看到 —— 它们是"绑网卡失败"与"网络不通"
    // 的分界线，混进 unknown 里就查不出来了。
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
    // 调用方（WSConnector::Close / 析构）已持有 state->lock。
    if (!channel_opened_)
    {
        // 从没 Open 过（或 Open 失败）：不会有任何回调，也就永远不会有
        // OnChannelClosed 来把它摘掉。直接告诉调用方"这条可以就地摘除"，
        // 否则析构会白等满 drainTimeoutMs。
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
    // 调用方必须已持有 state->lock。与各处读 handler_ 用的是同一把锁，
    // 所以"清空"与"读出来准备回调"之间不存在窗口。
    handler_ = NULL;
}

///////////////////////////////////////////////////////////////////////////////
// class : WSConnector::Config
///////////////////////////////////////////////////////////////////////////////

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

    // --------------------------------------------------------------
    // vpnPolicy（新键）与 bypassVpn（旧键，已废弃）合并，读法与
    // SMPConnector::Config::Init / HttpConnector::Create 完全一致。
    // ⚠️ 解析必须走 tt_vpn_policy_resolve，不要自己重写优先级。
    // --------------------------------------------------------------
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

    // --------------------------------------------------------------
    // 日志
    // --------------------------------------------------------------
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

    // --------------------------------------------------------------
    // 析构排空上限
    // --------------------------------------------------------------
    pVar = pConfig->Get("drainTimeoutMs");
    if (IS_BCF_NUMBER(pVar))
    {
        // ⚠️ GET_BCF_INT 返回 uint64_t，直接窄化成 uint32_t 会出两种事故：
        // 传 -1 变成 0xFFFFFFFF ≈ 49.7 天，正好把"带上限"想避免的事实上永久
        // 阻塞重新引回来；超过 2^32 的值则被静默截断成一个毫不相干的数字。
        // 这里夹到 5 分钟，越界就告警。0 是合法取值。
        const uint64_t kMaxDrainMs = 5 * 60 * 1000;
        uint64_t       v           = GET_BCF_INT(pVar);
        if (v > kMaxDrainMs)
        {
            bad_drain_timeout = v;
            v                 = kMaxDrainMs;
        }
        drain_timeout_ms = (uint32_t)v;
    }

    // --------------------------------------------------------------
    // 单条连接的默认配置（同一个 BCFObject 里读）
    // --------------------------------------------------------------
    conn.Init(pConfig);
    return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : WSConnector
///////////////////////////////////////////////////////////////////////////////

WSConnector::WSConnector()
    : state_(std::make_shared<WSConnectorState>())
{
    //
}

WSConnector::~WSConnector()
{
    Close();

    // 等到所有在途连接**彻底收尾**为止：OnChannelClosed 已发、TcpChannel 已被
    // Runtime 任务销毁、连接已从 conns 里摘除。
    //
    // ⚠️ 这个等待带上限（drainTimeoutMs，默认 30 秒），不是无限等。原因见头文件
    // 契约第 6 条：Runtime::PostTask 在 Runtime 已经 Destroy() 之后会被**静默
    // 丢弃**（Runtime.cpp:129 的 if (s_pInstance)），调用方若把销毁顺序写反，
    // 那些销毁任务永远不会跑，无限等就是永久死锁。
    //
    // ⚠️ 自检：业务在回调栈里析构连接器（头文件契约第 8 条 (4)）。那条连接要等
    // 本回调返回之后才会收尾，排空谓词在本次析构期间**注定不可能成立** ——
    // 只会白等满 drainTimeoutMs，最后还是走放弃语义。既然结局注定，就别白等。
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
        // ⚠️ 谓词必须**同时**要求"没有在途回调"。只看 conns 是不够的：最后一条
        // 连接销账时会在同一个临界区里把 OnClosed 登记为在途，若析构在那之后
        // 只检查集合就返回，调用方随即释放 handler，而 OnClosed 才刚要打出去。
        // 本线程自己那几层要排除在外，否则自等。
        const size_t mine = _DispatchDepthOn(state_.get());
        drained = state_->cv.wait_for(
            lk.Raw(), std::chrono::milliseconds(waitMs),
            [this, mine] {
                return state_->conns.empty()
                    && state_->callbacks_in_flight <= mine;
            });
        if (!drained)
        {
            // 放弃等待。共享状态本身会被残留连接与销毁任务继续持有，所以它们
            // 照常销账、照常回收 appender，**不会碰任何已析构的东西**。
            // 这里只需要把所有回调掐断：调用方一旦从析构返回，就可以释放它的
            // handler 了，之后任何回调都是 UAF。
            stranded          = state_->conns.size();
            state_->abandoned = true;
            state_->handler   = NULL;   // 不再发 OnClosed
            for (std::map<uint64_t, WSConnPtr>::iterator it = state_->conns.begin();
                 it != state_->conns.end(); ++it)
            {
                // 与各处读 handler_ 用的是同一把锁，没有中间窗口
                it->second->ClearHandlerLocked();
            }

            // ⚠️ 清空 handler 只挡住"还没开始的回调"。已经登记、正在锁外执行的
            // 那些还握着调用方的 handler 指针，必须等它们跑完才能让析构返回。
            state_->cv.wait(lk.Raw(), [this, mine] {
                return state_->callbacks_in_flight <= mine;
            });
        }
    }
    if (!drained && state_->log_sink)
    {
        // 日志回调也要断：它指向调用方的 IWSConnectorHandler。appender 本身留着
        // 不动 —— 残留连接和它们的 TcpChannel 还握着 logger_ctx 在打日志，
        // 等最后一个持有者撒手时由 ~WSConnectorState 统一回收。
        //
        // ⚠️ 刻意放在 state_->lock 的作用域**之外**：唯有如此，
        // state->lock -> sink->lock 这个嵌套才彻底不存在，业务在 OnLog 里回头调
        // Close()（sink->lock -> state->lock）也就不构成锁序反转。
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
    // state_ 在这里释放本对象那一份引用；appender 由 ~WSConnectorState 回收
}

BCRESULT WSConnector::Create(BCFObject* pConfig, IWSConnectorHandler* pHandler)
{
    if (created_)
    {
        return BC_R_ALREADYRUNNING;
    }

    // TcpChannel / DnsResolver 都要用 Runtime 的线程池。
    // ⚠️ Runtime::Config::Init 会无条件解引用 pConfig（Runtime.h:52），传 NULL
    // 直接 SEGV，所以这里补一个空对象。Initialize 幂等，重复调用只认第一次的
    // 线程数配置。
    BCFObject emptyCfg;
    BCRESULT  rtResult = Runtime::Initialize(pConfig ? pConfig : &emptyCfg);
    if (rtResult != BC_R_SUCCESS)
    {
        return rtResult;
    }
    config_.Init(pConfig);
    state_->handler = pHandler;

    // --------------------------------------------------------------
    // 日志。必须最先装好，后面的告警才有地方落。
    // --------------------------------------------------------------
    if (!config_.log_file.empty())
    {
        state_->logger_ctx = AddFileLogAppender(config_.log_file.c_str(),
                                                config_.log_level, true, true);
        state_->own_logger = (state_->logger_ctx != NULL);
    }
    else if (state_->handler)
    {
        // 交给 appender 的是 WSLogSink 而不是 this：连接器一析构，直接交进去的
        // 指针就是野的。所有权在共享状态上，由 ~WSConnectorState 回收。
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
        // 清掉，免得每建一条连接就重复告警一次（这份 conn 配置会被拷进每条连接）
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
        // 在 OnLog 里建连接是契约明令禁止的，理由同 WSConnection::Connect
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
        // ⚠️ Create 里不打日志、不发 IO，锁内调用是安全的。放在锁内是为了让
        // "插进 conns" 与 "对象初始化完成" 之间没有窗口 —— Close() 可能在别的
        // 线程上遍历 conns。
        if (pConn->Create(state_, pConfig, config_.conn, id, pHandler)
            != BC_R_SUCCESS)
        {
            return WSConnPtr();
        }
        state_->conns[id] = pConn;
        state_->total_allocated++;
    }
    // ⚠️ 在锁外打：CreateConnection 的锁内段禁止打日志（见文件顶部）。
    // 这里报的是**本次 CreateConnection 的 pConfig** 里越界的 maxFrameBytes；
    // 连接器级默认值的越界已经在 Create() 里报过并清零了。
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
    // ⚠️ **刻意不加 state->lock**：AddFileLogAppender 内部要拿 BCLogger 的全局
    // 非递归自旋锁（BCLog.cpp:519 的 AddLogLocation），持 state->lock 调它就是
    // 本文件顶部禁止的 state->lock -> BCLogger 全局锁 那个方向，与"业务在 OnLog
    // 里调 Close()"正好反向。
    // 本函数按契约只在初始化阶段（Create 之后、第一次 CreateConnection 之前）
    // 调一次，那时还没有任何并发，不加锁是安全的。
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
    // 摘出来的强引用留到锁外释放：~WSConnection 不该在持锁时跑
    std::vector<WSConnPtr> dead;
    {
        StateLock lk(*state_);
        state_->closing = true;
        // ⚠️ Cancel 必须在锁内调：连接从 conns 摘除也要拿这把锁，而真正的销毁
        // 严格排在摘除之后，所以"持锁 + 仍在 conns 里"就等价于"对象还活着"。
        // 先取快照再到锁外遍历的话，快照里的对象可能已经收尾。
        //
        // _CancelLocked() 只会走到 TcpChannel::Close()，那条路径不会回头来拿本锁
        // （OnChannelClosed 是从通道自己的事件循环线程上另外发出来的），
        // 因此锁内调用不存在死锁。跨模块假设见 WSConnection::Close 里的注释。
        std::map<uint64_t, WSConnPtr>::iterator it = state_->conns.begin();
        while (it != state_->conns.end())
        {
            if (it->second->_CancelLocked())
            {
                // 从没 Open 过的连接不会有 OnChannelClosed 来摘它，就地摘除，
                // 否则析构会白等满 drainTimeoutMs。
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
    // force-physical 相关：绑不到物理网卡 / pin 失败 / 路由复核不通过
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
    // handler 由 sink->lock 保护：析构放弃等待时会把它置空，而那一刻残留连接
    // 可能正在别的线程上打日志。
    //
    // ⚠️ 本回调是被 BCLogger 持着它的**全局非递归自旋锁**调进来的。因此业务在
    // OnLog 里回调本模块任何会打日志的 API（Create / CreateConnection / Connect /
    // 析构）都会重进那把锁而空转挂死 —— 这是 BC 日志器的结构约束，本模块修不了，
    // 只能写进头文件契约，并在入口处当场拒绝（见 Connect / CreateConnection）。
    std::lock_guard<std::mutex> lk(sink->lock);
    if (sink->handler)
    {
        LogCallbackScope scope;
        sink->handler->OnLog(level, msg);
    }
}

///////////////////////////////////////////////////////////////////////////////
// End of namespace : WS
///////////////////////////////////////////////////////////////////////////////

} // End of namespace : WS

///////////////////////////////////////////////////////////////////////////////
// End of file : WSConnector.cpp
///////////////////////////////////////////////////////////////////////////////
