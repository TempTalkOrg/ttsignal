///////////////////////////////////////////////////////////////////////////////
// file   : TcpChannel.cpp
// author : anto
//
// TCP + TLS 连接底座的实现。设计说明与生命周期契约见 TcpChannel.h 顶部注释。
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "TcpChannel.h"

#include "INetworkPathMonitor.h"
#include "NetworkRouteLookup.h"
#include "Runtime.h"
#include "SocketPinner.h"
#include "TTErrors.h"
#include "Utils.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

///////////////////////////////////////////////////////////////////////////////
// File-scope helpers
///////////////////////////////////////////////////////////////////////////////

namespace {

// 把 BCSockAddrS 里的地址部分（不含端口）转成可读文本。
std::string _AddrToIpText(const BCSockAddrS& sa)
{
    char buf[INET6_ADDRSTRLEN] = { 0 };

    if (sa.type.sa.sa_family == AF_INET6)
    {
        inet_ntop(AF_INET6, (void*)&sa.type.sin6.sin6_addr, buf, sizeof(buf));
    }
    else if (sa.type.sa.sa_family == AF_INET)
    {
        inet_ntop(AF_INET, (void*)&sa.type.sin.sin_addr, buf, sizeof(buf));
    }
    return std::string(buf);
}

const char* _StateName(TcpChannel::State s)
{
    switch (s)
    {
    case TcpChannel::TCPCH_IDLE:          return "IDLE";
    case TcpChannel::TCPCH_RESOLVING:     return "RESOLVING";
    case TcpChannel::TCPCH_PINNING:       return "PINNING";
    case TcpChannel::TCPCH_CONNECTING:    return "CONNECTING";
    case TcpChannel::TCPCH_TLS_HANDSHAKE: return "TLS_HANDSHAKE";
    case TcpChannel::TCPCH_READY:         return "READY";
    case TcpChannel::TCPCH_CLOSING:       return "CLOSING";
    case TcpChannel::TCPCH_CLOSED:        return "CLOSED";
    default:                              return "UNKNOWN";
    }
}

}   // namespace

///////////////////////////////////////////////////////////////////////////////
// DnsExecutor —— 阻塞式 DNS 的专属线程池
//
// 为什么必须专门开一组线程，三条路都堵死了：
//
//   * 不能用 Runtime::PostTask。Runtime 自身的事件队列建在 m_pTaskMgr 上，
//     而 workerThreads 默认是 1（Runtime.h:26）；更要命的是
//     SMPServer.cpp:958 把同一个 BCTaskMgr 交给 UDPSender::Create，后者在
//     UDPSender.cpp:199 用它又建了一个 BCEventQueue。BCTaskMgr 只有 1 个
//     worker 时，它服务的所有 BCTask 共用那一条线程——一次 4 秒的阻塞 DNS
//     会把 SMP 服务端整条 UDP 数据报处理流水线一起停住。
//
//   * 不能用 TcpChannel 自己的队列。BCDelayTask 的到期回调也是投到同一个
//     BCTask 上的，阻塞 DNS 会把本连接的超时定时器一并哑掉，而"超时覆盖
//     DNS 段"正是那个定时器存在的意义。
//
//   * 不能直接往 BCTaskMgr 上投任务。它的公开接口只有 Create / Destroy /
//     GetTaskCount，根本没有任务投递 API；能投递的最小单位是 BCEventQueue。
//
// 于是：自建一个 BCTaskMgr（kWorkers 条线程）+ kWorkers 个 BCEventQueue，
// 轮转派发。**每个 BCEventQueue 只有一个 BCTask，严格串行**，所以并发度等于
// 队列数而不是线程数，两者必须一比一。
//
// 生命周期：进程内单例，懒创建，**刻意不销毁**。理由是没有安全的销毁时机——
// 阻塞中的 getaddrinfo / recvfrom 没法取消，而在自己的 worker 线程上销毁自己
// 的 BCTaskMgr 必然自锁。线程数是常量（不随连接数增长），泄漏量有上界。
//
// ---------------------------------------------------------------------------
// ⚠️ 已知限制，接手前务必读完
// ---------------------------------------------------------------------------
//  1. **worker 数固定为 kWorkers**，派发是朴素轮转（fetch_add % kWorkers），
//     不是"最闲优先"。所以第 N 个解析可能排到一条正卡着的队列上，而另一条
//     空着 —— 存在队头阻塞。
//  2. **policy == OS 的解析没有超时上界。** 那条路直接走 getaddrinfo
//     （DnsResolver.cpp:474），系统解析器卡多久就卡多久，实测可达数十秒。
//     只有 policy != OS 的自建 UDP 查询才有 timeoutMs/server 的上界。
//  3. 因此**并发解析数超过 kWorkers 就会排队**，而排队时长没有上界。
//
//  这三条加起来意味着"DNS 一定能在 connectTimeoutMs 内出结果"是不成立的。
//  通道的生命周期因此**刻意不与 DNS 绑定**：超时/关闭时 TcpChannel 会调
//  _AbandonDns() 把在途解析丢掉并立刻收尾，不再等它 —— 详见 TcpChannel.h 里
//  TcpDnsRequest 上方的注释。卡死的解析只占住一条 worker 时隙，不拖住任何通道。
///////////////////////////////////////////////////////////////////////////////

namespace {

class DnsExecutor
{
public:
    // 并发度。取 4 而不是 2：单条解析没有可靠的时间上界（见上面的限制 2），
    // 多两条常驻线程的成本可以忽略，却能显著降低队头阻塞的概率。
    // 队列数与 BCTaskMgr 的 worker 数必须一比一（见上面的"一比一"）。
    static const int kWorkers = 4;

    // 拿不到执行器（BC 运行时还没起来 / 建线程失败）时返回 NULL，
    // 调用方必须把它当成一次 DNS 失败处理，绝不能退化成在事件循环线程上
    // 直接解析。
    static DnsExecutor* Instance()
    {
        BCSpinMutex::Owner lock(s_lock_);
        if (!s_instance_)
        {
            DnsExecutor* inst = new DnsExecutor();
            if (!inst)
            {
                return NULL;
            }
            if (inst->_Create() != BC_R_SUCCESS)
            {
                // _Create 内部已经把半成品清理干净了
                delete inst;
                return NULL;
            }
            s_instance_ = inst;
        }
        return s_instance_;
    }

    void Post(AsyncTaskFunc fTask)
    {
        int idx = (int)(s_next_.fetch_add(1) % (uint32_t)kWorkers);
        queues_[idx]->PostTask(fTask);
    }

private:
    // BCEventQueue 的构造是 protected 的，用一个最小派生类把它实例化出来。
    // 本队列不排任何定时器，OnEventProcess 走基类默认实现（处理 PostTask
    // 包装出来的 TaskFuncWrap 事件）就够了。
    class Queue : public BCEventQueue
    {
    public:
        Queue() {}
        ~Queue() override {}
    };

    DnsExecutor()
        : task_mgr_(NULL)
        , timer_mgr_(NULL)
    {
        memzero(queues_, sizeof(queues_));
    }

    ~DnsExecutor() {}   // 见上：单例刻意不销毁，这里不做任何清理

    BCRESULT _Create()
    {
        BCRESULT result;

        task_mgr_ = new BCTaskMgr();
        if (!task_mgr_)
        {
            return BC_R_NOMEMORY;
        }
        result = task_mgr_->Create(kWorkers, 0, BCThread::PRIORITY_NORMAL,
                                   "TTDnsThread");
        if (result != BC_R_SUCCESS)
        {
            BCTaskMgr::Destroy(&task_mgr_);
            return result;
        }

        // BCEventQueue::Create 要求 timer mgr 非空。这个队列不排定时器，但
        // 还是自己配一个，免得跟 Runtime 的生命周期缠在一起。
        result = BCTimerMgr::Create(&timer_mgr_);
        if (result != BC_R_SUCCESS)
        {
            BCTaskMgr::Destroy(&task_mgr_);
            return result;
        }

        for (int i = 0; i < kWorkers; i++)
        {
            queues_[i] = new Queue();
            if (!queues_[i])
            {
                return BC_R_NOMEMORY;
            }
            result = queues_[i]->Create(timer_mgr_, task_mgr_,
                                        "TTDnsQueue", this);
            if (result != BC_R_SUCCESS)
            {
                return result;
            }
        }
        return BC_R_SUCCESS;
    }

    BCTaskMgr           *   task_mgr_;
    BCTimerMgr          *   timer_mgr_;
    Queue               *   queues_[kWorkers];

    static BCSpinMutex              s_lock_;
    static DnsExecutor          *   s_instance_;
    static std::atomic<uint32_t>    s_next_;
};

BCSpinMutex           DnsExecutor::s_lock_;
DnsExecutor       *   DnsExecutor::s_instance_ = NULL;
std::atomic<uint32_t> DnsExecutor::s_next_(0);

}   // namespace

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— 纯函数（可单测，不碰成员、不发 IO）
///////////////////////////////////////////////////////////////////////////////

