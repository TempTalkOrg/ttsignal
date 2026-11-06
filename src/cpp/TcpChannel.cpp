





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





namespace {


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

}   













































namespace {

class DnsExecutor
{
public:
    
    
    
    static const int kWorkers = 4;

    
    
    
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

    ~DnsExecutor() {}   

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

}   





bool TcpChannel::IsLegalTransition(State from, State to)
{
    
    
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
        
        return to == TCPCH_CONNECTING;
    case TCPCH_CONNECTING:
        
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





TcpChannel::TcpChannel()
    : state_(TCPCH_IDLE)
{
    memzero(&peer_addr_, sizeof(peer_addr_));
    memzero(recv_buffer_, sizeof(recv_buffer_));
}

TcpChannel::~TcpChannel()
{
    
    
    
    if (pending_dns_ || pending_connect_ || pending_recv_ || pending_send_)
    {
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel] 析构时仍有在途异步操作（dns=%u connect=%u recv=%u "
             "send=%u）。调用方必须等到 OnChannelClosed 之后再销毁 TcpChannel。",
             pending_dns_, pending_connect_, pending_recv_, pending_send_);
    }

    
    
    
    
    
    
    
    
    
    
    
    _AbandonDns();

    _Cleanup();

    if (queue_created_ && !detached_)
    {
        
        
        
        
        
        
        
        
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel] 析构时事件队列还没走完关闭流程（state=%d）。"
             "调用方必须先 Close() 并等到 OnChannelClosed 再销毁 TcpChannel。",
             (int)state_.load());
        Detach(true);
    }
}





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

    
    
    
    
    
    
    
    
    
    
    
    if (config_.policy != TT_VPN_POLICY_OS)
    {
        bound_ifindex_ = _PickPhysicalIfIndex();
    }

    
    
    
    
    if (config_.policy == TT_VPN_POLICY_FORCE_PHYSICAL && !_HasBindTarget())
    {
        
        
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
        
        
        LogQ(config_.loggerCtx, _ERROR_,
             "[TcpChannel] 无法创建 DNS 执行线程池，连接失败");
        (void)_AcquirePendingTask();
        PostTask([this]() {
            _ReleasePendingTask();
            _Fail(BC_R_DNS_FAILED, "无法创建 DNS 执行线程池");
        });
        return BC_R_SUCCESS;
    }

    
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
    
    
    
    
    
    
    
    
    BCSpinMutex::Owner lock(guard_lock_);
    if (closed_notified_)
    {
        return false;   
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
            
            _MaybeFinishClose();
            return;
        }
        if (ssl_layer_)
        {
            
            
            
            SslReentryGuard guard(this);
            ssl_layer_->WriteToSSL(buf);
        }
        else
        {
            _SendRaw(buf);
            
            
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
        return;     
    }
    PostTask([this]() {
        _ReleasePendingTask();
        _Fail(BC_R_SUCCESS, "调用方主动关闭");
    });
}





void TcpChannel::Park(ITcpChannelHandler* watcher)
{
    
    
    if (state_.load() != TCPCH_READY)
    {
        return;
    }

    
    
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
    
    
    
    if (!_AcquirePendingTask())
    {
        return BC_R_NOTCONNECTED;
    }

    PostTask([this, handler]() {
        _ReleasePendingTask();

        
        
        
        if (!IsParkedAndUsable())
        {
            
            
            
            
            
            
            
            
            
            handler->OnChannelAdoptFailed();
            
            
            _Fail(BC_R_SUCCESS, "复用的连接在接手前已失效");
            return;
        }

        parked_.store(false);

        
        
        
        ITcpChannelHandler* watcher = handler_;
        handler_ = handler;
        if (watcher)
        {
            watcher->OnChannelDetached();
        }

        
        
        
        handler_->OnChannelReady();
    });
    return BC_R_SUCCESS;
}





