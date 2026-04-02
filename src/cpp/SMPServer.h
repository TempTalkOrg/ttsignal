///////////////////////////////////////////////////////////////////////////////
// file : SMPServer.h
// author : anto
///////////////////////////////////////////////////////////////////////////////
#ifndef SMPSERVER_H_INCLUDED__
#define SMPSERVER_H_INCLUDED__

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

class SMPServer;
class SMPServerConnection;
class SMPServerStream;

typedef std::shared_ptr<SMPServerConnection>    ServerConnPtr;

using namespace node;
using namespace SMP;


///////////////////////////////////////////////////////////////////////////////
// class : SMPServerStream
///////////////////////////////////////////////////////////////////////////////
class SMPServerStream : public ISMPacketHandler
{
public:
    xqc_stream_t    *   stream_;
    xqc_usec_t          start_time_;
    xqc_usec_t          end_time_;

    SMPServerStream();
    ~SMPServerStream();

    BCRESULT        Create(
                        xqc_stream_t *stream, 
                        SMPServerConnection *pConn);
    int             Send(const void *data, size_t size);
    BCRESULT        SendPacket(SMPacketPtr pkt);
    int             OnRead();
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
    SMPServerConnection *   conn_;
    SMPParser               parser_;
    unsigned char           recv_buff_[4096]    = { 0 };
    size_t                  recv_buff_size_     = 4096;
};

///////////////////////////////////////////////////////////////////////////////
// class : SMPServerConnection
///////////////////////////////////////////////////////////////////////////////
class SMPServerConnection 
    : public BCEventQueue
    , public std::enable_shared_from_this<SMPServerConnection>
{
    DECLARE_FIXED_ALLOC(SMPServerConnection);

    friend class SMPServer;
    friend class SMPServerStream;

    typedef enum {
        CONN_STATE_FREED        = 0,
        CONN_STATE_CLOSING_QUIC = 1,
        CONN_STATE_INIT         = 2,
        CONN_STATE_HANDSHAKE    = 3,
        CONN_STATE_CONNECTING   = 4,
        CONN_STATE_CONNECTED    = 5,
        CONN_STATE_MAX          = 9,
    }ConnState;

public:

    SMPServerConnection();
    ~SMPServerConnection();

    BCRESULT        Create(
                        SMPServer* Server,
                        IServerConnectionHandler* pHandler, 
                        const xqc_cid_t* cid,
                        uint64_t id,
                        UDPSender *pSender);
    BCRESULT        Accept(bool bAccept, std::shared_ptr<BCBuffer> respBuffer);
    BCRESULT		SendPacket(SMPacketPtr pkt);
    BCRESULT        AddCloseListener(ISMPServerConnListener* pListener);
    BCRESULT        RemoveCloseListener(ISMPServerConnListener* pListener);
    void            Close();

    inline uint64_t GetId() const { return id_; }
    inline IServerConnectionHandler* GetHandler() const { return handler_; }
    inline void     EnableMask(bool enable) { enable_mask_ = enable; }
protected:
    void            ProcessPacket(
                        const SMPHeader& refHeader,
                        const char* lpszMsg,
                        size_t size);
    BCRESULT		SendPacket_Internal(SMPacketPtr pkt);
    int             OnClose();
    void            OnHandshakeFinished();
    void            OnPacketAcked(
                        xqc_usec_t ack_delay_time,
                        size_t acked_bytes,
                        size_t inflight_bytes,
                        xqc_stream_id_t stream_id);
    void            OnUpdataCID(
                        const xqc_cid_t* retire_cid,
                        const xqc_cid_t* new_cid);
    int             OnConnCreate(
                        xqc_connection_t* h3_conn,
                        const xqc_cid_t* cid);
    int             OnStreamCreate(xqc_stream_t* stream);
    void            OnSaveTP(const char *data, size_t data_len);
    void            OnSaveSession(const char *data, size_t data_len);
    void            OnSaveToken(const unsigned char *token, size_t token_len);
    ssize_t         WritePacket(
                        const unsigned char* buf,
                        size_t size,
                        const struct sockaddr* peer_addr,
                        socklen_t peer_addrlen);
    void	        ProcessRecvData(std::shared_ptr<RecvInfo> pInfo, bool initial);
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
    DECLARE_NO_COPY_CLASS(SMPServerConnection);
    inline void		_SetState(ConnState eState, uint32_t nLineNumber)
    {
        new_state_ = eState;
        state_line_ = nLineNumber;
    }
    bool            _CloseCheck();

    BCSpinMutex                     lock_;
    SMPServer                   *   server_;
    xqc_connection_t            *   conn_;
    xqc_cid_t                       cid_;
    uint64_t                        id_ = 0;
    typedef std::unordered_map<xqc_stream_id_t, SMPServerStream*> StreamMap;
    StreamMap                       stream_map_;
    IServerConnectionHandler    *   handler_;
    UDPSender                   *   udp_socket_;
    // Asynch state
    ConnState                       state_;
    ConnState				        new_state_;
    uint32_t				        state_line_;
    uint32_t				        close_status_;
    std::string                     conn_close_msg_;
    std::atomic_bool                keep_working_;
    typedef std::unordered_map<ISMPServerConnListener*, ISMPServerConnListener*>
        CloseListenerList;
    CloseListenerList               close_listeners_;
    // Business options
    std::atomic_bool                enable_mask_;
};

