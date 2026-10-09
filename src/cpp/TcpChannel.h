///////////////////////////////////////////////////////////////////////////////
// file   : TcpChannel.h
// author : anto
//
// TCP + TLS 连接底座。把 DNS → socket → 绑网卡 → 路由复核 → connect →
// TLS 握手 → 读写 串成一个状态机，WSConnection 与 HttpConnection 共用。
//
// 上层协议实现（WS 帧 / HTTP 报文）不碰任何 socket 代码。
//
// ---------------------------------------------------------------------------
// 线程模型
// ---------------------------------------------------------------------------
// TcpChannel 自己是一个 BCEventQueue（Open() 里创建，跑在 Runtime 的 task 线程
// 池上）。除下面两处以外，所有状态都只在这条事件循环线程上读写，因此内部不加锁：
//
//   * Open / Send / Close 允许在任意线程调用，它们只做参数校验，真正的动作
//     一律 PostTask 回本对象的事件循环执行；与关闭并发也是安全的（见下面
//     生命周期契约第 5 条）。
//   * DNS 解析（DnsResolver::Resolve 是同步阻塞的）派到 DnsExecutor 的专属
//     线程池上跑（见 TcpChannel.cpp 里 DnsExecutor 上方的注释），跑完再
//     PostTask 回本对象的事件循环。这里既不能占本对象的事件循环（会把超时
//     定时器和 socket 回调一起堵死），也不能借 Runtime 的队列（那条线程和
//     SMP 服务端的 UDPSender 共用，一次慢 DNS 会停掉整条 UDP 流水线）。
//
// 另有一处例外与线程无关但同样要小心：SSLayer 的四个回调（OnSSLReady /
// OnSSLError / OnSSLFinished / OnSSLWrite）是从 SSLayer 自己的栈帧里发出来的，
// 属于**同线程重入**。收尾时的 _Cleanup() 会销毁 SSLayer 与 SSL*，所以凡是
// 调进 SSLayer 的地方都套了 SslReentryGuard，把销毁推迟到栈帧退回来之后。
//
// ---------------------------------------------------------------------------
// 生命周期契约（必读，违反会 UAF）
// ---------------------------------------------------------------------------
// 1. Open() 返回非 BC_R_SUCCESS 时不会有任何回调，对象可以立即销毁。
// 2. Open() 返回 BC_R_SUCCESS 之后，一切结果都通过 ITcpChannelHandler 回调，
//    且 OnChannelClosed 保证恰好回调一次。回调时序不做承诺 —— 极端情况下
//    OnChannelClosed 可以先于 Open() 返回（事件队列在另一条线程上跑）。
// 3. **只有收到 OnChannelClosed 之后销毁 TcpChannel 才是安全的** ——
//    那一刻所有异步 socket IO、DNS 任务、跨线程投递的 Send/Close lambda 都
//    已完成或取消，事件队列也已经 Detach，没有任何在途回调还持有 this。
//    OnChannelClosed 刻意是从 OnEventProcShutdown()（BCTask 关停流程的最后
//    一个回调）里发出来的，就是为了让这句话成立：早于此发出的话，
//    BCEventQueue::Detach() 排进队列的 _ShutdownCallback 还会再打回来一次。
// 4. 提前销毁（还没收到 OnChannelClosed 就 delete）是未定义行为，析构函数会
//    打 _ERROR_ 日志把违规现场记下来，但救不回来。
// 5. Send() / Close() 可以和关闭流程并发，不需要调用方自己加锁：两者都会先
//    向 guard_lock_ "取号"，取号成功才投递；OnChannelClosed 只会在所有号都
//    还清之后才发出去。已经发过 OnChannelClosed 之后再调 Send()，会直接拿到
//    BC_R_NOTCONNECTED 而不是投递到一个可能已被销毁的对象上。
//
// ---------------------------------------------------------------------------
// 物理网卡的选择方式
// ---------------------------------------------------------------------------
// 采用「本模块自己探测」而非「调用方传入」：
//   * 优先用调用方在 TcpChannelConfig::ifIndex / androidNetHandle 里显式指定的
//     网卡（Android 只能走这条路，见下）；
//   * 没指定时用 tt_netmon_query_default_ifindex_ex((int)policy) 自动探测，
//     与 SMPConnector / DnsResolver 的既有用法完全一致。该符号只在定义了
//     TT_HAS_PATH_MONITOR 的构建（NAPI / iOS）里可用；Android JNI 不定义它，
//     必须由调用方从 Java NetworkCallback 拿到 handle 后填进 config。
///////////////////////////////////////////////////////////////////////////////