uint32_t TcpChannel::_PickPhysicalIfIndex()
{
    
    
    if (config_.ifIndex != 0)
    {
        return config_.ifIndex;
    }

#if defined(TT_HAS_PATH_MONITOR)
    
    
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





void TcpChannel::_RunDnsRequest(std::shared_ptr<TcpDnsRequest> req)
{
    
    

    
    {
        BCSpinMutex::Owner lock(req->lock);
        if (req->abandoned || !req->channel)
        {
            return;
        }
    }

    DnsResult r = DnsResolver::Resolve(req->host, req->cfg);

    
    
    
    
    BCSpinMutex::Owner lock(req->lock);
    if (req->abandoned || !req->channel)
    {
        return;     
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





void TcpChannel::_EnterPinning()
{
    BCRESULT result;

    _SetState(TCPCH_PINNING);

    is_ipv6_       = (peer_addr_.type.sa.sa_family == AF_INET6);
    peer_addr_len_ = is_ipv6_ ? (socklen_t)sizeof(struct sockaddr_in6)
                              : (socklen_t)sizeof(struct sockaddr_in);
    peer_ip_       = _AddrToIpText(peer_addr_);

    
    
    if (config_.tls)
    {
        result = _SetupTls();
        if (result != BC_R_SUCCESS)
        {
            return;     
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
        
        
        socket_ = NULL;
        _Fail(result, "创建 TCP socket 失败");
        return;
    }

    
    
    
    
    
    
    result = _DecidePinBeforeConnect();
    if (result != BC_R_SUCCESS)
    {
        return;         
    }

    result = _PinSocket();
    if (result != BC_R_SUCCESS)
    {
        return;         
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
        
        
        return BC_R_SUCCESS;
    }

    TTPinRequest pin;
    memset(&pin, 0, sizeof(pin));
    pin.fd               = socket_->GetFd();
    pin.ifIndex          = bound_ifindex_;
    pin.androidNetHandle = config_.androidNetHandle;
    pin.ipv6             = is_ipv6_ ? 1 : 0;
    pin.isTcp            = 1;      
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

    
    
    
    
    
    
    
    
    if (config_.policy == TT_VPN_POLICY_FORCE_PHYSICAL)
    {
        if (scopedIf == TT_ROUTE_IFINDEX_UNKNOWN || scopedIf == bound_ifindex_)
        {
            return BC_R_SUCCESS;
        }

        char msg[520];
        if (peerClass != TT_PEER_CLASS_GLOBAL)
        {
            
            
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

    
    
    bound_ifindex_ = 0;
#ifdef OS_ANDROID
    
    
    config_.androidNetHandle = 0;
#endif
    pin_method_.clear();
    return BC_R_SUCCESS;
}





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
    
    
    
    
    
    
    if (!ssl_)
    {
        return;
    }

    long vr = SSL_get_verify_result(ssl_);
    LogQ(config_.loggerCtx, _ERROR_,
         "[TcpChannel] TLS %s 失败：host=%s peer=%s verify_result=%ld (%s)",
         stage, config_.host.c_str(), peer_ip_.c_str(), vr,
         X509_verify_cert_error_string(vr));

    
    
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





int TcpChannel::OnSSLWrite(const void* data, size_t len)
{
    if (!data || len == 0)
    {
        return 0;
    }
    
    
    
    
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
    
    if (!data || size == 0)
    {
        return;
    }
    if (parked_.load())
    {
        
        
        
        poisoned_.store(true);
        return;
    }
    if (!handler_)
    {
        return;
    }
    if (state_.load() != TCPCH_READY)
    {
        
        
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
    
    
    
    
    BCRESULT code = BC_R_FAILURE;
    if (ssl_ && SSL_get_verify_result(ssl_) != X509_V_OK)
    {
        code = BC_R_TLS_VERIFY_FAILED;
    }
    _Fail(code, msg);
}





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
        
        
        _TcpRecv();
        
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

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    if (closing_ || !socket_ || !buf)
    {
        return;
    }

    
    
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





void TcpChannel::_SetState(State next)
{
    State cur = state_.load();
    if (cur == next)
    {
        return;
    }
    if (!IsLegalTransition(cur, next))
    {
        
        
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

    
    
    
    _AbandonDns();

    
    
    
    

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
    
    
    
    if (ssl_depth_ > 0)
    {
        return;
    }
    
    
    if (pending_dns_ || pending_connect_ || pending_recv_ || pending_send_)
    {
        return;
    }

    {
        
        
        BCSpinMutex::Owner lock(guard_lock_);
        if (closed_notified_ || pending_tasks_ > 0)
        {
            return;
        }
        closed_notified_ = true;
    }

    _SetState(TCPCH_CLOSED);
    _Cleanup();

    
    
    
    
    
    
    
    
    
    
    
    detached_ = true;
    Detach();
}

void TcpChannel::OnEventProcShutdown()
{
    
    
    
    
    
    if (!closed_notified_)
    {
        return;     
    }

    ITcpChannelHandler* handler = handler_;
    handler_ = NULL;
    if (handler)
    {
        
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
            
            _this->_OnPeerClosed();
            break;
        }
        _this->_OnRecvDone(sockEv->n);
        
        
        _this->_TcpRecv();
        break;

    case BC_R_EOF:
        
        
        _this->_OnPeerClosed();
        break;

    case BC_R_CANCELED:
        
        break;

    default:
        _this->_Fail(sockEv->result, "TCP recv 失败");
        break;
    }

    
    
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

    
    _this->_MaybeFinishClose();
}