///////////////////////////////////////////////////////////////////////////////
// class : SMPServer
///////////////////////////////////////////////////////////////////////////////
class SMPServer : public IUDPSenderHandler
{
    typedef enum {
        SVR_STATE_FREED        = 0,
        SVR_STATE_INIT         = 1,
        SVR_STATE_DRAIN        = 2,
        SVR_STATE_WORKING      = 3,
        SVR_STATE_MAX          = 9,
    }ServerState;

    ///////////////////////////////////////////////////////////////////////////////
    // class : Config
    ///////////////////////////////////////////////////////////////////////////////

    class Config
    {
    public:
        Config() : ipv6(false), host(NULL), port(0)
            , server_host(NULL), server_port(0)
            , c_cong_ctl('b'), pacing_on(0), idle_time_out(0)
            , linger_on(0), log_level('d'), alpn(NULL), ping_on(false)
            , ping_interval(0), log_file(NULL)
        {
            memset(&engine_ssl_config, 0, sizeof(engine_ssl_config));
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
        uint8_t                     pacing_on;
        uint32_t                    idle_time_out;
        uint8_t                     linger_on;
        uint8_t                     log_level;
        xqc_engine_ssl_config_t     engine_ssl_config;
        LPCSTR                      alpn;
        bool                        ping_on;
        uint64_t                    ping_interval;
        LPCSTR                      log_file;

        BCRESULT		Init(BCFObject* pConfig);

    private:
        Config(const Config& other) = delete;
        Config& operator=(const Config& other) = delete;

        KBPool		pool_;
    };
public:
    SMPServer();
    ~SMPServer();

    BCRESULT        Create(BCFObject* pConfig, IServerHandler *pHandler);
    BCRESULT        CreateConnection(
                        IServerConnectionHandler* pHandler, 
                        RecvInfo *pInfo);
    BCRESULT        Start();
    void            NotifyConnCreated(IServerConnectionHandler *pHandler);
    void            UpdateCID(
                        const xqc_cid_t* retire_cid,
                        const xqc_cid_t* new_cid, 
                        SMPServerConnection *pConn);
    void            NotifyConnClosed(xqc_cid_t &cid);
    ssize_t         WritePacket(
                        const unsigned char* buf,
                        size_t size,
                        const struct sockaddr* peer_addr,
                        socklen_t peer_addrlen);
    BCRESULT		GetStats(ConnStatS& stats);
    void            Close();
private:
    DECLARE_NO_COPY_CLASS(SMPServer);

    inline void		_SetState(ServerState eState, uint32_t nLineNumber)
    {
        new_state_ = eState;
        state_line_ = nLineNumber;
    }
    bool            _CloseCheck();

