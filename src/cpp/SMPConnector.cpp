///////////////////////////////////////////////////////////////////////////////
// file : SMPConnector.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include <time.h>
#include <fcntl.h>
#include <inttypes.h> // for PRI format macros
#include <openssl/rand.h> // for RAND_bytes
#include <HTTP/HTTPProtocol.h>
#include "SMPConnector.h"
#include "Runtime.h"
#include "BC/BCJson.h"
#include "Utils.h"


#define XQC_PACKET_TMP_BUF_LEN 1500
#define DEFAULT_CONNECT_TIMEOUT 10000

#define LOG_LEVEL_DEBUG    0x01
#define LOG_LEVEL_INFO     0x02
#define LOG_LEVEL_WARN     0x03
#define LOG_LEVEL_ERROR    0x04
#define LOG_LEVEL_FATAL    0x05


static inline xqc_usec_t time_now() { return bc_time_now(); }

static const char webtransport_hdr_mask[] = {0x40, 0x41, 0x00};

///////////////////////////////////////////////////////////////////////////////
// function utilities
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// class : SMPStream
///////////////////////////////////////////////////////////////////////////////

SMPStream::SMPStream()
    : stream_(NULL)
    , start_time_(0)
    , end_time_(0)
    , conn_(NULL)
{
}

SMPStream::~SMPStream()
{
    conn_->stream_map_.erase(stream_id_);
}

BCRESULT SMPStream::Create(
    xqc_stream_id_t id,
    xqc_stream_t* stream, 
    SMPConnection* pConn)
{
    stream_id_ = id;
    stream_ = stream;
    conn_ = pConn;
    xqc_stream_set_user_data(stream, this);
    parser_.Create(this, id);
    start_time_ = bc_time_now();

    return BC_R_SUCCESS;
}

int SMPStream::Send(const void *data, size_t size)
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
        LogQ(conn_->connector_->logger_ctx_, "xqc_stream_send error %zd\n", ret);
        return 0;
    }

    return 0;
}

BCRESULT SMPStream::SendPacket(SMPacketPtr pkt)
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

void SMPStream::Close()
{
    if (stream_)
    {
        xqc_stream_close(stream_);
        stream_ = NULL;
    }
}

int SMPStream::OnRead()
{
    unsigned char fin = 0;
    ssize_t read, skip_size = 0;

    if (conn_->_CloseCheck()){
        return -XQC_CLOSING;
    }
    do 
    {
        read = xqc_stream_recv(stream_, recv_buff_, recv_buff_size_, &fin);
        if (read == -XQC_EAGAIN) 
        {
            break;
        }
        else if (read < 0) 
        {
            LogQ(conn_->connector_->logger_ctx_, "xqc_stream_recv_body error %zd\n", read);
            return 0;
        }
        else
        {
            if (conn_->webtransport_ && !recv_wt_mask_)
            {
                if (memcmp(recv_buff_, webtransport_hdr_mask, 3) == 0)
                {
                    skip_size = 3;
                }
            }
            parser_.Parse(recv_buff_+skip_size, read-skip_size);
            if (conn_->webtransport_ && !recv_wt_mask_)
            {
                recv_wt_mask_ = true;
                skip_size = 0;
            }
        }
    } while (read > 0 && !fin);

    return 0;
}

void SMPStream::OnClosing()
{
    
}

void SMPStream::OnClose()
{
    end_time_ = bc_time_now();
    if (conn_ && conn_->handler_)
    {
        conn_->handler_->OnStreamClosed(stream_id_);
    }

    delete this;
}

void SMPStream::OnPacketAcked(
    xqc_usec_t ack_delay_time,
    size_t acked_bytes,
    size_t inflight_bytes)
{
    if (inflight_bytes < 16384)
    {
        //Send();
    }
}

void SMPStream::OnPacketParsed(
    const SMPHeader& refHeader, 
    const char* payload,
    size_t payload_size) 
{
    if (conn_)
    {
        conn_->ProcessPacket(refHeader, payload, payload_size);
    }
}

void SMPStream::OnDataPacked(void* payload, size_t payload_size)
{
    Send(payload, payload_size);
}

///////////////////////////////////////////////////////////////////////////////
// class : H3Stream
///////////////////////////////////////////////////////////////////////////////

H3Stream::H3Stream()
    : stream_(NULL)
    , start_time_(0)
    , end_time_(0)
    , conn_(NULL)
{
}

H3Stream::~H3Stream()
{
    conn_->h3_stream_map_.erase(stream_id_);
}

BCRESULT H3Stream::Create(
    xqc_stream_id_t id,
    xqc_h3_request_t* stream, 
    SMPConnection* pConn)
{
    stream_id_ = id;
    stream_ = stream;
    conn_ = pConn;
    xqc_h3_request_set_user_data(stream, this);
    parser_.Create(this, id);
    start_time_ = bc_time_now();

    return BC_R_SUCCESS;
}

BCRESULT H3Stream::SendRequest()
{
    int ret, fin;
    /* send packet header/body */
    std::vector<xqc_http_header_t> headers;
    Json::Reader reader;
    Json::Value root;
    KBPool pool;

    std::string authority = conn_->host_ + ":" + std::to_string(conn_->port_);
    xqc_http_header_t method = {
        .name = {.iov_base = (char*)":method", .iov_len = 7 },
            .value = {.iov_base = (char*)"CONNECT", .iov_len = 7 },
            .flags = 0,
    };
    headers.push_back(method);
    xqc_http_header_t protocol = {
        .name = {.iov_base = (char*)":protocol", .iov_len = 9 },
            .value = {.iov_base = (char*)"webtransport", .iov_len = 12 },
            .flags = 0,
    };
    headers.push_back(protocol);
    xqc_http_header_t authority_hdr = {
        .name = {.iov_base = (char*)":authority", .iov_len = 10 },
        .value = {.iov_base = (char*)pool.Strdup(authority.c_str()), .iov_len = authority.length() },
        .flags = 0,
	};
    headers.push_back(authority_hdr);
    xqc_http_header_t scheme{
        .name = {.iov_base = kQScheme.Ptr, .iov_len = kQScheme.Len },
            .value = {.iov_base = (char*)conn_->scheme_.c_str(), .iov_len = conn_->scheme_.length() },
            .flags = 0,
    };
    headers.push_back(scheme);
    xqc_http_header_t host_hdr = {
        .name = {.iov_base = kHost.Ptr, .iov_len = kHost.Len},
        .value = {.iov_base = (char*)conn_->host_.c_str(), .iov_len = conn_->host_.length()},
        .flags = 0,
    };
    headers.push_back(host_hdr);
    {
        std::string path = conn_->path_;
        if (path.empty()) {
            path = "/";
        }
        if (conn_->query_.length() > 0)
        {
            path += "?" + conn_->query_;
        }
        xqc_http_header_t path_hdr = {
            .name = {.iov_base = kQPath.Ptr, .iov_len = kQPath.Len},
            .value = {.iov_base = (char*)pool.Strdup(path.c_str()), .iov_len = path.length()},
            .flags = 0,
		};
        headers.push_back(path_hdr);
    }
    xqc_http_header_t webtransport_hdr = {
        .name = {.iov_base = (char*)"Sec-WebTransport-Http3-Draft02", .iov_len = 30},
        .value = {.iov_base = (char*)"1", .iov_len = 1},
        .flags = 0,
    };
    headers.push_back(webtransport_hdr);
    if (conn_->props_.isObject()) {
        for (Json::Value::iterator it = conn_->props_.begin(); it != conn_->props_.end(); ++it) { 
            std::string key = it.key().asString();
            std::string value = (*it).asString();
            xqc_http_header_t hdr = {
                .name = {.iov_base = (char*)pool.Strndup(key.c_str(), key.length()), .iov_len = key.length()},
                .value = {.iov_base = (char*)pool.Strndup(value.c_str(), value.length()), .iov_len = value.length()},
                .flags = 0,
            };
            headers.push_back(hdr);
        }
    }

    fin = 0; // keep connection alive, or other stream will be closed
    h3_hdrs_.headers = headers.data();
    h3_hdrs_.count = headers.size();

    if (start_time_ == 0) {
        start_time_ = xqc_now();
    }
    /* send header */
    ret = xqc_h3_request_send_headers(stream_, &h3_hdrs_, fin);
    if (ret < 0) {
        LogQ(conn_->connector_->logger_ctx_, "[error] xqc_h3_request_send_headers error %d\n", ret);
    } else {
        hdr_sent_ = 1;
    }
    return BC_R_SUCCESS;
}