bool TcpChannel::IsLegalTransition(State from, State to)
{
    // 任何活动状态都可以被打断进入 CLOSING（超时 / 对端断开 / 调用方 Close）。
    // CLOSED 是终态，进去就出不来；CLOSING 只能走到 CLOSED。
    if (to == TCPCH_CLOSING)
    {
        return from != TCPCH_CLOSING && from != TCPCH_CLOSED;
    }
    if (to == TCPCH_CLOSED)
    {
        return from == TCPCH_CLOSING;
    }

    switch (from)
    {
    case TCPCH_IDLE:
        return to == TCPCH_RESOLVING;
    case TCPCH_RESOLVING:
        return to == TCPCH_PINNING;
    case TCPCH_PINNING:
        // 绑定必须完整走完才允许 connect，中间不存在任何跳过 PINNING 的捷径
        return to == TCPCH_CONNECTING;
    case TCPCH_CONNECTING:
        // tls == false 时直接进 READY，跳过 TLS_HANDSHAKE
        return to == TCPCH_TLS_HANDSHAKE || to == TCPCH_READY;
    case TCPCH_TLS_HANDSHAKE:
        return to == TCPCH_READY;
    default:
        return false;
    }
}

bool TcpChannel::ParseIpLiteral(const std::string& ip, uint16_t port,
                                BCSockAddrS& out)
{
    if (ip.empty())
    {
        return false;
    }

    struct in_addr v4;
    if (inet_pton(AF_INET, ip.c_str(), &v4) == 1)
    {
        bc_sockaddr_fromin(&out, &v4, port);
        return true;
    }

    struct in6_addr v6;
    if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1)
    {
        bc_sockaddr_fromin6(&out, &v6, port);
        return true;
    }

    return false;
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— 构造 / 析构
///////////////////////////////////////////////////////////////////////////////

TcpChannel::TcpChannel()
    : state_(TCPCH_IDLE)
{
    memzero(&peer_addr_, sizeof(peer_addr_));
    memzero(recv_buffer_, sizeof(recv_buffer_));
}

TcpChannel::~TcpChannel()
{
    // 契约要求调用方只在收到 OnChannelClosed 之后销毁本对象。真被违反了也
    // 不能装作没看见——在途回调还持有 this，析构完必然 UAF，打日志至少能让
    // 现场可查。
    if (pending_dns_ || pending_connect_ || pending_recv_ || pending_send_)
    {
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel] 析构时仍有在途异步操作（dns=%u connect=%u recv=%u "
             "send=%u）。调用方必须等到 OnChannelClosed 之后再销毁 TcpChannel。",
             pending_dns_, pending_connect_, pending_recv_, pending_send_);
    }

    // 契约违规路径上的最后一道防御：把在途 DNS 请求正式放弃掉。
    //
    // 光靠 dns_request_ 的成员析构不够 —— 那只丢掉通道这一侧的 shared_ptr
    // 引用，worker 手里还有一份，req->channel 仍然指向即将被释放的本对象，
    // 于是 worker 解析完会在野指针上调 _AcquirePendingTask()，**必然 UAF**。
    // _AbandonDns() 会在 req->lock 的保护下把 channel 置空，worker 随后看到
    // abandoned 就干净退出。
    //
    // 走正常关闭流程时 _CancelIo() 早就调过 _AbandonDns()，dns_request_ 已经
    // 是空的，这里是个 no-op；只有"没等 OnChannelClosed 就 delete"才会真正
    // 用上它。把"必然 UAF"降级成"干净放弃"，救不回契约，但至少不炸。
    _AbandonDns();

    _Cleanup();

    if (queue_created_ && !detached_)
    {
        // 正常关闭路径已经在 _MaybeFinishClose() 里 Detach 过了。走到这里说明
        // 对象在关闭流程走完之前就被销毁了——契约违规。
        //
        // 这里用 Detach(true)：bRemoveOnShutdown 会先 RemoveAllOnShutdown()
        // 把注册的 shutdown 事件摘掉（BCEventQueue.cpp:499-502），于是不会有
        // _ShutdownCallback 在**已析构的对象**上回调 OnEventProcShutdown()。
        // 代价是这条路径上不会再发 OnChannelClosed —— 但调用方既然已经在销毁
        // 对象了，它本来也收不到了。
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel] 析构时事件队列还没走完关闭流程（state=%d）。"
             "调用方必须先 Close() 并等到 OnChannelClosed 再销毁 TcpChannel。",
             (int)state_.load());
        Detach(true);
    }
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— 公开接口
///////////////////////////////////////////////////////////////////////////////

