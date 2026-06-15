///////////////////////////////////////////////////////////////////////////////
// file : SMPConnector.h
// author : anto
///////////////////////////////////////////////////////////////////////////////
#ifndef SMPCONNECTOR_H_INCLUDED__
#define SMPCONNECTOR_H_INCLUDED__

#include "BC/BCTimer.h"
#include "BC/BCSocket.h"
#include "BC/BCTask.h"
#include "BC/BCLog.h"
#include "UDPSenderGroup.h"
#include "Interface.h"
#include "SMPParser.h"
#include "Utils.h"
#include "SMPacket.h"
#if defined(TT_HAS_PATH_MONITOR)
#include "INetworkPathMonitor.h"
#endif

#include <xquic/xquic_typedef.h>
#include <xquic/xquic.h>
#include <xquic/xqc_http3.h>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <openssl/x509.h>

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

#define SMP_CID_TAG_LEN 11
#define SMP_CID_LEN 16

// Outbound proxy mode for the underlying QUIC transport. PROXY_NONE keeps the
// historical direct-to-server path; PROXY_MASQUE tunnels the inner QUIC
// connection through an RFC 9298 CONNECT-UDP (MASQUE) proxy over HTTP/3.
enum TTProxyType {
    TT_PROXY_NONE   = 0,
    TT_PROXY_MASQUE = 1,
};

class SMPConnector;
class SMPConnection;
class SMPStream;
class MasqueProxyChannel;

typedef std::shared_ptr<SMPConnection>    ConnPtr;

using namespace node;

///////////////////////////////////////////////////////////////////////////////
// class : SMPStream
///////////////////////////////////////////////////////////////////////////////
class SMPStream : public ISMPacketHandler
{
public:
    xqc_stream_t    *   stream_;
    xqc_usec_t          start_time_;
    xqc_usec_t          end_time_;

    SMPStream();
    ~SMPStream();

    BCRESULT        Create(
                        xqc_stream_id_t id,
                        xqc_stream_t *stream, 
                        SMPConnection *pConn);
    int             Send(const void *data, size_t size);
    BCRESULT        SendPacket(SMPacketPtr pkt);
    void            Close();
    int             OnRead();
    int             OnWriteNotify();
    void            OnClosing();
    void            OnClose();

    void            OnPacketAcked(
                        xqc_usec_t ack_delay_time,
                        size_t acked_bytes,
                        size_t inflight_bytes);
    void            OnPacketParsed(
                        const SMPHeader& refHeader, 
                        const char* payload,
                        size_t payload_size) override;
    void            OnDataPacked(
                        void* payload, 
                        size_t payload_size) override;
protected:
    void            _SendBuffList();
private:
    xqc_stream_id_t         stream_id_ = 0;
    SMPConnection       *   conn_ = NULL;
    SMPParser               parser_;
    unsigned char           recv_buff_[4096]    = { 0 };
    size_t                  recv_buff_size_     = 4096;
    bool                    recv_wt_mask_ = false;
    std::list<SMPacketPtr>  send_buff_list_;
};

///////////////////////////////////////////////////////////////////////////////
// class : H3Stream
///////////////////////////////////////////////////////////////////////////////
class H3Stream : public ISMPacketHandler
{
public:
    xqc_h3_request_t    *   stream_ = NULL;
    xqc_usec_t              start_time_;
    xqc_usec_t              end_time_;

    H3Stream();
    ~H3Stream();

    BCRESULT        Create(
                        xqc_stream_id_t id,
                        xqc_h3_request_t *stream, 
                        SMPConnection *pConn);
    BCRESULT        SendRequest();
    // Send an RFC 9298 CONNECT-UDP Extended CONNECT request for `target_host`/
    // `target_port` (the real QUIC server) towards the MASQUE proxy, instead of
    // the default WebTransport request. `authority` is the proxy authority.
    BCRESULT        SendConnectUdpRequest(
                        const std::string& authority,
                        const std::string& target_host,
                        uint16_t target_port);
    int             Send(const void *data, size_t size);
    BCRESULT        SendPacket(SMPacketPtr pkt);
    int             OnRead(xqc_request_notify_flag_t flags);
    void            OnClosing();
    void            OnClose();

