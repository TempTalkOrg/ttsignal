///////////////////////////////////////////////////////////////////////////////
// file : SMPConnector.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////

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


#define XQC_PACKET_TMP_BUF_LEN 1500
#define DEFAULT_CONNECT_TIMEOUT 10000

namespace {

// No-op IConnectionHandler for the internal MASQUE tunnel connection. The
// tunnel is invisible to the application, so none of these callbacks should
// surface anywhere; all tunnel state transitions are observed directly by the
// inner connection through the masque_* hand-offs. A single stateless
// instance is shared by every tunnel.
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

int addTrustedCAsToStore(X509_STORE* store, const std::vector<X509*>& certs, LPVOID logger_ctx)
{
    for (X509* cert : certs) {
        if (!cert) {
            continue;
        }
        if (X509_STORE_add_cert(store, cert) == 1) {
            continue;
        }

        const unsigned long err_code = ERR_peek_last_error();
#ifdef X509_R_CERT_ALREADY_IN_HASH_TABLE
        if (ERR_GET_LIB(err_code) == ERR_LIB_X509
            && ERR_GET_REASON(err_code) == X509_R_CERT_ALREADY_IN_HASH_TABLE) {
            ERR_clear_error();
            continue;
        }
#endif
        char err_buf[256] = {0};
        ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
        LogQ(logger_ctx, _ERROR_, "cert verify: failed to add trusted CA to store: %s", err_buf);
        ERR_clear_error();
        return -1;
    }
    return 0;
}

// Canonicalize a base64 SPKI pin for comparison: map url-safe and space-decoded
// variants back to standard base64 and strip padding/whitespace. Mirrors the
// Android-side normalizeBase64Pin so a url-safe pin from the share link
// (init-self-signed.sh: tr '+/' '-_') compares equal to the locally computed one.
std::string canonicalizeSpkiPin(const char* pin, size_t len)
{
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        char c = pin[i];
        switch (c) {
            case '-': out.push_back('+'); break;   // url-safe -> standard
            case '_': out.push_back('/'); break;   // url-safe -> standard
            case ' ': out.push_back('+'); break;   // '+' decoded as space in transit
            case '=':                              // drop padding
            case '\r':
            case '\n':
            case '\t': break;                       // drop whitespace
            default:  out.push_back(c); break;
        }
    }
    return out;
}

// SHA-256(SubjectPublicKeyInfo) of the leaf, standard base64 (no padding).
// Returns empty string on failure.
std::string computeSpkiPinBase64(X509* leaf)
{
    if (!leaf) {
        return std::string();
    }
    EVP_PKEY* pkey = X509_get_pubkey(leaf);
    if (!pkey) {
        return std::string();
    }
    // i2d_PUBKEY marshals the public key as a DER SubjectPublicKeyInfo — matches
    // the server's `openssl pkey -pubin -outform der` (init-self-signed.sh).
    unsigned char* spki_der = NULL;
    int spki_len = i2d_PUBKEY(pkey, &spki_der);
    EVP_PKEY_free(pkey);
    if (spki_len <= 0 || !spki_der) {
        return std::string();
    }
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(spki_der, (size_t)spki_len, hash);
    OPENSSL_free(spki_der);

    // EVP_EncodeBlock writes ceil(n/3)*4 + 1 bytes (incl. NUL) and returns the
    // number of bytes written excluding the NUL.
    unsigned char b64[((SHA256_DIGEST_LENGTH + 2) / 3) * 4 + 1];
    size_t b64_len = EVP_EncodeBlock(b64, hash, SHA256_DIGEST_LENGTH);
    if (b64_len == 0) {
        return std::string();
    }
    return canonicalizeSpkiPin((const char*)b64, b64_len);
}

} // namespace


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
    // LogQ(conn_->connector_->logger_ctx_, _DEBUG_, "stream_packet_acked: ack_delay_time:%" _U64BITARG_ " acked_bytes:%" _U64BITARG_ " inflight_bytes:%" _U64BITARG_, ack_delay_time, acked_bytes, inflight_bytes);
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

    fin = 0; // keep connection alive, or other stream will be closed
    h3_hdrs_.headers = headers.data();
    h3_hdrs_.count = headers.size();

    if (start_time_ == 0) {
        start_time_ = xqc_now();
    }
    /* send header */
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
    // RFC 9298 §3: Extended CONNECT with :protocol=connect-udp and the URI
    // template /.well-known/masque/udp/{target_host}/{target_port}/. The
    // capsule-protocol header advertises capsule support (we tolerate but do
    // not emit capsules for the base context-0 UDP flow).
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
    // fin=0: the request stream stays open for the lifetime of the tunnel.
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
    //     LogQ(conn_->connector_->logger_ctx_, _ERROR_, "xqc_stream_send error %zd\n", ret);
    //     return 0;
    // } 
    // else 
    // {
    //     //LogQ(conn_->connector_->logger_ctx_, _DEBUG_, "xqc_stream_send sent:%zd,  stream_id : %" PRIu64 "\n", ret, 
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
            LogQ(conn_->connector_->logger_ctx_, _INFO_, "[info] h3 request read header finish \n");
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
    //xqc_usec_t duration = end_time_ - start_time_;
    //duration = BCMAX(duration, 1);
    //LogQ(conn_->connector_->logger_ctx_, _INFO_, "total time : %" PRId64 "\n", duration);

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