BCRESULT TcpChannel::Open(const TcpChannelConfig& cfg,
                          ITcpChannelHandler* handler)
{
    BCRESULT result;

    open_error_.clear();

    if (!handler)
    {
        return BC_R_INVALIDARG;
    }
    if (state_.load() != TCPCH_IDLE)
    {
        return BC_R_ALREADYRUNNING;
    }
    if (cfg.host.empty() || cfg.port == 0)
    {
        return BC_R_INVALIDARG;
    }

    config_ = cfg;

    if (config_.policy == TT_VPN_POLICY_UNSET)
    {
        config_.policy = tt_vpn_policy_platform_default();
    }

    // resolvedIp 必须是 IP 字面量。这里就地校验，免得后面在 task 线程上
    // 拿它去发一次莫名其妙的 DNS 查询。
    BCSockAddrS literal;
    bool haveLiteral = false;
    if (!config_.resolvedIp.empty())
    {
        if (!ParseIpLiteral(config_.resolvedIp, config_.port, literal))
        {
            LogQ(config_.loggerCtx, _ERROR_,
                 "[TcpChannel] resolvedIp=\"%s\" 不是合法的 IP 字面量",
                 config_.resolvedIp.c_str());
            return BC_R_INVALIDARG;
        }
        haveLiteral = true;
    }

    // ------------------------------------------------------------------
    // 步骤 1：选网卡。必须在 DNS 之前定下来 —— DNS 查询本身也要绑到同一块
    // 网卡上，否则 TUN 全局模式下拿到的是 fake-IP，后面把 TCP socket 绑得
    // 再死也没用（见 DnsResolver.h 顶部注释）。
    //
    // policy == OS 时不选，bound_ifindex_ 保持 0，后续绑定与路由复核全部跳过。
    //
    // 整段刻意排在 BCEventQueue::Create 之前：配置层面就注定失败的连接不该
    // 先去占一条事件队列，而且这样 force-physical 的硬失败可以在不初始化
    // Runtime 的前提下单测（见 tests/TcpChannel_test.cpp）。
    // ------------------------------------------------------------------
    if (config_.policy != TT_VPN_POLICY_OS)
    {
        bound_ifindex_ = _PickPhysicalIfIndex();
    }

    // ⚠️ force-physical 的硬校验刻意写在 TT_HAS_PATH_MONITOR 之外、无条件执行。
    // 放进 guard 里的话，没有 path monitor 的构建（Android JNI / standalone）
    // 会静默放行，socket 完全没绑定却一路返回成功——这正是 DnsResolver 第一版
    // 栽过的坑。
    if (config_.policy == TT_VPN_POLICY_FORCE_PHYSICAL && !_HasBindTarget())
    {
        // 同一段文案既落日志、也交给调用方 —— 这条路径走不到 _Fail()，
        // 只落日志的话绑定层能给业务的就只剩一个错误码（见 LastOpenError）。
        open_error_ =
            "force-physical：找不到可用的物理网卡（当前 active interface 为"
            "虚拟网卡或取不到，ifIndex/androidNetHandle 均为 0）。连接不发起。"
            "业务可改用 prefer-physical 重试。";
        LogQ(config_.loggerCtx, _ERROR_, "[TcpChannel] %s", open_error_.c_str());
        return BC_R_NO_PHYSICAL_INTERFACE;
    }
    if (config_.policy == TT_VPN_POLICY_PREFER_PHYSICAL && !_HasBindTarget())
    {
        LogQ(config_.loggerCtx, _WARN_,
             "[TcpChannel] prefer-physical：找不到物理网卡，回落系统路由。"
             "TUN 全局代理下服务端可能看到 VPN 出口 IP。");
    }

    result = BCEventQueue::Create(Runtime::RandomTimerMgr(),
                                  Runtime::RandomTaskMgr(),
                                  "TcpChannel", this);
    if (result != BC_R_SUCCESS)
    {
        // 这里原本一声不吭。最常见的成因是 Runtime 没 Initialize()，
        // BCEventQueue 拿不到 timer/task mgr，于是回一个光秃秃的
        // INVALIDARG(63) —— 见 crash-2026.09.07.01 那一轮的排查。
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "事件队列创建失败 result=%d。最常见的成因是 Runtime 没有 "
                 "Initialize()，BCEventQueue 因此拿不到 timer/task mgr。",
                 (int)result);
        open_error_ = buf;
        LogQ(config_.loggerCtx, _ERROR_, "[TcpChannel] %s", open_error_.c_str());
        return result;
    }
    queue_created_ = true;
    handler_       = handler;

    recv_event_.ev_type         = BC_SOCKEVENT_RECVDONE;
    recv_event_.ev_action       = _RecvDoneCallback;
    recv_event_.ev_arg          = this;
    recv_event_.bufferlist_size = 1;

    // ------------------------------------------------------------------
    // 总超时定时器。刻意覆盖 DNS + connect + TLS 握手整段，而不是只盖 connect：
    // 一次卡死的 DNS 查询同样会让业务无限等待，只盖 connect 等于漏了一半。
    // 进入 READY 或 CLOSED 时取消。
    // ------------------------------------------------------------------
    uint32_t timeoutMs = config_.connectTimeoutMs ? config_.connectTimeoutMs
                                                  : 10000;
    ScheduleTask(connect_timer_, [this](int32_t) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "连接超时：%u ms 内没能走完 DNS/connect/TLS 握手（停在 %s）",
                 config_.connectTimeoutMs, _StateName(state_.load()));
        _Fail(BC_R_CONNECT_TIMEOUT, msg);
    }, (uint64_t)timeoutMs * 1000);

    _SetState(TCPCH_RESOLVING);

    if (haveLiteral)
    {
        // 已经有 IP 了，不必去 DNS 线程绕一圈，但仍然要回到事件循环线程上
        // 推进状态机 —— 状态机只能在那一条线程上跑。
        //
        // 注意这**不**保证"Open() 返回前不会有回调"：PostTask 之后队列线程
        // 随时可能派发，极端情况下 OnChannelClosed 可以先于 Open() 返回。
        // 契约保证的是"Open() 返回非 BC_R_SUCCESS 时没有任何回调"，成功返回
        // 之后的回调时序不做承诺。
        DnsResult r;
        r.result = BC_R_SUCCESS;
        r.addrs.push_back(literal);
        (void)_AcquirePendingTask();
        PostTask([this, r]() {
            _ReleasePendingTask();
            this->_OnResolveDone(r);
        });
        return BC_R_SUCCESS;
    }

    // ------------------------------------------------------------------
    // 步骤 2：DNS。DnsResolver::Resolve 是同步阻塞的，必须派到 DnsExecutor
    // 的专属线程上跑。既不能占着本对象的事件循环（会把超时定时器和 socket
    // 回调一起堵死），也不能借 Runtime 的队列（那条线程和 SMP 服务端的
    // UDPSender 共用，详见 DnsExecutor 上方的注释）。
    // ------------------------------------------------------------------
    DnsConfig dns = config_.dns;
    if (dns.policy == TT_VPN_POLICY_UNSET)
    {
        dns.policy = config_.policy;
    }
    if (dns.ifIndex == 0)
    {
        dns.ifIndex = bound_ifindex_;
    }
    if (dns.androidNetHandle == 0)
    {
        dns.androidNetHandle = config_.androidNetHandle;
    }
    if (dns.loggerCtx == NULL)
    {
        dns.loggerCtx = config_.loggerCtx;
    }

    DnsExecutor* executor = DnsExecutor::Instance();
    if (!executor)
    {
        // 起不来专属线程时宁可把这次连接判失败，也绝不退化成在事件循环线程
        // 上直接 Resolve —— 那会把本连接的超时定时器一起堵死，反而更难查。
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel] 无法创建 DNS 执行线程池，连接失败");
        (void)_AcquirePendingTask();
        PostTask([this]() {
            _ReleasePendingTask();
            _Fail(BC_R_DNS_FAILED, "无法创建 DNS 执行线程池");
        });
        return BC_R_SUCCESS;
    }

    // 请求上下文独立于本对象存活，通道随时可以撒手不管（见 TcpDnsRequest）
    dns_request_          = std::make_shared<TcpDnsRequest>();
    dns_request_->channel = this;
    dns_request_->host    = config_.host;
    dns_request_->cfg     = dns;

    pending_dns_++;
    std::shared_ptr<TcpDnsRequest> req = dns_request_;
    executor->Post([req]() { TcpChannel::_RunDnsRequest(req); });

    return BC_R_SUCCESS;
}

bool TcpChannel::_AcquirePendingTask()
{
    // Send() / Close() 可能在别的线程上被调用，而事件循环线程可能正好走到
    // _MaybeFinishClose()。光靠 state_ 预检堵不住这个窗口：
    //   线程 A：state_ == READY，检查通过
    //   线程 B：_MaybeFinishClose() 看四个计数全零 → 发 OnChannelClosed
    //           → 调用方按契约销毁 TcpChannel
    //   线程 A：PostTask(...)  ← 已经是野指针
    // 所以"取号"必须和"决定通知"互斥：拿到号之后 pending_tasks_ > 0，
    // _MaybeFinishClose() 就一定会退回去等这个 lambda 跑完。
    BCSpinMutex::Owner lock(guard_lock_);
    if (closed_notified_)
    {
        return false;   // 已经通知过关闭，对象随时可能被销毁，不能再投递
    }
    pending_tasks_++;
    return true;
}

void TcpChannel::_ReleasePendingTask()
{
    BCSpinMutex::Owner lock(guard_lock_);
    if (pending_tasks_ > 0)
    {
        pending_tasks_--;
    }
}

BCRESULT TcpChannel::Send(BufferPtr buf)
{
    if (!buf)
    {
        return BC_R_INVALIDARG;
    }
    if (state_.load() != TCPCH_READY)
    {
        return BC_R_NOTCONNECTED;
    }
    if (!_AcquirePendingTask())
    {
        return BC_R_NOTCONNECTED;
    }

    PostTask([this, buf]() {
        _ReleasePendingTask();
        if (state_.load() != TCPCH_READY)
        {
            // 已经在关闭途中。释放掉刚才那个号之后，可能就轮到我们来收尾了。
            _MaybeFinishClose();
            return;
        }
        if (ssl_layer_)
        {
            // TLS：明文先交给 SSLayer 加密，密文经 OnSSLWrite 回调落到 socket。
            // 守卫必须包住整个 WriteToSSL —— SSL_write 出错会回调 OnSSLError，
            // 那条路会一直走到 _Cleanup() 去销毁我们脚下的 SSLayer。
            SslReentryGuard guard(this);
            ssl_layer_->WriteToSSL(buf);
        }
        else
        {
            _SendRaw(buf);
            // 非 TLS 路径没有重入问题：_SendRaw 出错时 _Fail 已经把关闭流程
            // 走完（可能同步回调过 OnChannelClosed），这里不能再碰成员。
        }
    });
    return BC_R_SUCCESS;
}