    void            OnPacketAcked(
                        xqc_usec_t ack_delay_time,
                        size_t acked_bytes,
                        size_t inflight_bytes);
    void            OnPacketParsed(
                        const SMPHeader& refHeader, 
                        const char* payload,
                        size_t payload_size) override;
    void            OnDataPacked(
                        void* payload, 
                        size_t payload_size) override;
private:
    xqc_stream_id_t         stream_id_ = 0;
    SMPConnection       *   conn_ = NULL;
    SMPParser               parser_;
    unsigned char           recv_buff_[4096]    = { 0 };
    size_t                  recv_buff_size_     = 4096;
    xqc_http_headers_t      h3_hdrs_;
    uint8_t                 hdr_sent_ = 0;
    uint8_t                 recv_fin_ = 0;
    uint32_t                recv_body_size_ = 0;
};

///////////////////////////////////////////////////////////////////////////////
// class : SMPConnection
///////////////////////////////////////////////////////////////////////////////
class SMPConnection 
    : public BCEventQueue
    , public IUDPSenderHandler
{
    friend class SMPConnector;
    friend class SMPStream;
    friend class H3Stream;

    typedef enum {
        CONN_STATE_FREED        = 0,
        CONN_STATE_CLOSING_SOCK = 1,
        CONN_STATE_CLOSING_QUIC = 2,
        CONN_STATE_INIT         = 3,
        CONN_STATE_HANDSHAKE    = 4,
        CONN_STATE_CONNECTING   = 5,
        CONN_STATE_CONNECTED    = 6,
        CONN_STATE_MAX          = 9,
    }ConnState;

    ///////////////////////////////////////////////////////////////////////////////
    // class : Config
    ///////////////////////////////////////////////////////////////////////////////

    class Config
    {
    public:
        Config();

        ~Config()
        {
        }

        bool				        ipv6;
        LPCSTR				        host;
        uint16_t			        port;
        LPCSTR				        server_host;
        uint16_t			        server_port;
        uint8_t                     c_cong_ctl;
        bool                        pacing_on;
        uint32_t                    idle_time_out;
        bool                        ping_on;
        bool                        linger_on;
        uint32_t                    ping_interval;
        uint64_t                    active_connection_id_limit;
        LPCSTR                      alpn;
        uint8_t                     device_type;
        uint8_t                     cid_tag[SMP_CID_TAG_LEN];
        LPCSTR                      ca_cert_pem;
        size_t                      ca_cert_pem_len;
        TTProxyType   proxy_type;
        LPCSTR        proxy_host;
        uint16_t      proxy_port;
        LPCSTR        proxy_sni;
        LPCSTR        proxy_url;
        LPCSTR        proxy_ca_cert_pem;        // outer-hop (proxy) CA; empty => accept proxy cert unverified
        size_t        proxy_ca_cert_pem_len;
        LPCSTR        spki_pin;                 // outer-hop (proxy) SPKI pin (base64 SHA-256 of SPKI)
        size_t        spki_pin_len;

        BCRESULT		Init(BCFObject* pConfig);
        LPCSTR          Strdup(LPCSTR str)
        {
            return pool_.Strdup(str);
        }

    private:
        Config(const Config& other) = delete;
        Config& operator=(const Config& other) = delete;

        KBPool		pool_;
    };
public:

    SMPConnection();
    ~SMPConnection();

    // pTaskMgr / pTimerMgr let a caller pin this connection to specific event
    // managers (used to co-locate a MASQUE tunnel on its inner connection's
    // single thread); NULL picks random managers as before.
    BCRESULT        Create(
                        SMPConnector* connector,
                        IConnectionHandler* pHandler, 
                        BCFObject* pConfig,
                        uint64_t id,
                        BCTaskMgr* pTaskMgr = NULL,
                        BCTimerMgr* pTimerMgr = NULL);
    BCRESULT        Connect(IRPCStub* pStub);
    BCRESULT		SendPacket(SMPacketPtr pkt);
    void            Restart(int64_t networkHandle = 0);
    void            Close();
    void            CloseStream(uint32_t nStreamId);
protected:
    BCRESULT        Connect_Internal();
    void            ProcessPacket(
                        const SMPHeader& refHeader,
                        const char* lpszMsg,
                        size_t size);
    BCRESULT		SendPacket_Internal(SMPacketPtr pkt);
    void            OnPingAcked(void *ping_user_data);
    void            OnPacketAcked(
                        xqc_usec_t ack_delay_time,
                        size_t acked_bytes,
                        size_t inflight_bytes,
                        xqc_stream_id_t stream_id);
    void            OnUpdataCID(
                        const xqc_cid_t* retire_cid,
                        const xqc_cid_t* new_cid);
    int             OnConnCreate(
                        xqc_connection_t* conn,
                        const xqc_cid_t* cid);
    void            OnHandshakeFinished();
    int             OnStreamCreate(xqc_stream_t* stream);
    int             OnClose();
    int             OnH3ConnCreate(
                        xqc_h3_conn_t* h3_conn,
                        const xqc_cid_t* cid);
    void            OnH3HandshakeFinished();
    int             OnH3StreamCreate(xqc_h3_request_t* stream);
    int             OnH3Close();
    void            OnSaveTP(const char *data, size_t data_len);
    void            OnSaveSession(const char *data, size_t data_len);
    void            OnSaveToken(const unsigned char *token, size_t token_len);
    ssize_t         WritePacket(
                        const unsigned char* buf,
                        size_t size,
                        const struct sockaddr* peer_addr,
                        socklen_t peer_addrlen);
    void            OnH3RequestRecvHeaders(xqc_http_headers_t* headers);
    ///////////////////////////////////////////////////////////////////////////
    // MASQUE CONNECT-UDP helpers.
    ///////////////////////////////////////////////////////////////////////////
    // INNER: create + connect the outer tunnel to the proxy. Runs on our queue.
    BCRESULT        MasqueStartTunnel();
    // INNER: tunnel signalled readiness (or failure); proceed to jqc_connect
    // the inner connection over the tunnel, or fail the connect.
    void            MasqueOnTunnelReady(BCRESULT result);
    // INNER: feed a tunnelled inbound QUIC packet into the inner engine.
    void            MasqueOnTunneledPacket(const uint8_t* data, size_t len);
    // TUNNEL: after the outer h3 handshake, send the CONNECT-UDP request.
    BCRESULT        MasqueSendConnectUdp();
    // TUNNEL: CONNECT-UDP got a final response; notify the inner connection.
    void            MasqueOnConnectUdpResponse(bool ok);
    // TUNNEL: hand an inner QUIC packet to the proxy as an h3 datagram.
    ssize_t         MasqueSendDatagram(const unsigned char* buf, size_t size);
    // TUNNEL: a datagram arrived from the proxy; decode + route to the inner.
    void            MasqueOnDatagram(const uint8_t* data, size_t len);
    // TUNNEL: usable inner UDP payload size = outer h3 datagram MSS minus the
    // RFC 9297/9298 per-datagram header (quarter-stream-id + context-id).
    // Returns 0 if the tunnel cannot currently carry datagrams.
    size_t          MasqueDatagramMss();
    // TUNNEL: connect the outer h3 connection to the proxy (runs on our queue).
    void            MasqueConnectToProxy();
    // INNER: the owned tunnel finished closing (posted from the tunnel's
    // shutdown); delete it and continue/trigger our own teardown. Runs on the
    // inner's queue, a different call stack than the tunnel's shutdown.
    void            MasqueOnTunnelClosed();
    // Override IUDPPacketHandler interfaces
    void	        OnSendData(uint32_t nWrite, UDPSender* pSender) override;
    void	        OnRecvData(BCBuffer* pBuffer, BCSockAddrS& refSrcAddr) override;
    void	        OnCheckAvailable() override;
    void	        OnRestart(BCRESULT result) override;
    void            OnUdpClosed() override;
    // Override BCEventQueue interfaces
    void			OnEventProcShutdown() override;
private:
    static int32_t  on_timer_set(
                        void* ctx, 
                        xqc_usec_t expire_time, 
                        void(*timeout_cb)(void*), 
                        void* user_data);
    static void     on_timer_update(
                        void* ctx, 
                        int32_t timer_id, 
                        xqc_usec_t expire_time, 
                        void(*timeout_cb)(void*), 
                        void* user_data);
    static void     on_timer_unset(void* ctx, int32_t *timer_id, xqc_bool_t only_cancel);
    static void     on_timer_next_tick(void* ctx);

private:
    DECLARE_NO_COPY_CLASS(SMPConnection);
    inline void		_SetState(ConnState eState, uint32_t nLineNumber)
    {
        new_state_ = eState;
        state_line_ = nLineNumber;
    }
    bool            _CloseCheck();
    void            _OnConnectTimeout();
    void            _NotifyConnectResult(
                        IRPCStub *pStub,
                        BCRESULT result, 
                        LPCSTR msg = NULL, 
                        size_t size = 0);

    Config                  config_;
    std::string             scheme_;
    std::string             host_;
    uint16_t                port_ = 0;
    std::string             path_;
    std::string             query_;
    Json::Value             props_ = Json::Value(Json::objectValue);
    SMPConnector        *   connector_ = NULL;
    xqc_connection_t    *   conn_ = NULL;
    xqc_h3_conn_t       *   h3_conn_ = NULL;
    struct sockaddr_in6     local_addr_;
    socklen_t               local_addrlen_;
    struct sockaddr_in6     peer_addr_;
    socklen_t               peer_addrlen_;
    xqc_cid_t               cid_;
    uint64_t                id_ = 0;
    typedef std::unordered_map<xqc_stream_id_t, SMPStream*> StreamMap;
    StreamMap               stream_map_;
    typedef std::unordered_map<xqc_stream_id_t, H3Stream*> H3StreamMap;
    H3StreamMap             h3_stream_map_;
    IConnectionHandler  *   handler_ = NULL;
    IRPCStub            *   connect_rpc_ = NULL;
    int32_t                 connect_timer_id_ = 0;
    int64_t                 network_handle_ = 0;
    UDPSenderGroup      *   udp_socket_ = NULL;
    bool                    webtransport_ = false;
    ///////////////////////////////////////////////////////////////////////////
    // MASQUE CONNECT-UDP (RFC 9298) proxy tunnel state.
    //
    // When a connector is configured with a MASQUE proxy, an app-facing
    // connection (MASQUE_INNER) does not own a real UDP path to the server.
    // Instead it spins up an internal outer HTTP/3 connection (MASQUE_TUNNEL)
    // to the proxy, opens a CONNECT-UDP request for the real target, and
    // ferries its QUIC packets as HTTP/3 datagrams. The two SMPConnections
    // share one BCTaskMgr (the inner's) so all their work is serialised on a
    // single thread; cross-connection hand-offs still go through PostTask to
    // avoid re-entering the shared xquic engine.
    ///////////////////////////////////////////////////////////////////////////
    enum MasqueRole {
        MASQUE_NONE   = 0,  // direct, no proxy (default)
        MASQUE_INNER  = 1,  // app-facing connection, tunnelled
        MASQUE_TUNNEL = 2,  // internal outer h3 connection to the proxy
    };
    MasqueRole              masque_role_ = MASQUE_NONE;
    // INNER: the outer tunnel carrying our packets (we drive its lifetime).
    SMPConnection       *   masque_tunnel_ = NULL;
    // TUNNEL: the inner connection we serve (back-reference, not owned).
    SMPConnection       *   masque_inner_ = NULL;
    // TUNNEL: CONNECT-UDP request stream + its quarter stream id (RFC 9297).
    H3Stream            *   masque_connect_stream_ = NULL;
    uint64_t                masque_qsid_ = 0;
    // TUNNEL: real target the proxy should reach (the inner's server).
    std::string             masque_target_host_;
    uint16_t                masque_target_port_ = 0;
    // TUNNEL: set once the proxy answered CONNECT-UDP with a 2xx.
    bool                    masque_tunnel_ready_ = false;
    // INNER: true while we are waiting for our owned tunnel to finish closing
    // (so we don't self-destruct before the tunnel is gone).
    bool                    masque_tunnel_closing_ = false;
    // Event managers this connection runs on. A MASQUE tunnel reuses its inner
    // connection's managers so the pair is serialised on one thread.
    BCTaskMgr           *   task_mgr_ = NULL;
    BCTimerMgr          *   timer_mgr_ = NULL;
    // Asynch state
    ConnState               state_;
    ConnState				new_state_;
    uint32_t				state_line_;
    uint32_t				close_status_;
    std::string             conn_close_msg_;
    std::string             cert_verify_error_;
    std::atomic_bool        keep_working_;
    // stats
    ssize_t                 last_snd_sum    = 0;
    ssize_t                 snd_sum         = 0;
    uint64_t                last_snd_ts     = 0;
    uint64_t                wrote_counter   = 0;
    // Parsed trusted CA bundle loaded from per-connection ca_cert_pem (if provided).
    std::vector<X509*>      root_cas_;
};

