///////////////////////////////////////////////////////////////////////////////
// file : SMPServer.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include <time.h>
#include <fcntl.h>
#include <inttypes.h> // for PRI format macros
#include <openssl/rand.h> // for RAND_bytes
#include "SMPServer.h"
#include "Runtime.h"
#include "BC/BCJson.h"
#include "Utils.h"


#define XQC_PACKET_TMP_BUF_LEN 1500



#define _set_state(conn, _state, _status)	\
	(conn)->_SetState(_state, __LINE__);(conn)->close_status_ = _status


static inline xqc_usec_t time_now() { return bc_time_now(); }

///////////////////////////////////////////////////////////////////////////////
// function utilities
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// class : SMPServerStream
///////////////////////////////////////////////////////////////////////////////

SMPServerStream::SMPServerStream()
    : stream_(NULL)
    , start_time_(0)
    , end_time_(0)
    , conn_(NULL)
{
}

SMPServerStream::~SMPServerStream()
{
    
}

BCRESULT SMPServerStream::Create(
    xqc_stream_t* stream, 
    SMPServerConnection* pConn)
{
    stream_ = stream;
    conn_ = pConn;
    xqc_stream_set_user_data(stream, this);
    parser_.Create(this, xqc_stream_id(stream_));
    start_time_ = bc_time_now();

    return BC_R_SUCCESS;
}

int SMPServerStream::Send(const void *data, size_t size)
{
    ssize_t ret = 0;

    if (!stream_)
    {
        return -1;
    }
    ret = xqc_stream_send(stream_, (unsigned char*)data, size, 0);
    if (ret == -XQC_EAGAIN) 
    {
        // retry to send
    } 
    else if (ret < 0) 
    {
        printf("xqc_stream_send error %zd\n", ret);
        return 0;
    } 
    else 
    {
        //printf("xqc_stream_send sent:%zd,  stream_id : %" PRIu64 "\n", ret, 
        //    xqc_stream_id(stream_));
    }

    return 0;
}

BCRESULT SMPServerStream::SendPacket(SMPacketPtr pkt)
{
    if (pkt->packed_jmp_data)
    {
        BufferPtr payload(pkt->packed_jmp_data->RefClone());
        void* data;
        uint32_t nReadSize = INFINITE;
        while ((data = payload->ReadBlock(INFINITE, nReadSize)) && nReadSize > 0)
        {
            Send(data, nReadSize);
        }
    }
    else
    {
        parser_.PackData(pkt->type, pkt->timestamp, pkt->trans_id, pkt->origion_data);
    }
    return BC_R_SUCCESS;
}

int SMPServerStream::OnRead()
{
    unsigned char fin = 0;
    ssize_t read;
    do 
    {
        read = xqc_stream_recv(stream_, recv_buff_, recv_buff_size_, &fin);
        if (read == -XQC_EAGAIN) 
        {
            break;
        }
        else if (read < 0) 
        {
            printf("xqc_stream_recv_body error %zd\n", read);
            return 0;
        }
        else
        {
            parser_.Parse(recv_buff_, read);
        }
    } while (read > 0 && !fin);

    return 0;
}

void SMPServerStream::OnClose()
{
    end_time_ = bc_time_now();
    //xqc_usec_t duration = end_time_ - start_time_;
    //duration = BCMAX(duration, 1);
    //printf("total time : %" PRId64 "\n", duration);

    delete this;
}

void SMPServerStream::OnPacketAcked(
    xqc_usec_t ack_delay_time,
    size_t acked_bytes,
    size_t inflight_bytes)
{
    if (inflight_bytes < 16384)
    {
        //Send();
    }
}

void SMPServerStream::OnPacketParsed(
    const SMPHeader& refHeader, 
    const char* payload,
    size_t payload_size) 
{
    if (conn_)
    {
        conn_->ProcessPacket(refHeader, payload, payload_size);
    }
}