#ifndef TT_TCP_CHANNEL_H
#define TT_TCP_CHANNEL_H

#include <atomic>
#include <list>
#include <memory>
#include <string>

#include <openssl/ssl.h>

#include <BC/BCBuffer.h>
#include <BC/BCEventQueue.h>
#include <BC/BCSockAddr.h>
#include <BC/BCSocket.h>
#include <BC/BCStream.h>

#include "DnsResolver.h"
#include "SSLayer.h"
#include "TlsContext.h"
#include "VpnPolicy.h"

using namespace BC;

// 与 SMPacket.h:16 的定义逐字一致 —— 同一类型的重复 typedef 在 C++ 里合法。
// 这里重新写一遍而不是 include SMPacket.h，是为了不把 SMP/QUIC 那一侧的头文件
// 拖进 HTTP/WS 这条独立的协议栈。
typedef std::shared_ptr<BCBuffer> BufferPtr;

///////////////////////////////////////////////////////////////////////////////
// class : ITcpChannelHandler
///////////////////////////////////////////////////////////////////////////////

class ITcpChannelHandler
{
public:
    ITcpChannelHandler() {}
    virtual ~ITcpChannelHandler() {}

    // TCP 已连上且（若 tls）TLS 握手已完成，可以开始收发
    virtual void OnChannelReady()                                = 0;

    // 本 handler 被 Adopt() 换下了，此后通道的任何回调都与它无关。
    //
    // 只有连接池那个"看守 handler"用得上：它需要一个确定的退场信号，才能在
    // 不与 OnChannelClosed 抢所有权的前提下销毁自己。对它来说这两个回调是互斥
    // 且必有其一的——要么连接死了（OnChannelClosed），要么连接被下一个请求
    // 接走了（本回调）。普通 handler 不会收到，默认空实现。
    virtual void OnChannelDetached()                             {}
    // 收到明文数据（TLS 已解密）
    virtual void OnChannelData(const void* data, size_t size)    = 0;
    // 连接结束。result == BC_R_SUCCESS 表示对端正常关闭（或调用方主动 Close）
    virtual void OnChannelClosed(BCRESULT result,
                                 const std::string& reason)      = 0;
};

///////////////////////////////////////////////////////////////////////////////
// struct : TcpChannelConfig
///////////////////////////////////////////////////////////////////////////////

struct TcpChannelConfig
{
    std::string host;                   // 域名或 IP 字面量。TLS 的 SNI /
                                        // hostname 校验都用它，即使填了
                                        // resolvedIp 也必须给
    uint16_t    port             = 0;
    bool        tls              = false;
    std::string resolvedIp;             // 非空则跳过 DNS。必须是 IP 字面量，
                                        // 否则 Open() 直接返回 BC_R_INVALIDARG
    TTVpnPolicy policy           = TT_VPN_POLICY_UNSET;

    // 调用方显式指定要绑的网卡；0 表示交给本模块自动探测。
    // Android 上没有任何自动探测手段（JNI 构建不定义 TT_HAS_PATH_MONITOR），
    // 必须由 Java NetworkCallback 把 network handle 填进 androidNetHandle，
    // 否则 force-physical 一定失败。
    uint32_t    ifIndex          = 0;
    uint64_t    androidNetHandle = 0;

    DnsConfig   dns;
    TlsConfig   tls_cfg;
    // 覆盖 DNS + connect + TLS 握手的总时长。超时以 BC_R_CONNECT_TIMEOUT 结束
    uint32_t    connectTimeoutMs = 10000;
    void*       loggerCtx        = nullptr;
};

///////////////////////////////////////////////////////////////////////////////
// struct : TcpDnsRequest —— 一次 DNS 解析的上下文
//
// 刻意用 shared_ptr 让它**独立于 TcpChannel 存活**，理由是 DNS 解析可能根本
// 停不下来：policy == OS 时 DnsResolver::Resolve 走的是 getaddrinfo
// （DnsResolver.cpp:474），**没有任何超时**，实测可以阻塞几十秒；只有
// policy != OS 的自建 UDP 查询才有 timeoutMs/server 的上界。
//
// 如果 DNS worker 上的任务直接捕获裸 this、而通道又必须等 pending_dns_ 归零
// 才敢发 OnChannelClosed，那么一次卡死的 getaddrinfo 就会把 connectTimeoutMs
// 彻底架空——业务设了 10 秒，实际得等几十秒。
//
// 所以改成：通道超时/关闭时调 _AbandonDns()，置 abandoned、把 channel 置空、
// 并**立刻把 pending_dns_ 归零**，于是收尾流程照常按时走完；DNS worker 最终
// 跑完，看到 abandoned 就把结果丢掉，一个通道成员都不碰。代价只是那条 worker
// 时隙被占着直到 getaddrinfo 自己返回——有上界，线程本来也是常驻的。
//
// lock 同时保护 abandoned 与 channel：worker 必须在持锁状态下完成
// 「查 abandoned → 取号 → PostTask」这一整段，才能保证 channel 指针在使用期间
// 一定有效（放弃动作与它互斥，而放弃严格早于 OnChannelClosed）。
///////////////////////////////////////////////////////////////////////////////