int H3Stream::Send(const void *data, size_t size)
{
    // ssize_t ret = 0;

    // if (!stream_)
    // {
    //     return -1;
    // }
    // ret = xqc_h3_stream_send(stream_, (unsigned char*)data, size, 0);
    // if (ret == -XQC_EAGAIN) 
    // {
    //     // retry to send
    // } 
    // else if (ret < 0) 
    // {
    //     LogQ(conn_->connector_->logger_ctx_, "xqc_stream_send error %zd\n", ret);
    //     return 0;
    // } 
    // else 
    // {
    //     //LogQ(conn_->connector_->logger_ctx_, "xqc_stream_send sent:%zd,  stream_id : %" PRIu64 "\n", ret, 
    //     //    xqc_stream_id(stream_));
    // }

    return 0;
}

BCRESULT H3Stream::SendPacket(SMPacketPtr pkt)
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

int H3Stream::OnRead(xqc_request_notify_flag_t flags)
{
    ssize_t read, read_sum;
    unsigned char fin = 0;

    if (conn_->_CloseCheck()){
        return -XQC_CLOSING;
    }

    if (flags & XQC_REQ_NOTIFY_READ_HEADER) {
        xqc_http_headers_t *headers;
        headers = xqc_h3_request_recv_headers(stream_, &fin);
        if (headers == NULL) {
            if (conn_)
            {
                conn_->OnH3RequestRecvHeaders(headers);
            }
            return XQC_ERROR;
        }

        if (conn_)
        {
            conn_->OnH3RequestRecvHeaders(headers);
        }

        if (fin) {
            /* only header in request */
            recv_fin_ = 1;
            LogQ(conn_->connector_->logger_ctx_, "[stats] h3 request read header finish \n");
            return XQC_OK;
        }
    }

    /* continue to recv body */
    if (!(flags & XQC_REQ_NOTIFY_READ_BODY)) {
        return XQC_OK;
    }

    read = read_sum = 0;

    do {
        read = xqc_h3_request_recv_body(stream_, recv_buff_, recv_buff_size_, &fin);
        if (read == -XQC_EAGAIN) {
            break;

        } else if (read < 0) {
            LogQ(conn_->connector_->logger_ctx_, "xqc_h3_request_recv_body error %zd\n", read);
            return XQC_OK;
        }
    
        read_sum += read;
        recv_body_size_ += read;
    } while (read > 0 && !fin);

    return 0;
}

void H3Stream::OnClosing()
{

}

void H3Stream::OnClose()
{
    end_time_ = bc_time_now();
    //xqc_usec_t duration = end_time_ - start_time_;
    //duration = BCMAX(duration, 1);
    //LogQ(conn_->connector_->logger_ctx_, "total time : %" PRId64 "\n", duration);

    delete this;
}

void H3Stream::OnPacketAcked(
    xqc_usec_t ack_delay_time,
    size_t acked_bytes,
    size_t inflight_bytes)
{
    if (inflight_bytes < 16384)
    {
        //Send();
    }
}

void H3Stream::OnPacketParsed(
    const SMPHeader& refHeader, 
    const char* payload,
    size_t payload_size) 
{
    if (conn_)
    {
        conn_->ProcessPacket(refHeader, payload, payload_size);
    }
}