///////////////////////////////////////////////////////////////////////////////
// class : SMPConnector
///////////////////////////////////////////////////////////////////////////////
class SMPConnector
{
    typedef enum {
        CONNTOR_STATE_FREED        = 0,
        CONNTOR_STATE_INIT         = 1,
        CONNTOR_STATE_WORKING      = 2,
        CONNTOR_STATE_MAX          = 9,
    }ConnectorState;

    ///////////////////////////////////////////////////////////////////////////////
    // class : Config
    ///////////////////////////////////////////////////////////////////////////////

    class Config
    {
    public:
        Config() : ipv6(false), publishId(5), host(NULL), port(0)
            , server_host(NULL), server_port(0)
            , c_cong_ctl('b'), pacing_on(0), idle_time_out(0)
            , linger_on(0), log_level('d'), alpn(NULL), ping_on(false)
            , ping_interval(0), log_file(NULL), active_connection_id_limit(0)
            , ca_cert_pem(NULL), ca_cert_pem_len(0)
            , disableAutoRestart(false)
            , bypassVpn(true)
            , proxy_type(TT_PROXY_NONE), proxy_host(NULL), proxy_port(0)
            , proxy_sni(NULL), proxy_url(NULL)
        {
            memset(&engine_ssl_config, 0, sizeof(engine_ssl_config));
        }