SMPConnection::Config::Config() 
    : ipv6(false), host(NULL), port(0), server_host(NULL), server_port(0)
    , c_cong_ctl('b'), pacing_on(false), idle_time_out(0), ping_on(false)
    , linger_on(false), ping_interval(0), active_connection_id_limit(0)
    , alpn(NULL), device_type(0)
    , ca_cert_pem(NULL), ca_cert_pem_len(0)
    , proxy_type(TT_PROXY_NONE), proxy_host(NULL), proxy_port(0)
    , proxy_sni(NULL), proxy_url(NULL)
    , proxy_ca_cert_pem(NULL), proxy_ca_cert_pem_len(0)
    , spki_pin(NULL), spki_pin_len(0)
{
    RAND_bytes((uint8_t*)cid_tag, SMP_CID_TAG_LEN);
}

// Forward declaration: ParseProxyUrl is a file-local static defined later in
// this translation unit (alongside SMPConnector::Config::Init).
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
        if (strlen(cidTag) > SMP_CID_TAG_LEN) {
            return BC_R_INVALIDARG;
        }
        memcpy(cid_tag, cidTag, SMP_CID_TAG_LEN);
    }
    pVar = pConfig->Get("ca_cert_pem");
    if (IS_BCF_STRING(pVar))
    {
        ca_cert_pem = pool_.Strdup(GET_BCF_STRING(pVar));
        ca_cert_pem_len = strlen(ca_cert_pem);
    }
    // Outbound proxy (RFC 9298 CONNECT-UDP / MASQUE). Primary input is the
    // "proxy_url" key; explicit proxy_host / proxy_port / proxy_sni override
    // the parsed values for callers that prefer split fields.
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
    // Default outer TLS SNI to the proxy host when not explicitly supplied.
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

    if (!connector || !pHandler || !pConfig)
    {
        LogQ(connector->logger_ctx_, _ERROR_, "invalid arguments: invalid connector or handler or config");
        return BC_R_INVALIDARG;
    }
    config_.Init(pConfig);
    if (config_.alpn == NULL || config_.server_host == NULL) {
        LogQ(connector->logger_ctx_, _ERROR_, "invalid arguments: invalid alpn or server_host");
        return BC_R_INVALIDARG;
    }
    // Co-locate on caller-provided managers when given (MASQUE tunnel pins to
    // its inner connection's thread); otherwise pick random managers.
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
    result = udp_socket_->Create(connector->logger_ctx_, pTaskMgr, pTimerMgr, 
        Runtime::SocketMgr(), pConfig, this, false, false);
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