void H3Stream::OnDataPacked(void* payload, size_t payload_size)
{
    Send(payload, payload_size);
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPConnection::Config
///////////////////////////////////////////////////////////////////////////////

BCRESULT SMPConnection::Config::Init(BCFObject* pConfig)
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
    if (IS_BCF_BOOL(pVar))
    {
        pacing_on = GET_BCF_BOOL(pVar);
    }
    pVar = pConfig->Get("idle_time_out");
    if (IS_BCF_NUMBER(pVar))
    {
        idle_time_out = (uint32_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("ping_on");
    if (IS_BCF_BOOL(pVar))
    {
        ping_on = GET_BCF_BOOL(pVar);
    }
    pVar = pConfig->Get("linger_on");
    if (IS_BCF_BOOL(pVar))
    {
        linger_on = GET_BCF_BOOL(pVar);
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
    pVar = pConfig->Get("active_connection_id_limit");
    if (IS_BCF_NUMBER(pVar))
    {
        active_connection_id_limit = (uint64_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("alpn");
    if (IS_BCF_STRING(pVar))
    {
        alpn = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPConnection
///////////////////////////////////////////////////////////////////////////////

#define _set_state(conn, _state, _status)	\
	(conn)->_SetState(_state, __LINE__);(conn)->close_status_ = _status

SMPConnection::SMPConnection() 
    : connector_(NULL)
    , conn_(NULL)
    , h3_conn_(NULL)
    , local_addrlen_(0)
    , peer_addrlen_(0)
    , handler_(NULL)
    , connect_rpc_(NULL)
    , udp_socket_(NULL)
    , state_(CONN_STATE_INIT)
    , new_state_(CONN_STATE_MAX)
    , state_line_(0)
    , close_status_(BC_R_SUCCESS)
    , keep_working_(false)
{
    memset(&local_addr_, 0, sizeof(local_addr_));
    memset(&peer_addr_, 0, sizeof(peer_addr_));
    memset(&cid_, 0, sizeof(cid_));
}

SMPConnection::~SMPConnection()
{
}

BCRESULT SMPConnection::Create(
    SMPConnector* connector, 
    IConnectionHandler *pHandler, 
    BCFObject* pConfig,
    uint64_t id)
{
    BCRESULT result;
    BCSockAddrS localAddr;
    BCTaskMgr* pTaskMgr;
    BCTimerMgr* pTimerMgr;

    if (!connector || !pHandler || !pConfig)
    {
        LogQ(connector->logger_ctx_, "invalid arguments: invalid connector or handler or config");
        return BC_R_INVALIDARG;
    }
    pTaskMgr = Runtime::RandomTaskMgr();
    pTimerMgr = Runtime::RandomTimerMgr();
    udp_socket_ = new UDPSender();
    if (!udp_socket_)
    {
        return BC_R_NOMEMORY;
    }
    config_.Init(pConfig);
    result = udp_socket_->Create(connector->logger_ctx_, pTaskMgr, pTimerMgr, 
        Runtime::SocketMgr(), pConfig, this);
    if (result != BC_R_SUCCESS)
    {
        goto delete_socket;
    }
    result = BCEventQueue::Create(pTimerMgr, pTaskMgr, "SMPConnection", this);
    if (result != BC_R_SUCCESS)
    {
        goto close_socket;
    }
    udp_socket_->GetSockName(localAddr);
    local_addrlen_ = localAddr.length;
    memcpy(&local_addr_, &localAddr.type, local_addrlen_);
    connector_ = connector;
    handler_ = pHandler;
    id_ = id;
    keep_working_ = true;

    return BC_R_SUCCESS;

close_socket:
    udp_socket_->Destroy(&udp_socket_);
delete_socket:
    BC_SAFE_DELETE_PTR(udp_socket_);

    return result;
}

BCRESULT SMPConnection::Connect(IRPCStub *pStub)
{
    if (keep_working_)
    {
       PostTask([pStub, this] {
            if (_CloseCheck())
            {
                return;
            }
            if (state_ > CONN_STATE_INIT)
            {
                _NotifyConnectResult(pStub, BC_R_ALREADYRUNNING);
            }
            else
            {
                BCRESULT result;

                scheme_ = (LPCSTR)pStub->m_lParams[0];
                host_ = (LPCSTR)pStub->m_lParams[1];
                port_ = (uint16_t)pStub->m_lParams[2];
                path_ = (LPCSTR)pStub->m_lParams[3];
                query_ = (LPCSTR)pStub->m_lParams[4];
                if (pStub->m_lParams[5]) 
                {
                    Json::Reader reader;
                    reader.parse((LPCSTR)pStub->m_lParams[5], props_);
                }
                else
                {
                    props_ = Json::Value(Json::objectValue);
                }
                result = Connect_Internal();
                if (result != BC_R_SUCCESS)
                {
                    _NotifyConnectResult(pStub, result);
                    _set_state(this, CONN_STATE_FREED, result);
                }
                else
                {
                    uint64_t connect_timeout_ms = pStub->m_lParams[6];
                    if (connect_timeout_ms == 0)
                    {
                        connect_timeout_ms = DEFAULT_CONNECT_TIMEOUT;
                    }
                    result = ScheduleTask(connect_timer_id_, [this](int32_t timer_id) {
                        _OnConnectTimeout();
                    }, connect_timeout_ms*1000, false);
                    if (result != BC_R_SUCCESS)
                    {
                        _NotifyConnectResult(pStub, result);
                        _set_state(this, CONN_STATE_FREED, result);
                    }
                    else
                    {
                        connect_rpc_ = pStub;
                    }
                }
                state_ = CONN_STATE_HANDSHAKE;
            }
            _CloseCheck();
        });
        return BC_R_SUCCESS;
    }
    return BC_R_NETDOWN;
}

BCRESULT SMPConnection::SendPacket(SMPacketPtr pkt)
{
    if (keep_working_)
    {
        PostTask([pkt, this] {
            if (_CloseCheck())
            {
                return;
            }
            SendPacket_Internal(pkt);
            _CloseCheck();
        });
        return BC_R_SUCCESS;
    }
    return BC_R_NETDOWN;
}

void SMPConnection::Restart()
{
    if (keep_working_)
    {
        PostTask([this] {
            if (_CloseCheck())
            {
                return;
            }
            if (udp_socket_)
            {
                udp_socket_->Restart();
            }
            _CloseCheck();
        });
    }
}

void SMPConnection::Close()
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

void SMPConnection::CloseStream(uint32_t nStreamId)
{
    if (keep_working_)
    {
        PostTask([this, nStreamId] {
            if (_CloseCheck())
            {
                return;
            }
            if (stream_map_.find(nStreamId) != stream_map_.end()) {
                stream_map_[nStreamId]->Close();
            }
            _CloseCheck();
        });
    }
}

BCRESULT SMPConnection::Connect_Internal()
{
    BCRESULT result;
    xqc_cong_ctrl_callback_t cong_ctrl;
    uint32_t cong_flags = 0;
    if (config_.c_cong_ctl == 'b') {
        cong_ctrl = xqc_bbr_cb;
        cong_flags = XQC_BBR_FLAG_NONE;
    }
#ifndef XQC_DISABLE_RENO
    else if (config_.c_cong_ctl == 'r') {
        cong_ctrl = xqc_reno_cb;
    }
#endif
    else if (config_.c_cong_ctl == 'c') {
        cong_ctrl = xqc_cubic_cb;
    }
#ifdef XQC_ENABLE_BBR2
    else if (config_.c_cong_ctl == 'B') {
        cong_ctrl = xqc_bbr2_cb;
#if XQC_BBR2_PLUS_ENABLED
        if (c_cong_plus) {
            cong_flags |= XQC_BBR2_FLAG_RTTVAR_COMPENSATION;
            cong_flags |= XQC_BBR2_FLAG_FAST_CONVERGENCE;
        }
#endif
    }
#endif
    else {
        LogQ(connector_->logger_ctx_, "invalid arguments: unknown cong_ctrl, option is b, r, c\n");
        return BC_R_INVALIDARG;
    }

    xqc_conn_settings_t conn_settings = connector_->conn_settings_;
    if (config_.pacing_on) {
        conn_settings.pacing_on = config_.pacing_on;
    }
    if (config_.pacing_on) {
        conn_settings.pacing_on = config_.pacing_on;
    }
    if (config_.ping_on) {
        conn_settings.ping_on = config_.ping_on;
    }
	conn_settings.cong_ctrl_callback = cong_ctrl;
	conn_settings.cc_params.cc_optimization_flags = cong_flags;
    if (config_.ping_interval) {
        conn_settings.ping_time_out = config_.ping_interval;
    }
    if (config_.idle_time_out) {
        conn_settings.idle_time_out = config_.idle_time_out;
    }
    if (config_.linger_on) {
        conn_settings.linger.linger_on = config_.linger_on;
    }
    if (config_.active_connection_id_limit) {
        conn_settings.active_connection_id_limit = config_.active_connection_id_limit;
    }
    if (config_.alpn == NULL) {
        config_.alpn = config_.Strdup(connector_->config_.alpn);
    }

    xqc_conn_ssl_config_t conn_ssl_config;
    memset(&conn_ssl_config, 0, sizeof(conn_ssl_config));

    const xqc_cid_t *cid;
    int no_crypt = 0;
    jqc_timer_callback_t timer_cbs = {
        this,
        on_timer_set,
        on_timer_update,
        on_timer_unset,
        on_timer_next_tick
    };
    if (strncasecmp(config_.alpn, "h3", 2) == 0)
    {
        // enable datagram RFC 9221
        conn_settings.max_datagram_frame_size = 65535;
        webtransport_ = true;
    }

    if (udp_socket_)
    {
        udp_socket_->StartRecv();
    }
    result = TextToSockaddr(connector_->config_.ipv6?AF_INET6 : AF_INET, 
        host_.c_str(), port_, (sockaddr*)&peer_addr_, &peer_addrlen_);
    if (result != BC_R_SUCCESS) {
        LogQ(connector_->logger_ctx_, "%s: invalid host(%s) or port(%d)",bc_result2string(result), host_.c_str(), (int)port_);
        return result;
    }
    cid = jqc_connect(connector_->engine_, &conn_settings, NULL,
                      0, connector_->config_.server_host, no_crypt,
                      &conn_ssl_config, (sockaddr*)&peer_addr_,
                      peer_addrlen_, config_.alpn, &timer_cbs, this);

    /* copy cid to its own memory space to prevent crashes caused by internal
     * cid being freed */
    memcpy(&cid_, cid, sizeof(*cid));

    return BC_R_SUCCESS;
}

void SMPConnection::ProcessPacket(
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
            if (root["transId"] == 1 && connect_rpc_)
            {
                if (root["name"] == "_result")
                {
                    std::string msgStr(lpszMsg, size);
                    state_ = CONN_STATE_CONNECTED;
                    if (root["props"].isObject()) {
                        Json::Value& props = root["props"];
                        props["sdkVersion"] = "1.0.0.20260202";
                        Json::FastWriter writer;
                        msgStr = writer.write(root);
                    }
                    _NotifyConnectResult(NULL, BC_R_SUCCESS, msgStr.c_str(), msgStr.length());
                    handler_->OnStreamCreated(refHeader.m_nStreamId);

                    bc_time_t now = bc_time_now();
                    if (connector_->start_time.load() == 0)
                    {
                        connector_->start_time.store(now);
                    }
                    connector_->total_succeed_connections++;
                    LogQ(connector_->logger_ctx_, "connection succeed(%" _U64BITARG_ 
                        "), duration : %" _U64BITARG_ ".", 
                        connector_->total_succeed_connections.load(),
                        (now - connector_->start_time.load())/1000);
                }
                else if (root["name"] == "_error")
                {
                    std::string msgStr(lpszMsg, size);
                    state_ = CONN_STATE_INIT;
                    msgStr += "(sdkVersion=1.0.0.20260202)";
                    _NotifyConnectResult(NULL, BC_R_FAILURE, msgStr.c_str(), msgStr.length());
                    Close();
                }
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
    case SMP::SMP_TYPE_PING:
        break;
    case SMP::SMP_TYPE_PONG:
        break;
    default:
        break;
    }
}

BCRESULT SMPConnection::SendPacket_Internal(SMPacketPtr pkt)
{
    if (stream_map_.find(pkt->stream_id) != stream_map_.end())
    {
        stream_map_[pkt->stream_id]->SendPacket(pkt);
        return BC_R_SUCCESS;
    }
    return BC_R_FAILURE;
}

int SMPConnection::OnConnCreate(
    xqc_connection_t* conn,
    const xqc_cid_t* cid)
{
    conn_ = conn;
    xqc_conn_set_alp_user_data(conn, this);

    return 0;
}

void SMPConnection::OnHandshakeFinished()
{
    connector_->active_connections++;
    state_ = CONN_STATE_CONNECTING;
    if (handler_)
    {
        handler_->OnHandshakeFinished();
    }
    if (webtransport_) {
        return;
    }

    // Create default user stream if not webtransport
    xqc_stream_t *stream = xqc_stream_create(connector_->engine_, &cid_, NULL, NULL);
    if (!stream) {
        return;
    }
    // Send connect command to server
    if (connect_rpc_)
    {
        SMPacketPtr pkt(new SMPacket);
        Json::Value root(Json::objectValue);
        Json::FastWriter writer;
        root["name"] = "connect";
        root["transId"] = 1;
        props_["scheme"] = scheme_;
        props_["host"] = host_;
        props_["port"] = port_;
        props_["path"] = path_;
        props_["query"] = query_;
        root["props"] = props_;
        std::string json_str = writer.write(root);
        pkt->Write(json_str.c_str(), json_str.size());
        pkt->type = SMP_TYPE_COMMAND;
        pkt->timestamp = bc_time_now() / 1000;
        SendPacket_Internal(pkt);
    }
}

int SMPConnection::OnStreamCreate(xqc_stream_t* stream)
{
    SMPStream* user_stream = new SMPStream();
    if (!user_stream)
    {
        return -1;
    }
    
    xqc_stream_id_t id = xqc_stream_id(stream);
    BCRESULT result = user_stream->Create(id, stream, this);
    if (result != BC_R_SUCCESS)
    {
        goto delete_stream;
    }
    stream_map_[id] = user_stream;

    return 0;

delete_stream:
    delete user_stream;
    return -1;
}

int SMPConnection::OnClose()
{
    LogQ(connector_->logger_ctx_, "quic connection closed");
    if (conn_) {
        conn_close_msg_ = jqc_conn_close_msg(conn_);
    }
    conn_ = NULL;
    if (_CloseCheck())
    {
        return -1;
    }
    _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    _CloseCheck();

    return 0;
}

int SMPConnection::OnH3ConnCreate(
    xqc_h3_conn_t* h3_conn,
    const xqc_cid_t* cid)
{
    h3_conn_ = h3_conn;
    xqc_h3_conn_set_user_data(h3_conn, this);
    xqc_h3_conn_settings_t settings = {
        .max_field_section_size = 0,
        .max_pushes = 0,
        .qpack_enc_max_table_capacity = 0,
        .qpack_dec_max_table_capacity = 0,
        .qpack_blocked_streams = 0,
        .extended_connect = true,
        .datagram = true,
    };
    xqc_h3_conn_set_settings(h3_conn, &settings);

    return 0;
}

void SMPConnection::OnH3HandshakeFinished()
{
    connector_->active_connections++;
    state_ = CONN_STATE_CONNECTING;
    if (handler_)
    {
        handler_->OnHandshakeFinished();
    }

    // Create default user stream
    H3Stream* user_stream = new H3Stream();
    if (!user_stream)
    {
        return;
    }
    xqc_stream_settings_t settings = { .recv_rate_bytes_per_sec = 0 };
    xqc_h3_request_t *stream = xqc_h3_request_create(connector_->engine_, &cid_, &settings, user_stream);
    if (!stream) {
        return;
    }
    xqc_stream_id_t id = xqc_h3_request_id(stream);
    BCRESULT result = user_stream->Create(id, stream, this);
    if (result != BC_R_SUCCESS)
    {
        goto close_stream;
    }
    h3_stream_map_[id] = user_stream;
    user_stream->SendRequest();
    return;

close_stream:
    xqc_h3_request_close(stream);
}

int SMPConnection::OnH3Close()
{
    conn_close_msg_ = jqc_h3_conn_close_msg(h3_conn_);
    h3_conn_ = NULL;
    if (_CloseCheck())
    {
        return -1;
    }
    _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    _CloseCheck();

    return 0;
}

void SMPConnection::OnPingAcked(void *ping_user_data)
{
    if (handler_)
    {
        // handler_->OnPingAcked(ping_user_data);
    }
}

void SMPConnection::OnPacketAcked(
    xqc_usec_t ack_delay_time,
    size_t acked_bytes, 
    size_t inflight_bytes, 
    xqc_stream_id_t stream_id)
{
    //LogQ(connector_->logger_ctx_, "packet acked, ack_delay_time : %" PRIu64 ", acked_bytes : %" PRIu64 
    //    ", inflight_bytes : %" PRIu64 "\n", ack_delay_time, acked_bytes, inflight_bytes);
    if (stream_map_.find(stream_id) != stream_map_.end())
    {
        stream_map_[stream_id]->OnPacketAcked(ack_delay_time, 
            acked_bytes, inflight_bytes);
    }
}

void SMPConnection::OnUpdataCID(
    const xqc_cid_t* retire_cid,
    const xqc_cid_t* new_cid)
{

    memcpy(&cid_, new_cid, sizeof(*new_cid));

    //LogQ(connector_->logger_ctx_, "====>RETIRE SCID:%s\n", xqc_scid_str(retire_cid));
    //LogQ(connector_->logger_ctx_, "====>SCID:%s\n", xqc_scid_str(new_cid));
    //LogQ(connector_->logger_ctx_, "====>DCID:%s\n", xqc_dcid_str_by_scid(
    //    connector_->engine_, new_cid));
}

void SMPConnection::OnSaveTP(const char* data, size_t data_len)
{

}

void SMPConnection::OnSaveSession(const char* data, size_t data_len)
{

}

void SMPConnection::OnSaveToken(const unsigned char* token, size_t token_len)
{

}

ssize_t
SMPConnection::WritePacket(
    const unsigned char* buf,
    size_t size,
    const struct sockaddr* peer_addr,
    socklen_t peer_addrlen)
{
    ssize_t res;
    BCRESULT result;

    if (_CloseCheck())
    {
        return -1;
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
            //LogBin(connector_->logger_ctx_, _LOCAL_, buf, size);
            wrote_counter++;
        }
    } while (0);

    if (res > 0) {
        if ((bc_time_now() - last_snd_ts) > 200000)
        {
            //LogQ(connector_->logger_ctx_, "sending rate: %.3f Kbps\n", (snd_sum - last_snd_sum) *
            //    8.0 * 1000 / (time_now() - last_snd_ts));
            last_snd_ts = bc_time_now();
            last_snd_sum = snd_sum;
        }
        //LogQ(connector_->logger_ctx_, "WritePacket(id:%u, conn:%p) ", 
        //    id_, conn_);
    }
    else
    {
        LogQ(connector_->logger_ctx_, "WritePacket(write error : %d) ", res);
        _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    }
    _CloseCheck();

    return res;
}

void SMPConnection::OnH3RequestRecvHeaders(xqc_http_headers_t* headers)
{
    Json::Value props(Json::objectValue);
    if (headers) {
        for (size_t i = 0; i < headers->count; i++) {
            props[std::string((char *)headers->headers[i].name.iov_base, headers->headers[i].name.iov_len)] 
                = std::string((char *)headers->headers[i].value.iov_base, headers->headers[i].value.iov_len);
        }
    }
    Json::FastWriter writer;
    std::string json = writer.write(props);
    if (connect_rpc_) {
        connect_rpc_->m_lParams[0] = (uint64_t)connect_rpc_->m_sPool.memdup(json.c_str(), json.size());
        connect_rpc_->m_lParams[1] = json.size();
        if (headers)
        {
            state_ = CONN_STATE_CONNECTED;
            _NotifyConnectResult(NULL, BC_R_SUCCESS, json.c_str(), json.size());

            bc_time_t now = bc_time_now();
            if (connector_->start_time.load() == 0)
            {
                connector_->start_time.store(now);
            }
            connector_->total_succeed_connections++;
            LogQ(connector_->logger_ctx_, "connection succeed(%" _U64BITARG_ 
                "), duration : %" _U64BITARG_ ".", 
                connector_->total_succeed_connections.load(),
                (now - connector_->start_time.load())/1000);
        }
        else
        {
            state_ = CONN_STATE_INIT;
            _NotifyConnectResult(NULL, BC_R_FAILURE, json.c_str(), json.size());
            Close();
        }
    }
}

void SMPConnection::OnSendData(uint32_t nWrite, UDPSender* pSender)
{

}

void SMPConnection::OnRecvData(BCBuffer* pBuffer, BCSockAddrS& refSrcAddr)
{
    if (_CloseCheck())
    {
        return;
    }
    socklen_t peer_addrlen = connector_->config_.ipv6 ? sizeof(struct sockaddr_in6) 
        : sizeof(struct sockaddr_in);
    uint64_t recv_time;
    unsigned char* packet_buf = NULL;
    uint32_t recv_size = 0;
    xqc_int_t ret;

    while (pBuffer->RemainingLength() > 0) {
        packet_buf = (unsigned char *)pBuffer->ReadBlock(pBuffer->RemainingLength(), recv_size);
        recv_time = bc_time_now();
        if (h3_conn_)
        {
            ret = jqc_h3_conn_packet_process(h3_conn_, packet_buf, recv_size,
                (struct sockaddr*)(&this->local_addr_), this->local_addrlen_,
                (struct sockaddr*)(&refSrcAddr.type.sa), peer_addrlen,
                /*path id*/XQC_UNKNOWN_PATH_ID, (xqc_msec_t)recv_time, this, NULL, NULL, 0);
        } else {
            ret = jqc_conn_packet_process(conn_, packet_buf, recv_size,
                (struct sockaddr*)(&this->local_addr_), this->local_addrlen_,
                (struct sockaddr*)(&refSrcAddr.type.sa), peer_addrlen,
                /*path id*/XQC_UNKNOWN_PATH_ID, (xqc_msec_t)recv_time, this, NULL, NULL, 0);
        }
        if (ret != XQC_OK)
        {
            LogQ(connector_->logger_ctx_, "%s: packet process err\n", __FUNCTION__);
            return;
        }
        if (h3_conn_)
        {
            jqc_h3_conn_finish_recv(h3_conn_);
        }
        else
        {
            jqc_conn_finish_recv(conn_);
        }
    }

    _CloseCheck();
}

void SMPConnection::OnRestart(BCRESULT result)
{
    PostTask([this, result] {
        if (_CloseCheck())
        {
            return;
        }
        if (result == BC_R_SUCCESS && udp_socket_)
        {
            BCSockAddrS localAddr;
            udp_socket_->GetSockName(localAddr);
            local_addrlen_ = localAddr.length;
            memcpy(&local_addr_, &localAddr.type, local_addrlen_);
            udp_socket_->StartRecv();
            if (handler_)
            {
                char local_addr_str_[128];
                bc_sockaddr_format(&localAddr, local_addr_str_, sizeof(local_addr_str_));
                handler_->OnRestart(result, local_addr_str_);
            }
        }
        else
        {
            if (handler_)
            {
                handler_->OnRestart(result, NULL);
            }
        }
        _CloseCheck();
    });
}

void SMPConnection::OnUdpClosed()
{
    PostTask([this] {
        BC_SAFE_DELETE_PTR(udp_socket_);
        if (_CloseCheck())
        {
            return;
        }
        _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
        _CloseCheck();
    });
}

void SMPConnection::OnEventProcShutdown()
{
    if (connector_)
    {
        connector_->NotifyConnClosed(id_);
    }
    if (handler_)
    {
        if (conn_close_msg_.empty()) {
            conn_close_msg_ = bc_result2string(close_status_);
        } else {
            handler_->OnClosed(conn_close_msg_.c_str());
        }
    }
}

int32_t  SMPConnection::on_timer_set(
    void* ctx,
    xqc_usec_t expire_time,
    void(*timeout_cb)(void*),
    void* user_data)
{
    int taskId = 0;
    int64_t now = bc_time_now();
    int64_t duration = expire_time - now;
    SMPConnection* _this = (SMPConnection*)ctx;
    if (!_this)
    {
        return 0;
    }
    if (duration < 1000) {
        duration = 1000;
    }
    _this->ScheduleTask(taskId, [=](int32_t timerId) {
        if (_this->_CloseCheck())
        {
            return;
        }
        if (timeout_cb)
        {
            timeout_cb(user_data);
        }
        _this->_CloseCheck();
    }, duration);
    return taskId;
}

void SMPConnection::on_timer_update(
    void* ctx,
    int32_t timer_id,
    xqc_usec_t expire_time,
    void(*timeout_cb)(void*),
    void* user_data)
{
    int64_t now = bc_time_now();
    int64_t duration = expire_time - now;
    SMPConnection* _this = (SMPConnection*)ctx;
    if (!_this)
    {
        return;
    }
    if (duration < 1000) {
        duration = 1000;
    }
    _this->ScheduleTask(timer_id, [=](int32_t timerId) {
        if (_this->_CloseCheck())
        {
            return;
        }
        if (timeout_cb)
        {
            timeout_cb(user_data);
        }
        _this->_CloseCheck();
    }, duration);
}

void SMPConnection::on_timer_unset(void* ctx, int32_t *timer_id, xqc_bool_t only_cancel)
{
    SMPConnection* _this = (SMPConnection*)ctx;
    if (!_this)
    {
        return;
    }
    _this->UnscheduleTask(*timer_id, only_cancel);
}

void SMPConnection::on_timer_next_tick(void* ctx)
{
    SMPConnection* _this = (SMPConnection*)ctx;
    if (!_this)
    {
        return;
    }
    _this->PostTask([=]() {
        if (_this->_CloseCheck() || !_this->conn_)
        {
            return;
        }
        jqc_conn_main_logic(_this->conn_);
        _this->_CloseCheck();
    });
}

bool SMPConnection::_CloseCheck()
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
        // Cancel all timers
        UnscheduleAllTasks();
        if (udp_socket_)
        {
            udp_socket_->Close();
        }
        state_ = CONN_STATE_CLOSING_SOCK;
    }

    if (state_ == CONN_STATE_CLOSING_SOCK)
    {
        if (udp_socket_)
        {
            return true;
        }
        _NotifyConnectResult(NULL, close_status_);
        Detach();
        state_ = CONN_STATE_FREED;
    }

    return true;
}

void SMPConnection::_OnConnectTimeout()
{
    _NotifyConnectResult(NULL, BC_R_TIMEDOUT);
    _set_state(this, CONN_STATE_FREED, BC_R_TIMEDOUT);
    _CloseCheck();
}

void SMPConnection::_NotifyConnectResult(
    IRPCStub *pStub, 
    BCRESULT result, 
    LPCSTR msg, 
    size_t size)
{
    if (pStub == NULL)
    {
        pStub = connect_rpc_;
    }
    if (pStub && handler_)
    {
        pStub->m_result = result;
        if (msg && size > 0)
        {
            pStub->m_lParams[0] = (uint64_t)pStub->m_sPool.Strndup(msg, size);
            pStub->m_lParams[1] = size;
        }
        else
        {
            msg = bc_result2string(result);
            size = strlen(msg);
            pStub->m_lParams[0] = (uint64_t)pStub->m_sPool.Strndup(msg, size);
            pStub->m_lParams[1] = size;
        }
        handler_->OnExecDone(pStub);
    }
    if (pStub == connect_rpc_)
    {
        connect_rpc_ = NULL;
    }
    if (connect_timer_id_ > 0)
    {
        UnscheduleTask(connect_timer_id_);
        connect_timer_id_ = 0;
    }
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPConnector::Config
///////////////////////////////////////////////////////////////////////////////

BCRESULT SMPConnector::Config::Init(BCFObject* pConfig)
{
    BCFVar* pVar;

    pVar = pConfig->Get("ipv6");
    if (IS_BCF_BOOL(pVar))
    {
        ipv6 = GET_BCF_BOOL(pVar);
    }
    pVar = pConfig->Get("publishId");
    if (IS_BCF_NUMBER(pVar))
    {
        publishId = (uint32_t)GET_BCF_INT(pVar);
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
    pVar = pConfig->Get("active_connection_id_limit");
    if (IS_BCF_NUMBER(pVar))
    {
        active_connection_id_limit = (uint64_t)GET_BCF_INT(pVar);
    }
    return BC_R_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
// class : SMPConnector
///////////////////////////////////////////////////////////////////////////////

SMPConnector::SMPConnector()
    : engine_(NULL)
    , timer_id_(0)
    , logger_ctx_(NULL)
    , handler_(NULL)
    , state_(CONNTOR_STATE_INIT)
    , new_state_(CONNTOR_STATE_MAX)
    , state_line_(0)
    , close_status_(BC_R_SUCCESS)
    , total_connections(0)
    , active_connections(0)
    , start_time(0)
    , total_succeed_connections(0)
    , total_allocated_conns_(0)
    , total_freed_conns_(0)
{
}

SMPConnector::~SMPConnector()
{
}

BCRESULT SMPConnector::Create(BCFObject* pConfig, IConnectorHandler* pHandler)
{
    if (!pConfig || !pHandler)
    {
        LogQ(logger_ctx_, "invalid arguments: invalid config or handler");
        return BC_R_INVALIDARG;
    }

    handler_ = pHandler;
    config_.Init(pConfig);    
	if (!config_.alpn)
    {
        LogQ(logger_ctx_, "invalid arguments: invalid alpn");
        return BC_R_INVALIDARG;
    }
    if (config_.log_file) {
        logger_ctx_ = AddFileLogAppender(config_.log_file, _FINEST_, true, true);
    } else {
        logger_ctx_ = AddExternalLogAppender(log_callback, this, _FINEST_, true);
    }
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
    conn_settings_.pacing_on = config_.pacing_on;
    conn_settings_.active_connection_id_limit = config_.active_connection_id_limit;

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
        }
    };

    xqc_transport_callbacks_t tcbs = {
        .server_accept = NULL,
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
    if (xqc_engine_get_default_config(&config, XQC_ENGINE_CLIENT) < 0) 
    {
        LogQ(logger_ctx_, "invalid arguments: invalid engine config");
        return BC_R_INVALIDARG;
    }
    switch (config_.log_level)
    {
    case LOG_LEVEL_FATAL:
        config.cfg_log_level = XQC_LOG_FATAL;
        break;
    case LOG_LEVEL_ERROR:
        config.cfg_log_level = XQC_LOG_ERROR;
        break;
    case LOG_LEVEL_WARN:
        config.cfg_log_level = XQC_LOG_WARN;
        break;
    case LOG_LEVEL_INFO:
        config.cfg_log_level = XQC_LOG_INFO;
        break;
    case LOG_LEVEL_DEBUG:
        config.cfg_log_level = XQC_LOG_DEBUG;
        break;
    default:
        config.cfg_log_level = XQC_LOG_WARN;
        break;
    }
    /* for lb cid generate */
    //RAND_bytes(quic_lb_ctx_.sid_buf, 4);
    //quic_lb_ctx_.sid_len = 4;
    quic_lb_ctx_.conf_id = 0;
    quic_lb_ctx_.cid_len = config.cid_len;
    quic_lb_ctx_.sid_len = 0;

    engine_ = xqc_engine_create(XQC_ENGINE_CLIENT, &config, 
        &config_.engine_ssl_config, &callback, &tcbs, this);
    if (engine_ == NULL) {
        LogQ(logger_ctx_, "invalid arguments: error create engine");
        return BC_R_INVALIDARG;
    }

    /* register application-protocol callbacks */
    xqc_h3_callbacks_t h3_cbs = {
        .h3c_cbs = {
            .h3_conn_create_notify = on_h3_conn_create_notify,
            .h3_conn_close_notify = on_h3_conn_close_notify,
            .h3_conn_handshake_finished = on_h3_conn_handshake_finished,
            .h3_conn_ping_acked = on_h3_conn_ping_acked,
            .h3_conn_packet_acked = on_h3_conn_packet_acked
        },
        .h3r_cbs = {
            .h3_request_create_notify = on_h3_stream_create_notify,
            .h3_request_close_notify = on_h3_stream_close_notify,
            .h3_request_read_notify = on_h3_stream_read_notify,
            .h3_request_write_notify = on_h3_stream_write_notify,
            .h3_request_closing_notify = on_h3_stream_closing_notify,
        },
        .h3_ext_dgram_cbs = {},
        .h3_ext_bs_cbs = {},
        .wt_stream_cbs = {
            .stream_read_notify = on_raw_stream_read_notify,
            .stream_write_notify = on_raw_stream_write_notify,
            .stream_create_notify = on_raw_stream_create_notify,
            .stream_close_notify = on_raw_stream_close_notify,
        }
    };
    /* init http3 context and register h3 callbacks */
    int ret = xqc_h3_ctx_init(engine_, &h3_cbs);
    if (ret != XQC_OK) {
        return BC_R_UNEXPECTED;
    }

    /* register raw transport callbacks */
    xqc_app_proto_callbacks_t raw_cbs = {
        .conn_cbs = {
            .conn_create_notify = on_raw_conn_create_notify,
            .conn_close_notify = on_raw_conn_close_notify,
            .conn_handshake_finished = on_raw_conn_handshake_finished,
            .conn_ping_acked = on_raw_conn_ping_acked,
            .conn_packet_acked = on_raw_conn_packet_acked
        },
        .stream_cbs = {
            .stream_read_notify = on_raw_stream_read_notify,
            .stream_write_notify = on_raw_stream_write_notify,
            .stream_create_notify = on_raw_stream_create_notify,
            .stream_close_notify = on_raw_stream_close_notify,
        }
    };

    /* register raw transport callbacks */
    xqc_engine_register_alpn(engine_, config_.alpn, strlen(config_.alpn), &raw_cbs);

    state_ = CONNTOR_STATE_WORKING;

    return BC_R_SUCCESS;
}

SMPConnection* SMPConnector::CreateConnection(
    IConnectionHandler* pHandler, 
    BCFObject *pConfig)
{
    BCRESULT result;
    BCSpinMutex::Owner lock(lock_);
    if (_CloseCheck())
    {
        return NULL;
    }
    SMPConnection *user_conn = new SMPConnection();
    if (!user_conn) {
        return NULL;
    }
    result = user_conn->Create(this, pHandler, pConfig, next_conn_id_);
    if (result != BC_R_SUCCESS) {
        goto delete_conn;
    }
    conns_map_[next_conn_id_++] = user_conn;
    total_allocated_conns_++;
    return user_conn;

delete_conn:
    delete user_conn;

    return NULL;
}

void SMPConnector::NotifyConnClosed(uint64_t conn_id)
{
    BCSpinMutex::Owner lock(lock_);
    conns_map_.erase(conn_id);
    total_freed_conns_++;
    Runtime::PostTask([this] {
        BCSpinMutex::Owner lock(lock_);
        _CloseCheck();
    });
}

BCRESULT SMPConnector::GetStats(ConnStatS& stats)
{
    BCSpinMutex::Owner lock(lock_);
    stats.allocated_conn_size = total_allocated_conns_;
    stats.freed_conn_size = total_freed_conns_;
    stats.active_conn_size = conns_map_.size();
    return BC_R_SUCCESS;
}

void SMPConnector::Close()
{
    Runtime::PostTask([this] {
        BCSpinMutex::Owner lock(lock_);
        if (_CloseCheck())
        {
            return;
        }
        _set_state(this, CONNTOR_STATE_FREED, BC_R_SUCCESS);
        _CloseCheck();
    });
}

bool SMPConnector::_CloseCheck()
{
    if (state_ <= new_state_ && state_ > CONNTOR_STATE_FREED)
    {
        return false;
    }

    if (state_ == CONNTOR_STATE_WORKING)
    {
        for (auto &iter : conns_map_)
        {
            iter.second->Close();
        }
        state_ = CONNTOR_STATE_INIT;
    }

    if (state_ == CONNTOR_STATE_INIT)
    {
        if (conns_map_.size() > 0)
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
        state_ = CONNTOR_STATE_FREED;
    }

    return true;
}

void SMPConnector::set_timer_cb(xqc_usec_t wake_after, void *user_data)
{
    SMPConnector *server = (SMPConnector *) user_data;
    if (server)
    {
    }
}

void SMPConnector::on_conn_save_session(
    const char *data, 
    size_t data_len,
    void *user_data) {
    SMPConnection *user_conn = (SMPConnection *)user_data;
    if (user_conn)
    {
        user_conn->OnSaveSession(data, data_len);
    }
    return;
}

void SMPConnector::on_conn_save_tp(
    const char *data, 
    size_t data_len, 
    void *user_data) 
{
    SMPConnection *user_conn = (SMPConnection *)user_data;
    if (user_conn)
    {
        user_conn->OnSaveTP(data, data_len);
    }
    return;
}

void SMPConnector::on_conn_save_token(
    const unsigned char *token,
    unsigned token_len,
    void *user_data)
{
    SMPConnection *user_conn = (SMPConnection *)user_data;
    if (user_conn) {
        user_conn->OnSaveToken(token, token_len);
    }
}

void
SMPConnector::on_conn_update_cid_notify(
    xqc_connection_t *conn, 
    const xqc_cid_t *retire_cid, 
    const xqc_cid_t *new_cid, 
    void *user_data)
{
    SMPConnection *user_conn = (SMPConnection *) user_data;
    if (user_conn)
    {
        user_conn->OnUpdataCID(retire_cid, new_cid);
    }
}

int SMPConnector::on_conn_cert_verify(
    const unsigned char *certs[],
    const size_t cert_len[], 
    size_t certs_len,
    void *conn_user_data) {
    /* self-signed cert used in test cases, return >= 0 means success */
    return 0;
}

int SMPConnector::on_conn_closing_notify(
    xqc_connection_t *conn,
    const xqc_cid_t *cid,
    xqc_int_t err_code,
    void *conn_user_data) {
    SMPConnection* user_conn = (SMPConnection*)conn_user_data;
    //LogQ(user_conn->connector_->logger_ctx_, "conn closing: %d\n", err_code);
    return XQC_OK;
}

// raw alpn callbacks
int
SMPConnector::on_raw_conn_create_notify(
    xqc_connection_t *conn, 
    const xqc_cid_t *cid, 
    void *conn_user_data,
    void *conn_proto_data) {
    SMPConnection* user_conn = (SMPConnection*)conn_user_data;
    if (user_conn)
    {
        LogQ(user_conn->connector_->logger_ctx_, "quic conn created");
        user_conn->OnConnCreate(conn, cid);
    }
    return 0;
}

int
SMPConnector::on_raw_conn_close_notify(
    xqc_connection_t *h3_conn, 
    const xqc_cid_t *cid, 
    void *conn_user_data,
    void *conn_proto_data) {
    SMPConnection *user_conn = (SMPConnection*)conn_user_data;
    if (user_conn)
    {
        return user_conn->OnClose();
    }
    return -1;
}

void
SMPConnector::on_raw_conn_handshake_finished(
    xqc_connection_t *h3_conn, 
    void *conn_user_data,
    void *conn_proto_data) {
    SMPConnection *user_conn = (SMPConnection *)conn_user_data;
    if (user_conn)
    {
        user_conn->OnHandshakeFinished();
    }
}

void
SMPConnector::on_raw_conn_ping_acked(
    xqc_connection_t *conn, 
    const xqc_cid_t *cid, 
    void *ping_user_data, 
    void *conn_user_data, 
    void *conn_proto_data) {
    SMPConnection *user_conn = (SMPConnection *)conn_user_data;
    if (user_conn)
    {
        user_conn->OnPingAcked(ping_user_data);
    }
}

void     
SMPConnector::on_raw_conn_packet_acked(
    xqc_connection_t* h3_conn,
    xqc_usec_t ack_delay_time,
    size_t acked_bytes,
    size_t inflight_bytes,
    xqc_stream_id_t stream_id,
    void* conn_user_data,
    void *conn_proto_data) {
    SMPConnection *user_conn = (SMPConnection *)conn_user_data;
    if (user_conn)
    {
        user_conn->OnPacketAcked(ack_delay_time, acked_bytes, 
            inflight_bytes, stream_id);
    }
}

int
SMPConnector::on_raw_stream_create_notify(xqc_stream_t *stream, void *strm_user_data)
{
    int ret = -1;

    SMPConnection *conn = (SMPConnection *)xqc_get_conn_user_data_by_stream(stream);
    if (conn)
    {
        ret = conn->OnStreamCreate(stream);
        uint32_t id = xqc_stream_id(stream);
        if ((conn->webtransport_ || (!conn->webtransport_ && id > 0)) && conn->handler_)
        {
            conn->handler_->OnStreamCreated(id);
        }
        LogQ(conn->connector_->logger_ctx_, "quic stream created: %" _U32BITARG_, id); 
    }
    return ret;
}

int
SMPConnector::on_raw_stream_close_notify(xqc_stream_t *stream, void *user_data)
{
    SMPConnection *conn =
        (SMPConnection *)xqc_get_conn_user_data_by_stream(stream);
    SMPStream *user_stream = (SMPStream*)user_data;
    if (conn && user_stream) {
        user_stream->OnClose();
        uint32_t id = xqc_stream_id(stream);
        if ((conn->webtransport_ || (!conn->webtransport_ && id > 0)) && conn->handler_)
        {
            conn->handler_->OnStreamClosed(id);
        }
    }

    return 0;
}

int
SMPConnector::on_raw_stream_write_notify(xqc_stream_t *stream, void *user_data)
{
    //DEBUG_PRINT;
    int ret = 0;
    SMPStream *user_stream = (SMPStream *) user_data;
    if (user_stream)
    {
        //ret = user_stream->Send();
    }
    return ret;
}

int
SMPConnector::on_raw_stream_read_notify(
    xqc_stream_t *stream, 
    void *user_data)
{
    int ret = 0;
    SMPStream *user_stream = (SMPStream *) user_data;
    if (user_stream)
    {
        ret = user_stream->OnRead();
    }
    return ret;
}

// http3 alpn callbacks
int
SMPConnector::on_h3_conn_create_notify(
    xqc_h3_conn_t *conn, 
    const xqc_cid_t *cid, 
    void *user_data) {
    SMPConnection* user_conn = (SMPConnection*)user_data;
    if (user_conn)
    {
        user_conn->OnH3ConnCreate(conn, cid);
    }
    return 0;
}

int
SMPConnector::on_h3_conn_close_notify(
    xqc_h3_conn_t *conn, 
    const xqc_cid_t *cid, 
    void *user_data) {
    SMPConnection *user_conn = (SMPConnection*)user_data;
    if (user_conn)
    {
        return user_conn->OnH3Close();
    }
    return -1;
}

void
SMPConnector::on_h3_conn_handshake_finished(
    xqc_h3_conn_t *conn, 
    void *user_data) {
    SMPConnection *user_conn = (SMPConnection *)user_data;
    if (user_conn)
    {
        user_conn->OnH3HandshakeFinished();
    }
}

void
SMPConnector::on_h3_conn_ping_acked(
    xqc_h3_conn_t *conn, 
    const xqc_cid_t *cid, 
    void *ping_user_data,
    void *user_data) {
    SMPConnection *user_conn = (SMPConnection *)user_data;
    if (user_conn)
    {
        user_conn->OnPingAcked(ping_user_data);
    }
}

void     
SMPConnector::on_h3_conn_packet_acked(
    xqc_h3_conn_t *h3_conn, 
    xqc_usec_t ack_delay_time, 
    size_t acked_bytes, 
    size_t inflight_bytes, 
    xqc_stream_id_t stream_id, 
    void *h3c_user_data) {
    SMPConnection *user_conn = (SMPConnection *)h3c_user_data;
    if (user_conn)
    {
        user_conn->OnPacketAcked(ack_delay_time, acked_bytes, 
            inflight_bytes, stream_id);
    }
}

int
SMPConnector::on_h3_stream_create_notify(xqc_h3_request_t *h3_request, void *h3s_user_data)
{
    // int ret = -1;

    // SMPConnection *conn =
    //     (SMPConnection *)xqc_get_h(stream);
    // if (conn)
    // {
    //     ret = conn->OnH3StreamCreate(h3_request);
    // }
    // return ret;
    return 0;
}

void
SMPConnector::on_h3_stream_closing_notify(
    xqc_h3_request_t *h3_request, 
    xqc_int_t err, 
    void *h3s_user_data)
{
    H3Stream *user_stream = (H3Stream*)h3s_user_data;
    if (user_stream)
    {
        user_stream->OnClosing();
    }
}

int
SMPConnector::on_h3_stream_write_notify(xqc_h3_request_t *h3_request, void *h3s_user_data)
{
    //DEBUG_PRINT;
    int ret = 0;
    H3Stream *user_stream = (H3Stream *)h3s_user_data;
    if (user_stream)
    {
        //ret = user_stream->Send();
    }
    return ret;
}

int
SMPConnector::on_h3_stream_read_notify(
    xqc_h3_request_t *h3_request, 
    xqc_request_notify_flag_t flags, 
    void *h3s_user_data)
{
    int ret = 0;
    H3Stream *user_stream = (H3Stream *)h3s_user_data;
    if (user_stream)
    {
        ret = user_stream->OnRead(flags);
    }
    return ret;
}

int
SMPConnector::on_h3_stream_close_notify(
    xqc_h3_request_t *h3_request, 
    void *h3s_user_data)
{
    H3Stream *user_stream = (H3Stream*)h3s_user_data;
    if (user_stream)
    {
        user_stream->OnClose();
    }

    return 0;
}

ssize_t 
SMPConnector::conn_write_socket_cb(
    const unsigned char *buf, 
    size_t size,
    const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, 
    void *user_data)
{
    int res = 0;
    SMPConnection *conn = (SMPConnection*)user_data; //user_data may be empty when "reset" is sent
    if (conn)
    {
        res = conn->WritePacket(buf, size, peer_addr, peer_addrlen);
    }

    return res;
}


void 
SMPConnector::on_write_log(
    xqc_log_level_t lvl, 
    const void *buf, 
    size_t count, 
    void *engine_user_data)
{
    SMPConnector *ctx = (SMPConnector*)engine_user_data;

    LogCustom(ctx->logger_ctx_, "%.*s", count, buf);
}

void SMPConnector::on_keylog_cb(const xqc_cid_t *scid, const char *line,
                            void *user_data) {
    SMPConnector *ctx = (SMPConnector*)user_data;
    
    LogCustom(ctx->logger_ctx_, "%s", line);
}

void SMPConnector::on_server_refuse(
    xqc_engine_t* engine,
    xqc_connection_t* conn,
    const xqc_cid_t* cid,
    void* user_data)
{
    /* ALPN context is not initialized, ClientHello has not been received */
    //We MUST set this callback to handle close event before ALPN context initialized,
    // or we will accidentally loss conn_close_notify event
    SMPConnection* user_conn = (SMPConnection*)user_data;
    if (conn)
    {
        user_conn->OnClose();
    }
}

ssize_t
SMPConnector::on_stateless_reset(
    const unsigned char *buf, 
    size_t size,
    const struct sockaddr *peer_addr,
    socklen_t peer_addrlen,
    const struct sockaddr *local_addr,
    socklen_t local_addrlen, 
    void *user_data) 
{
    int res = 0;
    SMPConnector *_this = (SMPConnector*)user_data; //user_data may be empty when "reset" is sent
    if (_this)
    {
        //res = _this->WritePacket(buf, size, peer_addr, peer_addrlen);
    }

    return res;
}

ssize_t
SMPConnector::cid_generate(
    const xqc_cid_t* ori_cid, 
    uint8_t* cid_buf, 
    size_t cid_buflen, 
    void* engine_user_data)
{
    SMPConnector *_this = (SMPConnector*)engine_user_data;
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

void SMPConnector::log_callback(void *data, int level, LPCSTR lpszMsg) 
{
    std::string msg(lpszMsg);
    Runtime::PostTask([data, level, msg] {
        auto *ctx = (SMPConnector*)data;
        if (ctx && ctx->handler_) {
            ctx->handler_->OnLog(level, msg.c_str());
        }
    });
}

///////////////////////////////////////////////////////////////////////////////
// End of file : SMPConnector.cpp
///////////////////////////////////////////////////////////////////////////////