    // Override IUDPPacketHandler interfaces
    void	        OnSendData(uint32_t nWrite, UDPSender* pSender) override;
    void	        OnRecvData(BCBuffer* pBuffer, BCSockAddrS& refSrcAddr) override;
    void            OnRestart(BCRESULT result) override;
    void            OnUdpClosed() override;

    static void     set_timer_cb(xqc_usec_t wake_after, void* user_data);
    static ssize_t  conn_write_socket_cb(
                        const unsigned char* buf,
                        size_t size,
                        const struct sockaddr* peer_addr,
                        socklen_t peer_addrlen,
                        void* user_data);
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
    static int      on_sm_conn_create_notify(
                        xqc_connection_t* conn,
                        const xqc_cid_t* cid,
                        void* conn_user_data,
                        void* conn_proto_data);
    static int      on_sm_conn_close_notify(
                        xqc_connection_t* h3_conn,
                        const xqc_cid_t* cid,
                        void* conn_user_data,
                        void* conn_proto_data);
    static void     on_sm_conn_handshake_finished(
                        xqc_connection_t* h3_conn,
                        void* conn_user_data,
                        void* conn_proto_data);
    static void     on_sm_conn_packet_acked(
                        xqc_connection_t* h3_conn, 
                        xqc_usec_t ack_delay_time,
                        size_t acked_bytes, 
                        size_t inflight_bytes, 
                        xqc_stream_id_t stream_id,
                        void* conn_user_data,
                        void *conn_proto_data);
    static void     on_write_log(
                        xqc_log_level_t lvl,
                        const void* buf,
                        size_t count,
                        void* engine_user_data);
    static void     on_keylog_cb(
                        const xqc_cid_t* scid, 
                        const char* line, 
                        void* user_data);
    static int      on_server_accept(
                        xqc_engine_t* engine,
                        xqc_connection_t* conn,
                        const xqc_cid_t* cid,
                        void* user_data);
    static void      on_server_refuse(
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
    static int      on_stream_create_notify(
                        xqc_stream_t* h3_request, 
                        void* user_data);
    static int      on_stream_close_notify(
                        xqc_stream_t* h3_request, 
                        void* user_data);
    static int      on_stream_write_notify(
                        xqc_stream_t* h3_request, 
                        void* user_data);
    static int      on_stream_read_notify(
                        xqc_stream_t* h3_request,
                        void* user_data);
    static ssize_t  cid_generate(
                        const xqc_cid_t* ori_cid, 
                        uint8_t* cid_buf, 
                        size_t cid_buflen, 
                        void* engine_user_data);

public:
    BCSpinMutex             lock_;
    Config                  config_;
    xqc_engine_t        *   engine_;
    xqc_conn_settings_t     conn_settings_;
    int32_t                 timer_id_;
    LPVOID                  logger_ctx_;
    IServerHandler      *   handler_;
    UDPSender           *   udp_socket_;
    BCSockAddrS             local_addr_;
    typedef std::unordered_map<xqc_cid_t, IServerConnectionHandler*>    CidHandlerMap;
    CidHandlerMap           handlers_map_;
    typedef std::unordered_map<xqc_cid_t, ServerConnPtr>         CidConnMap;
    CidConnMap              conns_map_;
    uint64_t                next_conn_id_ = 1;
    xqc_quic_lb_ctx_t       quic_lb_ctx_;
    // Asynch state
    ServerState             state_;
    ServerState			    new_state_;
    uint32_t				state_line_;
    uint32_t				close_status_;
    // stats
    std::atomic<uint64_t>   start_time;
    std::atomic<uint64_t>   total_succeed_connections;
    std::atomic<uint64_t>   initial_pkts_recv;
    std::atomic<uint64_t>   initial_pkts_in_js;
    std::atomic<uint64_t>   initial_pkts_in_c;
    // Stats
    std::atomic<size_t>		total_allocated_conns_;
    std::atomic<size_t>		total_freed_conns_;

    void            DumpStats();
};

#endif // SMPSERVER_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : SMPServer.h
///////////////////////////////////////////////////////////////////////////////