void TcpChannel::Close()
{
    if (!queue_created_)
    {
        return;
    }
    if (!_AcquirePendingTask())
    {
        return;     // 已经关闭并通知过了，幂等
    }
    PostTask([this]() {
        _ReleasePendingTask();
        _Fail(BC_R_SUCCESS, "调用方主动关闭");
    });
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— 连接复用
///////////////////////////////////////////////////////////////////////////////

void TcpChannel::Park(ITcpChannelHandler* watcher)
{
    // 只有真正建立起来的连接才谈得上复用。没就绪就 Park 是调用方写错了，
    // 这里什么都不做——绝不能让一条半途而废的通道被标成"可复用"。
    if (state_.load() != TCPCH_READY)
    {
        return;
    }

    // ⚠️ 本函数跑在队列线程上（契约见头文件），handler_ 由这条线程独占，
    // 所以这里是普通赋值而不是原子操作。
    handler_ = watcher;
    parked_.store(true);
}

bool TcpChannel::IsParkedAndUsable() const
{
    return parked_.load()
        && !poisoned_.load()
        && state_.load() == TCPCH_READY;
}

BCRESULT TcpChannel::Adopt(ITcpChannelHandler* handler)
{
    if (!handler)
    {
        return BC_R_INVALIDARG;
    }
    if (!IsParkedAndUsable())
    {
        return BC_R_NOTCONNECTED;
    }
    // 取号的理由与 Send() 完全一致（见 _AcquirePendingTask 的注释）：没有它，
    // 事件循环线程可能正好走到 _MaybeFinishClose() 把对象通知销毁掉，我们随后
    // 的 PostTask 就打在野指针上。
    if (!_AcquirePendingTask())
    {
        return BC_R_NOTCONNECTED;
    }

    PostTask([this, handler]() {
        _ReleasePendingTask();

        // 排队这一会儿里对端完全可能 FIN —— keep-alive 下服务端有自己的空闲
        // 超时，这是日常而不是异常。上面那次预检堵不住这个窗口，只有在队列
        // 线程上复核才作数。
        if (!IsParkedAndUsable())
        {
            // Adopt 已经返回成功，调用方正等着回调，不能默默咽掉。补一个
            // OnChannelClosed，让它和"连接中途断掉"走同一条失败路径。
            handler->OnChannelClosed(BC_R_NOTCONNECTED,
                                     "复用的连接在接手前已失效");
            // 刚才那个号可能是最后一个，释放之后也许就轮到我们收尾了。
            _MaybeFinishClose();
            return;
        }

        parked_.store(false);

        // 看守（如果有）到此退场。它靠这个回调和 OnChannelClosed 二选一来定
        // 自己的生死，所以**必须**在换上新 handler 之前发出去，而且只发这
        // 一次。
        ITcpChannelHandler* watcher = handler_;
        handler_ = handler;
        if (watcher)
        {
            watcher->OnChannelDetached();
        }

        // 走 OnChannelReady 而不是另开一个"已接手"回调：对上层来说，一条
        // 捡来的连接和一条刚握完手的连接此刻处境完全相同，都是"可以发报文
        // 了"。发请求那段代码因此一行都不用改。
        handler_->OnChannelReady();
    });
    return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— 网卡选择
///////////////////////////////////////////////////////////////////////////////

uint32_t TcpChannel::_PickPhysicalIfIndex()
{
    // 调用方显式指定优先。Android 只能走这条路：JNI 构建不定义
    // TT_HAS_PATH_MONITOR，没有任何可用的自动探测手段。
    if (config_.ifIndex != 0)
    {
        return config_.ifIndex;
    }

#if defined(TT_HAS_PATH_MONITOR)
    // 与 SMPConnector.cpp:1012 / DnsResolver.cpp:744 的既有用法保持一致，
    // 不要为 TcpChannel 另发明一套探测机制。
    int64_t detected = tt_netmon_query_default_ifindex_ex((int)config_.policy);
    if (detected > 0)
    {
        return (uint32_t)detected;
    }
#endif
    return 0;
}

bool TcpChannel::_HasBindTarget() const
{
    if (bound_ifindex_ != 0)
    {
        return true;
    }
#ifdef OS_ANDROID
    if (config_.androidNetHandle != 0)
    {
        return true;
    }
#endif
    return false;
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— RESOLVING
///////////////////////////////////////////////////////////////////////////////

void TcpChannel::_RunDnsRequest(std::shared_ptr<TcpDnsRequest> req)
{
    // ⚠️ 本函数跑在 DnsExecutor 的专属线程上，不是通道的事件循环线程。
    // 除了在持 req->lock 的临界区里，任何时候都不许碰 req->channel。

    // 短路：通道可能在任务还排着队的时候就已经超时/关闭了，没必要再去解析。
    {
        BCSpinMutex::Owner lock(req->lock);
        if (req->abandoned || !req->channel)
        {
            return;
        }
    }

    DnsResult r = DnsResolver::Resolve(req->host, req->cfg);

    // 交接：整段「查 abandoned → 取号 → PostTask」必须在同一个临界区里完成。
    // _AbandonDns() 与它互斥，而放弃动作严格早于 OnChannelClosed，所以只要
    // 这里看到 channel 非空，它就一定还活着；取号成功之后 pending_tasks_ > 0
    // 又会把 OnChannelClosed 挡到下面这个 lambda 跑完为止。
    BCSpinMutex::Owner lock(req->lock);
    if (req->abandoned || !req->channel)
    {
        return;     // 通道已经撒手，结果直接丢弃，一个成员都不碰
    }
    TcpChannel* ch = req->channel;
    if (!ch->_AcquirePendingTask())
    {
        return;
    }
    ch->PostTask([ch, r]() {
        ch->_ReleasePendingTask();
        ch->_OnResolveDone(r);
    });
}

void TcpChannel::_AbandonDns()
{
    if (!dns_request_)
    {
        return;
    }

    {
        BCSpinMutex::Owner lock(dns_request_->lock);
        dns_request_->abandoned = true;
        dns_request_->channel   = NULL;
    }
    dns_request_.reset();

    // 关键的一步：**立刻**把在途计数归零，不再等那次解析返回。
    // policy == OS 的 getaddrinfo 没有超时上界，等它等于把 connectTimeoutMs
    // 作废；worker 那边最终会看到 abandoned 自行丢弃结果。
    pending_dns_ = 0;

    LogQ(config_.loggerCtx, _WARN_,
         "[TcpChannel] 放弃在途 DNS 解析（%s），不再等待它返回；"
         "该次解析仍会占住一条 DnsExecutor worker 直到系统解析器自己返回",
         config_.host.c_str());
}

void TcpChannel::_OnResolveDone(const DnsResult& result)
{
    if (pending_dns_ > 0)
    {
        pending_dns_--;
    }
    dns_request_.reset();

    // 超时或调用方 Close 可能已经先一步把连接判死了。这时 DNS 结果直接丢弃，
    // 但必须走一次 _MaybeFinishClose——刚刚归零的 pending_dns_ 可能正是最后
    // 一个在途操作，OnChannelClosed 还等着它。
    if (closing_ || state_.load() != TCPCH_RESOLVING)
    {
        _MaybeFinishClose();
        return;
    }

    if (result.result != BC_R_SUCCESS || result.addrs.empty())
    {
        BCRESULT code = (result.result != BC_R_SUCCESS) ? result.result
                                                        : BC_R_HOSTUNREACH;
        _Fail(code, "DNS 解析 " + config_.host + " 失败: " + result.errMessage);
        return;
    }

    peer_addr_ = result.addrs[0];
    bc_sockaddr_setport(&peer_addr_, config_.port);
    _EnterPinning();
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— PINNING → CONNECTING
///////////////////////////////////////////////////////////////////////////////

void TcpChannel::_EnterPinning()
{
    BCRESULT result;

    _SetState(TCPCH_PINNING);

    is_ipv6_       = (peer_addr_.type.sa.sa_family == AF_INET6);
    peer_addr_len_ = is_ipv6_ ? (socklen_t)sizeof(struct sockaddr_in6)
                              : (socklen_t)sizeof(struct sockaddr_in);
    peer_ip_       = _AddrToIpText(peer_addr_);

    // TLS 上下文先建好：配置错了（畸形 CA PEM、客户端证书加载失败）应该在
    // 碰网络之前就暴露出来，而不是白建一条 TCP 连接再失败。
    if (config_.tls)
    {
        result = _SetupTls();
        if (result != BC_R_SUCCESS)
        {
            return;     // _SetupTls 内部已经 _Fail 过了
        }
    }

    socket_ = new BCSocket();
    if (!socket_)
    {
        _Fail(BC_R_NOMEMORY, "创建 TCP socket 对象失败");
        return;
    }
    result = socket_->Create(Runtime::SocketMgr(),
                             is_ipv6_ ? PF_INET6 : PF_INET,
                             bc_sockettype_tcp);
    if (result != BC_R_SUCCESS)
    {
        // Create 失败时内部已经把自己 _Free 掉了，这里只能丢掉指针，
        // 不能再 Detach（会二次释放）。
        socket_ = NULL;
        _Fail(result, "创建 TCP socket 失败");
        return;
    }

    // ⚠️ 绑网卡必须在 connect() 之前完成，这是 TCP 的硬约束：
    //    macOS/iOS 的 IP_BOUND_IF 对已建立连接的缓存路由无效；Linux 的 TCP
    //    路由在 connect() 时就定死了；Windows 的 IP_UNICAST_IF 文档明确要求
    //    在 connect 前设置。见 SocketPinner.h 顶部注释。
    // ⚠️ 顺序：先判定该不该绑，再真的去绑。判定只看内核路由表与对端地址，
    //    不需要 socket；提前判完，"放弃绑定"就退化成"干脆不绑"。
    result = _DecidePinBeforeConnect();
    if (result != BC_R_SUCCESS)
    {
        return;         // _DecidePinBeforeConnect 内部已经 _Fail 过了
    }

    result = _PinSocket();
    if (result != BC_R_SUCCESS)
    {
        return;         // _PinSocket 内部已经 _Fail 过了
    }

    _SetState(TCPCH_CONNECTING);

    result = socket_->Connect(&peer_addr_, GetTask(), _ConnectDoneCallback, this);
    if (result == BC_R_SUCCESS || result == BC_R_INPROGRESS)
    {
        pending_connect_++;
    }
    else
    {
        _Fail(result, "发起 TCP connect 失败");
    }
}

BCRESULT TcpChannel::_PinSocket()
{
    if (!_HasBindTarget())
    {
        // policy == OS，或 prefer-physical 下压根没找到物理网卡（Open 里已经
        // 打过 WARN）。不绑，跟随系统路由。
        return BC_R_SUCCESS;
    }

    TTPinRequest pin;
    memset(&pin, 0, sizeof(pin));
    pin.fd               = socket_->GetFd();
    pin.ifIndex          = bound_ifindex_;
    pin.androidNetHandle = config_.androidNetHandle;
    pin.ipv6             = is_ipv6_ ? 1 : 0;
    pin.isTcp            = 1;      // ⚠️ 必须在 connect 之前
    pin.policy           = config_.policy;
    pin.loggerCtx        = config_.loggerCtx;

    char method[32] = "";
    int  pinErrno   = 0;
    TTPinResult pr = tt_socket_pin(&pin, method, sizeof(method), &pinErrno);

    if (pr == TT_PIN_OK)
    {
        pin_method_ = method;
        LogQ(config_.loggerCtx, _INFO_,
             "[TcpChannel] socket 已绑定到 ifIndex=%u（手段：%s）",
             bound_ifindex_, pin_method_.c_str());
        return BC_R_SUCCESS;
    }

    // ⚠️ TT_PIN_NOT_NEEDED 不等于绑定成功 —— 它只表示"调用方没要求绑定"。
    // 走到这里说明 _HasBindTarget() 为真却依然没绑上（参数被 SocketPinner
    // 判为无效等），force-physical 下必须当失败处理，绝不能悄悄放行。
    if (config_.policy == TT_VPN_POLICY_FORCE_PHYSICAL)
    {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "force-physical：把 socket 绑到 ifIndex=%u 失败"
                 "（TTPinResult=%d，errno=%d）。Linux 上 SO_BINDTODEVICE 需要 "
                 "CAP_NET_RAW 或 root；TT_PIN_NOT_NEEDED/UNSUPPORTED 同样按"
                 "失败处理——不绑就出网等于没有绕过 VPN。",
                 bound_ifindex_, (int)pr, pinErrno);
        _Fail(BC_R_PIN_FAILED, msg);
        return BC_R_PIN_FAILED;
    }

    LogQ(config_.loggerCtx, _WARN_,
         "[TcpChannel] prefer-physical：绑定 ifIndex=%u 失败"
         "（TTPinResult=%d errno=%d），不绑继续，服务端可能看到 VPN 出口 IP",
         bound_ifindex_, (int)pr, pinErrno);
    bound_ifindex_ = 0;
    pin_method_.clear();
    return BC_R_SUCCESS;
}

BCRESULT TcpChannel::_DecidePinBeforeConnect()
{
    // 决定"这条连接到底该不该绑物理网卡"。**必须在 connect() 之前**，而且现在
    // 也排在 _PinSocket() 之前 —— 判定只依赖内核路由表与对端地址，不需要
    // socket，提前判完就能"干脆不绑"，省掉一次没意义的 setsockopt，也绕开
    // "Android 的 android_setsocknetwork 在活着的 fd 上无法撤销"这个麻烦。
    //
    // 判定逻辑与 UDPSender::Connect() 共用 tt_vpn_pin_recheck()，两条路径因此
    // 不可能再漂移 —— 这正是上一轮 prefer-physical 在 QUIC 与 TCP 上行为相反
    // 的根因（那时两边各自实现、各自选 scope）。
    //
    // 两次查询的用途见 VpnPolicy.h 里 tt_vpn_pin_recheck() 的长注释：
    //   * scoped（scope=bound_ifindex_）—— 绑定有效性；
    //   * global（scope=0）+ 对端地址段 —— 黑洞判定。
    //
    // tt_route_lookup_ifindex 在 iOS / Android 上是返回 UNKNOWN 的桩
    // （见 NetworkRouteLookup.h）。UNKNOWN 表示"不知道"，绝不能当失败信号。
    if (!_HasBindTarget())
    {
        return BC_R_SUCCESS;
    }

    const uint32_t scopedIf = tt_route_lookup_ifindex(
        (const struct sockaddr*)&peer_addr_.type.sa, peer_addr_len_,
        bound_ifindex_);
    const uint32_t globalIf = tt_route_lookup_ifindex(
        (const struct sockaddr*)&peer_addr_.type.sa, peer_addr_len_, 0);
    const uint32_t peerClass = tt_peer_address_class(
        (const struct sockaddr*)&peer_addr_.type.sa, (size_t)peer_addr_len_);

    // ------------------------------------------------------------------
    // force-physical：永不回落。做不到就以专属错误码明确失败。
    //
    // 为什么对 loopback / 内网对端也照样失败（而不是放宽）：这一档的语义是
    // "只接受物理网卡，任何阶段都不回落"。127.0.0.1 从物理网卡出不去，这是
    // 物理事实；此时静默改成不绑定，恰恰是这一档存在的意义所要禁止的那种
    // 隐式回落。明确失败 + 说清原因，业务一眼就知道该改用 os。
    // ------------------------------------------------------------------
    if (config_.policy == TT_VPN_POLICY_FORCE_PHYSICAL)
    {
        if (scopedIf == TT_ROUTE_IFINDEX_UNKNOWN || scopedIf == bound_ifindex_)
        {
            return BC_R_SUCCESS;
        }

        char msg[520];
        if (peerClass != TT_PEER_CLASS_GLOBAL)
        {
            // 最常见的踩坑形态：业务用默认档连本机 / 内网服务端。单独给一条
            // 能直接照着改的文案，不要让人去猜 ROUTE_MISMATCH 是什么意思。
            snprintf(msg, sizeof(msg),
                     "force-physical：对端 %s 不是全局可路由地址"
                     "（peerClass=0x%x：0x1=TUN 代理 fake-IP 段，"
                     "0x2=环回 / 私网 / CGNAT / 链路本地 / 保留段），"
                     "从物理网卡 ifIndex=%u 出不去（scoped 路由表把它判给 "
                     "ifIndex=%u）。force-physical 的语义是"
                     "\"只走物理网卡，做不到就失败\"，所以这里不回落。"
                     "要连本机或内网服务端请显式使用 vpnPolicy=os，"
                     "或改用 prefer-physical（会自动放弃绑定）。",
                     peer_ip_.c_str(), (unsigned)peerClass,
                     bound_ifindex_, scopedIf);
        }
        else if (scopedIf == TT_ROUTE_IFINDEX_NO_ROUTE)
        {
            snprintf(msg, sizeof(msg),
                     "force-physical：socket 要绑到 ifIndex=%u，但内核在这块"
                     "网卡的 scoped 路由表里找不到到 %s 的路由（该网卡没有可用"
                     "的默认路由 / 地址族不匹配）。从物理网卡出不去，拒绝继续。",
                     bound_ifindex_, peer_ip_.c_str());
        }
        else
        {
            snprintf(msg, sizeof(msg),
                     "force-physical：即使把查询 scope 到 ifIndex=%u，内核"
                     "仍然把到 %s 的包判给 ifIndex=%u。包不会从我们要求的物理"
                     "网卡出去，服务端拿不到真实 IP，拒绝继续。",
                     bound_ifindex_, peer_ip_.c_str(), scopedIf);
        }
        _Fail(BC_R_ROUTE_MISMATCH, msg);
        return BC_R_ROUTE_MISMATCH;
    }

    // ------------------------------------------------------------------
    // prefer-physical：尽力而为。判定交给共用决策函数。
    //
    // ⚠️ 这里以前是"记一条 WARN 然后保持绑定，让 connect() 去失败"。那是错的：
    //    prefer-physical 是**桌面与 Android 的默认档**，于是默认配置下连
    //    127.0.0.1（本机测试服务端）或只能经 VPN 到达的内网服务端一律
    //    connect 失败。"尽力走物理网卡"的语义里，够不着就该放弃绑定，而不是
    //    抱着绑定把连接判死。
    // ------------------------------------------------------------------
    TTPinRecheckReason reason = TT_PIN_REASON_OK;
    const TTPinVerdict verdict = tt_vpn_pin_recheck(
        config_.policy, bound_ifindex_, scopedIf, globalIf, peerClass, &reason);

    if (verdict == TT_PIN_VERDICT_KEEP)
    {
        return BC_R_SUCCESS;
    }

    LogQ(config_.loggerCtx, _WARN_,
         "[TcpChannel] %s：放弃绑定 ifIndex=%u（reason=%s，scoped=%u，global=%u，"
         "peerClass=0x%x），改走系统默认路由连 %s。对端不是全局可路由地址时"
         "（环回 / 私网 / fake-IP）这是唯一连得通的做法；对端是公网地址时说明"
         "这块网卡到不了它，服务端将看到系统默认出口的 IP。",
         tt_vpn_policy_to_string(config_.policy), bound_ifindex_,
         tt_vpn_pin_reason_to_string(reason), scopedIf, globalIf,
         (unsigned)peerClass, peer_ip_.c_str());

    // 判定在 _PinSocket() 之前，所以"放弃绑定"就是把绑定目标清空即可，
    // 不需要 unpin —— socket 从来没被绑过。
    bound_ifindex_ = 0;
#ifdef OS_ANDROID
    // Android 的绑定目标是 network handle，不清掉的话 _HasBindTarget() 仍为真、
    // _PinSocket() 照样会调 android_setsocknetwork。
    config_.androidNetHandle = 0;
#endif
    pin_method_.clear();
    return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— TLS
///////////////////////////////////////////////////////////////////////////////

BCRESULT TcpChannel::_SetupTls()
{
    std::string err;

    ssl_ctx_ = TlsContext::Create(config_.tls_cfg, err);
    if (!ssl_ctx_)
    {
        _Fail(BC_R_FAILURE, "创建 SSL_CTX 失败: " + err);
        return BC_R_FAILURE;
    }

    ssl_ = SSL_new(ssl_ctx_);
    if (!ssl_)
    {
        _Fail(BC_R_NOMEMORY, "SSL_new 失败");
        return BC_R_NOMEMORY;
    }

    BCRESULT result = TlsContext::PrepareSsl(ssl_, config_.host,
                                             config_.tls_cfg, err);
    if (result != BC_R_SUCCESS)
    {
        _Fail(result, "配置 SNI / hostname 校验失败: " + err);
        return result;
    }

    // SPKI pin 的语义（与 SMPConnector 的 on_conn_cert_verify 一致，见
    // TlsContext::VerifyChain）是"只比对 leaf 公钥指纹，不做链校验"——典型
    // 场景是自建代理的外层跳，证书本来就不由公共 CA 签发。但 TlsContext::
    // Create 只会设 SSL_VERIFY_PEER，BoringSSL 会在握手里先做链校验并直接
    // 失败，pin 根本轮不到比对。
    //
    // 所以这里显式把本条连接的校验关掉，改由 _VerifySpkiPin() 在握手完成、
    // 向上层发 OnChannelReady 之前自己比对。绝不存在"设了 pin 却谁都没校验"
    // 的静默放行路径：pin 不匹配一律 BC_R_TLS_VERIFY_FAILED 断开。
    if (!config_.tls_cfg.spkiPin.empty())
    {
        SSL_set_verify(ssl_, SSL_VERIFY_NONE, NULL);
        LogQ(config_.loggerCtx, _INFO_,
             "[TcpChannel] 已配置 spkiPin，本条连接改用公钥指纹校验，"
             "不做证书链校验（与 SMP 侧语义一致）");
    }

    ssl_layer_.reset(new SSLayer(false));
    result = ssl_layer_->Create(ssl_, this);
    if (result != BC_R_SUCCESS)
    {
        ssl_layer_.reset();
        _Fail(result, "初始化 SSLayer 失败");
        return result;
    }

    return BC_R_SUCCESS;
}

BCRESULT TcpChannel::_VerifySpkiPin()
{
    if (config_.tls_cfg.spkiPin.empty())
    {
        return BC_R_SUCCESS;
    }

    // VerifyChain 的 spkiPin 分支只读 chain[0]（leaf），所以这里只取 leaf 就够，
    // 也顺带避开了 SSL_get_peer_cert_chain 在不同 SSL 实现下"含不含 leaf"的
    // 差异。SSL_get_peer_certificate 返回的是加过引用的副本，用完要 X509_free。
    X509* leaf = SSL_get_peer_certificate(ssl_);
    if (!leaf)
    {
        _Fail(BC_R_TLS_VERIFY_FAILED, "spki pin 校验失败：对端没有提供证书");
        return BC_R_TLS_VERIFY_FAILED;
    }

    std::vector<X509*> chain;
    chain.push_back(leaf);

    std::string err;
    BCRESULT result = TlsContext::VerifyChain(chain, config_.host,
                                              config_.tls_cfg, err);
    X509_free(leaf);

    if (result != BC_R_SUCCESS)
    {
        _Fail(BC_R_TLS_VERIFY_FAILED, "spki pin 校验失败: " + err);
        return BC_R_TLS_VERIFY_FAILED;
    }
    return BC_R_SUCCESS;
}

void TcpChannel::_DumpTlsDiagnostics(const char* stage)
{
    // TLS 侧的诊断落地放在这里，而不是塞回 TlsContext：
    //   * TcpChannel 走的是 BoringSSL 握手内建的链校验（SSL_VERIFY_PEER +
    //     X509_VERIFY_PARAM_set1_host），压根不调 TlsContext::VerifyChain，
    //     所以 VerifyChain 里那份 dump 对 https/wss 没有意义；
    //   * TlsContext 刻意不接 BC 日志子系统（见 TlsContext.h 顶部注释），
    //     而 TcpChannel 本来就链 libenv，能直接 LogQ。
    if (!ssl_)
    {
        return;
    }

    long vr = SSL_get_verify_result(ssl_);
    LogQ(config_.loggerCtx, _ERROR_,
         "[TcpChannel] TLS %s 失败：host=%s peer=%s verify_result=%ld (%s)",
         stage, config_.host.c_str(), peer_ip_.c_str(), vr,
         X509_verify_cert_error_string(vr));

    // 把对端证书链的 subject / issuer 逐张打出来。"服务端发的链缺中间证书"
    // 与"本地信任库里没有这个根"在错误码上长得一模一样，只有这份 dump 能分开。
    STACK_OF(X509)* chain = SSL_get_peer_cert_chain(ssl_);
    int n = chain ? (int)sk_X509_num(chain) : 0;
    if (n == 0)
    {
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel]   对端证书链为空（很可能握手在收到 Certificate "
             "之前就失败了）");
        return;
    }
    for (int i = 0; i < n; i++)
    {
        X509* cert = sk_X509_value(chain, i);
        if (!cert)
        {
            continue;
        }
        char subject[512] = { 0 };
        char issuer[512]  = { 0 };
        X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
        X509_NAME_oneline(X509_get_issuer_name(cert), issuer, sizeof(issuer));
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel]   chain[%d] subject=%s issuer=%s",
             i, subject, issuer);
    }
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— ISSLayerHandler
///////////////////////////////////////////////////////////////////////////////

int TcpChannel::OnSSLWrite(const void* data, size_t len)
{
    if (!data || len == 0)
    {
        return 0;
    }
    // 关闭途中（或 socket 已经拆掉）时 _SendRaw 会直接丢弃，这里就得如实返回
    // 0，不能报"已写出 len 字节"骗 SSL —— 骗它只会让密文凭空消失得无声无息。
    // 返回 0 会让 BIO 写失败，SSL_write/SSL_connect 随即报错，而这条错误在
    // 关闭途中会被 _Fail 的幂等判断吃掉，不影响已经定下来的关闭原因。
    if (closing_ || !socket_)
    {
        return 0;
    }
    BufferPtr buf(new BCBuffer);
    buf->Write(data, len);
    _SendRaw(buf);
    return (int)len;
}

void TcpChannel::OnRecvDataFromSSL(const void* data, size_t size)
{
    // 非 TLS 时本函数由 _OnRecvDone 直接调用，data 就是原始字节。
    if (!data || size == 0)
    {
        return;
    }
    if (parked_.load())
    {
        // Park 期间本不该有任何字节到来：上一条报文已经完整收完，下一条请求
        // 还没发出去。真收到了就说明报文边界对不齐，这条连接不能再交给下一个
        // 请求——否则这段字节会被当成下一条响应的开头。
        poisoned_.store(true);
        return;
    }
    if (!handler_)
    {
        return;
    }
    if (state_.load() != TCPCH_READY)
    {
        // 还没 READY（TLS 握手没走完、或 pin 校验没过）就收到应用数据：丢弃。
        // 绝不能在校验通过之前把字节交给上层。
        return;
    }
    handler_->OnChannelData(data, size);
}

void TcpChannel::OnSSLReady()
{
    if (closing_ || state_.load() != TCPCH_TLS_HANDSHAKE)
    {
        return;
    }
    if (_VerifySpkiPin() != BC_R_SUCCESS)
    {
        return;
    }
    _EnterReady();
}

void TcpChannel::OnSSLFinished()
{
    // SSL_read 返回 0 —— 对端发来了 close_notify，这是 TLS 层面干净的收尾。
    tls_shutdown_clean_ = true;
    _OnPeerClosed();
}

void TcpChannel::OnSSLError(const char* file, int line,
                            const char* data, int flags)
{
    UNUSED(flags);

    const bool handshaking = (state_.load() == TCPCH_TLS_HANDSHAKE);
    _DumpTlsDiagnostics(handshaking ? "握手" : "读写");

    char msg[512];
    snprintf(msg, sizeof(msg), "TLS 错误 [%s:%d] %s",
             file ? file : "?", line, data ? data : "");
    // 证书类错误统一映射到 BC_R_TLS_VERIFY_FAILED，业务据此区分"网络不通"
    // 与"证书不对"；其余归为通用失败。
    // ssl_ 可能已经在关闭流程里被释放置空，判空后再取 verify_result
    // ——与 _DumpTlsDiagnostics 的写法保持一致。
    BCRESULT code = BC_R_FAILURE;
    if (ssl_ && SSL_get_verify_result(ssl_) != X509_V_OK)
    {
        code = BC_R_TLS_VERIFY_FAILED;
    }
    _Fail(code, msg);
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— CONNECTING → READY 的收发
///////////////////////////////////////////////////////////////////////////////

void TcpChannel::_OnConnectDone(BCRESULT result)
{
    if (result != BC_R_SUCCESS)
    {
        _Fail(result, "TCP connect 失败 (" + peer_ip_ + ")");
        return;
    }
    if (closing_ || state_.load() != TCPCH_CONNECTING)
    {
        return;
    }

    BCSockAddrS actual;
    memzero(&actual, sizeof(actual));
    if (socket_->GetPeerName(&actual) == BC_R_SUCCESS)
    {
        peer_ip_ = _AddrToIpText(actual);
    }

    if (ssl_layer_)
    {
        _SetState(TCPCH_TLS_HANDSHAKE);
        // 先挂上收，再发 ClientHello：ConnectSSL 会经 OnSSLWrite 把 ClientHello
        // 推到 socket，服务端的 ServerHello 随时可能回来。
        _TcpRecv();
        // 守卫必须是本作用域最后一个析构的对象，见 SslReentryGuard 注释
        SslReentryGuard guard(this);
        ssl_layer_->ConnectSSL();
    }
    else
    {
        _EnterReady();
        _TcpRecv();
    }
}

void TcpChannel::_EnterReady()
{
    _SetState(TCPCH_READY);
    if (connect_timer_ > 0)
    {
        UnscheduleTask(connect_timer_);
    }
    LogQ(config_.loggerCtx, _INFO_,
         "[TcpChannel] 已就绪：%s:%u peer=%s tls=%d ifIndex=%u pin=%s",
         config_.host.c_str(), (unsigned)config_.port, peer_ip_.c_str(),
         config_.tls ? 1 : 0, bound_ifindex_,
         pin_method_.empty() ? "(none)" : pin_method_.c_str());
    if (handler_)
    {
        handler_->OnChannelReady();
    }
}

void TcpChannel::_OnRecvDone(size_t size)
{
    if (size == 0)
    {
        return;
    }
    if (ssl_layer_)
    {
        // ReadFromSSL 会在自己的栈帧里回调 OnSSLReady / OnSSLError /
        // OnSSLFinished / OnSSLWrite，而这些回调可能走到 _Cleanup() 去销毁
        // 我们脚下的 SSLayer。守卫把销毁推迟到栈帧退回来之后。
        SslReentryGuard guard(this);
        ssl_layer_->ReadFromSSL(recv_buffer_, size);
    }
    else
    {
        OnRecvDataFromSSL(recv_buffer_, size);
    }
}

void TcpChannel::_OnSendDone()
{
    // TLS 握手期间：ClientHello 之类的握手报文刚发完，推一把 SSL 状态机。
    // 握手完成后 ConnectSSL 是空操作（SSLayer 内部状态已经是 RW）。
    if (ssl_layer_ && state_.load() == TCPCH_TLS_HANDSHAKE)
    {
        SslReentryGuard guard(this);
        ssl_layer_->ConnectSSL();
    }
}

void TcpChannel::_OnPeerClosed()
{
    const State st = state_.load();

    if (st != TCPCH_READY)
    {
        // 握手/建连还没走完对端就断了，**绝不能报成 BC_R_SUCCESS**。
        // 按头文件契约，BC_R_SUCCESS 表示"对端正常关闭"；中间人只要在握手
        // 中途 FIN，上层就会以为一切正常——而证书压根还没验过。Task 6/7 要
        // 据此决定重试还是降级，报错了才有得选。
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "对端在 %s 阶段就关闭了连接（%s）", _StateName(st),
                 st == TCPCH_TLS_HANDSHAKE
                     ? "TLS 握手未完成，证书尚未校验通过"
                     : "连接尚未就绪");
        _Fail(BC_R_UNEXPECTEDEND, msg);
        return;
    }

    if (config_.tls && !tls_shutdown_clean_)
    {
        // READY 之后收到裸 FIN、却没收到 close_notify：经典的 TLS truncation
        // attack —— 攻击者伪造 FIN 就能让接收方以为响应已经完整结束。
        // 这里如实报错，让上层自己判断已收到的数据够不够（比如 HTTP 的
        // Content-Length 已满足就可以忽略这个码）。
        _Fail(BC_R_UNEXPECTEDEND,
              "对端未发送 close_notify 就直接 FIN（TLS truncation，"
              "已收到的数据可能被截断）");
        return;
    }

    _Fail(BC_R_SUCCESS, "对端正常关闭了连接");
}

void TcpChannel::_TcpRecv()
{
    if (closing_ || !socket_ || pending_recv_ > 0)
    {
        return;
    }

    BCRegionS region = { recv_buffer_, sizeof(recv_buffer_) };
    BCRESULT result = socket_->Recv2(&region, 1, GetTask(), &recv_event_, 0);
    if (result == BC_R_SUCCESS || result == BC_R_INPROGRESS)
    {
        pending_recv_++;
    }
    else
    {
        _Fail(result, "挂载 TCP recv 失败");
    }
}

void TcpChannel::_SendRaw(BufferPtr buf)
{
    BCRESULT result = BC_R_SUCCESS;

    // ⚠️ 这里静默返回、**不**驱动收尾，是有前提的，改动 Send() 的前置检查时
    // 务必先读完这段：
    //
    //   * 走到这条早退分支且"手里还捏着一个 token"的唯一入口是 Send() 的
    //     lambda。而那个 lambda 在调 _SendRaw() 之前已经先查过
    //     state_ != TCPCH_READY 并在那里驱动过收尾了；
    //   * closing_ 与 TCPCH_CLOSING 是在 _Fail() 里同一条线程上连续置位的
    //     （中间没有任何可被抢占的点，事件循环单线程），所以不存在
    //     "closing_ 已置位但 state_ 还是 READY" 的窗口；
    //   * socket_ 只在 _Cleanup() 里被置空，而那时 closed_notified_ 已为真，
    //     根本不会再有新的 Send lambda 被派发进来。
    //
    // 三条合起来才保证"能走到这里的调用方都已经驱动过收尾了"。任何一条被打破
    // （比如给 Send() 加一条绕过 state_ 检查的快路径），这里就必须补上
    // _MaybeFinishClose()，否则 token 会被搁死，通道永久卡在 CLOSING。
    if (closing_ || !socket_ || !buf)
    {
        return;
    }

    // 一次 SendV 能带的块数有上限，超了先切片分批送 —— 沿用 WSConnection
    // ::_SendV 的既有写法。
    while (buf->GetBlockCount() > BC_SOCKET_MAXSCATTERGATHER)
    {
        BCBuffer* slice = new BCBuffer();
        buf->Extract(slice, BC_SOCKET_MAXSCATTERGATHER);
        result = socket_->SendV(slice, GetTask(), _SendDoneCallback, this);
        if (result == BC_R_SUCCESS || result == BC_R_INPROGRESS)
        {
            pending_send_++;
            result = BC_R_SUCCESS;
        }
        else
        {
            delete slice;
            _Fail(result, "TCP send 失败");
            return;
        }
    }

    if (buf->RemainingLength() > 0)
    {
        result = socket_->SendV(buf->RefClone(), GetTask(),
                                _SendDoneCallback, this);
        if (result == BC_R_SUCCESS || result == BC_R_INPROGRESS)
        {
            pending_send_++;
        }
        else
        {
            _Fail(result, "TCP send 失败");
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— 关闭
///////////////////////////////////////////////////////////////////////////////

void TcpChannel::_SetState(State next)
{
    State cur = state_.load();
    if (cur == next)
    {
        return;
    }
    if (!IsLegalTransition(cur, next))
    {
        // 只记录不阻断：真出现非法迁移时，硬拦下来只会把连接卡死在半路，
        // 日志才是能定位问题的东西。
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel] 非法状态迁移 %s -> %s", _StateName(cur),
             _StateName(next));
    }
    state_.store(next);
}

void TcpChannel::_Fail(BCRESULT result, const std::string& reason)
{
    if (closing_)
    {
        // 幂等：第一个原因才是真原因，后面的连锁错误全忽略。
        //
        // ⚠️ 但**必须再驱动一次收尾**，不能直接 return。
        // 典型漏法（曾经真的漏了）：Close() 先取号再投递 lambda，最后一个在途
        // IO 回调恰好在"号还没还"的那一瞬间跑完 —— _MaybeFinishClose() 被
        // pending_tasks_ > 0 挡下；等 Close 的 lambda 还完号再调 _Fail，如果
        // 这里直接 return，就没有任何人会再推一下，通道永久卡在 CLOSING，
        // 调用方永远等不到 OnChannelClosed。
        //
        // 通则：凡是"还掉了一个 token"的路径，都必须重新驱动一次收尾。让幂等
        // 分支自己兜底，比在每个调用点补一行更不容易漏。
        _MaybeFinishClose();
        return;
    }
    closing_      = true;
    close_result_ = result;
    close_reason_ = reason;

    if (result != BC_R_SUCCESS)
    {
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel] 连接结束（%s:%u，停在 %s）：result=%d %s",
             config_.host.c_str(), (unsigned)config_.port,
             _StateName(state_.load()), (int)result, reason.c_str());
    }

    _SetState(TCPCH_CLOSING);
    _CancelIo();
    _MaybeFinishClose();
}