        ~Config()
        {
        }

        bool				        ipv6;
        uint32_t			        publishId;
        LPCSTR				        host;
        uint16_t			        port;
        LPCSTR				        server_host;
        uint16_t			        server_port;
        uint8_t                     c_cong_ctl;
        uint8_t                     pacing_on;
        uint32_t                    idle_time_out;
        uint8_t                     linger_on;
        uint8_t                     log_level;
        xqc_engine_ssl_config_t     engine_ssl_config;
        std::vector<std::string>    alpn;
        bool                        ping_on;
        uint64_t                    ping_interval;
        LPCSTR                      log_file;
        uint64_t                    active_connection_id_limit;
        LPCSTR                      ca_cert_pem;
        size_t                      ca_cert_pem_len;
        // Opt-out of the platform-native path-change monitor (NWPathMonitor /
        // netlink / NotifyIpInterfaceChange). Default false on platforms that
        // ship a monitor; ignored entirely on Android (Java NetworkCallback
        // path is independent). Set true for long-lived server deployments.
        bool                        disableAutoRestart;
        // macOS-only behaviour switch surfaced through
        // TTNetworkMonitorOptions::bypassVpn. Default true: keep QUIC on
        // physical interfaces (wifi / wired / cellular) even when a
        // utun / ipsec tunnel briefly wins the default route. Set false
        // for apps that explicitly want to ride a VPN tunnel. iOS, Linux
        // and Windows monitors honour the flag for ABI parity but ignore
        // its value — see TTNetworkMonitorOptions in
        // INetworkPathMonitor.h.
        bool                        bypassVpn;
        // Outbound proxy for the underlying QUIC transport (RFC 9298
        // CONNECT-UDP / MASQUE). Parsed from the "proxy_url" config key, e.g.
        // "masque://proxy.example.com:443". TT_PROXY_NONE means direct.
        TTProxyType                 proxy_type;
        LPCSTR                      proxy_host;   // MASQUE proxy hostname/IP
        uint16_t                    proxy_port;   // MASQUE proxy UDP port
        LPCSTR                      proxy_sni;     // outer TLS SNI/authority (defaults to proxy_host)
        LPCSTR                      proxy_url;     // raw value, retained for logging

