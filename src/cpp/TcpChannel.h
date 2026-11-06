



























































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




typedef std::shared_ptr<BCBuffer> BufferPtr;





class ITcpChannelHandler
{
public:
    ITcpChannelHandler() {}
    virtual ~ITcpChannelHandler() {}

    
    virtual void OnChannelReady()                                = 0;

    
    
    
    
    
    
    virtual void OnChannelDetached()                             {}

    
    
    
    
    
    
    
    
    
    
    virtual void OnChannelAdoptFailed()                          {}
    
    virtual void OnChannelData(const void* data, size_t size)    = 0;
    
    virtual void OnChannelClosed(BCRESULT result,
                                 const std::string& reason)      = 0;
};





struct TcpChannelConfig
{
    std::string host;                   
                                        
                                        
    uint16_t    port             = 0;
    bool        tls              = false;
    std::string resolvedIp;             
                                        
    TTVpnPolicy policy           = TT_VPN_POLICY_UNSET;

    
    
    
    
    uint32_t    ifIndex          = 0;
    uint64_t    androidNetHandle = 0;

    DnsConfig   dns;
    TlsConfig   tls_cfg;
    
    uint32_t    connectTimeoutMs = 10000;
    void*       loggerCtx        = nullptr;
};























class TcpChannel;

struct TcpDnsRequest
{
    BCSpinMutex lock;
    bool        abandoned = false;
    TcpChannel* channel   = NULL;   
    std::string host;
    DnsConfig   cfg;
};





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

    
    BCRESULT    Open(const TcpChannelConfig& cfg, ITcpChannelHandler* handler);
    
    
    BCRESULT    Send(BufferPtr buf);
    
    void        Close();

    
    
    
    
    
    
    

    
    
    
    
    
    
    
    
    
    
    
    
    void        Park(ITcpChannelHandler* watcher = NULL);

    
    
    
    
    
    
    
    
    BCRESULT    Adopt(ITcpChannelHandler* handler);

    
    
    
    bool        IsParkedAndUsable() const;

    
    
    
    
    
    
    
    
    const std::string&
                LastOpenError() const { return open_error_; }

    
    std::string PeerIp()       const { return peer_ip_; }
    uint32_t    BoundIfIndex() const { return bound_ifindex_; }
    std::string PinMethod()    const { return pin_method_; }
    State       GetState()     const { return state_.load(); }

    
    
    

    
    static bool IsLegalTransition(State from, State to);
    
    static bool ParseIpLiteral(const std::string& ip, uint16_t port,
                               BCSockAddrS& out);

private:
    DECLARE_NO_COPY_CLASS(TcpChannel);

    
    
    
    
    
    
    
    
    
    
    
    
    
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
                
                
                
                
                ch_->_MaybeFinishClose();
            }
        }
    private:
        DECLARE_NO_COPY_CLASS(SslReentryGuard);
        TcpChannel* ch_;
    };
    friend class SslReentryGuard;

    
    
    
    void        OnEventProcShutdown() override;

    
    int         OnSSLWrite(const void* data, size_t len) override;
    void        OnRecvDataFromSSL(const void* data, size_t size) override;
    void        OnSSLReady() override;
    void        OnSSLFinished() override;
    void        OnSSLError(const char* file, int line,
                           const char* data, int flags) override;

    
    bool        _AcquirePendingTask();
    void        _ReleasePendingTask();

    
    uint32_t    _PickPhysicalIfIndex();
    bool        _HasBindTarget() const;
    
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

    
    
    std::atomic<bool>           parked_{false};
    
    
    
    std::atomic<bool>           poisoned_{false};

    BCSocket                *   socket_         = NULL;
    bool                        is_ipv6_        = false;
    BCSockAddrS                 peer_addr_;
    socklen_t                   peer_addr_len_  = 0;
    std::string                 peer_ip_;

    uint32_t                    bound_ifindex_  = 0;
    std::string                 pin_method_;
    
    
    std::string                 open_error_;

    SSL_CTX                 *   ssl_ctx_        = NULL;
    SSL                     *   ssl_            = NULL;
    std::unique_ptr<SSLayer>    ssl_layer_;

    int32_t                     connect_timer_  = 0;
    BCSockEvent                 recv_event_;
    char                        recv_buffer_[16384];

    
    
    std::shared_ptr<TcpDnsRequest> dns_request_;

    
    
    
    bool                        tls_shutdown_clean_ = false;

    
    
    
    uint32_t                    pending_dns_        = 0;
    uint32_t                    pending_connect_    = 0;
    uint32_t                    pending_recv_       = 0;
    uint32_t                    pending_send_       = 0;

    
    uint32_t                    ssl_depth_          = 0;

    
    
    
    
    BCSpinMutex                 guard_lock_;
    uint32_t                    pending_tasks_      = 0;

    bool                        closing_            = false;
    bool                        closed_notified_    = false;   
    BCRESULT                    close_result_       = BC_R_SUCCESS;
    std::string                 close_reason_;
};

#endif 




