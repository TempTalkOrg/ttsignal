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
#include "UDPSender.h"
#include "Interface.h"
#include "SMPParser.h"
#include "Utils.h"
#include "SMPacket.h"

#include <xquic/xquic_typedef.h>
#include <xquic/xquic.h>
#include <xquic/xqc_http3.h>
#include <unordered_map>
#include <memory>
#include <atomic>

///////////////////////////////////////////////////////////////////////////////
// Forward declarations
///////////////////////////////////////////////////////////////////////////////

class SMPConnector;
class SMPConnection;
class SMPStream;

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
    bool                    recv_wt_mask_ = false;
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
        Config() : ipv6(false), host(NULL), port(0), server_host(NULL), server_port(0)
            , c_cong_ctl('b'), pacing_on(false), idle_time_out(0), ping_on(false)
            , linger_on(false), ping_interval(0), active_connection_id_limit(0)
            , alpn(NULL)
        {
        }

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

    BCRESULT        Create(
                        SMPConnector* connector,
                        IConnectionHandler* pHandler, 
                        BCFObject* pConfig,
                        uint64_t id);
    BCRESULT        Connect(IRPCStub* pStub);
    BCRESULT		SendPacket(SMPacketPtr pkt);
    void            Restart();
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
    // Override IUDPPacketHandler interfaces
    void	        OnSendData(uint32_t nWrite, UDPSender* pSender) override;
    void	        OnRecvData(BCBuffer* pBuffer, BCSockAddrS& refSrcAddr) override;
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
    UDPSender           *   udp_socket_ = NULL;
    bool                    webtransport_ = false;
    // Asynch state
    ConnState               state_;
    ConnState				new_state_;
    uint32_t				state_line_;
    uint32_t				close_status_;
    std::string             conn_close_msg_;
    std::atomic_bool        keep_working_;
    // stats
    ssize_t                 last_snd_sum    = 0;
    ssize_t                 snd_sum         = 0;
    uint64_t                last_snd_ts     = 0;
    uint64_t                wrote_counter   = 0;
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
        LPCSTR                      alpn;
        bool                        ping_on;
        uint64_t                    ping_interval;
        LPCSTR                      log_file;
        uint64_t                    active_connection_id_limit;

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
    static ssize_t  conn_write_socket_cb(
                        const unsigned char* buf,
                        size_t size,
                        const struct sockaddr* peer_addr,
                        socklen_t peer_addrlen,
                        void* user_data);
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
                        void* engine_user_data);

    static void     log_callback(void *data, int level, LPCSTR lpszMsg);
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
};

#endif // SMPCONNECTOR_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : SMPConnector.h
///////////////////////////////////////////////////////////////////////////////