void SMPConnection::Restart(int64_t networkHandle)
{
    // Public API hit by:
    //   - SMPConnector::OnPathChange (internal auto-restart on OS path
    //     change; that entry point applies its own cooldown so a single
    //     Wi-Fi handover doesn't fan out into multiple restarts here)
    //   - tt_connection_restart from the JNI / Swift / NAPI bridge when
    //     the application has set disableAutoRestart=true and is driving
    //     migration itself
    //
    // We do NOT impose any cooldown here: if the caller asked for a
    // restart, they get a restart. Coalescing has to live at the source
    // of the noise (OnPathChange), otherwise a legitimate
    // application-driven restart (e.g. livekit-swift-sdk's
    // SignalClient.restartTransport on cellular handover) gets silently
    // dropped and the connection idle-timeouts.
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

    // MASQUE proxy gate: a fresh app-facing connection (role still NONE) on a
    // proxy-configured connector must first stand up the outer CONNECT-UDP
    // tunnel. Once the tunnel is ready, MasqueOnTunnelReady() re-enters this
    // function with masque_role_ == MASQUE_INNER to perform the real
    // jqc_connect over the tunnel. The tunnel connection itself has role
    // MASQUE_TUNNEL and falls straight through to a normal direct connect.
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
    // Enable the verify callback when EITHER a CA bundle (inner CA-chain path)
    // OR an SPKI pin (outer self-signed proxy hop) is configured. Without this,
    // a pin-only connection would skip the callback and connect unverified.
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
        // enable datagram RFC 9221
        conn_settings.max_datagram_frame_size = 65535;
        webtransport_ = true;
    }

    // MASQUE tunnel: the inner QUIC client must put a >=1200B padded Initial
    // (RFC 9000 anti-amplification; the real xquic server also rejects smaller
    // Initials) into a single outer HTTP/3 datagram, which must in turn fit in
    // one outer QUIC packet. The default 1200B outer MTU only yields a ~1184B
    // datagram MSS, too small for the 1200B inner Initial. Raise the outer MTU
    // to xquic's ceiling (XQC_QUIC_MAX_MSS = 1420) so the datagram MSS (~1404)
    // can carry the inner Initial. Safe on standard >=1420B paths.
    if (masque_role_ == MASQUE_TUNNEL)
    {
        conn_settings.max_pkt_out_size = 1420;
    }

    // Inner MASQUE connections have no real UDP path of their own; their
    // packets ride the tunnel as HTTP/3 datagrams. Skip the local socket
    // recv and shrink the inner MTU so a wrapped datagram fits the tunnel's
    // datagram MSS (and disable PMTUD, which can't probe a tunnelled path).
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
        // Synthesize a stable local address for xquic path bookkeeping; the
        // inner connection never migrates so any consistent pair works.
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

    // Create default user stream if not webtransport
    xqc_stream_t *stream = xqc_stream_create(connector_->engine_, &cid_, NULL, NULL);
    if (!stream) {
        LogQ(connector_->logger_ctx_, _ERROR_, "OnHandshakeFinished: xqc_stream_create FAILED");
        return;
    }
    LogQ(connector_->logger_ctx_, _INFO_, "OnHandshakeFinished: stream created, stream_map size:%zu, connect_rpc:%p",
         stream_map_.size(), connect_rpc_);
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
    /* Cancel all timers first (e.g. on network switch) so no callback runs with stale conn */
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

    // For the MASQUE tunnel, datagrams carry the inner connection's packets;
    // bind the h3 datagram callbacks' user_data to this tunnel so
    // on_h3_ext_datagram_read can route by quarter-stream-id.
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

    // MASQUE tunnel: the outer h3 handshake is up; open the CONNECT-UDP
    // request for the inner target instead of a WebTransport session. The
    // inner connection is notified once the proxy answers with a 2xx.
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
    //LogQ(connector_->logger_ctx_, _INFO_, "packet acked, ack_delay_time : %" PRIu64 ", acked_bytes : %" PRIu64 
    //    ", inflight_bytes : %" PRIu64 "\n", ack_delay_time, acked_bytes, inflight_bytes);
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

    //LogQ(connector_->logger_ctx_, _DEBUG"====>RETIRE SCID:%s\n", xqc_scid_str(retire_cid));
    //LogQ(connector_->logger_ctx_, _DEBUG_, "====>SCID:%s\n", xqc_scid_str(new_cid));
    //LogQ(connector_->logger_ctx_, _DEBUG_, "====>DCID:%s\n", xqc_dcid_str_by_scid(
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

    // MUST NOT do close check here, because it will stop the connection from sending close frame
    if (state_ <= CONN_STATE_CLOSING_SOCK)
    {
        return -1;
    }
    // Inner MASQUE connection: ferry the QUIC packet to the proxy as an HTTP/3
    // datagram via the tunnel instead of writing a real socket. peer_addr is
    // ignored here (the proxy already knows the CONNECT-UDP target).
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
    // MASQUE tunnel: the CONNECT-UDP response carries the tunnel verdict in
    // :status. 2xx means the UDP proxying flow is established; anything else
    // (or missing headers) is a failure. We do not surface anything to an app
    // handler here — the inner connection is the observer.
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