void TcpChannel::_CancelIo()
{
    if (connect_timer_ > 0)
    {
        UnscheduleTask(connect_timer_);
    }

    // 在途的 DNS 解析取消不掉（getaddrinfo 没有超时），所以直接撒手：置
    // abandoned 并把 pending_dns_ 归零，收尾流程不再等它。否则一次卡死的
    // 系统解析会把 connectTimeoutMs 彻底架空。
    _AbandonDns();

    // 刻意不发 TLS close_notify：SSL_shutdown 会经 OnSSLWrite 再往 socket 上
    // 压一次发送，把刚要归零的 pending_send_ 重新顶起来，让关闭流程多绕一圈。
    // TCP FIN 已经足够，上层协议（WS Close 帧 / HTTP Content-Length）自己有
    // 干净的收尾手段。

    if (!socket_)
    {
        return;
    }
    BCTask* task = GetTask();
    if (pending_connect_ > 0)
    {
        socket_->Cancel(task, BC_SOCKCANCEL_CONNECT);
    }
    if (pending_send_ > 0)
    {
        socket_->Cancel(task, BC_SOCKCANCEL_SEND);
    }
    if (pending_recv_ > 0)
    {
        socket_->Cancel(task, BC_SOCKCANCEL_RECV);
    }
}

void TcpChannel::_MaybeFinishClose()
{
    if (!closing_ || closed_notified_)
    {
        return;
    }
    // ⚠️ 栈上还有 SSLayer 的帧时绝对不能往下走：_Cleanup() 会销毁那个 SSLayer
    // 和它的 SSL*，返回后 SSLayer::ReadFromSSL 继续读 ssl_state_ / 调 SSL_read
    // 就是 heap-use-after-free。守卫析构（深度归零）时会补调一次本函数。
    if (ssl_depth_ > 0)
    {
        return;
    }
    // 所有在途异步操作归零之前不能通知调用方：OnChannelClosed 就是"现在销毁
    // 我是安全的"这一承诺本身。
    if (pending_dns_ || pending_connect_ || pending_recv_ || pending_send_)
    {
        return;
    }

    {
        // 跨线程投递进来的 Send/Close lambda 也必须先跑完。取号与通知互斥，
        // 见 _AcquirePendingTask 的注释。
        BCSpinMutex::Owner lock(guard_lock_);
        if (closed_notified_ || pending_tasks_ > 0)
        {
            return;
        }
        closed_notified_ = true;
    }

    _SetState(TCPCH_CLOSED);
    _Cleanup();

    // ⚠️ 这里**不能**直接回调 OnChannelClosed。
    //
    // BCEventQueue::Detach() 会 m_pTask->Shutdown()，而 Shutdown() 是把注册好
    // 的 _ShutdownCallback 事件塞进本 BCTask 的事件队列（BCTask.cpp:129-134），
    // 由同一个 worker 在当前事件返回之后再派发 —— 也就是说，Detach() 之后仍然
    // 有一个回调在路上，它会调 OnEventProcShutdown()。如果我们在这里就通知
    // 调用方"可以销毁了"，那个迟到的 _ShutdownCallback 就会打在已释放的对象上
    // （ASan 实证：SEGV in BCEventQueue::_ShutdownCallback）。
    //
    // 所以把通知挪到 OnEventProcShutdown() 里：那才是真正的"最后一个回调"，
    // 之后 _ShutdownCallback 只剩一句 pEvent->Destroy()，不再碰本对象。
    detached_ = true;
    Detach();
}

