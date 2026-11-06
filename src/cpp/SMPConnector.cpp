



#include "StdAfx.h"
#include <time.h>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fcntl.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/x509_vfy.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/base64.h>
#include <HTTP/HTTPProtocol.h>
#include "SMPConnector.h"
#include "Runtime.h"
#include "BC/BCJson.h"
#include "Utils.h"
#include "MasqueFraming.h"
#include "TlsContext.h"


#define XQC_PACKET_TMP_BUF_LEN 1500
#define DEFAULT_CONNECT_TIMEOUT 10000

namespace {






class MasqueTunnelHandler : public IConnectionHandler
{
public:
    void OnHandshakeFinished() override {}
    void OnExecDone(IRPCStub*) override {}
    void OnStreamCreated(uint32_t) override {}
    void OnStreamClosed(uint32_t) override {}
    void OnStreamDataAcked(uint32_t, xqc_usec_t, size_t, size_t) override {}
    void OnStreamDataSent(uint32_t, uint32_t, size_t) override {}
    void OnRecvCmd(const SMPHeader&, const char*, size_t) override {}
    void OnRecvData(const SMPHeader&, LPCVOID, size_t) override {}
    void OnRestart(BCRESULT, const char*) override {}
    void OnClosed(LPCSTR) override {}
    void OnException(BCException&) override {}

    static MasqueTunnelHandler* Instance()
    {
        static MasqueTunnelHandler s_instance;
        return &s_instance;
    }
};

void freeCertVector(std::vector<X509*>& certs)
{
    for (X509* cert : certs) {
        if (cert) {
            X509_free(cert);
        }
    }
    certs.clear();
}

bool isPemBundleEof(unsigned long err_code)
{
    if (err_code == 0) {
        return true;
    }
    return ERR_GET_LIB(err_code) == ERR_LIB_PEM
        && ERR_GET_REASON(err_code) == PEM_R_NO_START_LINE;
}

BCRESULT parsePemCertBundle(
    const char* pem,
    size_t pem_len,
    std::vector<X509*>& certs,
    LPVOID logger_ctx,
    const char* source_tag)
{
    freeCertVector(certs);
    if (!pem || pem_len == 0) {
        return BC_R_SUCCESS;
    }

    BIO* bio = BIO_new_mem_buf(pem, (int)pem_len);
    if (!bio) {
        LogQ(logger_ctx, _ERROR_, "%s: failed to allocate BIO for ca_cert_pem", source_tag);
        return BC_R_NOMEMORY;
    }

    while (true) {
        X509* cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        if (cert) {
            certs.push_back(cert);
            continue;
        }

        const unsigned long err_code = ERR_peek_last_error();
        if (isPemBundleEof(err_code)) {
            ERR_clear_error();
            break;
        }

        char err_buf[256] = {0};
        ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
        LogQ(logger_ctx, _ERROR_, "%s: failed to parse ca_cert_pem bundle: %s",
            source_tag, err_buf);
        ERR_clear_error();
        BIO_free(bio);
        freeCertVector(certs);
        return BC_R_INVALIDARG;
    }

    BIO_free(bio);
    if (certs.empty()) {
        LogQ(logger_ctx, _ERROR_, "%s: ca_cert_pem did not contain a valid certificate", source_tag);
        return BC_R_INVALIDARG;
    }

    return BC_R_SUCCESS;
}






} 


static inline xqc_usec_t time_now() { return bc_time_now(); }

static const char webtransport_hdr_mask[] = {0x40, 0x41, 0x00};









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
    if (!stream_)
    {
        LogQ(conn_->connector_->logger_ctx_, _ERROR_, "stream_send: stream is NULL, stream_id:%" _S32BITARG_, stream_id_);
        return -1;
    }
    return xqc_stream_send(stream_, (unsigned char*)data, size, 0);
}