///////////////////////////////////////////////////////////////////////////////
// MASQUE CONNECT-UDP helpers (SMPConnection)
//
// Threading: each method documents whether it runs on the INNER or TUNNEL
// connection's event queue. Cross-connection hand-offs always go through the
// peer's PostTask so we never re-enter the shared xquic engine synchronously.
///////////////////////////////////////////////////////////////////////////////

BCRESULT SMPConnection::MasqueStartTunnel()
{
    // INNER queue. Create the outer tunnel and kick its connect to the proxy.
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
    // TUNNEL role. Drive the outer h3 connect on our own queue.
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
    // TUNNEL role. Max inner UDP payload = outer datagram MSS minus our
    // RFC 9297/9298 header (quarter-stream-id + context-id varints).
    if (masque_role_ != MASQUE_TUNNEL || !h3_conn_)
    {
        return 0;
    }
    size_t mss = xqc_h3_ext_datagram_get_mss(h3_conn_);
    size_t overhead = masque::DatagramOverhead(masque_qsid_);
    // Safety margin: get_mss reflects the outer datagram capacity at query time,
    // but the real per-packet headroom shrinks afterwards (longer packet numbers,
    // coalesced ACK frames). Sizing the inner MTU at exactly (mss - overhead)
    // leaves zero slack, so steady-state full 1RTT packets overflow the outer
    // datagram and xqc_h3_ext_datagram_send returns -681 (XQC_EDGRAM_TOO_LARGE)
    // on every packet. Reserve a margin so wrapped inner packets always fit. With
    // the standard 1420B outer MTU the inner still stays >=1200B for the padded
    // Initial.
    const size_t kMasqueMssSafetyMargin = 64;
    if (mss <= overhead + kMasqueMssSafetyMargin)
    {
        return 0;
    }
    return mss - overhead - kMasqueMssSafetyMargin;
}