void TcpChannel::OnEventProcShutdown()
{
    // BCTask 关停流程里的最后一个回调。走到这里说明：
    //   * 所有 socket 异步 IO 已完成或取消（pending_* 早就归零）
    //   * 跨线程投递的 Send/Close lambda 已全部执行完（pending_tasks_ 归零）
    //   * 事件队列已经 Detach，不会再有任何事件派发到本对象
    // 现在通知调用方"销毁我是安全的"，才名副其实。
    if (!closed_notified_)
    {
        return;     // 不是走关闭流程进来的（理论上不会发生），什么都不做
    }

    ITcpChannelHandler* handler = handler_;
    handler_ = NULL;
    if (handler)
    {
        // ⚠️ 本调用之后不许再碰任何成员：调用方可以在这里就把 TcpChannel 销毁。
        handler->OnChannelClosed(close_result_, close_reason_);
    }
}

void TcpChannel::_Cleanup()
{
    if (socket_)
    {
        socket_->Detach(&socket_);
        socket_ = NULL;
    }
    ssl_layer_.reset();
    if (ssl_)
    {
        SSL_free(ssl_);
        ssl_ = NULL;
    }
    if (ssl_ctx_)
    {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = NULL;
    }
}

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel —— BCSocket 异步回调
///////////////////////////////////////////////////////////////////////////////