BCRESULT SMPStream::SendPacket(SMPacketPtr pkt)
{
    if (pkt->packed_jmp_data)
    {
        send_buff_list_.push_back(pkt);
        _SendBuffList();
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
            LogQ(conn_->connector_->logger_ctx_, _ERROR_, "xqc_stream_recv_body error %zd\n", read);
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

int SMPStream::OnWriteNotify()
{
    _SendBuffList();
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

void SMPStream::_SendBuffList()
{
    while (!send_buff_list_.empty())
    {
        SMPacketPtr pkt = send_buff_list_.front();
        BufferPtr buff = pkt->packed_jmp_data;
        void* data;
        uint32_t nPeekSize = INFINITE;
        while ((data = buff->PeekBlock(INFINITE, nPeekSize)) && nPeekSize > 0)
        {
            int ret = Send(data, nPeekSize);
            if (ret < 0)
            {
                return;
            }
            if (ret == 0)
            {
                return;
            }
            if ((uint32_t)ret > nPeekSize)
            {
                LogQ(conn_->connector_->logger_ctx_, _ERROR_,
                    "stream_send: ret>nPeekSize stream_id:%" _S32BITARG_ " ret:%d nPeek:%u",
                    stream_id_, ret, nPeekSize);
                return;
            }
            buff->Forward((uint32_t)ret);
        }
        if (conn_ && conn_->handler_)
        {
            conn_->handler_->OnStreamDataSent(stream_id_, 
                pkt->trans_id, pkt->origion_data->UsedLength());
        }
        send_buff_list_.pop_front();
    }
}





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
    
    std::vector<xqc_http_header_t> headers;
    Json::Reader reader;
    Json::Value root;
    KBPool pool;

    const char* logical_host = conn_->config_.server_host;
    if (!logical_host || strlen(logical_host) == 0) {
        logical_host = conn_->connector_->config_.server_host;
    }
    if (!logical_host || strlen(logical_host) == 0) {
        logical_host = conn_->host_.c_str();
    }
    std::string authority = std::string(logical_host) + ":" + std::to_string(conn_->port_);
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
        .value = {.iov_base = (char*)logical_host, .iov_len = strlen(logical_host)},
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

    fin = 0; 
    h3_hdrs_.headers = headers.data();
    h3_hdrs_.count = headers.size();

    if (start_time_ == 0) {
        start_time_ = xqc_now();
    }
    
    ret = xqc_h3_request_send_headers(stream_, &h3_hdrs_, fin);
    if (ret < 0) {
        LogQ(conn_->connector_->logger_ctx_, _ERROR_, "[error] xqc_h3_request_send_headers error %d\n", ret);
    } else {
        hdr_sent_ = 1;
    }
    return BC_R_SUCCESS;
}

BCRESULT H3Stream::SendConnectUdpRequest(
    const std::string& authority,
    const std::string& target_host,
    uint16_t target_port)
{
    
    
    
    
    KBPool pool;
    std::vector<xqc_http_header_t> headers;
    std::string path = masque::BuildConnectUdpPath(target_host, target_port);

    xqc_http_header_t method = {
        .name = {.iov_base = (char*)":method", .iov_len = 7 },
        .value = {.iov_base = (char*)"CONNECT", .iov_len = 7 },
        .flags = 0,
    };
    headers.push_back(method);
    xqc_http_header_t protocol = {
        .name = {.iov_base = (char*)":protocol", .iov_len = 9 },
        .value = {.iov_base = (char*)"connect-udp", .iov_len = 11 },
        .flags = 0,
    };
    headers.push_back(protocol);
    xqc_http_header_t scheme = {
        .name = {.iov_base = (char*)":scheme", .iov_len = 7 },
        .value = {.iov_base = (char*)"https", .iov_len = 5 },
        .flags = 0,
    };
    headers.push_back(scheme);
    xqc_http_header_t authority_hdr = {
        .name = {.iov_base = (char*)":authority", .iov_len = 10 },
        .value = {.iov_base = (char*)pool.Strdup(authority.c_str()), .iov_len = authority.length() },
        .flags = 0,
    };
    headers.push_back(authority_hdr);
    xqc_http_header_t path_hdr = {
        .name = {.iov_base = (char*)":path", .iov_len = 5 },
        .value = {.iov_base = (char*)pool.Strdup(path.c_str()), .iov_len = path.length() },
        .flags = 0,
    };
    headers.push_back(path_hdr);
    xqc_http_header_t capsule = {
        .name = {.iov_base = (char*)"capsule-protocol", .iov_len = 16 },
        .value = {.iov_base = (char*)"?1", .iov_len = 2 },
        .flags = 0,
    };
    headers.push_back(capsule);

    h3_hdrs_.headers = headers.data();
    h3_hdrs_.count = headers.size();
    if (start_time_ == 0) {
        start_time_ = xqc_now();
    }
    
    int ret = xqc_h3_request_send_headers(stream_, &h3_hdrs_, 0);
    if (ret < 0) {
        LogQ(conn_->connector_->logger_ctx_, _ERROR_,
             "[masque] CONNECT-UDP send_headers error %d", ret);
        return BC_R_FAILURE;
    }
    hdr_sent_ = 1;
    LogQ(conn_->connector_->logger_ctx_, _INFO_,
         "[masque] CONNECT-UDP sent: authority=%s path=%s",
         authority.c_str(), path.c_str());
    return BC_R_SUCCESS;
}

int H3Stream::Send(const void *data, size_t size)
{
    

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    

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
            
            recv_fin_ = 1;
            LogQ(conn_->connector_->logger_ctx_, _INFO_, "[info] h3 request read header finish \n");
            return XQC_OK;
        }
    }

    
    if (!(flags & XQC_REQ_NOTIFY_READ_BODY)) {
        return XQC_OK;
    }

    read = read_sum = 0;

    do {
        read = xqc_h3_request_recv_body(stream_, recv_buff_, recv_buff_size_, &fin);
        if (read == -XQC_EAGAIN) {
            break;

        } else if (read < 0) {
            LogQ(conn_->connector_->logger_ctx_, _ERROR_, "xqc_h3_request_recv_body error %zd\n", read);
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
    
    
    

    delete this;
}

void H3Stream::OnPacketAcked(
    xqc_usec_t ack_delay_time,
    size_t acked_bytes,
    size_t inflight_bytes)
{
    if (inflight_bytes < 16384)
    {
        
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





SMPConnection::Config::Config() 
    : ipv6(false), host(NULL), port(0), server_host(NULL), server_port(0)
    , c_cong_ctl('b'), pacing_on(false), idle_time_out(0), ping_on(false)
    , linger_on(false), ping_interval(0), active_connection_id_limit(0)
    , alpn(NULL), device_type(0)
    , ca_cert_pem(NULL), ca_cert_pem_len(0)
    , vpn_policy(TT_VPN_POLICY_UNSET)
    , android_net_handle(0)
    , proxy_type(TT_PROXY_NONE), proxy_host(NULL), proxy_port(0)
    , proxy_sni(NULL), proxy_url(NULL)
    , proxy_ca_cert_pem(NULL), proxy_ca_cert_pem_len(0)
    , spki_pin(NULL), spki_pin_len(0)
{
    RAND_bytes((uint8_t*)cid_tag, SMP_CID_TAG_LEN);
}



static bool ParseProxyUrl(const char* url, TTProxyType* type_out,
                          std::string* host_out, uint16_t* port_out);

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
    pVar = pConfig->Get("device_type");
    if (IS_BCF_NUMBER(pVar))
    {
        device_type = (uint8_t)GET_BCF_INT(pVar);
        device_type = device_type == 1 ? 1 : 0;
    }
    pVar = pConfig->Get("cid_tag");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR cidTag = GET_BCF_STRING(pVar);
        size_t cidTagLen = strlen(cidTag);
        if (cidTagLen > SMP_CID_TAG_LEN) {
            return BC_R_INVALIDARG;
        }
        
        
        memcpy(cid_tag, cidTag, cidTagLen);
    }
    pVar = pConfig->Get("ca_cert_pem");
    if (IS_BCF_STRING(pVar))
    {
        ca_cert_pem = pool_.Strdup(GET_BCF_STRING(pVar));
        ca_cert_pem_len = strlen(ca_cert_pem);
    }
    pVar = pConfig->Get("vpnPolicy");
    if (IS_BCF_STRING(pVar))
    {
        vpn_policy = tt_vpn_policy_from_string(GET_BCF_STRING(pVar));
    }
    pVar = pConfig->Get("androidNetHandle");
    if (IS_BCF_NUMBER(pVar))
    {
        android_net_handle = (uint64_t)GET_BCF_INT(pVar);
    }
    
    
    
    pVar = pConfig->Get("proxy_url");
    if (IS_BCF_STRING(pVar))
    {
        proxy_url = pool_.Strdup(GET_BCF_STRING(pVar));
        std::string parsed_host;
        uint16_t parsed_port = 0;
        TTProxyType parsed_type = TT_PROXY_NONE;
        if (proxy_url && *proxy_url
            && ParseProxyUrl(proxy_url, &parsed_type, &parsed_host, &parsed_port))
        {
            proxy_type = parsed_type;
            proxy_host = pool_.Strdup(parsed_host.c_str());
            proxy_port = parsed_port;
        }
    }
    pVar = pConfig->Get("proxy_host");
    if (IS_BCF_STRING(pVar))
    {
        proxy_host = pool_.Strdup(GET_BCF_STRING(pVar));
        if (proxy_type == TT_PROXY_NONE)
        {
            proxy_type = TT_PROXY_MASQUE;
        }
    }
    pVar = pConfig->Get("proxy_port");
    if (IS_BCF_NUMBER(pVar))
    {
        proxy_port = (uint16_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("proxy_sni");
    if (IS_BCF_STRING(pVar))
    {
        proxy_sni = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    
    if (proxy_type != TT_PROXY_NONE && (!proxy_sni || !*proxy_sni) && proxy_host)
    {
        proxy_sni = pool_.Strdup(proxy_host);
    }
    pVar = pConfig->Get("proxy_ca_cert_pem");
    if (IS_BCF_STRING(pVar)) {
        proxy_ca_cert_pem = pool_.Strdup(GET_BCF_STRING(pVar));
        proxy_ca_cert_pem_len = strlen(proxy_ca_cert_pem);
    }
    pVar = pConfig->Get("spki_pin");
    if (IS_BCF_STRING(pVar)) {
        spki_pin = pool_.Strdup(GET_BCF_STRING(pVar));
        spki_pin_len = strlen(spki_pin);
    }
    return BC_R_SUCCESS;
}





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
    freeCertVector(root_cas_);
}

BCRESULT SMPConnection::Create(
    SMPConnector* connector, 
    IConnectionHandler *pHandler, 
    BCFObject* pConfig,
    uint64_t id,
    BCTaskMgr* pTaskMgr,
    BCTimerMgr* pTimerMgr)
{
    BCRESULT result;
    BCSockAddrS localAddr;
    
    int64_t initial_ifindex = 0;

    if (!connector || !pHandler || !pConfig)
    {
        LogQ(connector->logger_ctx_, _ERROR_, "invalid arguments: invalid connector or handler or config");
        return BC_R_INVALIDARG;
    }
    config_.Init(pConfig);
    if (config_.vpn_policy == TT_VPN_POLICY_UNSET)
    {
        config_.vpn_policy = connector->config_.vpn_policy;
    }
    if (config_.alpn == NULL || config_.server_host == NULL) {
        LogQ(connector->logger_ctx_, _ERROR_, "invalid arguments: invalid alpn or server_host");
        return BC_R_INVALIDARG;
    }
    
    
    if (!pTaskMgr)  pTaskMgr  = Runtime::RandomTaskMgr();
    if (!pTimerMgr) pTimerMgr = Runtime::RandomTimerMgr();
    task_mgr_  = pTaskMgr;
    timer_mgr_ = pTimerMgr;
    udp_socket_ = new UDPSenderGroup();
    if (!udp_socket_)
    {
        return BC_R_NOMEMORY;
    }
    config_.Init(pConfig);
    if (config_.ca_cert_pem && config_.ca_cert_pem_len > 0) {
        BCRESULT parse_result = parsePemCertBundle(
            config_.ca_cert_pem,
            config_.ca_cert_pem_len,
            root_cas_,
            connector->logger_ctx_,
            "SMPConnection");
        if (parse_result != BC_R_SUCCESS) {
            LogQ(connector->logger_ctx_, _ERROR_,
                "SMPConnection: failed to parse per-connection ca_cert_pem");
        } else {
            LogQ(connector->logger_ctx_, _INFO_,
                "SMPConnection: loaded %zu cert(s) from per-connection ca_cert_pem",
                root_cas_.size());
        }
    }
    
    
    
    
    
    
    
    
    
    
    
    initial_ifindex = (int64_t)config_.android_net_handle;
#if defined(TT_HAS_PATH_MONITOR)
    if (initial_ifindex == 0 &&
        config_.vpn_policy != TT_VPN_POLICY_OS)
    {
        initial_ifindex = tt_netmon_query_default_ifindex_ex(
            (int)config_.vpn_policy);
        LogQ(connector->logger_ctx_, _INFO_,
            "SMPConnection: vpnPolicy=%s 初始网卡查询结果 ifIndex=%lld",
            tt_vpn_policy_to_string(config_.vpn_policy),
            (long long)initial_ifindex);
    }
#endif
    result = udp_socket_->Create(connector->logger_ctx_, pTaskMgr, pTimerMgr,
        Runtime::SocketMgr(), pConfig, this, false, false, initial_ifindex);
    if (result != BC_R_SUCCESS)
    {
        goto delete_socket;
    }
    
    
    udp_socket_->SetVpnPolicy(config_.vpn_policy);
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
            
            
            
            
            else if (config_.vpn_policy == TT_VPN_POLICY_FORCE_PHYSICAL
                     && config_.android_net_handle == 0
#if defined(TT_HAS_PATH_MONITOR)
                     && tt_netmon_query_default_ifindex_ex(
                            (int)TT_VPN_POLICY_FORCE_PHYSICAL) <= 0
#endif
                    )
            {
                
                
                
                int64_t osBest = 0;
#if defined(TT_HAS_PATH_MONITOR)
                osBest = tt_netmon_query_default_ifindex_ex(
                    (int)TT_VPN_POLICY_OS);
#endif
                LogQ(connector_->logger_ctx_, _ERROR_,
                     "[SMPConnection] vpnPolicy=force-physical 但找不到可用物理"
                     "网卡（系统默认出口 ifIndex=%lld）。连接不发起，返回 "
                     "BC_R_NO_PHYSICAL_INTERFACE。常见原因：只挂了 VPN 而 "
                     "Wi-Fi / 有线均未连接。",
                     (long long)osBest);
                _NotifyConnectResult(pStub, BC_R_NO_PHYSICAL_INTERFACE);
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

void SMPConnection::Restart(int64_t networkHandle)
{
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    if (keep_working_)
    {
        PostTask([this, networkHandle] {
            if (_CloseCheck())
            {
                return;
            }
            network_handle_ = networkHandle;
            if (udp_socket_)
            {
                udp_socket_->Restart(networkHandle);
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

    
    
    
    
    
    
    if (masque_role_ == MASQUE_NONE
        && config_.proxy_type == TT_PROXY_MASQUE)
    {
        masque_role_ = MASQUE_INNER;
        masque_target_host_ = host_;
        masque_target_port_ = port_;
        LogQ(connector_->logger_ctx_, _INFO_,
             "[masque] inner connect deferred; bringing up tunnel to %s:%u for target %s:%u",
             config_.proxy_host ? config_.proxy_host : "?",
             (unsigned)config_.proxy_port,
             masque_target_host_.c_str(), (unsigned)masque_target_port_);
        return MasqueStartTunnel();
    }
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
        LogQ(connector_->logger_ctx_, _ERROR_, "invalid arguments: unknown cong_ctrl, option is b, r, c\n");
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

    xqc_conn_ssl_config_t conn_ssl_config;
    memset(&conn_ssl_config, 0, sizeof(conn_ssl_config));
    
    
    
    if (!root_cas_.empty() || !connector_->root_cas_.empty()
        || (config_.spki_pin && config_.spki_pin_len > 0
            && !(config_.proxy_host && config_.proxy_host[0] != '\0'))) {
        conn_ssl_config.cert_verify_flag = XQC_TLS_CERT_FLAG_NEED_VERIFY
                                         | XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;
    }

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
        
        conn_settings.max_datagram_frame_size = 65535;
        webtransport_ = true;
    }

    
    
    
    
    
    
    
    if (masque_role_ == MASQUE_TUNNEL)
    {
        conn_settings.max_pkt_out_size = 1420;
    }

    
    
    
    
    if (masque_role_ == MASQUE_INNER)
    {
        size_t inner_mss = masque_tunnel_ ? masque_tunnel_->MasqueDatagramMss() : 0;
        if (inner_mss == 0)
        {
            LogQ(connector_->logger_ctx_, _ERROR_,
                 "[masque] tunnel datagram MSS unavailable; cannot connect inner");
            return BC_R_FAILURE;
        }
        conn_settings.enable_pmtud = 0;
        conn_settings.max_pkt_out_size = inner_mss;
        
        
        memset(&local_addr_, 0, sizeof(local_addr_));
        local_addrlen_ = connector_->config_.ipv6 ? sizeof(struct sockaddr_in6)
                                                   : sizeof(struct sockaddr_in);
        ((struct sockaddr*)&local_addr_)->sa_family =
            connector_->config_.ipv6 ? AF_INET6 : AF_INET;
        LogQ(connector_->logger_ctx_, _INFO_,
             "[masque] inner connect over tunnel, max_pkt_out_size=%zu", inner_mss);
    }
    else if (udp_socket_)
    {
        udp_socket_->StartRecv();
    }
    result = TextToSockaddr(connector_->config_.ipv6?AF_INET6 : AF_INET, 
        host_.c_str(), port_, (sockaddr*)&peer_addr_, &peer_addrlen_);
    if (result != BC_R_SUCCESS) {
        LogQ(connector_->logger_ctx_, _ERROR_, "%s: invalid host(%s) or port(%d)",bc_result2string(result), host_.c_str(), (int)port_);
        return result;
    }

    const char *sni_host = (config_.server_host && strlen(config_.server_host) > 0)
                         ? config_.server_host
                         : (host_.length() > 0
                            ? host_.c_str()
                            : connector_->config_.server_host);
    cid = jqc_connect(connector_->engine_, &conn_settings, NULL,
                      0, sni_host, no_crypt,
                      &conn_ssl_config, (sockaddr*)&peer_addr_,
                      peer_addrlen_, config_.alpn, &timer_cbs, this);
    if (cid == NULL) {
        LogQ(connector_->logger_ctx_, _ERROR_, "jqc_connect FAILED");
        return BC_R_FAILURE;
    }

    

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
                        props["sdkVersion"] = GetSDKVersion();
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
                    LogQ(connector_->logger_ctx_, _INFO_, "connection succeed(%" _U64BITARG_ 
                        "), duration : %" _U64BITARG_ ".", 
                        connector_->total_succeed_connections.load(),
                        (now - connector_->start_time.load())/1000);
                }
                else if (root["name"] == "_error")
                {
                    std::string msgStr(lpszMsg, size);
                    state_ = CONN_STATE_INIT;
                    msgStr += std::string("(sdkVersion=") + GetSDKVersion() + ")";
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
        pkt->PackPacket();
        stream_map_[pkt->stream_id]->SendPacket(pkt);
        return BC_R_SUCCESS;
    }
    LogQ(connector_->logger_ctx_, _ERROR_, "SendPacket_Internal: stream_id %" _U32BITARG_ " NOT found in stream_map_, map_size:%zu",
         pkt->stream_id, stream_map_.size());
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

    
    xqc_stream_t *stream = xqc_stream_create(connector_->engine_, &cid_, NULL, NULL);
    if (!stream) {
        LogQ(connector_->logger_ctx_, _ERROR_, "OnHandshakeFinished: xqc_stream_create FAILED");
        return;
    }
    LogQ(connector_->logger_ctx_, _INFO_, "OnHandshakeFinished: stream created, stream_map size:%zu, connect_rpc:%p",
         stream_map_.size(), connect_rpc_);
    
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
        BCRESULT send_result = SendPacket_Internal(pkt);
        LogQ(connector_->logger_ctx_, _INFO_, "OnHandshakeFinished: SendPacket_Internal result:%d, json_size:%zu",
             send_result, json_str.size());
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
    LogQ(connector_->logger_ctx_, _INFO_, "quic connection closed");
    if (conn_) {
        conn_close_msg_ = jqc_conn_close_msg(conn_);
    }
    
    UnscheduleAllTasks();
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

    
    
    
    if (masque_role_ == MASQUE_TUNNEL)
    {
        xqc_h3_ext_datagram_set_user_data(h3_conn, this);
    }

    return 0;
}

void SMPConnection::OnH3HandshakeFinished()
{
    connector_->active_connections++;
    state_ = CONN_STATE_CONNECTING;

    
    
    
    if (masque_role_ == MASQUE_TUNNEL)
    {
        if (MasqueSendConnectUdp() != BC_R_SUCCESS)
        {
            MasqueOnConnectUdpResponse(false);
        }
        return;
    }

    if (handler_)
    {
        handler_->OnHandshakeFinished();
    }

    
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
        
    }
}

void SMPConnection::OnPacketAcked(
    xqc_usec_t ack_delay_time,
    size_t acked_bytes, 
    size_t inflight_bytes, 
    xqc_stream_id_t stream_id)
{
    
    
    if (stream_map_.find(stream_id) != stream_map_.end())
    {
        stream_map_[stream_id]->OnPacketAcked(ack_delay_time, 
            acked_bytes, inflight_bytes);
        if (handler_)
        {
            handler_->OnStreamDataAcked(stream_id, ack_delay_time, acked_bytes, inflight_bytes);
        }
    }
}

void SMPConnection::OnUpdataCID(
    const xqc_cid_t* retire_cid,
    const xqc_cid_t* new_cid)
{
    memcpy(&cid_, new_cid, sizeof(*new_cid));

    
    
    
    
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

    
    if (state_ <= CONN_STATE_CLOSING_SOCK)
    {
        return -1;
    }
    
    
    
    if (masque_role_ == MASQUE_INNER)
    {
        if (!masque_tunnel_)
        {
            return XQC_SOCKET_EAGAIN;
        }
        return masque_tunnel_->MasqueSendDatagram(buf, size);
    }
    if (udp_socket_ == NULL)
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
            res = XQC_SOCKET_EAGAIN;
        }
        else
        {
            res = size;
            wrote_counter++;
        }
    } while (0);

    if (res > 0) {
        if ((bc_time_now() - last_snd_ts) > 200000)
        {
            last_snd_ts = bc_time_now();
            last_snd_sum = snd_sum;
        }
    }

    return res;
}

void SMPConnection::OnH3RequestRecvHeaders(xqc_http_headers_t* headers)
{
    
    
    
    
    if (masque_role_ == MASQUE_TUNNEL)
    {
        bool ok = false;
        if (headers)
        {
            for (size_t i = 0; i < headers->count; i++)
            {
                const char* name = (const char*)headers->headers[i].name.iov_base;
                size_t name_len = headers->headers[i].name.iov_len;
                if (name_len == 7 && memcmp(name, ":status", 7) == 0)
                {
                    const char* val = (const char*)headers->headers[i].value.iov_base;
                    if (headers->headers[i].value.iov_len >= 1 && val[0] == '2')
                    {
                        ok = true;
                    }
                    break;
                }
            }
        }
        LogQ(connector_->logger_ctx_, _INFO_,
             "[masque] CONNECT-UDP response: %s", ok ? "established (2xx)" : "rejected");
        MasqueOnConnectUdpResponse(ok);
        return;
    }

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
            LogQ(connector_->logger_ctx_, _INFO_, "connection succeed(%" _U64BITARG_ 
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









BCRESULT SMPConnection::MasqueStartTunnel()
{
    
    masque_tunnel_ = connector_->CreateMasqueTunnel(this);
    if (!masque_tunnel_)
    {
        _NotifyConnectResult(NULL, BC_R_FAILURE, "masque: failed to create tunnel", 0);
        return BC_R_FAILURE;
    }
    masque_tunnel_->MasqueConnectToProxy();
    return BC_R_SUCCESS;
}

void SMPConnection::MasqueConnectToProxy()
{
    
    if (!keep_working_)
    {
        return;
    }
    PostTask([this] {
        if (_CloseCheck())
        {
            return;
        }
        if (state_ > CONN_STATE_INIT)
        {
            return;
        }
        scheme_ = "https";
        host_   = (masque_inner_ && masque_inner_->config_.proxy_host) ? masque_inner_->config_.proxy_host : "";
        port_   = masque_inner_ ? masque_inner_->config_.proxy_port : 0;
        state_  = CONN_STATE_HANDSHAKE;
        BCRESULT result = Connect_Internal();
        if (result != BC_R_SUCCESS)
        {
            MasqueOnConnectUdpResponse(false);
        }
        _CloseCheck();
    });
}

size_t SMPConnection::MasqueDatagramMss()
{
    
    
    if (masque_role_ != MASQUE_TUNNEL || !h3_conn_)
    {
        return 0;
    }
    size_t mss = xqc_h3_ext_datagram_get_mss(h3_conn_);
    size_t overhead = masque::DatagramOverhead(masque_qsid_);
    
    
    
    
    
    
    
    
    const size_t kMasqueMssSafetyMargin = 64;
    if (mss <= overhead + kMasqueMssSafetyMargin)
    {
        return 0;
    }
    return mss - overhead - kMasqueMssSafetyMargin;
}

BCRESULT SMPConnection::MasqueSendConnectUdp()
{
    
    
    H3Stream* st = new H3Stream();
    if (!st)
    {
        return BC_R_NOMEMORY;
    }
    xqc_stream_settings_t settings = { .recv_rate_bytes_per_sec = 0 };
    xqc_h3_request_t* stream = xqc_h3_request_create(connector_->engine_, &cid_, &settings, st);
    if (!stream)
    {
        delete st;
        return BC_R_FAILURE;
    }
    xqc_stream_id_t id = xqc_h3_request_id(stream);
    if (st->Create(id, stream, this) != BC_R_SUCCESS)
    {
        xqc_h3_request_close(stream);
        return BC_R_FAILURE;
    }
    h3_stream_map_[id] = st;
    masque_connect_stream_ = st;
    masque_qsid_ = masque::QuarterStreamId(id);

    std::string authority = host_ + ":" + std::to_string((unsigned)port_);
    return st->SendConnectUdpRequest(authority, masque_target_host_, masque_target_port_);
}

void SMPConnection::MasqueOnConnectUdpResponse(bool ok)
{
    
    if (masque_tunnel_ready_ && ok)
    {
        return; 
    }
    if (ok)
    {
        masque_tunnel_ready_ = true;
    }
    SMPConnection* inner = masque_inner_;
    BCRESULT result = ok ? BC_R_SUCCESS : BC_R_FAILURE;
    if (inner)
    {
        inner->PostTask([inner, result] {
            inner->MasqueOnTunnelReady(result);
        });
    }
}

void SMPConnection::MasqueOnTunnelReady(BCRESULT result)
{
    
    if (_CloseCheck())
    {
        return;
    }
    if (result != BC_R_SUCCESS)
    {
        LogQ(connector_->logger_ctx_, _ERROR_, "[masque] tunnel failed; failing inner connect");
        _NotifyConnectResult(NULL, BC_R_FAILURE, "masque: tunnel establishment failed", 0);
        _set_state(this, CONN_STATE_FREED, BC_R_FAILURE);
        _CloseCheck();
        return;
    }
    BCRESULT r = Connect_Internal();
    if (r != BC_R_SUCCESS)
    {
        _NotifyConnectResult(NULL, r, "masque: inner connect failed", 0);
        _set_state(this, CONN_STATE_FREED, r);
    }
    _CloseCheck();
}

ssize_t SMPConnection::MasqueSendDatagram(const unsigned char* buf, size_t size)
{
    
    
    
    
    if (!keep_working_ || masque_role_ != MASQUE_TUNNEL)
    {
        return XQC_SOCKET_EAGAIN;
    }
    std::vector<uint8_t> wire;
    if (!masque::EncodeDatagram(masque_qsid_, buf, size, wire))
    {
        return XQC_SOCKET_ERROR;
    }
    PostTask([this, wire = std::move(wire)]() mutable {
        if (_CloseCheck())
        {
            return;
        }
        if (h3_conn_)
        {
            uint64_t dgram_id = 0;
            xqc_int_t ret = xqc_h3_ext_datagram_send(
                h3_conn_, wire.data(), wire.size(), &dgram_id, XQC_DATA_QOS_HIGH);
            if (ret < 0 && ret != -XQC_EAGAIN)
            {
                LogQ(connector_->logger_ctx_, _WARN_, "[masque] datagram_send err %d", (int)ret);
            }
        }
        _CloseCheck();
    });
    return (ssize_t)size;
}

void SMPConnection::MasqueOnDatagram(const uint8_t* data, size_t len)
{
    
    uint64_t qsid = 0, ctx = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    if (!masque::DecodeDatagram(data, len, &qsid, &ctx, &payload, &payload_len))
    {
        return;
    }
    if (qsid != masque_qsid_ || ctx != masque::kUdpContextId || payload_len == 0)
    {
        return;
    }
    SMPConnection* inner = masque_inner_;
    if (!inner)
    {
        return;
    }
    std::vector<uint8_t> pkt(payload, payload + payload_len);
    inner->PostTask([inner, pkt = std::move(pkt)]() mutable {
        inner->MasqueOnTunneledPacket(pkt.data(), pkt.size());
    });
}

void SMPConnection::MasqueOnTunneledPacket(const uint8_t* data, size_t len)
{
    
    if (_CloseCheck())
    {
        return;
    }
    if (!conn_)
    {
        return;
    }
    uint64_t recv_time = bc_time_now();
    xqc_int_t ret = jqc_conn_packet_process(
        conn_, data, len,
        (struct sockaddr*)&local_addr_, local_addrlen_,
        (struct sockaddr*)&peer_addr_, peer_addrlen_,
        XQC_UNKNOWN_PATH_ID, (xqc_msec_t)recv_time, this, NULL, NULL, 0);
    if (ret != XQC_OK)
    {
        LogQ(connector_->logger_ctx_, _ERROR_, "[masque] inner packet process err %d", (int)ret);
        _CloseCheck();
        return;
    }
    jqc_conn_finish_recv(conn_);
    _CloseCheck();
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
    if (udp_socket_)
    {
        BCSockAddrS localAddr;
        udp_socket_->GetSockName(localAddr);
        local_addrlen_ = localAddr.length;
        memcpy(&local_addr_, &localAddr.type, local_addrlen_);
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
                XQC_UNKNOWN_PATH_ID, (xqc_msec_t)recv_time, this, NULL, NULL, 0);
        } else {
            ret = jqc_conn_packet_process(conn_, packet_buf, recv_size,
                (struct sockaddr*)(&this->local_addr_), this->local_addrlen_,
                (struct sockaddr*)(&refSrcAddr.type.sa), peer_addrlen,
                XQC_UNKNOWN_PATH_ID, (xqc_msec_t)recv_time, this, NULL, NULL, 0);
        }
        if (ret != XQC_OK)
        {
            LogQ(connector_->logger_ctx_, _ERROR_, "%s: packet process err\n", __FUNCTION__);
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

void SMPConnection::OnCheckAvailable()
{
    if (keep_working_)
    {
        PostTask([this] {
            if (_CloseCheck())
            {
                return;
            }
            if (conn_ && udp_socket_)
            {
                BCSockAddrS localAddr;
                udp_socket_->GetSockName(localAddr);
                xqc_int_t ret = jqc_conn_local_addr_changed(conn_,
                    (struct sockaddr *)&localAddr.type, localAddr.length);
                if (ret != XQC_OK)
                {
                    LogQ(connector_->logger_ctx_, _ERROR_, "%s: local addr changed err\n", __FUNCTION__);
                }
            }
            _CloseCheck();
        });
    }
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
            BCSockAddrS peerAddr;
            memcpy(&peerAddr, &peer_addr_, peer_addrlen_);
            peerAddr.length = peer_addrlen_;
            udp_socket_->Connect(peerAddr);

            BCSockAddrS localAddr;
            udp_socket_->GetSockName(localAddr);
            local_addrlen_ = localAddr.length;
            memcpy(&local_addr_, &localAddr.type, local_addrlen_);
            udp_socket_->StartRecv();

            {
                char local_str[128];
                bc_sockaddr_format(&localAddr, local_str, sizeof(local_str));
                LogQ(connector_->logger_ctx_, _DEBUG_,
                    "OnRestart: socket local_addr=%s, addrlen=%d, network_handle=%lld",
                    local_str, (int)local_addrlen_, (long long)network_handle_);
            }

            xqc_connection_t *qconn = conn_ ? conn_ : (h3_conn_ ? xqc_h3_conn_get_xqc_conn(h3_conn_) : NULL);
            if (qconn) {
                jqc_conn_local_addr_changed(qconn,
                    (struct sockaddr *)&local_addr_, local_addrlen_);
            }
            
            
            
            
            
            
            
            
            
            
            
            
            
            

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
    if (masque_role_ == MASQUE_TUNNEL)
    {
        
        
        
        
        
        SMPConnection* inner = masque_inner_;
        masque_inner_ = NULL;
        if (inner)
        {
            inner->PostTask([inner] { inner->MasqueOnTunnelClosed(); });
        }
        return;
    }
    
    
    
    if (connector_)
    {
        connector_->NotifyConnClosed(id_);
    }
    if (handler_)
    {
        if (conn_close_msg_.empty()) {
            conn_close_msg_ = bc_result2string(close_status_);
        }
        handler_->OnClosed(conn_close_msg_.c_str());
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
        
        if (!_this || !_this->conn_)
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
        
        if (!_this || !_this->conn_)
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
        
        if (!_this || !_this->conn_)
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
        
        keep_working_ = false;
        if (conn_)
        {
            jqc_conn_close(conn_, &cid_);
        }
        if (connect_timer_id_ > 0)
        {
            UnscheduleTask(connect_timer_id_);
            connect_timer_id_ = 0;
        }
        state_ = CONN_STATE_CLOSING_QUIC;
    }

    if (state_ == CONN_STATE_CLOSING_QUIC)
    {
        if (conn_)
        {
            return true;
        }
        
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
        
        
        
        
        if (masque_role_ == MASQUE_INNER && masque_tunnel_)
        {
            if (!masque_tunnel_closing_)
            {
                masque_tunnel_closing_ = true;
                masque_tunnel_->Close();
            }
            return true;
        }
        _NotifyConnectResult(NULL, close_status_);
        Detach();
        state_ = CONN_STATE_FREED;
    }

    return true;
}

void SMPConnection::MasqueOnTunnelClosed()
{
    
    
    SMPConnection* tunnel = masque_tunnel_;
    masque_tunnel_ = NULL;
    masque_tunnel_closing_ = false;
    if (tunnel)
    {
        delete tunnel;
    }
    if (state_ == CONN_STATE_INIT)
    {
        
        
        
        LogQ(connector_->logger_ctx_, _WARN_, "[masque] tunnel closed before inner connect; failing connect");
        _NotifyConnectResult(NULL, BC_R_NETDOWN, "masque: tunnel closed before connect", 0);
        _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    }
    else if (state_ > CONN_STATE_INIT)
    {
        
        
        LogQ(connector_->logger_ctx_, _WARN_, "[masque] tunnel closed; tearing down inner connection");
        _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    }
    
    _CloseCheck();
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
    
    
    
    if (pStub != NULL && pStub == connect_rpc_)
    {
        connect_rpc_ = NULL;
        if (connect_timer_id_ > 0)
        {
            UnscheduleTask(connect_timer_id_);
            connect_timer_id_ = 0;
        }
    }
}









static bool ParseProxyUrl(const char* url,
                          TTProxyType* type_out,
                          std::string* host_out,
                          uint16_t* port_out)
{
    if (!url || !*url) {
        return false;
    }
    std::string s(url);
    std::string scheme;
    std::string authority;
    size_t sep = s.find("://");
    if (sep != std::string::npos) {
        scheme = s.substr(0, sep);
        authority = s.substr(sep + 3);
    } else {
        
        authority = s;
    }
    
    size_t slash = authority.find('/');
    if (slash != std::string::npos) {
        authority = authority.substr(0, slash);
    }
    if (authority.empty()) {
        return false;
    }

    
    for (char& c : scheme) { c = (char)tolower((unsigned char)c); }
    if (scheme.empty() || scheme == "masque" || scheme == "connect-udp"
        || scheme == "https" || scheme == "h3") {
        *type_out = TT_PROXY_MASQUE;
    } else {
        return false;
    }

    
    std::string host;
    uint16_t port = 443;
    if (authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = authority.substr(0, close + 1); 
        if (close + 1 < authority.size() && authority[close + 1] == ':') {
            port = (uint16_t)atoi(authority.c_str() + close + 2);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = (uint16_t)atoi(authority.c_str() + colon + 1);
        } else {
            host = authority;
        }
    }
    if (host.empty() || port == 0) {
        return false;
    }
    *host_out = host;
    *port_out = port;
    return true;
}

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
        std::string alpn_str = GET_BCF_STRING(pVar).c_str();
        std::vector<std::string> alpn_list;
        SplitString(alpn_str, ',', &alpn_list);
        alpn.assign(alpn_list.begin(), alpn_list.end());
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
    pVar = pConfig->Get("ca_cert_pem");
    if (IS_BCF_STRING(pVar))
    {
        ca_cert_pem = pool_.Strdup(GET_BCF_STRING(pVar));
        ca_cert_pem_len = strlen(ca_cert_pem);
    }
    pVar = pConfig->Get("disableAutoRestart");
    if (IS_BCF_BOOL(pVar))
    {
        disableAutoRestart = GET_BCF_BOOL(pVar);
    }
    
    
    
    
    bool hasBypassVpn = false;
    pVar = pConfig->Get("bypassVpn");
    if (IS_BCF_BOOL(pVar))
    {
        bypassVpn    = GET_BCF_BOOL(pVar);
        hasBypassVpn = true;
    }
    TTVpnPolicy explicitPolicy = TT_VPN_POLICY_UNSET;
    vpn_policy_bad_string      = NULL;
    pVar = pConfig->Get("vpnPolicy");
    if (IS_BCF_STRING(pVar))
    {
        LPCSTR raw = GET_BCF_STRING(pVar);
        explicitPolicy = tt_vpn_policy_from_string(raw);
        if (explicitPolicy == TT_VPN_POLICY_UNSET && raw && raw[0])
        {
            vpn_policy_bad_string = pool_.Strdup(raw);
        }
    }
    int bothGiven = 0;
    vpn_policy = tt_vpn_policy_resolve(explicitPolicy,
                                       hasBypassVpn ? 1 : 0,
                                       bypassVpn ? 1 : 0,
                                       tt_vpn_policy_platform_default(),
                                       &bothGiven);
    vpn_policy_both_given = (bothGiven != 0);
    
    
    
    pVar = pConfig->Get("proxy_url");
    if (IS_BCF_STRING(pVar))
    {
        proxy_url = pool_.Strdup(GET_BCF_STRING(pVar));
        std::string parsed_host;
        uint16_t parsed_port = 0;
        TTProxyType parsed_type = TT_PROXY_NONE;
        if (proxy_url && *proxy_url
            && ParseProxyUrl(proxy_url, &parsed_type, &parsed_host, &parsed_port))
        {
            proxy_type = parsed_type;
            proxy_host = pool_.Strdup(parsed_host.c_str());
            proxy_port = parsed_port;
        }
    }
    pVar = pConfig->Get("proxy_host");
    if (IS_BCF_STRING(pVar))
    {
        proxy_host = pool_.Strdup(GET_BCF_STRING(pVar));
        if (proxy_type == TT_PROXY_NONE)
        {
            proxy_type = TT_PROXY_MASQUE;
        }
    }
    pVar = pConfig->Get("proxy_port");
    if (IS_BCF_NUMBER(pVar))
    {
        proxy_port = (uint16_t)GET_BCF_INT(pVar);
    }
    pVar = pConfig->Get("proxy_sni");
    if (IS_BCF_STRING(pVar))
    {
        proxy_sni = pool_.Strdup(GET_BCF_STRING(pVar));
    }
    
    if (proxy_type != TT_PROXY_NONE && (!proxy_sni || !*proxy_sni) && proxy_host)
    {
        proxy_sni = pool_.Strdup(proxy_host);
    }
    return BC_R_SUCCESS;
}





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
#if defined(TT_HAS_PATH_MONITOR)
    if (path_monitor_)
    {
        
        
        tt_netmon_stop(path_monitor_);
        path_monitor_ = nullptr;
    }
#endif
    freeCertVector(root_cas_);
}

#if defined(TT_HAS_PATH_MONITOR)





void SMPConnector::OnPathRawLog(void* userdata, const char* line)
{
    SMPConnector* self = static_cast<SMPConnector*>(userdata);
    if (!self || !line) return;
    LogQ(self->logger_ctx_, _INFO_,
         "[SMPConnector] netmon raw %s", line);
}

void SMPConnector::OnPathChange(void* userdata, int64_t newIfIndex,
                                const char* pathDesc)
{
    SMPConnector* self = static_cast<SMPConnector*>(userdata);
    if (!self || newIfIndex <= 0) return;

    
    
    
    
    
    
    
    
    bool was_first = !self->first_path_callback_seen_.exchange(
        true, std::memory_order_acq_rel);
    if (was_first)
    {
        LogQ(self->logger_ctx_, _INFO_,
             "[SMPConnector] initial path ifIndex=%lld desc=\"%s\" (no restart)",
             (long long)newIfIndex, pathDesc ? pathDesc : "");
        return;
    }

    
    
    
    
    
    
    
    
    
    
    
    
    
    
    
    constexpr int64_t kPathChangeCooldownMs = 3000;
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (self->last_path_change_ms_ != 0 &&
        (now_ms - self->last_path_change_ms_) < kPathChangeCooldownMs)
    {
        LogQ(self->logger_ctx_, _INFO_,
             "[SMPConnector] path change ifIndex=%lld desc=\"%s\" "
             "ignored (cooldown %lldms since last)",
             (long long)newIfIndex, pathDesc ? pathDesc : "",
             (long long)(now_ms - self->last_path_change_ms_));
        return;
    }
    self->last_path_change_ms_ = now_ms;

    
    
    
    
    std::vector<SMPConnection*> snapshot;
    {
        BCSpinMutex::Owner lock(self->lock_);
        snapshot.reserve(self->conns_map_.size());
        for (auto& kv : self->conns_map_) snapshot.push_back(kv.second);
    }
    LogQ(self->logger_ctx_, _INFO_,
         "[SMPConnector] path change ifIndex=%lld desc=\"%s\" -> %zu connections",
         (long long)newIfIndex, pathDesc ? pathDesc : "", snapshot.size());

    for (SMPConnection* conn : snapshot)
    {
        if (conn) conn->Restart(newIfIndex);
    }
}
#endif 

BCRESULT SMPConnector::Create(BCFObject* pConfig, IConnectorHandler* pHandler)
{
    if (!pConfig || !pHandler)
    {
        LogQ(logger_ctx_, _ERROR_, "invalid arguments: invalid config or handler");
        return BC_R_INVALIDARG;
    }

    handler_ = pHandler;
    config_.Init(pConfig);
	if (config_.alpn.empty())
    {
        LogQ(logger_ctx_, _ERROR_, "invalid arguments: invalid alpn");
        return BC_R_INVALIDARG;
    }
    if (config_.ca_cert_pem && config_.ca_cert_pem_len > 0) {
        BCRESULT parse_result = parsePemCertBundle(
            config_.ca_cert_pem,
            config_.ca_cert_pem_len,
            root_cas_,
            logger_ctx_,
            "SMPConnector");
        if (parse_result != BC_R_SUCCESS) {
            return parse_result;
        }
        LogQ(logger_ctx_, _INFO_, "SMPConnector: loaded %zu cert(s) from ca_cert_pem",
            root_cas_.size());
    }
    int32_t log_level = LogLevelToBCLogLevel(config_.log_level);
    if (config_.log_file) {
        logger_ctx_ = AddFileLogAppender(config_.log_file, log_level, true, true);
    } else {
        logger_ctx_ = AddExternalLogAppender(log_callback, this, log_level, true);
    }

    
    
    
    if (config_.vpn_policy_bad_string)
    {
        LogQ(logger_ctx_, _WARN_,
             "[SMPConnector] 无法识别的 vpnPolicy=\"%s\"，已忽略；合法取值为 "
             "os / prefer-physical / force-physical。本次回落为 %s",
             config_.vpn_policy_bad_string,
             tt_vpn_policy_to_string(config_.vpn_policy));
    }
    if (config_.vpn_policy_both_given)
    {
        LogQ(logger_ctx_, _WARN_,
             "[SMPConnector] 同时配置了 vpnPolicy 与已废弃的 bypassVpn，"
             "以 vpnPolicy=%s 为准，bypassVpn 被忽略",
             tt_vpn_policy_to_string(config_.vpn_policy));
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
        .write_socket_ex = conn_write_socket_ex_cb,
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
        LogQ(logger_ctx_, _ERROR_, "invalid arguments: invalid engine config");
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
    
    config.cid_len = SMP_CID_LEN;
    



    config.enable_h3_ext = 1;
    quic_lb_ctx_.conf_id = 0;
    quic_lb_ctx_.cid_len = SMP_CID_LEN;
    quic_lb_ctx_.sid_len = 0;

    engine_ = xqc_engine_create(XQC_ENGINE_CLIENT, &config, 
        &config_.engine_ssl_config, &callback, &tcbs, this);
    if (engine_ == NULL) {
        LogQ(logger_ctx_, _ERROR_, "invalid arguments: error create engine");
        return BC_R_INVALIDARG;
    }

    
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
        .h3_ext_dgram_cbs = {
            .dgram_read_notify = on_h3_ext_datagram_read,
        },
        .h3_ext_bs_cbs = {},
        .wt_stream_cbs = {
            .stream_read_notify = on_raw_stream_read_notify,
            .stream_write_notify = on_raw_stream_write_notify,
            .stream_create_notify = on_raw_stream_create_notify,
            .stream_close_notify = on_raw_stream_close_notify,
        }
    };
    
    int ret = xqc_h3_ctx_init(engine_, &h3_cbs);
    if (ret != XQC_OK) {
        return BC_R_UNEXPECTED;
    }

    
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

    
    for (const auto &alpn : config_.alpn) {
        ret = xqc_engine_register_alpn(engine_, alpn.c_str(), alpn.size(), &raw_cbs);
        if (ret != XQC_OK) {
            return BC_R_UNEXPECTED;
        }
    }

    state_ = CONNTOR_STATE_WORKING;

#if defined(TT_HAS_PATH_MONITOR)
    
    
    
    
    
    
    
    if (!config_.disableAutoRestart)
    {
        TTNetworkMonitorOptions netmon_opts{};
        
        
        
        netmon_opts.vpnPolicy = (int)config_.vpn_policy;
        netmon_opts.bypassVpn =
            (config_.vpn_policy == TT_VPN_POLICY_OS) ? 0 : 1;
        netmon_opts.rawLogFn  = &SMPConnector::OnPathRawLog;
        netmon_opts.rawLogCtx = this;
        path_monitor_ = tt_netmon_start(&netmon_opts,
                                        &SMPConnector::OnPathChange, this);
        if (!path_monitor_)
        {
            LogQ(logger_ctx_, _WARN_,
                 "[SMPConnector] tt_netmon_start failed; auto-restart disabled");
        }
        else
        {
            LogQ(logger_ctx_, _INFO_,
                 "[SMPConnector] tt_netmon_start ok vpnPolicy=%s",
                 tt_vpn_policy_to_string(config_.vpn_policy));
        }
    }
    else
    {
        LogQ(logger_ctx_, _INFO_,
             "[SMPConnector] auto-restart disabled by config");
    }
#endif

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

SMPConnection* SMPConnector::CreateMasqueTunnel(SMPConnection* inner)
{
    if (!inner)
    {
        return NULL;
    }
    if (inner->config_.proxy_type != TT_PROXY_MASQUE
        || !inner->config_.proxy_host || inner->config_.proxy_port == 0)
    {
        LogQ(logger_ctx_, _ERROR_, "[masque] tunnel requested but proxy not configured");
        return NULL;
    }

    
    
    
    
    BCFObject cfg;
    cfg.PutBool("ipv6", config_.ipv6 ? 1 : 0);
    cfg.PutString("alpn", "h3");
    const char* sni = (inner->config_.proxy_sni && *inner->config_.proxy_sni)
                    ? inner->config_.proxy_sni : inner->config_.proxy_host;
    cfg.PutString("server_host", sni);
    cfg.PutInt("server_port", inner->config_.proxy_port);
    cfg.PutInt("c_cong_ctl", config_.c_cong_ctl ? config_.c_cong_ctl : 'B');
    cfg.PutInt("log_level", config_.log_level);
    cfg.PutInt("num_of_senders", 1);
    
    
    
    
    if (inner->config_.proxy_ca_cert_pem && inner->config_.proxy_ca_cert_pem_len > 0) {
        cfg.PutString("ca_cert_pem", inner->config_.proxy_ca_cert_pem);
    }
    
    
    
    
    if (inner->config_.spki_pin && inner->config_.spki_pin_len > 0) {
        cfg.PutString("spki_pin", inner->config_.spki_pin);
    }

    SMPConnection* tunnel = new SMPConnection();
    if (!tunnel)
    {
        return NULL;
    }
    BCSpinMutex::Owner lock(lock_);
    
    
    
    BCRESULT result = tunnel->Create(this, MasqueTunnelHandler::Instance(),
                                     &cfg, next_conn_id_++,
                                     inner->task_mgr_, inner->timer_mgr_);
    if (result != BC_R_SUCCESS)
    {
        delete tunnel;
        return NULL;
    }
    tunnel->masque_role_  = SMPConnection::MASQUE_TUNNEL;
    tunnel->masque_inner_ = inner;
    
    
    tunnel->masque_target_host_ = inner->masque_target_host_;
    tunnel->masque_target_port_ = inner->masque_target_port_;
    
    
    
    return tunnel;
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
        
        
        xqc_h3_ctx_destroy(engine_);
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
    SMPConnection *user_conn = (SMPConnection *)conn_user_data;
    if (!user_conn || !user_conn->connector_ || certs_len == 0) {
        return -1;
    }
    SMPConnector *connector = user_conn->connector_;

    
    
    
    
    if (user_conn->config_.spki_pin && user_conn->config_.spki_pin_len > 0
        && !(user_conn->config_.proxy_host && user_conn->config_.proxy_host[0] != '\0')) {
        const unsigned char *p = certs[0];
        X509 *leaf = d2i_X509(nullptr, &p, cert_len[0]);
        if (!leaf) {
            user_conn->cert_verify_error_ = "spki pin: failed to parse leaf cert";
            LogQ(connector->logger_ctx_, _ERROR_, "spki pin: failed to parse leaf cert");
            return -1;
        }
        std::string computed = TlsContext::ComputeSpkiPinBase64(leaf);
        X509_free(leaf);
        std::string expected = TlsContext::CanonicalizeSpkiPin(
            user_conn->config_.spki_pin, user_conn->config_.spki_pin_len);
        if (!computed.empty() && computed == expected) {
            LogQ(connector->logger_ctx_, _INFO_, "spki pin: matched");
            return 0;
        }
        user_conn->cert_verify_error_ = "spki pin mismatch";
        LogQ(connector->logger_ctx_, _ERROR_,
            "spki pin: MISMATCH computed=%s expected=%s",
            computed.c_str(), expected.c_str());
        return -1;
    }

    const std::vector<X509*>* trusted_cas = !user_conn->root_cas_.empty()
        ? &user_conn->root_cas_
        : &connector->root_cas_;
    if (trusted_cas->empty()) {
        
        
        
        
        
        
        
        return 0;
    }

    
    
    
    std::vector<X509*> chain;
    chain.reserve(certs_len);
    {
        const unsigned char *p0 = certs[0];
        X509 *leaf_cert = d2i_X509(nullptr, &p0, cert_len[0]);
        if (!leaf_cert) {
            return -1;
        }
        chain.push_back(leaf_cert);
    }
    for (size_t i = 1; i < certs_len; i++) {
        const unsigned char *p = certs[i];
        X509 *cert = d2i_X509(nullptr, &p, cert_len[i]);
        if (cert) {
            chain.push_back(cert);
        }
    }

    const char *host = user_conn->config_.server_host;
    if (!host || strlen(host) == 0) {
        host = user_conn->host_.length() > 0
             ? user_conn->host_.c_str()
             : connector->config_.server_host;
    }

    
    
    
    
    
    TlsConfig cfg;
    const bool conn_has_ca = !user_conn->root_cas_.empty();
    const char *ca_pem = conn_has_ca
        ? user_conn->config_.ca_cert_pem
        : connector->config_.ca_cert_pem;
    const size_t ca_pem_len = conn_has_ca
        ? user_conn->config_.ca_cert_pem_len
        : connector->config_.ca_cert_pem_len;
    cfg.caCertsPem.assign(ca_pem ? ca_pem : "", ca_pem ? ca_pem_len : 0);
    cfg.loggerCtx = connector->logger_ctx_;

    std::string err;
    BCRESULT verify_result = TlsContext::VerifyChain(
        chain, host ? std::string(host) : std::string(), cfg, err);

    int result = (verify_result == BC_R_SUCCESS) ? 0 : -1;
    if (result != 0) {
        user_conn->cert_verify_error_ = err;
        LogQ(connector->logger_ctx_, _ERROR_, "cert verify: %s", err.c_str());

        
        
        X509* leaf = chain[0];
        char subj_buf[256] = {0};
        char issuer_buf[256] = {0};
        X509_NAME_oneline(X509_get_subject_name(leaf), subj_buf, sizeof(subj_buf));
        X509_NAME_oneline(X509_get_issuer_name(leaf), issuer_buf, sizeof(issuer_buf));
        LogQ(connector->logger_ctx_, _ERROR_,
            "cert verify: server leaf cert subject=%s, issuer=%s", subj_buf, issuer_buf);
        for (size_t i = 1; i < chain.size(); ++i) {
            X509* ic = chain[i];
            char ic_subj[256] = {0};
            char ic_issuer[256] = {0};
            X509_NAME_oneline(X509_get_subject_name(ic), ic_subj, sizeof(ic_subj));
            X509_NAME_oneline(X509_get_issuer_name(ic), ic_issuer, sizeof(ic_issuer));
            LogQ(connector->logger_ctx_, _ERROR_,
                "cert verify: chain[%zu] subject=%s, issuer=%s", i - 1, ic_subj, ic_issuer);
        }
        LogQ(connector->logger_ctx_, _ERROR_,
            "cert verify: local trusted CA count=%zu", trusted_cas->size());
        for (size_t i = 0; i < trusted_cas->size(); ++i) {
            X509* ca_cert = (*trusted_cas)[i];
            if (!ca_cert) {
                continue;
            }
            char ca_subj[256] = {0};
            char ca_issuer[256] = {0};
            X509_NAME_oneline(X509_get_subject_name(ca_cert), ca_subj, sizeof(ca_subj));
            X509_NAME_oneline(X509_get_issuer_name(ca_cert), ca_issuer, sizeof(ca_issuer));
            LogQ(connector->logger_ctx_, _ERROR_,
                "cert verify: trusted_ca[%zu] subject=%s, issuer=%s",
                i, ca_subj, ca_issuer);
        }
    }

    freeCertVector(chain);
    return result;
}

int SMPConnector::on_conn_closing_notify(
    xqc_connection_t *conn,
    const xqc_cid_t *cid,
    xqc_int_t err_code,
    void *conn_user_data) {
    SMPConnection* user_conn = (SMPConnection*)conn_user_data;
    if (!user_conn) {
        return XQC_OK;
    }
    if (!user_conn->cert_verify_error_.empty() && user_conn->connect_rpc_) {
        std::string msg = user_conn->cert_verify_error_;
        user_conn->_NotifyConnectResult(NULL, BC_R_FAILURE,
            msg.c_str(), msg.size());
    }
    return XQC_OK;
}


int
SMPConnector::on_raw_conn_create_notify(
    xqc_connection_t *conn, 
    const xqc_cid_t *cid, 
    void *conn_user_data,
    void *conn_proto_data) {
    SMPConnection* user_conn = (SMPConnection*)conn_user_data;
    if (user_conn)
    {
        LogQ(user_conn->connector_->logger_ctx_, _INFO_, "quic conn created");
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
        LogQ(conn->connector_->logger_ctx_, _INFO_, "quic stream created: %" _U32BITARG_, id); 
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
    
    int ret = 0;
    SMPStream *user_stream = (SMPStream *) user_data;
    if (user_stream)
    {
        ret = user_stream->OnWriteNotify();
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
    
    int ret = 0;
    H3Stream *user_stream = (H3Stream *)h3s_user_data;
    if (user_stream)
    {
        
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

void
SMPConnector::on_h3_ext_datagram_read(
    xqc_h3_conn_t *conn,
    const void *data,
    size_t data_len,
    void *user_data,
    uint64_t data_recv_time)
{
    
    
    
    SMPConnection *tunnel = (SMPConnection*)user_data;
    if (tunnel && data && data_len > 0)
    {
        tunnel->MasqueOnDatagram((const uint8_t*)data, data_len);
    }
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
    SMPConnection *conn = (SMPConnection*)user_data; 
    if (conn)
    {
        res = conn->WritePacket(buf, size, peer_addr, peer_addrlen);
        if (res == XQC_SOCKET_ERROR) {
            LogQ(conn->connector_->logger_ctx_, _ERROR_, "write_socket: FAILED res:%d size:%zu", res, size);
        }
    }
    else
    {
        LogQ(NULL, _ERROR_, "write_socket: conn is NULL, size:%zu", size);
    }

    return res;
}

ssize_t
SMPConnector::conn_write_socket_ex_cb(
    uint64_t path_id,
    const unsigned char *buf,
    size_t size,
    const struct sockaddr *peer_addr,
    socklen_t peer_addrlen,
    void *conn_user_data)
{
    return conn_write_socket_cb(buf, size, peer_addr, peer_addrlen, conn_user_data);
}


void 
SMPConnector::on_write_log(
    xqc_log_level_t lvl, 
    const void *buf, 
    size_t count, 
    void *engine_user_data)
{
    SMPConnector *ctx = (SMPConnector*)engine_user_data;

    int32_t level = XQCLogLevelToBCLogLevel(lvl);
    LogCustom(ctx->logger_ctx_, level, "%.*s", count, buf);
}

void SMPConnector::on_keylog_cb(const xqc_cid_t *scid, const char *line,
                            void *user_data) {
    SMPConnector *ctx = (SMPConnector*)user_data;
    
    LogCustom(ctx->logger_ctx_, _INFO_, "%s", line);
}

void SMPConnector::on_server_refuse(
    xqc_engine_t* engine,
    xqc_connection_t* conn,
    const xqc_cid_t* cid,
    void* user_data)
{
    
    
    
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
    SMPConnector *_this = (SMPConnector*)user_data; 
    if (_this)
    {
        
    }

    return res;
}

ssize_t
SMPConnector::cid_generate(
    const xqc_cid_t* ori_cid, 
    uint8_t* cid_buf, 
    size_t cid_buflen, 
    void* engine_user_data,
    void *conn_user_data)
{
    SMPConnector *_this = (SMPConnector*)engine_user_data;
    SMPConnection *conn = (SMPConnection*)conn_user_data;
    if (!_this || cid_buflen < SMP_CID_LEN || !conn)
    {
        return XQC_ERROR;
    }

    if (ori_cid && ori_cid->cid_len >= 14) {
        memcpy(cid_buf, ori_cid->cid_buf, 14);
    } else {
        cid_buf[0] = (conn->config_.device_type << 7) | 'J';
        cid_buf[1] = 'Q';
        cid_buf[2] = 'C';
        memset(cid_buf + 3, 0, 11);
        memcpy(cid_buf + 3, conn->config_.cid_tag, SMP_CID_TAG_LEN);
    }
    RAND_bytes(cid_buf + 14, 2);

    return SMP_CID_LEN;
}

void SMPConnector::log_callback(void *data, int level, LPCSTR lpszMsg) 
{
    std::string msg(lpszMsg);
    Runtime::PostTask([data, level, msg] {
        auto *ctx = (SMPConnector*)data;
        if (ctx && ctx->handler_) {
            int32_t log_level = BCLogLevelToLogLevel(level);
            ctx->handler_->OnLog(log_level, msg.c_str());
        }
    });
}