BCRESULT SMPConnection::MasqueSendConnectUdp()
{
    // TUNNEL role. Open the CONNECT-UDP request and remember its quarter
    // stream id for datagram framing.
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
    // TUNNEL role. Tell the inner connection (on its queue) the verdict.
    if (masque_tunnel_ready_ && ok)
    {
        return; // ignore duplicate header notifications
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
    // INNER queue. Tunnel is up (or failed); perform/abandon the real connect.
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
    // TUNNEL role, but invoked synchronously from the INNER connection's
    // WritePacket. Encode now, then defer the actual h3 datagram send onto our
    // own queue to avoid re-entering the shared engine. We report the bytes as
    // sent; loss is recovered by the inner QUIC connection itself.
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
    // TUNNEL queue. Decode RFC 9297/9298 framing and route the inner packet.
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
    // INNER queue. Feed the tunnelled packet into the inner QUIC engine.
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
                /*path id*/XQC_UNKNOWN_PATH_ID, (xqc_msec_t)recv_time, this, NULL, NULL, 0);
        } else {
            ret = jqc_conn_packet_process(conn_, packet_buf, recv_size,
                (struct sockaddr*)(&this->local_addr_), this->local_addrlen_,
                (struct sockaddr*)(&refSrcAddr.type.sa), peer_addrlen,
                /*path id*/XQC_UNKNOWN_PATH_ID, (xqc_msec_t)recv_time, this, NULL, NULL, 0);
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
            // Note: we deliberately do NOT schedule a "if no data in 3s,
            // re-Restart" verify timer here. It used to live in this spot
            // and was the source of a real bug — one upper-layer Restart()
            // call would fan out into up to 3 OnRestart callbacks (initial
            // + 2 retries), and each retry was a destructive
            // udp_socket_->Restart() that rebuilt the socket and forced a
            // brand-new PATH_CHALLENGE, tearing down a still-validating
            // migration. xqc handles PATH_CHALLENGE retransmission itself
            // (PING + idle timeout will surface a genuinely dead path),
            // and any upper-layer monitor (Java NetworkCallback, iOS/macOS
            // NWPathMonitor) will drive a fresh Restart on the next real
            // network event. If we ever do need an actual re-send of
            // PATH_CHALLENGE without rebuilding the socket, that belongs
            // in a separate jqc API, not here.

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
        // Internal tunnel: invisible to the app and absent from conns_map_.
        // Hand ourselves back to the owning inner connection for deletion. We
        // post onto the inner's queue (same task manager, but a different call
        // stack than this shutdown callback) so the inner deletes us only
        // after this shutdown has fully unwound.
        SMPConnection* inner = masque_inner_;
        masque_inner_ = NULL;
        if (inner)
        {
            inner->PostTask([inner] { inner->MasqueOnTunnelClosed(); });
        }
        return;
    }
    // INNER reaches here only after its owned tunnel has been fully torn down
    // by the _CloseCheck tunnel-wait stage (masque_tunnel_ == NULL), so no
    // tunnel task can reference us after the app deletes us.
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
        // MUST NOT do close check here, because it will stop the connection from closing
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
        // MUST NOT do close check here, because it will stop the connection from working
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
        // MUST NOT do close check here, because it will stop the connection from closing
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
        // Stop receive external event
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
        // MASQUE inner: ensure the owned tunnel is fully torn down before we
        // self-destruct. We request its close once and then wait; the tunnel's
        // shutdown posts MasqueOnTunnelClosed back to us, which deletes it and
        // re-enters _CloseCheck to finish our own teardown.
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
    // INNER queue. The tunnel's event task has fully shut down by now, so it is
    // safe to delete here (a different call stack than the tunnel's shutdown).
    SMPConnection* tunnel = masque_tunnel_;
    masque_tunnel_ = NULL;
    masque_tunnel_closing_ = false;
    if (tunnel)
    {
        delete tunnel;
    }
    if (state_ == CONN_STATE_INIT)
    {
        // The tunnel failed before the inner connect could start (still parked
        // at INIT waiting for the tunnel); fail the connect immediately rather
        // than waiting for the connect timeout.
        LogQ(connector_->logger_ctx_, _WARN_, "[masque] tunnel closed before inner connect; failing connect");
        _NotifyConnectResult(NULL, BC_R_NETDOWN, "masque: tunnel closed before connect", 0);
        _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    }
    else if (state_ > CONN_STATE_INIT)
    {
        // The tunnel went away while we were live (e.g. the proxy dropped the
        // CONNECT-UDP flow); we have no transport left, so tear down.
        LogQ(connector_->logger_ctx_, _WARN_, "[masque] tunnel closed; tearing down inner connection");
        _set_state(this, CONN_STATE_FREED, BC_R_NETDOWN);
    }
    // state_ < CONN_STATE_INIT: we are already tearing down; just continue.
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