void TcpChannel::_ConnectDoneCallback(BCTask* task, BCTaskEvent* pEvent)
{
    BCSockOCEvent* sockEv = (BCSockOCEvent*)pEvent;
    TcpChannel*    _this  = (TcpChannel*)pEvent->ev_arg;

    UNUSED(task);
    ASSERT(_this != NULL);

    if (_this->pending_connect_ > 0)
    {
        _this->pending_connect_--;
    }
    _this->_OnConnectDone(sockEv->result);
    // ⚠️ _MaybeFinishClose 可能同步回调 OnChannelClosed，之后不许再碰 _this。
    _this->_MaybeFinishClose();

    pEvent->Destroy();
}

void TcpChannel::_RecvDoneCallback(BCTask* task, BCTaskEvent* pEvent)
{
    BCSockEvent* sockEv = (BCSockEvent*)pEvent;
    TcpChannel*  _this  = (TcpChannel*)sockEv->ev_arg;

    UNUSED(task);
    ASSERT(_this != NULL);

    if (_this->pending_recv_ > 0)
    {
        _this->pending_recv_--;
    }

    switch (sockEv->result)
    {
    case BC_R_SUCCESS:
        if (sockEv->n == 0)
        {
            // 成功但零字节：按 EOF 处理，免得在这里空转重挂 recv。
            _this->_OnPeerClosed();
            break;
        }
        _this->_OnRecvDone(sockEv->n);
        // _OnRecvDone 可能已经把连接判死（TLS 出错 / 上层要求关闭），
        // _TcpRecv 内部会看 closing_ 自行退出。
        _this->_TcpRecv();
        break;

    case BC_R_EOF:
        // recv 返回 0 = 对端发了 FIN。是不是"正常关闭"要看停在哪个状态、
        // 以及 TLS 有没有收到 close_notify，判定见 _OnPeerClosed。
        _this->_OnPeerClosed();
        break;

    case BC_R_CANCELED:
        // 关闭流程里我们自己取消的，不覆盖已有的关闭原因。
        break;

    default:
        _this->_Fail(sockEv->result, "TCP recv 失败");
        break;
    }

    // ⚠️ 之后不许再碰 _this。recv_event_ 是 _this 的成员，所以这里也不能
    // 像 send 那样去 Destroy 事件——它本来就不是 new 出来的。
    _this->_MaybeFinishClose();
}

void TcpChannel::_SendDoneCallback(BCTask* task, BCTaskEvent* pEvent)
{
    BCSockEvent* sockEv = (BCSockEvent*)pEvent;
    TcpChannel*  _this  = (TcpChannel*)sockEv->ev_arg;
    ScopedPointer<BCSockEvent> evDtor(sockEv);
    ScopedPointer<BCBuffer>    bufDtor(sockEv->bufferlist);

    UNUSED(task);
    ASSERT(_this != NULL);

    if (_this->pending_send_ > 0)
    {
        _this->pending_send_--;
    }

    if (sockEv->result == BC_R_SUCCESS)
    {
        _this->_OnSendDone();
    }
    else if (sockEv->result != BC_R_CANCELED)
    {
        _this->_Fail(sockEv->result, "TCP send 失败");
    }

    // ⚠️ 之后不许再碰 _this（ScopedPointer 析构只碰事件与缓冲，不碰 _this）。
    _this->_MaybeFinishClose();
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