        BCRESULT		Init(BCFObject* pConfig);

    private:
        Config(const Config& other) = delete;
        Config& operator=(const Config& other) = delete;

        KBPool		pool_;
    };

public:
    SMPConnector();
    ~SMPConnector();

    BCRESULT        	Create(
	                        BCFObject* pConfig,
	                        IConnectorHandler *pHandler);
    SMPConnection   *   CreateConnection(
	                        IConnectionHandler* pHandler, 
	                        BCFObject* pConfig);
    // Create the internal outer HTTP/3 connection that tunnels `inner` through
    // the configured MASQUE proxy. The returned connection is NOT registered
    // in conns_map_ (it is invisible to the app) and shares `inner`'s task /
    // timer managers so the pair runs on a single thread. Returns NULL on
    // failure.
    SMPConnection   *   CreateMasqueTunnel(SMPConnection* inner);
    void                NotifyConnClosed(uint64_t conn_id);
    BCRESULT			GetStats(ConnStatS& stats);
    void                Close();
private:
    DECLARE_NO_COPY_CLASS(SMPConnector);
    inline void		_SetState(ConnectorState eState, uint32_t nLineNumber)
    {
        new_state_ = eState;
        state_line_ = nLineNumber;
    }
    bool            _CloseCheck();

    static void     set_timer_cb(xqc_usec_t wake_after, void* user_data);
    static void     on_conn_save_session(
                        const char* data, 
                        size_t data_len,
                        void* user_data);
    static void     on_conn_save_tp(
                        const char* data, 
                        size_t data_len, 
                        void* user_data);
    static void     on_conn_save_token(
                        const unsigned char* token, 
                        unsigned token_len,
                        void* user_data);
    static void     on_conn_update_cid_notify(
                        xqc_connection_t* conn,
                        const xqc_cid_t* retire_cid,
                        const xqc_cid_t* new_cid,
                        void* user_data);
    static int      on_conn_cert_verify(
                        const unsigned char* certs[],
                        const size_t cert_len[], 
                        size_t certs_len,
                        void* conn_user_data);
    static int      on_conn_closing_notify(
                        xqc_connection_t* conn,
                        const xqc_cid_t* cid,
                        xqc_int_t err_code,
                        void* conn_user_data);
    // raw alpn callbacks
    static int      on_raw_conn_create_notify(
                        xqc_connection_t* conn,
                        const xqc_cid_t* cid,
                        void* conn_user_data,
                        void* conn_proto_data);
    static int      on_raw_conn_close_notify(
                        xqc_connection_t* h3_conn,
                        const xqc_cid_t* cid,
                        void* conn_user_data,
                        void* conn_proto_data);
    static void     on_raw_conn_handshake_finished(
                        xqc_connection_t* h3_conn,
                        void* conn_user_data,
                        void* conn_proto_data);
    static void     on_raw_conn_ping_acked(
                        xqc_connection_t *conn, 
                        const xqc_cid_t *cid, 
                        void *ping_user_data, 
                        void *conn_user_data, 
                        void *conn_proto_data);
    static void     on_raw_conn_packet_acked(
                        xqc_connection_t* h3_conn, 
                        xqc_usec_t ack_delay_time,
                        size_t acked_bytes, 
                        size_t inflight_bytes, 
                        xqc_stream_id_t stream_id,
                        void* conn_user_data,
                        void *conn_proto_data);
    static int      on_raw_stream_create_notify(
                        xqc_stream_t* h3_request, 
                        void* user_data);
    static int      on_raw_stream_close_notify(
                        xqc_stream_t* h3_request, 
                        void* user_data);
    static int      on_raw_stream_write_notify(
                        xqc_stream_t* h3_request, 
                        void* user_data);
    static int      on_raw_stream_read_notify(
                        xqc_stream_t* h3_request,
                        void* user_data);
    // http3 alpn callbacks
    static int      on_h3_conn_create_notify(
                        xqc_h3_conn_t *conn, 
                        const xqc_cid_t *cid, 
                        void *user_data);
    static int      on_h3_conn_close_notify(
                        xqc_h3_conn_t *conn, 
                        const xqc_cid_t *cid, 
                        void *user_data);
    static void     on_h3_conn_handshake_finished(
                        xqc_h3_conn_t *conn, 
                        void *user_data);
    static void     on_h3_conn_ping_acked(
                        xqc_h3_conn_t *conn, 
                        const xqc_cid_t *cid, 
                        void *ping_user_data,
                        void *user_data);
    static void     on_h3_conn_packet_acked(
                        xqc_h3_conn_t *h3_conn, 
                        xqc_usec_t ack_delay_time, 
                        size_t acked_bytes, 
                        size_t inflight_bytes, 
                        xqc_stream_id_t stream_id, 
                        void *h3c_user_data);
    static int      on_h3_stream_create_notify(
                        xqc_h3_request_t *h3_request, 
                        void *h3s_user_data);
    static void     on_h3_stream_closing_notify(
                        xqc_h3_request_t *h3_request, 
                        xqc_int_t err, 
                        void *h3s_user_data);
    static int      on_h3_stream_write_notify(
                        xqc_h3_request_t *h3_request, 
                        void *h3s_user_data);
    static int      on_h3_stream_read_notify(
                        xqc_h3_request_t *h3_request, 
                        xqc_request_notify_flag_t flag, 
                        void *h3s_user_data);
    static int      on_h3_stream_close_notify(
                        xqc_h3_request_t *h3_request, 
                        void *h3s_user_data);
    // h3 datagram (RFC 9297) read callback for the MASQUE tunnel. user_data is
    // the outer (MASQUE_TUNNEL) SMPConnection set via xqc_h3_conn_set_user_data.
    static void     on_h3_ext_datagram_read(
                        xqc_h3_conn_t *conn,
                        const void *data,
                        size_t data_len,
                        void *user_data,
                        uint64_t data_recv_time);
    static ssize_t  conn_write_socket_cb(
                        const unsigned char* buf,
                        size_t size,
                        const struct sockaddr* peer_addr,
                        socklen_t peer_addrlen,
                        void* user_data);
    static ssize_t  conn_write_socket_ex_cb(
                        uint64_t path_id,
                        const unsigned char* buf,
                        size_t size,
                        const struct sockaddr* peer_addr,
                        socklen_t peer_addrlen,
                        void* conn_user_data);
    static void     on_write_log(
                        xqc_log_level_t lvl,
                        const void* buf,
                        size_t count,
                        void* engine_user_data);
    static void     on_keylog_cb(
                        const xqc_cid_t* scid, 
                        const char* line, 
                        void* user_data);
    static void     on_server_refuse(
                        xqc_engine_t* engine,
                        xqc_connection_t* conn,
                        const xqc_cid_t* cid,
                        void* user_data);
    static ssize_t  on_stateless_reset(
                        const unsigned char* buf, 
                        size_t size,
                        const struct sockaddr* peer_addr,
                        socklen_t peer_addrlen,
                        const struct sockaddr* local_addr,
                        socklen_t local_addrlen, 
                        void* user_data);
    static ssize_t  cid_generate(
                        const xqc_cid_t* ori_cid, 
                        uint8_t* cid_buf, 
                        size_t cid_buflen, 
                        void* engine_user_data,
                        void *conn_user_data);