// Parse a proxy URL of the form "<scheme>://<host>[:<port>]" into its
// components. Recognised schemes map to TT_PROXY_MASQUE (the only proxy type
// implemented today): masque, connect-udp, https, h3. Returns false on a
// syntactically invalid URL. host_out / port_out are only written on success.
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
        // No scheme: assume MASQUE and treat the whole value as authority.
        authority = s;
    }
    // Strip any path/query that follows the authority.
    size_t slash = authority.find('/');
    if (slash != std::string::npos) {
        authority = authority.substr(0, slash);
    }
    if (authority.empty()) {
        return false;
    }

    // Scheme -> proxy type. Empty scheme defaults to MASQUE.
    for (char& c : scheme) { c = (char)tolower((unsigned char)c); }
    if (scheme.empty() || scheme == "masque" || scheme == "connect-udp"
        || scheme == "https" || scheme == "h3") {
        *type_out = TT_PROXY_MASQUE;
    } else {
        return false;
    }

    // Split host[:port], honouring [IPv6]:port bracket form.
    std::string host;
    uint16_t port = 443;
    if (authority[0] == '[') {
        size_t close = authority.find(']');
        if (close == std::string::npos) {
            return false;
        }
        host = authority.substr(0, close + 1); // keep brackets for the host
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
    pVar = pConfig->Get("bypassVpn");
    if (IS_BCF_BOOL(pVar))
    {
        bypassVpn = GET_BCF_BOOL(pVar);
    }
    // Outbound proxy (RFC 9298 CONNECT-UDP / MASQUE). Primary input is the
    // "proxy_url" key; explicit proxy_host / proxy_port / proxy_sni override
    // the parsed values for callers that prefer split fields.
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
    // Default outer TLS SNI to the proxy host when not explicitly supplied.
    if (proxy_type != TT_PROXY_NONE && (!proxy_sni || !*proxy_sni) && proxy_host)
    {
        proxy_sni = pool_.Strdup(proxy_host);
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
#if defined(TT_HAS_PATH_MONITOR)
    if (path_monitor_)
    {
        // tt_netmon_stop guarantees the callback won't fire again, so it's
        // safe to call here even though the SMPConnector is half-destroyed.
        tt_netmon_stop(path_monitor_);
        path_monitor_ = nullptr;
    }
#endif
    freeCertVector(root_cas_);
}

#if defined(TT_HAS_PATH_MONITOR)
// Diagnostic-only: forwards every raw NWPathMonitor / netlink / Windows
// IP-change wakeup to LogQ before any debounce / dedup / VPN-filter is
// applied. Lets us audit "OS reported N raw path events, SDK reacted to
// M of them" from a single log file. Hot-but-not-pathological path: at
// most a few events per second during a Wi-Fi handover.
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

    // The first callback after tt_netmon_start fires ~immediately and is
    // really just the monitor reporting the current default route, not a
    // real change. If the app raced with us and already created/connected
    // a connection between Create() and this first callback, restarting
    // it here would bounce its UDP socket mid-handshake for no reason.
    // Swallow the first invocation and only treat subsequent ones as true
    // migration events. exchange returns the prior value, so we restart
    // iff `first_path_callback_seen_` was already true.
    bool was_first = !self->first_path_callback_seen_.exchange(
        true, std::memory_order_acq_rel);
    if (was_first)
    {
        LogQ(self->logger_ctx_, _INFO_,
             "[SMPConnector] initial path ifIndex=%lld desc=\"%s\" (no restart)",
             (long long)newIfIndex, pathDesc ? pathDesc : "");
        return;
    }

    // Path-monitor side cooldown: a single user-visible Wi-Fi handover
    // often reaches us as TWO satisfied callbacks several seconds apart
    // (satisfied-old → unsatisfied → satisfied-new), each debounced
    // independently. Letting both restarts through tears down a QUIC
    // path the server may have just finished validating on the first
    // restart's new socket, and on TUN-mode-VPN hosts (Clash / Surge)
    // the peer's fake-IP NAT table never recovers from the second
    // migration. 3000 ms safely covers a normal handover while still
    // letting a legitimately distinct event (Wi-Fi → cellular → Wi-Fi)
    // come through on the next decision boundary.
    //
    // IMPORTANT: this cooldown is scoped to the auto-restart entry
    // point. SMPConnection::Restart itself never coalesces, so apps
    // that set disableAutoRestart=true (and therefore never reach
    // OnPathChange) keep full control over their restart cadence.
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

    // Snapshot connection list under the lock, then defer the actual
    // SMPConnection::Restart calls onto the runtime thread so the monitor
    // thread (NWPathMonitor queue / netlink reader / Windows worker) is
    // never blocked on QUIC bookkeeping.
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
#endif // TT_HAS_PATH_MONITOR

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
    /* fixed 16-byte CID */
    config.cid_len = SMP_CID_LEN;
    /* Enable HTTP/3 extensions so inbound RFC 9297 datagrams are delivered to the
     * h3 datagram callback. Required for the MASQUE CONNECT-UDP tunnel to receive
     * the proxy's relayed packets. Only affects h3 connections; the raw "ttsignal"
     * signaling transport is unaffected. */
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
    for (const auto &alpn : config_.alpn) {
        ret = xqc_engine_register_alpn(engine_, alpn.c_str(), alpn.size(), &raw_cbs);
        if (ret != XQC_OK) {
            return BC_R_UNEXPECTED;
        }
    }

    state_ = CONNTOR_STATE_WORKING;

#if defined(TT_HAS_PATH_MONITOR)
    // Auto-restart on OS-reported active interface changes. Off-switch lives
    // in config_.disableAutoRestart so server deployments (livekit-ai et al.)
    // can opt out. tt_netmon_start fires its first callback ~immediately
    // with the current default-route ifIndex; OnPathChange swallows that
    // first invocation (see first_path_callback_seen_ in the header) so
    // even if the app calls CreateConnection + Connect before the initial
    // callback lands, no spurious mid-handshake Restart goes out.
    if (!config_.disableAutoRestart)
    {
        TTNetworkMonitorOptions netmon_opts{};
        netmon_opts.bypassVpn = config_.bypassVpn ? 1 : 0;
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
                 "[SMPConnector] tt_netmon_start ok bypassVpn=%d",
                 netmon_opts.bypassVpn);
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

    // Minimal outer-connection config: HTTP/3 ALPN to the proxy, TLS SNI =
    // proxy_sni (defaults to proxy_host), single UDP sender, inheriting the
    // connector's IP family / CC / log level. Proxy endpoint and outer-hop TLS
    // trust come from the inner connection's per-connection proxy config.
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
    // Outer-hop (proxy) TLS trust is separate from the inner SFU CA. Use the
    // per-connection proxy CA bundle if provided; otherwise leave the tunnel with
    // NO CA bundle (xquic then accepts the proxy cert — acceptable for a self-signed
    // Mode-B proxy on a trusted path; supply proxy_ca_cert_pem to enforce verification).
    if (inner->config_.proxy_ca_cert_pem && inner->config_.proxy_ca_cert_pem_len > 0) {
        cfg.PutString("ca_cert_pem", inner->config_.proxy_ca_cert_pem);
    }
    // Forward the inner's SPKI pin to the TUNNEL connection so the OUTER hop to
    // the proxy is pinned. The tunnel has no proxy_host (it IS the proxy hop), so
    // the pin gate (spki_pin set AND proxy_host empty) fires only on the tunnel,
    // never on the inner (which keeps verifying the SFU via ca_cert_pem).
    if (inner->config_.spki_pin && inner->config_.spki_pin_len > 0) {
        cfg.PutString("spki_pin", inner->config_.spki_pin);
    }

    SMPConnection* tunnel = new SMPConnection();
    if (!tunnel)
    {
        return NULL;
    }
    BCSpinMutex::Owner lock(lock_);
    // Pin the tunnel to the inner connection's event managers so the pair is
    // serialised on a single thread (all masque_* hand-offs then run without
    // cross-thread races, and the tunnel can be deleted safely by the inner).
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
    // The CONNECT-UDP target is the inner connection's real server; copy it so
    // the tunnel can build the request path (the inner stashed it in its gate).
    tunnel->masque_target_host_ = inner->masque_target_host_;
    tunnel->masque_target_port_ = inner->masque_target_port_;
    // Intentionally NOT inserted into conns_map_ and NOT counted in stats: the
    // tunnel is internal and invisible to the application (no app-facing close
    // callback). Its lifetime is fully owned by the inner connection.
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

    // SPKI-pin path (outer self-hosted-proxy hop, see design §9.6): verify the
    // leaf's public-key fingerprint instead of CA-chain. Fully isolated from the
    // inner CA-chain path below — a connection carries EITHER spki_pin OR
    // ca_cert_pem, never both, so the inner chative-CA verification is untouched.
    if (user_conn->config_.spki_pin && user_conn->config_.spki_pin_len > 0
        && !(user_conn->config_.proxy_host && user_conn->config_.proxy_host[0] != '\0')) {
        const unsigned char *p = certs[0];
        X509 *leaf = d2i_X509(nullptr, &p, cert_len[0]);
        if (!leaf) {
            user_conn->cert_verify_error_ = "spki pin: failed to parse leaf cert";
            LogQ(connector->logger_ctx_, _ERROR_, "spki pin: failed to parse leaf cert");
            return -1;
        }
        std::string computed = computeSpkiPinBase64(leaf);
        X509_free(leaf);
        std::string expected = canonicalizeSpkiPin(
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

    X509_STORE *store = X509_STORE_new();
    if (!store) {
        return -1;
    }
    if (addTrustedCAsToStore(store, *trusted_cas, connector->logger_ctx_) != 0) {
        X509_STORE_free(store);
        return -1;
    }

    STACK_OF(X509) *chain = sk_X509_new_null();
    X509 *leaf = nullptr;
    for (size_t i = 0; i < certs_len; i++) {
        const unsigned char *p = certs[i];
        X509 *cert = d2i_X509(nullptr, &p, cert_len[i]);
        if (!cert) continue;
        if (i == 0) leaf = cert;
        else sk_X509_push(chain, cert);
    }

    int result = -1;
    if (leaf) {
        X509_STORE_CTX *ctx = X509_STORE_CTX_new();
        if (ctx && X509_STORE_CTX_init(ctx, store, leaf, chain) == 1) {
            if (X509_verify_cert(ctx) == 1) {
                const char *host = user_conn->config_.server_host;
                if (!host || strlen(host) == 0) {
                    host = user_conn->host_.length() > 0
                         ? user_conn->host_.c_str()
                         : connector->config_.server_host;
                }
                if (host && strlen(host) > 0
                    && X509_check_host(leaf, host,
                        strlen(host), 0, nullptr) == 1) {
                    result = 0;
                } else {
                    std::string msg = std::string("certificate hostname mismatch, expected=")
                                    + (host ? host : "(null)");
                    user_conn->cert_verify_error_ = msg;
                    LogQ(connector->logger_ctx_, _ERROR_,
                        "cert verify: %s", msg.c_str());
                }
            } else {
                int err = X509_STORE_CTX_get_error(ctx);
                std::string msg = std::string("certificate chain validation failed: ")
                                + X509_verify_cert_error_string(err);
                user_conn->cert_verify_error_ = msg;
                LogQ(connector->logger_ctx_, _ERROR_,
                    "cert verify: %s", msg.c_str());
            }
        }
        if (result != 0) {
            char subj_buf[256] = {0};
            char issuer_buf[256] = {0};
            X509_NAME_oneline(X509_get_subject_name(leaf), subj_buf, sizeof(subj_buf));
            X509_NAME_oneline(X509_get_issuer_name(leaf), issuer_buf, sizeof(issuer_buf));
            LogQ(connector->logger_ctx_, _ERROR_,
                "cert verify: server leaf cert subject=%s, issuer=%s", subj_buf, issuer_buf);
            for (int i = 0; i < sk_X509_num(chain); i++) {
                X509 *ic = sk_X509_value(chain, i);
                char ic_subj[256] = {0};
                char ic_issuer[256] = {0};
                X509_NAME_oneline(X509_get_subject_name(ic), ic_subj, sizeof(ic_subj));
                X509_NAME_oneline(X509_get_issuer_name(ic), ic_issuer, sizeof(ic_issuer));
                LogQ(connector->logger_ctx_, _ERROR_,
                    "cert verify: chain[%d] subject=%s, issuer=%s", i, ic_subj, ic_issuer);
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
        if (ctx) X509_STORE_CTX_free(ctx);
        X509_free(leaf);
    }

    sk_X509_pop_free(chain, X509_free);
    X509_STORE_free(store);
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
    //DEBUG_PRINT;
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

void
SMPConnector::on_h3_ext_datagram_read(
    xqc_h3_conn_t *conn,
    const void *data,
    size_t data_len,
    void *user_data,
    uint64_t data_recv_time)
{
    // user_data is the MASQUE tunnel SMPConnection (set in OnH3ConnCreate via
    // xqc_h3_ext_datagram_set_user_data). Route the raw HTTP/3 datagram into
    // the tunnel for RFC 9297/9298 decode + delivery to its inner connection.
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
    SMPConnection *conn = (SMPConnection*)user_data; //user_data may be empty when "reset" is sent
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

///////////////////////////////////////////////////////////////////////////////
// End of file : SMPConnector.cpp
///////////////////////////////////////////////////////////////////////////////