void SMPServerStream::OnDataPacked(void* payload, size_t payload_size)
{
    Send(payload, payload_size);
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPServerConnection
///////////////////////////////////////////////////////////////////////////////

IMPLEMENT_FIXED_ALLOC(SMPServerConnection, 32);

SMPServerConnection::SMPServerConnection() 
    : server_(NULL)
    , conn_(NULL)
    , handler_(NULL)
    , udp_socket_(NULL)
    , state_(CONN_STATE_INIT)
    , new_state_(CONN_STATE_MAX)
    , state_line_(0)
    , close_status_(BC_R_SUCCESS)
    , keep_working_(false)
    , enable_mask_(false)
{
    memset(&cid_, 0, sizeof(cid_));
}

SMPServerConnection::~SMPServerConnection()
{
}

BCRESULT SMPServerConnection::Create(
    SMPServer* server, 
    IServerConnectionHandler *pHandler, 
    const xqc_cid_t *cid,
    uint64_t id,
    UDPSender *pSender)
{
    BCRESULT result;
    BCTaskMgr* pTaskMgr;
    BCTimerMgr* pTimerMgr;

    if (!server || !pHandler || !pSender)
    {
        return BC_R_INVALIDARG;
    }
    pTaskMgr = Runtime::RandomTaskMgr();
    pTimerMgr = Runtime::RandomTimerMgr();
    result = BCEventQueue::Create(pTimerMgr, pTaskMgr, "SMPServerConnection", this);
    if (result != BC_R_SUCCESS)
    {
        return result;
    }
    memcpy(&cid_, cid, sizeof(*cid));
    id_ = id;
    server_ = server;
    handler_ = pHandler;
    udp_socket_ = pSender;
    state_ = CONN_STATE_HANDSHAKE;
    keep_working_ = true;

    return BC_R_SUCCESS;
}

BCRESULT SMPServerConnection::Accept(bool bAccept, std::shared_ptr<BCBuffer> respBuffer)
{
    if (keep_working_)
    {
        PostTask([bAccept, respBuffer, this] {
            if (_CloseCheck())
            {
                return;
            }
            Json::Value retRoot(Json::objectValue);
            if (bAccept)
            {
                retRoot["name"] = "_result";
            }
            else
            {
                retRoot["name"] = "_error";
            }
            retRoot["transId"] = 1;
            Json::Value userProps;
            bool result = ParseJsonFromBuffer(*respBuffer, userProps);
            if (!result || !userProps.isObject())
            {
                Json::Value props(Json::objectValue);

                props["serverVersion"] = "TTServer-Linux/1.0.0";
                props["protocolVersion"] = "1.0.0";
                retRoot["props"] = props;
            }
            else
            {
                userProps["serverVersion"] = "TTServer-Linux/1.0.0";
                userProps["protocolVersion"] = "1.0.0";
                retRoot["props"] = userProps;
            }
            Json::FastWriter writer;
            std::string ret = writer.write(retRoot);
            SMPacketPtr pkt(new SMPacket);
            pkt->Write(ret.c_str(), ret.length());
            pkt->type = SMP_TYPE_COMMAND;
            pkt->timestamp = bc_time_now() / 1000;
            SendPacket_Internal(pkt);
            if (!bAccept)
            {
                _set_state(this, CONN_STATE_FREED, BC_R_SUCCESS);
                _CloseCheck();
            }
        });
        return BC_R_SUCCESS;
    }
    return BC_R_NETDOWN;
}

BCRESULT SMPServerConnection::SendPacket(SMPacketPtr pkt)
{
    if (keep_working_)
    {
        PostTask([pkt, this] {
            SendPacket_Internal(pkt);
        });
        return BC_R_SUCCESS;
    }
    return BC_R_NETDOWN;
}

BCRESULT SMPServerConnection::AddCloseListener(ISMPServerConnListener* listener)
{
    BCSpinMutex::Owner lock(lock_);
    if (close_listeners_.find(listener) != close_listeners_.end())
    {
        return BC_R_EXISTS;
    }
    close_listeners_[listener] = listener;
    return BC_R_SUCCESS;
}

BCRESULT SMPServerConnection::RemoveCloseListener(ISMPServerConnListener* listener)
{
    BCSpinMutex::Owner lock(lock_);
    close_listeners_.erase(listener);
    return BC_R_SUCCESS;
}

void SMPServerConnection::ProcessPacket(
    const SMPHeader& refHeader,
    const char* lpszMsg,
    size_t size)
{
    switch (refHeader.m_eDataType)
    {
    case SMP::SMP_TYPE_COMMAND:
        {
            Json::Reader reader;
            Json::Value root;
            reader.parse(lpszMsg, lpszMsg + size, root);
            if (root["transId"] == 1 && root["name"] == "connect")
            {
                Json::FastWriter writer;
                std::string strProps = writer.write(root["props"]);
                state_ = CONN_STATE_CONNECTED;
                handler_->OnConnect(strProps.c_str(), strProps.length());
				
                bc_time_t now = bc_time_now();

                if (server_->start_time.load() == 0)
                {
                    server_->start_time.store(now);
                }
                server_->total_succeed_connections++;
                LogQ(server_->logger_ctx_, _INFO_, "connection succeed(%" _U64BITARG_
                    "), duration : %" _U64BITARG_ ".",
                    server_->total_succeed_connections.load(),
                    (now - server_->start_time.load()) / 1000);
            }
            else
            {
                handler_->OnRecvCmd(refHeader, lpszMsg, size);
            }
        }
        break;
    case SMP::SMP_TYPE_MESSAGE:
        if (handler_)
        {
            handler_->OnRecvData(refHeader, lpszMsg, size);
        }
        break;
    case SMP::SMP_TYPE_USER_CONTROL:
        break;
    default:
        break;
    }
}

BCRESULT SMPServerConnection::SendPacket_Internal(SMPacketPtr pkt)
{
    if (stream_map_.size() > 0 && conn_)
    {
        stream_map_[0]->SendPacket(pkt);
        return BC_R_SUCCESS;
    }
    return BC_R_FAILURE;
}

void SMPServerConnection::Close()
{
    if (keep_working_)
    {
        PostTask([this] {
            if (_CloseCheck())
            {
                return;
            }
            _set_state(this, CONN_STATE_FREED, BC_R_SUCCESS);
            _CloseCheck();
        });
    }
}

int SMPServerConnection::OnClose()
{
    conn_close_msg_ = jqc_conn_close_msg(conn_);
    conn_ = NULL;
    if (_CloseCheck())
    {
        return -1;
    }
    _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    _CloseCheck();

    return 0;
}

void SMPServerConnection::OnHandshakeFinished()
{
    state_ = CONN_STATE_CONNECTING;
    if (handler_)
    {
        handler_->OnHandshakeFinished();
    }
}

void SMPServerConnection::OnPacketAcked(
    xqc_usec_t ack_delay_time,
    size_t acked_bytes, 
    size_t inflight_bytes, 
    xqc_stream_id_t stream_id)
{
    //printf("packet acked, ack_delay_time : %" PRIu64 ", acked_bytes : %" PRIu64 
    //    ", inflight_bytes : %" PRIu64 "\n", ack_delay_time, acked_bytes, inflight_bytes);
    if (stream_map_.find(stream_id) != stream_map_.end())
    {
        stream_map_[stream_id]->OnPacketAcked(ack_delay_time, 
            acked_bytes, inflight_bytes);
    }
}

void SMPServerConnection::OnUpdataCID(
    const xqc_cid_t* retire_cid,
    const xqc_cid_t* new_cid)
{
    memcpy(&cid_, new_cid, sizeof(*new_cid));
}

int SMPServerConnection::OnConnCreate(
    xqc_connection_t* conn,
    const xqc_cid_t* cid)
{
    xqc_conn_set_alp_user_data(conn, this);

    return 0;
}

int SMPServerConnection::OnStreamCreate(xqc_stream_t* stream)
{
    SMPServerStream* user_stream = new SMPServerStream();
    if (!user_stream)
    {
        return -1;
    }
    BCRESULT result = user_stream->Create(stream, this);
    if (result != BC_R_SUCCESS)
    {
        goto delete_stream;
    }
    stream_map_[xqc_stream_id(stream)] = user_stream;

    return 0;

delete_stream:
    delete user_stream;
    return -1;
}

void SMPServerConnection::OnSaveTP(const char* data, size_t data_len)
{

}

void SMPServerConnection::OnSaveSession(const char* data, size_t data_len)
{

}

void SMPServerConnection::OnSaveToken(const unsigned char* token, size_t token_len)
{

}

ssize_t
SMPServerConnection::WritePacket(
    const unsigned char* buf,
    size_t size,
    const struct sockaddr* peer_addr,
    socklen_t peer_addrlen)
{
    ssize_t res;
    BCRESULT result;

    ASSERT(udp_socket_);
    do
    {
        BCSockAddrS peerAddr;
        memcpy(&peerAddr, peer_addr, peer_addrlen);
        peerAddr.length = peer_addrlen;
        result = udp_socket_->Send(peerAddr, buf, size);
        if (result != BC_R_SUCCESS && result != BC_R_INPROGRESS) {
            res = XQC_SOCKET_ERROR;
        }
        else
        {
            res = size;
            //LogBin(server_->logger_ctx_, _LOCAL_, buf, size);
        }
    } while (0);

    if (res < 0) {
        LogQ(server_->logger_ctx_, _ERROR_, "WritePacket(write error : %d) ", res);
        _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    }
    _CloseCheck();

    return res;
}

void SMPServerConnection::ProcessRecvData(std::shared_ptr<RecvInfo> info, bool initial)
{
    if (keep_working_)
    {
        PostTask([this, info, initial] {
            BCSpinMutex::Owner lock(lock_);
            if (!info || _CloseCheck())
            {
                return;
            }
            if (initial)
            {
                jqc_timer_callback_t timer_cbs = {
                    this,
                    on_timer_set,
                    on_timer_update,
                    on_timer_unset,
                    on_timer_next_tick
                };
                conn_ = jqc_engine_create_server_connection(server_->engine_, info->data, info->size,
                    (struct sockaddr*)(&info->local_addr.type.sa), info->local_addr.length,
                    (struct sockaddr*)(&info->peer_addr.type.sa), info->peer_addr.length,
                    /*path id*/XQC_UNKNOWN_PATH_ID, info->recv_time, &timer_cbs, this,
                    &info->scid, &info->dcid, &info->sr_process, &server_->conn_settings_);
                if (conn_)
                {
                    handler_->SetConnection(shared_from_this());
                    if (server_)
                    {
                        server_->NotifyConnCreated(handler_);
                    }
                }
                else
                {
                    _set_state(this, CONN_STATE_FREED, BC_R_NETUNREACH);
                }
            }
            if (conn_)
            {
                if (jqc_conn_packet_process(conn_, info->data, info->size,
                    (struct sockaddr*)(&info->local_addr.type.sa), info->local_addr.length,
                    (struct sockaddr*)(&info->peer_addr.type.sa), info->peer_addr.length,
                    /*path id*/XQC_UNKNOWN_PATH_ID, (xqc_msec_t)info->recv_time, this, 
                    &info->scid, &info->dcid, info->sr_process) != XQC_OK)
                {
                    LogQ(server_->logger_ctx_, _ERROR_, "SMPServerConnection::ProcessRecvData: packet process err\n");
                    return;
                }
                jqc_conn_finish_recv(conn_);
            }
            _CloseCheck();
        });
    }
    
}

void SMPServerConnection::OnEventProcShutdown()
{
    // Notify close listeners with lock
    {
        BCSpinMutex::Owner lock(lock_);
        for (auto &it : close_listeners_)
        {
            it.second->OnConnClosed(shared_from_this());
        }
    }
    if (server_)
    {
        server_->NotifyConnClosed(cid_);
    }
    if (handler_)
    {
        handler_->OnClosed(conn_close_msg_.c_str());
    }
}

int32_t  SMPServerConnection::on_timer_set(
    void* ctx,
    xqc_usec_t expire_time,
    void(*timeout_cb)(void*),
    void* user_data)
{
    int taskId = 0;
    int64_t now = bc_time_now();
    int64_t duration = expire_time - now;
    SMPServerConnection* _this = (SMPServerConnection*)ctx;
    if (!_this)
    {
        return 0;
    }
    if (duration < 1000) {
        duration = 1000;
    }
    ServerConnPtr conn = _this->shared_from_this();
    _this->ScheduleTask(taskId, [conn, timeout_cb, user_data](int32_t timerId) {
        if (timeout_cb)
        {
            timeout_cb(user_data);
        }
    }, duration);
    return taskId;
}

void SMPServerConnection::on_timer_update(
    void* ctx,
    int32_t timer_id,
    xqc_usec_t expire_time,
    void(*timeout_cb)(void*),
    void* user_data)
{
    int64_t now = bc_time_now();
    int64_t duration = expire_time - now;
    SMPServerConnection* _this = (SMPServerConnection*)ctx;
    if (!_this)
    {
        return;
    }
    if (duration < 1000) {
        duration = 1000;
    }
    ServerConnPtr conn = _this->shared_from_this();
    _this->ScheduleTask(timer_id, [conn, timeout_cb, user_data](int32_t timerId) {
        if (timeout_cb)
        {
            timeout_cb(user_data);
        }
    }, duration);
}

void SMPServerConnection::on_timer_unset(void* ctx, int32_t *timer_id, xqc_bool_t only_cancel)
{
    SMPServerConnection* _this = (SMPServerConnection*)ctx;
    if (!_this)
    {
        return;
    }
    _this->UnscheduleTask(*timer_id, only_cancel);
}

void SMPServerConnection::on_timer_next_tick(void* ctx)
{
    SMPServerConnection* _this = (SMPServerConnection*)ctx;
    if (!_this)
    {
        return;
    }
    ServerConnPtr conn = _this->shared_from_this();
    _this->PostTask([conn]() {
        if (!conn || conn->_CloseCheck() || !conn->conn_) 
        {
            return;
        }
        jqc_conn_main_logic(conn->conn_);
        conn->_CloseCheck();
    });
}

bool SMPServerConnection::_CloseCheck()
{
    if (state_ <= new_state_ && state_ > CONN_STATE_FREED)
    {
        return false;
    }

    if (state_ > CONN_STATE_INIT)
    {
        // Stop receive external event
        keep_working_ = false;
        if (conn_)
        {
            jqc_conn_close(conn_, &cid_);
        }
        state_ = CONN_STATE_CLOSING_QUIC;
    }

    if (state_ == CONN_STATE_CLOSING_QUIC)
    {
        if (conn_)
        {
            return true;
        }
        udp_socket_ = NULL;
        Detach();
        state_ = CONN_STATE_FREED;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPServer::Config
///////////////////////////////////////////////////////////////////////////////

BCRESULT SMPServer::Config::Init(BCFObject* pConfig)
{
    BCFVar* pVar;

    pVar = pConfig->Get("ipv6");
    if (IS_BCF_BOOL(pVar))
    {
        ipv6 = GET_BCF_BOOL(pVar);
    }
    pVar = pConfig->Get("host");
    if (IS_BCF_STRING(pVar))
    {
        host = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    pVar = pConfig->Get("port");
    if (IS_BCF_NUMBER(pVar))
    {
        port = (uint16_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("server_host");
    if (IS_BCF_STRING(pVar))
    {
        server_host = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    pVar = pConfig->Get("server_port");
    if (IS_BCF_NUMBER(pVar))
    {
        server_port = (uint16_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("c_cong_ctl");
    if (IS_BCF_NUMBER(pVar))
    {
        c_cong_ctl = (uint8_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("pacing_on");
    if (IS_BCF_NUMBER(pVar))
    {
        pacing_on = (uint8_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("idle_time_out");
    if (IS_BCF_NUMBER(pVar))
    {
        idle_time_out = (uint32_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("linger_on");
    if (IS_BCF_NUMBER(pVar))
    {
        linger_on = (uint8_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("log_level");
    if (IS_BCF_NUMBER(pVar))
    {
        log_level = (uint8_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("private_key_file");
    if (IS_BCF_STRING(pVar))
    {
        engine_ssl_config.private_key_file = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    pVar = pConfig->Get("cert_file");
    if (IS_BCF_STRING(pVar))
    {
        engine_ssl_config.cert_file = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    pVar = pConfig->Get("tls_ciphers");
    if (IS_BCF_STRING(pVar))
    {
        engine_ssl_config.ciphers = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    pVar = pConfig->Get("tls_groups");
    if (IS_BCF_STRING(pVar))
    {
        engine_ssl_config.groups = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    pVar = pConfig->Get("alpn");
    if (IS_BCF_STRING(pVar))
    {
        alpn = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    pVar = pConfig->Get("ping_on");
    if (IS_BCF_BOOL(pVar))
    {
        ping_on = GET_BCF_BOOL(pVar);
    }
    pVar = pConfig->Get("ping_interval");
    if (IS_BCF_NUMBER(pVar))
    {
        ping_interval = (uint32_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("log_file");
    if (IS_BCF_STRING(pVar))
    {
        log_file = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPServer
///////////////////////////////////////////////////////////////////////////////

SMPServer::SMPServer()
    : engine_(NULL)
    , timer_id_(0)
    , logger_ctx_(NULL)
    , udp_socket_(NULL)
    , state_(SVR_STATE_INIT)
    , new_state_(SVR_STATE_MAX)
    , state_line_(0)
    , close_status_(BC_R_SUCCESS)
    , start_time(0)
    , total_succeed_connections(0)
    , initial_pkts_recv(0)
    , initial_pkts_in_js(0)
    , initial_pkts_in_c(0)
    , total_allocated_conns_(0)
    , total_freed_conns_(0)
{
    memset(&local_addr_, 0, sizeof(local_addr_));
}

SMPServer::~SMPServer()
{
}

BCRESULT SMPServer::Create(BCFObject* pConfig, IServerHandler* pHandler)
{
    BCRESULT result;

    if (!pConfig || !pHandler)
    {
        return BC_R_INVALIDARG;
    }
    config_.Init(pConfig);
    if (!config_.log_file || !config_.alpn)
    {
        return BC_R_INVALIDARG;
    }
	logger_ctx_ = AddFileLogAppender(config_.log_file, _FINEST_, true, true);
    conn_settings_ = xqc_conn_get_conn_settings_template(XQC_CONN_SETTINGS_LOW_DELAY);
    if (config_.ping_on)
    {
        conn_settings_.ping_on = 1;
    }
    if (config_.ping_interval > 0)
    {
        conn_settings_.ping_time_out = config_.ping_interval;
    }
    if (config_.idle_time_out > 0)
    {
        conn_settings_.idle_time_out = config_.idle_time_out;
    }

    xqc_engine_callback_t callback = {
        .set_event_timer = set_timer_cb,
        .log_callbacks = {
            .xqc_log_write_err = on_write_log,
            .xqc_log_write_stat = on_write_log,
        },
        .cid_generate_cb = cid_generate,
        .keylog_cb = on_keylog_cb,
        .realtime_ts = time_now,
        .monotonic_ts = time_now,
        .lock_callbacks = {
            EngCallbacks::lock_create,
            EngCallbacks::lock_acquire,
            EngCallbacks::lock_release,
            EngCallbacks::lock_destory
        },
        .print_stack_cb = EngCallbacks::print_stack
    };

    xqc_transport_callbacks_t tcbs = {
        .server_accept = on_server_accept,
        .server_refuse = on_server_refuse,
        .stateless_reset = on_stateless_reset,
        .write_socket = conn_write_socket_cb,
        .write_mmsg = NULL,
        .write_socket_ex = NULL,
        .write_mmsg_ex = NULL,
        .conn_update_cid_notify = on_conn_update_cid_notify,
        .save_token = on_conn_save_token,
        .save_session_cb = on_conn_save_session,
        .save_tp_cb = on_conn_save_tp,
        .cert_verify_cb = on_conn_cert_verify,
        .ready_to_create_path_notify = NULL,
        .path_created_notify = NULL,
        .path_removed_notify = NULL,
        .conn_closing = on_conn_closing_notify,
    };

    xqc_config_t config;
    if (xqc_engine_get_default_config(&config, XQC_ENGINE_SERVER) < 0) 
    {
        return BC_R_INVALIDARG;
    }
    switch (config_.log_level)
    {
    case 'f':
        config.cfg_log_level = XQC_LOG_FATAL;
        break;
    case 'e':
        config.cfg_log_level = XQC_LOG_ERROR;
        break;
    case 'w':
        config.cfg_log_level = XQC_LOG_WARN;
        break;
    case 'i':
        config.cfg_log_level = XQC_LOG_INFO;
        break;
    case 'd':
        config.cfg_log_level = XQC_LOG_DEBUG;
        break;
    default:
        config.cfg_log_level = XQC_LOG_WARN;
        break;
    }
    config.cid_negotiate = 1;
    quic_lb_ctx_.conf_id = 0;
    quic_lb_ctx_.cid_len = config.cid_len;
    quic_lb_ctx_.sid_len = 0;

    engine_ = xqc_engine_create(XQC_ENGINE_SERVER, &config, 
        &config_.engine_ssl_config, &callback, &tcbs, this);
    if (engine_ == NULL) {
        LogQ(logger_ctx_, _ERROR_, "SMPServer create failed : error create engine\n");
        return BC_R_INVALIDARG;
    }

    /* register application-protocol callbacks */
    xqc_app_proto_callbacks_t ap_cbs = {
        .conn_cbs = {
            .conn_create_notify = on_sm_conn_create_notify,
            .conn_close_notify = on_sm_conn_close_notify,
            .conn_handshake_finished = on_sm_conn_handshake_finished,
            .conn_ping_acked = NULL,
            .conn_packet_acked = on_sm_conn_packet_acked
        },
        .stream_cbs = {
            .stream_read_notify = on_stream_read_notify,
            .stream_write_notify = on_stream_write_notify,
            .stream_create_notify = on_stream_create_notify,
            .stream_close_notify = on_stream_close_notify,
        }
    };

    /* register transport callbacks */
    xqc_engine_register_alpn(engine_, config_.alpn, strlen(config_.alpn), &ap_cbs);
    handler_ = pHandler;
    udp_socket_ = new UDPSender();
    if (!udp_socket_)
    {
        return BC_R_NOMEMORY;
    }
    result = udp_socket_->Create(logger_ctx_, Runtime::TaskMgr(), Runtime::TimerMgr(),
        Runtime::SocketMgr(), pConfig, this, true, true);
    if (result != BC_R_SUCCESS)
    {
        goto delete_socket;
    }
    udp_socket_->GetSockName(local_addr_);
    state_ = SVR_STATE_WORKING;

    return BC_R_SUCCESS;

delete_socket:
    BC_SAFE_DELETE_PTR(udp_socket_);

    return result;
}

BCRESULT SMPServer::CreateConnection(
    IServerConnectionHandler* pHandler, 
    RecvInfo *pInfo)
{
    if (!pHandler || !pInfo)
    {
        return BC_R_INVALIDARG;
    }
    std::shared_ptr<RecvInfo> info(pInfo);
    Runtime::PostTask([this, pHandler, info] {

        BCSpinMutex::Owner lock(lock_);
        if (_CloseCheck())
        {
            return;
        }
        initial_pkts_in_c++;
        DumpStats();

        auto iter = conns_map_.find(info->scid);
        if (iter != conns_map_.end())
        {
            iter->second->ProcessRecvData(info, true);
            return;
        }

        ServerConnPtr pConn(new SMPServerConnection());
        if (!pConn)
        {
            return;
        }
        BCRESULT result = pConn->Create(this, pHandler, &info->scid,
            next_conn_id_++, udp_socket_);
        if (result != BC_R_SUCCESS)
        {
            pConn.reset();
            return;
        }
        handlers_map_[info->scid] = pHandler;
        conns_map_[info->scid] = pConn;
        total_allocated_conns_++;
        pConn->ProcessRecvData(info, true);
        _CloseCheck();
    });
    return BC_R_SUCCESS;
}

BCRESULT SMPServer::Start()
{
    if (!udp_socket_)
    {
        return BC_R_UNEXPECTED;
    }
    return udp_socket_->StartRecv();
}

void SMPServer::NotifyConnCreated(IServerConnectionHandler* pHandler)
{
    if (handler_)
    {
        handler_->OnAccept(pHandler);
    }
}

void SMPServer::UpdateCID(
    const xqc_cid_t* retire_cid,
    const xqc_cid_t* new_cid,
    SMPServerConnection* pConn)
{
    BCSpinMutex::Owner lock(lock_);
    auto iter = conns_map_.find(*retire_cid);
    if (iter != conns_map_.end())
    {
        ServerConnPtr pConn = iter->second;
        conns_map_.erase(iter);
        conns_map_[*new_cid] = pConn;
    }
}

void SMPServer::NotifyConnClosed(xqc_cid_t& cid)
{
    BCSpinMutex::Owner lock(lock_);
    handlers_map_.erase(cid);
    conns_map_.erase(cid);
    total_freed_conns_++;
    Runtime::PostTask([this] {
        BCSpinMutex::Owner lock(lock_);
        _CloseCheck();
    });
}

ssize_t SMPServer::WritePacket(
    const unsigned char* buf,
    size_t size,
    const struct sockaddr* peer_addr,
    socklen_t peer_addrlen)
{
    ssize_t res;
    BCRESULT result;

    if (size > XQC_PACKET_TMP_BUF_LEN)
    {
        LogQ(logger_ctx_, _ERROR_, "WritePacket err: size=%zu is too long\n", size);
        return XQC_SOCKET_ERROR;
    }

    do
    {
        BCSockAddrS peerAddr;
        memcpy(&peerAddr, peer_addr, peer_addrlen);
        peerAddr.length = peer_addrlen;
        result = udp_socket_->Send(peerAddr, buf, size);
        if (result != BC_R_SUCCESS && result != BC_R_INPROGRESS) {
            res = XQC_SOCKET_ERROR;
        }
        else
        {
            res = size;
        }
    } while (0);

    return res;
}

BCRESULT SMPServer::GetStats(ConnStatS& stats)
{
    BCSpinMutex::Owner lock(lock_);
    stats.allocated_conn_size = total_allocated_conns_;
    stats.freed_conn_size = total_freed_conns_;
    stats.active_conn_size = conns_map_.size();
    return BC_R_SUCCESS;
}

void SMPServer::Close()
{
    Runtime::PostTask([this] {
        BCSpinMutex::Owner lock(lock_);
        if (_CloseCheck())
        {
            return;
        }
        _set_state(this, SVR_STATE_FREED, BC_R_SUCCESS);
        _CloseCheck();
    });
}

bool SMPServer::_CloseCheck()
{
    if (state_ <= new_state_ && state_ > SVR_STATE_FREED)
    {
        return false;
    }

    if (state_ == SVR_STATE_WORKING)
    {
        for (auto &iter : conns_map_)
        {
            iter.second->Close();
        }
        state_ = SVR_STATE_DRAIN;
    }

    if (state_ == SVR_STATE_DRAIN)
    {
        if (conns_map_.size() > 0)
        {
            return true;
        }
        if (udp_socket_)
        {
            udp_socket_->Close();
        }
        state_ = SVR_STATE_INIT;
    }

    if (state_ == SVR_STATE_INIT)
    {
        if (udp_socket_)
        {
            return true;
        }
        xqc_engine_destroy(engine_);
        if (logger_ctx_)
        {
            RemoveLogAppender(logger_ctx_);
        }
        Runtime::PostTask([this] {
            handler_->OnClosed();
        });
        state_ = SVR_STATE_FREED;
    }

    return true;
}

void SMPServer::OnSendData(uint32_t nWrite, UDPSender* pSender)
{

}

void SMPServer::OnRecvData(BCBuffer* pBuffer, BCSockAddrS& refSrcAddr)
{
    //socklen_t peer_addrlen = config_.ipv6 ? sizeof(struct sockaddr_in6)
    //    : sizeof(struct sockaddr_in);
    uint64_t recv_time;
    unsigned char* packet_buf = NULL;
    uint32_t recv_size = 0;
    jqc_pre_process_t pre;
    xqc_int_t ret;

    BCSpinMutex::Owner lock(lock_);
    if (_CloseCheck())
    {
        return;
    }

    recv_time = bc_time_now();
    do {
        packet_buf = (unsigned char*)pBuffer->ReadBlock(pBuffer->RemainingLength(), recv_size);

        ret = jqc_engine_packet_pre_process(engine_, packet_buf, recv_size, &pre);
        if (ret != XQC_OK)
        {
            continue;
        }
        else if (pre.conn)
        {
            SMPServerConnection* pConn = (SMPServerConnection*)xqc_conn_get_transport_user_data(pre.conn);
            if (pConn && pConn->keep_working_)
            {
                std::shared_ptr<RecvInfo> pInfo(new RecvInfo);
                memcpy(pInfo->data, packet_buf, recv_size);
                pInfo->size = recv_size;
                pInfo->scid = pre.scid;
                pInfo->dcid = pre.dcid;
                pInfo->local_addr = local_addr_;
                pInfo->peer_addr = refSrcAddr;
                pInfo->recv_time = recv_time;
                pConn->ProcessRecvData(pInfo, false);
            }
        }
        else if (pre.init_or_0rtt && handler_)
        {
            initial_pkts_recv++;
            DumpStats();

            RecvInfo* pInfo = new RecvInfo();
            memcpy(pInfo->data, packet_buf, recv_size);
            pInfo->size = recv_size;
            pInfo->scid = pre.scid;
            pInfo->dcid = pre.dcid;
            pInfo->local_addr = local_addr_;
            pInfo->peer_addr = refSrcAddr;
            pInfo->recv_time = recv_time;
            handler_->OnNewConn(pInfo);
        }
        //LogBin(logger_ctx_, _LOCAL_, packet_buf, recv_size);
        //if (xqc_engine_packet_process(this->engine_, packet_buf, recv_size,
        //    (struct sockaddr*)(&this->local_addr_), this->local_addrlen_,
        //    (struct sockaddr*)(&refSrcAddr.type.sa), peer_addrlen,
        //    /*path id*/0, (xqc_msec_t)recv_time, this) != XQC_OK)
        //{
        //    printf("xqc_server_read_handler: packet process err\n");
        //    return;
        //}
    } while (pBuffer->RemainingLength() > 0);
    _CloseCheck();
}

void SMPServer::OnRestart(BCRESULT result)
{

}

void SMPServer::OnUdpClosed()
{
    Runtime::PostTask([this] {
        BC_SAFE_DELETE_PTR(udp_socket_);
        if (_CloseCheck())
        {
            return;
        }
        _set_state(this, SVR_STATE_FREED, BC_R_NETDOWN);
        _CloseCheck();
    });
}

void SMPServer::set_timer_cb(xqc_usec_t wake_after, void *user_data)
{
    SMPServer *server = (SMPServer *) user_data;
    if (server)
    {
    }
}

void SMPServer::on_conn_save_session(
    const char *data, 
    size_t data_len,
    void *user_data) {
    SMPServerConnection *user_conn = (SMPServerConnection *)user_data;
    if (user_conn)
    {
        user_conn->OnSaveSession(data, data_len);
    }
    return;
}

void SMPServer::on_conn_save_tp(
    const char *data, 
    size_t data_len, 
    void *user_data) 
{
    SMPServerConnection *user_conn = (SMPServerConnection *)user_data;
    if (user_conn)
    {
        user_conn->OnSaveTP(data, data_len);
    }
    return;
}

void SMPServer::on_conn_save_token(
    const unsigned char *token,
    unsigned token_len,
    void *user_data)
{
    SMPServerConnection *user_conn = (SMPServerConnection *)user_data;
    if (user_conn) {
        user_conn->OnSaveToken(token, token_len);
    }
}

void
SMPServer::on_conn_update_cid_notify(
    xqc_connection_t *conn, 
    const xqc_cid_t *retire_cid, 
    const xqc_cid_t *new_cid, 
    void *user_data)
{
    SMPServerConnection *user_conn = (SMPServerConnection *) user_data;
    if (user_conn)
    {
        user_conn->OnUpdataCID(retire_cid, new_cid);
    }
}

int SMPServer::on_conn_cert_verify(
    const unsigned char *certs[],
    const size_t cert_len[], 
    size_t certs_len,
    void *conn_user_data) {
    /* self-signed cert used in test cases, return >= 0 means success */
    return 0;
}

int SMPServer::on_conn_closing_notify(
    xqc_connection_t *conn,
    const xqc_cid_t *cid,
    xqc_int_t err_code,
    void *conn_user_data) {
    //printf("conn closing: %d\n", err_code);
    return XQC_OK;
}

int
SMPServer::on_sm_conn_create_notify(
    xqc_connection_t *conn, 
    const xqc_cid_t *cid, 
    void *conn_user_data,
    void *conn_proto_data) {
    SMPServerConnection* user_conn = (SMPServerConnection*)conn_user_data;
    if (user_conn)
    {
        user_conn->OnConnCreate(conn, cid);
    }
    return 0;
}

int
SMPServer::on_sm_conn_close_notify(
    xqc_connection_t *conn, 
    const xqc_cid_t *cid, 
    void *conn_user_data,
    void *conn_proto_data) {
    SMPServerConnection *user_conn = (SMPServerConnection*)conn_user_data;
    if (user_conn)
    {
        return user_conn->OnClose();
    }
    return -1;
}

void
SMPServer::on_sm_conn_handshake_finished(
    xqc_connection_t *h3_conn, 
    void *conn_user_data,
    void *conn_proto_data) {
    SMPServerConnection *user_conn = (SMPServerConnection *)conn_user_data;
    if (user_conn)
    {
        user_conn->OnHandshakeFinished();
    }
}

void     
SMPServer::on_sm_conn_packet_acked(
    xqc_connection_t* h3_conn,
    xqc_usec_t ack_delay_time,
    size_t acked_bytes,
    size_t inflight_bytes,
    xqc_stream_id_t stream_id,
    void* conn_user_data,
    void *conn_proto_data) {
    SMPServerConnection *user_conn = (SMPServerConnection *)conn_user_data;
    if (user_conn)
    {
        user_conn->OnPacketAcked(ack_delay_time, acked_bytes, 
            inflight_bytes, stream_id);
    }
}

int
SMPServer::on_stream_create_notify(xqc_stream_t *stream, void *strm_user_data)
{
    int ret = -1;

    SMPServerConnection *conn =
        (SMPServerConnection *)xqc_get_conn_user_data_by_stream(stream);
    if (conn)
    {
        ret = conn->OnStreamCreate(stream);
    }
    return ret;
}

int
SMPServer::on_stream_close_notify(xqc_stream_t *stream, void *user_data)
{
    SMPServerStream *user_stream = (SMPServerStream*)user_data;
    if (user_stream)
    {
        user_stream->OnClose();
    }

    return 0;
}

int
SMPServer::on_stream_write_notify(xqc_stream_t *stream, void *user_data)
{
    //DEBUG_PRINT;
    int ret = 0;
    SMPServerStream *user_stream = (SMPServerStream *) user_data;
    if (user_stream)
    {
        //ret = user_stream->Send();
    }
    return ret;
}

int
SMPServer::on_stream_read_notify(
    xqc_stream_t *stream, 
    void *user_data)
{
    int ret = 0;
    SMPServerStream *user_stream = (SMPServerStream *) user_data;
    if (user_stream)
    {
        ret = user_stream->OnRead();
    }
    return ret;
}


ssize_t 
SMPServer::conn_write_socket_cb(
    const unsigned char *buf, 
    size_t size,
    const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, 
    void *user_data)
{
    int res = 0;
    SMPServerConnection *conn = (SMPServerConnection*)user_data; //user_data may be empty when "reset" is sent
    if (conn)
    {
        res = conn->WritePacket(buf, size, peer_addr, peer_addrlen);
    }

    return res;
}

void 
SMPServer::on_write_log(
    xqc_log_level_t lvl, 
    const void *buf, 
    size_t count, 
    void *engine_user_data)
{
    SMPServer *ctx = (SMPServer*)engine_user_data;

    int32_t level = XQCLogLevelToBCLogLevel(lvl);
    LogQ(ctx->logger_ctx_, level, "%.*s", count, buf);
}

void SMPServer::on_keylog_cb(const xqc_cid_t *scid, const char *line,
                            void *user_data) {
    SMPServer *ctx = (SMPServer*)user_data;
    
    LogCustom(ctx->logger_ctx_, _INFO_, "%s", line);
}

int
SMPServer::on_server_accept(
    xqc_engine_t* engine,
    xqc_connection_t* conn,
    const xqc_cid_t* cid,
    void* user_data)
{
    //We MUST set this callback to make XQC_CONN_FLAG_UPPER_CONN_EXIST flag to be set,
    // or we will accidentally loss conn_close_notify event
    return 0;
}

void
SMPServer::on_server_refuse(
    xqc_engine_t* engine,
    xqc_connection_t* conn,
    const xqc_cid_t* cid,
    void* user_data)
{
    /* ALPN context is not initialized, ClientHello has not been received */
    //We MUST set this callback to handle close event before ALPN context initialized,
    // or we will accidentally loss conn_close_notify event
    SMPServerConnection* user_conn = (SMPServerConnection*)user_data;
    if (conn)
    {
        user_conn->OnClose();
    }
}

ssize_t
SMPServer::on_stateless_reset(
    const unsigned char *buf, 
    size_t size,
    const struct sockaddr *peer_addr,
    socklen_t peer_addrlen,
    const struct sockaddr *local_addr,
    socklen_t local_addrlen, 
    void *user_data) 
{
    int res = 0;
    SMPServer *_this = (SMPServer*)user_data; //user_data may be empty when "reset" is sent
    if (_this)
    {
        res = _this->WritePacket(buf, size, peer_addr, peer_addrlen);
    }

    return res;
}

void SMPServer::DumpStats()
{
    //LogQ(logger_ctx_, _DEBUG_, "initial_pkts_recv : %" _U64BITARG_ 
    //    "; initial_pkts_in_js : %" _U64BITARG_ 
    //    "; initial_pkts_in_c : %" _U64BITARG_, 
    //    initial_pkts_recv.load(),
    //    initial_pkts_in_js.load(),
    //    initial_pkts_in_c.load());
}

ssize_t
SMPServer::cid_generate(
    const xqc_cid_t* ori_cid, 
    uint8_t* cid_buf, 
    size_t cid_buflen, 
    void* engine_user_data,
    void *conn_user_data)
{
    SMPServer*_this = (SMPServer*)engine_user_data;
    if (!_this)
    {
        return 0;
    }

    ssize_t              cid_buf_index = 0, i;
    ssize_t              cid_len, sid_len;
    xqc_quic_lb_ctx_t   * quic_lb_ctx;

    quic_lb_ctx = &(_this->quic_lb_ctx_);

    cid_len = quic_lb_ctx->cid_len;
    sid_len = quic_lb_ctx->sid_len;

    if (sid_len < 0 || sid_len > cid_len || cid_len > (ssize_t)cid_buflen) {
        return XQC_ERROR;
    }

    cid_buf[cid_buf_index] = quic_lb_ctx->conf_id;
    cid_buf_index += 1;

    memcpy(cid_buf + cid_buf_index, quic_lb_ctx->sid_buf, sid_len);
    cid_buf_index += sid_len;

    //for (i = cid_buf_index; i < cid_len; i++) {
    //    cid_buf[i] = (uint8_t)rand();
    //}
    RAND_bytes(&cid_buf[cid_buf_index], cid_len - cid_buf_index);

    /* xqc_log(engine->log, XQC_LOG_DEBUG, "|cid:%s|cid_len:%ud|", xqc_scid_str(cid), cid->cid_len); */
    return cid_len;
}

///////////////////////////////////////////////////////////////////////////////
// End of file : SMPServer.cpp
///////////////////////////////////////////////////////////////////////////////