    static void     log_callback(void *data, int level, LPCSTR lpszMsg);
#if defined(TT_HAS_PATH_MONITOR)
    // Path-change callback delivered by AppleNetworkMonitor / LinuxNetlinkMonitor
    // / WinIpChangeMonitor. Runs on the monitor's own thread; we marshal onto
    // the SMP runtime via Runtime::PostTask before touching conns_map_.
    static void     OnPathChange(void* userdata, int64_t newIfIndex,
                                 const char* pathDesc);
    // Optional diagnostic sink wired into TTNetworkMonitorOptions::rawLogFn:
    // logs every pre-filter NWPathMonitor / netlink / IP-change wakeup via
    // LogQ so we can correlate "raw OS event count" with the post-debounce
    // OnPathChange count above. Same threading model as OnPathChange.
    static void     OnPathRawLog(void* userdata, const char* line);
#endif
public:
    BCSpinMutex             lock_;
    Config                  config_;
    xqc_engine_t        *   engine_;
    xqc_conn_settings_t     conn_settings_;
    int32_t                 timer_id_;
    LPVOID                  logger_ctx_;
    IConnectorHandler   *   handler_;
    typedef std::unordered_map<uint64_t, SMPConnection*>   CidConnMap;
    CidConnMap              conns_map_;
    uint64_t                next_conn_id_ = 1;
    xqc_quic_lb_ctx_t       quic_lb_ctx_;
    // Asynch state
    ConnectorState          state_;
    ConnectorState			new_state_;
    uint32_t				state_line_;
    uint32_t				close_status_;
    // stats
    std::atomic<uint64_t>   total_connections;
    std::atomic<uint64_t>   active_connections;
    std::atomic<uint64_t>   start_time;
    std::atomic<uint64_t>   total_succeed_connections;
    // Stats
    std::atomic<size_t>		total_allocated_conns_;
    std::atomic<size_t>		total_freed_conns_;
    // Parsed trusted CA bundle loaded from connector-level ca_cert_pem.
    std::vector<X509*>      root_cas_;
#if defined(TT_HAS_PATH_MONITOR)
    // Owned by SMPConnector. Started in Create() if config_.disableAutoRestart
    // is false; stopped in dtor. nullptr means "monitor disabled or unavailable",
    // in which case the caller is responsible for invoking SMPConnection::Restart
    // on roaming events itself.
    TTNetworkMonitorRef         path_monitor_ = nullptr;
    // tt_netmon_start fires its first callback ~immediately after Create()
    // to report the *current* default-route ifIndex — that's not a real
    // path change, just an initial notification. Restarting connections at
    // that point would bounce a freshly-created (or in-flight handshake)
    // SMPConnection's UDP socket for no benefit; we swallow the first
    // OnPathChange invocation and only treat subsequent ones as actual
    // migration events. atomic so the monitor thread (NWPathMonitor queue
    // / netlink reader / Windows worker) and Runtime worker can both touch
    // it safely.
    std::atomic<bool>           first_path_callback_seen_{false};
    // Monotonic millisecond timestamp of the most recent *accepted* OS
    // path-change callback (steady_clock; 0 means "never seen one").
    // Used inside OnPathChange to coalesce a single noisy Wi-Fi handover
    // — which the monitor can fire as satisfied(old) → unsatisfied →
    // satisfied(new) over several seconds — into one socket migration.
    // This guard is deliberately scoped to the path-monitor entry point:
    // SMPConnection::Restart (the public API hit by JNI / Swift / NAPI
    // when the app sets disableAutoRestart=true and drives migration
    // itself) is NOT subject to this cooldown and always honours the
    // caller. Touched only from the runtime thread inside OnPathChange.
    int64_t                     last_path_change_ms_ = 0;
#endif
};

#endif // SMPCONNECTOR_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : SMPConnector.h
///////////////////////////////////////////////////////////////////////////////