class TcpChannel;

struct TcpDnsRequest
{
    BCSpinMutex lock;
    bool        abandoned = false;
    TcpChannel* channel   = NULL;   // 仅在持 lock 且 abandoned 为 false 时可用
    std::string host;
    DnsConfig   cfg;
};

///////////////////////////////////////////////////////////////////////////////
// class : TcpChannel
///////////////////////////////////////////////////////////////////////////////

class TcpChannel
    : public BCEventQueue
    , public ISSLayerHandler
{
public:
    typedef enum {
        TCPCH_IDLE           = 0,
        TCPCH_RESOLVING      = 1,
        TCPCH_PINNING        = 2,
        TCPCH_CONNECTING     = 3,
        TCPCH_TLS_HANDSHAKE  = 4,
        TCPCH_READY          = 5,
        TCPCH_CLOSING        = 6,
        TCPCH_CLOSED         = 7,
    } State;

    TcpChannel();
    ~TcpChannel() override;

    // 参数校验同步进行（失败即返回，不产生任何回调）；其余全部异步。
    BCRESULT    Open(const TcpChannelConfig& cfg, ITcpChannelHandler* handler);
    // 只有 READY 状态下才接受发送。数据的实际落地是异步的，返回值只表示
    // "已排进事件循环"，发送失败会以 OnChannelClosed 体现。
    BCRESULT    Send(BufferPtr buf);
    // 幂等。异步生效，完成时回调 OnChannelClosed(BC_R_SUCCESS, ...)
    void        Close();

    // ------------------------------------------------------------------
    // 连接复用（keep-alive）
    //
    // 一次请求一条连接的代价是每次都重付 TCP 握手 + 完整 TLS 握手。下面这对
    // 接口让一条已经建立的连接在两个 handler 之间转手：上一个请求收完响应就
    // Park()，下一个请求 Adopt() 接手，直接从发报文开始。
    // ------------------------------------------------------------------

    // 把通道从当前 handler 上摘下来，连接本身保持建立。摘下之后到 Adopt()
    // 之前，通道不会向任何人回调（期间到来的字节见 poisoned 的说明）。
    //
    // ⚠️ 只能在通道自己的回调线程上调用（OnChannelReady / OnChannelData 的
    // 栈内）—— handler_ 是由队列线程独占的裸指针，换线程写就是数据竞争。
    // 通道没就绪时本调用什么都不做：没建立的连接没有复用价值。
    //
    // watcher 非空时由它接管这段无主期：Park 期间通道只会给它派发
    // OnChannelClosed（对端关掉了连接），以及被 Adopt() 换下时的
    // OnChannelDetached。数据不会转给它——Park 期间来的字节只用来把通道标成
    // 不可复用（见 poisoned_）。连接池拿它当池中连接的所有者，没有它，一条
    // 躺在池里的连接死掉时没有任何人收得到信号。
    void        Park(ITcpChannelHandler* watcher = NULL);

    // 让一条 Park 过的通道接手新 handler。可从任意线程调用。
    //
    // 返回 BC_R_SUCCESS：已排进队列，随后必定在通道线程上收到 **恰好一个**
    // 回调 —— 接手成功是 OnChannelReady()（与新建连接完全一致，调用方不必
    // 区分两者），排队期间连接失效则是 OnChannelClosed()。
    // 返回其它值：通道不可复用，**不产生任何回调**，调用方改走新建连接。
    BCRESULT    Adopt(ITcpChannelHandler* handler);

    // 这条 Park 过的通道还值不值得交给下一个请求。Adopt() 内部会再查一次
    // （并且是在队列线程上查，堵住投递期间失效的窗口），这里单独暴露是给
    // 连接池取用前做廉价筛选、以及归还时判断该入池还是该关掉。
    bool        IsParkedAndUsable() const;

    // Open() 同步失败时的现场描述；成功时为空串。
    //
    // ⚠️ 存在的理由与 WSConnection::LastErrorMessage() 一样：同步失败发生在通道
    // 建起来之前，走不到 _Fail()，文案原本只落在日志里 —— 绑定层能交给业务的
    // 就只剩一个错误码。force-physical 拿不到物理网卡
    // （BC_R_NO_PHYSICAL_INTERFACE）尤其如此，那段文案是排查的唯一依据。
    //
    // 只在 Open() 返回后、下一次 Open() 之前读有意义（下一次 Open 会清空它）。
    const std::string&
                LastOpenError() const { return open_error_; }

    // 诊断信息，业务据此判断是否真的走了物理网卡
    std::string PeerIp()       const { return peer_ip_; }
    uint32_t    BoundIfIndex() const { return bound_ifindex_; }
    std::string PinMethod()    const { return pin_method_; }
    State       GetState()     const { return state_.load(); }

    // ------------------------------------------------------------------
    // 下面两个是给单测用的纯函数，不碰任何成员，也不发起任何 IO。
    // ------------------------------------------------------------------

    // 状态机的合法迁移表。TcpChannel 内部每次 _SetState 都过这张表。
    static bool IsLegalTransition(State from, State to);
    // 把 IP 字面量 + 端口解析成 BCSockAddrS。非法字面量返回 false。
    static bool ParseIpLiteral(const std::string& ip, uint16_t port,
                               BCSockAddrS& out);

private:
    DECLARE_NO_COPY_CLASS(TcpChannel);

    // SSLayer 重入守卫。
    //
    // SSLayer 的所有回调（OnSSLReady / OnSSLError / OnSSLFinished /
    // OnSSLWrite）都是从 SSLayer 自己的栈帧里发出来的，而这些回调有可能走到
    // _Fail() → _MaybeFinishClose() → _Cleanup()，那里会 ssl_layer_.reset()
    // 和 SSL_free(ssl_) —— 销毁的正是当前栈上还在执行的那个 SSLayer / SSL，
    // 返回后 SSLayer::ReadFromSSL 继续读 ssl_state_、继续 SSL_read，就是
    // heap-use-after-free（ASan 实证，见 task-5 review）。
    //
    // 四个 pending 计数只保护"调用方持有的 this"，管不了 SSLayer 对自己的
    // 重入，所以另开一个深度计数：凡是调进 SSLayer 的地方都套上这个守卫，
    // 深度不为 0 时 _MaybeFinishClose() 直接推迟，等守卫析构（栈帧已经退回
    // 到 SSLayer 之外）再补一次。
    class SslReentryGuard
    {
    public:
        explicit SslReentryGuard(TcpChannel* ch) : ch_(ch)
        {
            ch_->ssl_depth_++;
        }
        ~SslReentryGuard()
        {
            if (--ch_->ssl_depth_ == 0)
            {
                // 这里会做真正的清理（_Cleanup + Detach）。OnChannelClosed
                // 本身是稍后由 OnEventProcShutdown() 发出的，不在本调用栈上，
                // 但清理之后成员已经不可用，所以守卫仍然必须是所在作用域里
                // 最后一个析构的东西。
                ch_->_MaybeFinishClose();
            }
        }
    private:
        DECLARE_NO_COPY_CLASS(SslReentryGuard);
        TcpChannel* ch_;
    };
    friend class SslReentryGuard;

    // Override BCEventQueue interface —— BCTask 关停流程里的最后一个回调，
    // OnChannelClosed 就是在这里发出去的，理由见 .cpp 里 _MaybeFinishClose
    // 末尾的注释。
    void        OnEventProcShutdown() override;

    // Override ISSLayerHandler interfaces
    int         OnSSLWrite(const void* data, size_t len) override;
    void        OnRecvDataFromSSL(const void* data, size_t size) override;
    void        OnSSLReady() override;
    void        OnSSLFinished() override;
    void        OnSSLError(const char* file, int line,
                           const char* data, int flags) override;

    // 跨线程投递 lambda 前后的"取号 / 还号"。见 .cpp 里的实现注释。
    bool        _AcquirePendingTask();
    void        _ReleasePendingTask();

    // 状态机各步骤，除 _RunDnsRequest 外全部跑在本对象的事件循环线程上
    uint32_t    _PickPhysicalIfIndex();
    bool        _HasBindTarget() const;
    // ⚠️ 跑在 DnsExecutor 的专属线程上，不碰任何通道成员（见 TcpDnsRequest）
    static void _RunDnsRequest(std::shared_ptr<TcpDnsRequest> req);
    void        _AbandonDns();
    void        _OnResolveDone(const DnsResult& result);
    void        _EnterPinning();
    BCRESULT    _PinSocket();
    BCRESULT    _DecidePinBeforeConnect();
    BCRESULT    _SetupTls();
    void        _OnConnectDone(BCRESULT result);
    void        _OnRecvDone(size_t size);
    void        _OnSendDone();
    void        _TcpRecv();
    void        _SendRaw(BufferPtr buf);
    BCRESULT    _VerifySpkiPin();
    void        _DumpTlsDiagnostics(const char* stage);
    void        _EnterReady();
    void        _OnPeerClosed();
    void        _Fail(BCRESULT result, const std::string& reason);
    void        _SetState(State next);
    void        _CancelIo();
    void        _MaybeFinishClose();
    void        _Cleanup();

    static void _ConnectDoneCallback(BCTask*, BCTaskEvent*);
    static void _RecvDoneCallback(BCTask*, BCTaskEvent*);
    static void _SendDoneCallback(BCTask*, BCTaskEvent*);

private:
    TcpChannelConfig            config_;
    ITcpChannelHandler      *   handler_        = NULL;
    std::atomic<State>          state_;
    bool                        queue_created_  = false;
    bool                        detached_       = false;

    // 已 Park、正在等下一个 handler 接手。原子是因为 Adopt() /
    // IsParkedAndUsable() 会从别的线程读它。
    std::atomic<bool>           parked_{false};
    // Park 期间收到过字节。HTTP/1.1 上这说明报文边界已经对不齐了（服务端多发
    // 了东西，或者我们把边界算错了），继续复用会把这段字节当成下一条响应的
    // 开头。一旦置上就不再清除：这条连接只能丢弃。
    std::atomic<bool>           poisoned_{false};

    BCSocket                *   socket_         = NULL;
    bool                        is_ipv6_        = false;
    BCSockAddrS                 peer_addr_;
    socklen_t                   peer_addr_len_  = 0;
    std::string                 peer_ip_;

    uint32_t                    bound_ifindex_  = 0;
    std::string                 pin_method_;
    // Open() 同步失败的现场描述。只有 Open() 这条线程会碰它（Open 返回前不会
    // 有任何回调，返回非成功时更是一个回调都不会有），所以不用加锁。
    std::string                 open_error_;

    SSL_CTX                 *   ssl_ctx_        = NULL;
    SSL                     *   ssl_            = NULL;
    std::unique_ptr<SSLayer>    ssl_layer_;

    int32_t                     connect_timer_  = 0;
    BCSockEvent                 recv_event_;
    char                        recv_buffer_[16384];

    // 在途的 DNS 请求。只在事件循环线程上读写；worker 那边持有同一个
    // shared_ptr 的另一份引用，两边靠 TcpDnsRequest::lock 交接。
    std::shared_ptr<TcpDnsRequest> dns_request_;

    // 对端发来过 close_notify —— 只有它为真才算 TLS 层面干净的收尾。
    // READY 之后直接 FIN（没有 close_notify）是经典的 TLS truncation 攻击，
    // 必须能和"对端正常关闭"区分开。
    bool                        tls_shutdown_clean_ = false;

    // 在途异步操作计数。全部归零之前不允许发 OnChannelClosed —— 否则调用方
    // 会在还有回调持有 this 的时候把对象销毁掉。
    // 只在本对象的事件循环线程上读写，所以不需要原子。
    uint32_t                    pending_dns_        = 0;
    uint32_t                    pending_connect_    = 0;
    uint32_t                    pending_recv_       = 0;
    uint32_t                    pending_send_       = 0;

    // SSLayer 重入深度，见 SslReentryGuard。同样只在事件循环线程上读写。
    uint32_t                    ssl_depth_          = 0;

    // Send() / Close() 从别的线程投递进来、但还没被事件循环取走的 lambda
    // 计数。它们按值捕获 this，所以在跑完之前同样不能让 OnChannelClosed
    // 发出去。跨线程读写，必须由 guard_lock_ 保护 —— 光用原子计数堵不住
    // "检查通过之后、自增之前，队列线程刚好完成通知并被调用方销毁"这个窗口。
    BCSpinMutex                 guard_lock_;
    uint32_t                    pending_tasks_      = 0;

    bool                        closing_            = false;
    bool                        closed_notified_    = false;   // 由 guard_lock_ 保护
    BCRESULT                    close_result_       = BC_R_SUCCESS;
    std::string                 close_reason_;
};

#endif // TT_TCP_CHANNEL_H

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
