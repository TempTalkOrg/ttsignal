








#include "TlsContext.h"
#include "TTErrors.h"

#include <cctype>
#include <cstring>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509v3.h>
#include <openssl/x509_vfy.h>




#ifdef _WIN32
#include <ws2tcpip.h>
#endif

namespace {

void FreeCertVector(std::vector<X509*>& certs)
{
    for (X509* cert : certs) {
        if (cert) {
            X509_free(cert);
        }
    }
    certs.clear();
}



bool IsPemBundleEof(unsigned long err_code)
{
    if (err_code == 0) {
        return true;
    }
    return ERR_GET_LIB(err_code) == ERR_LIB_PEM
        && ERR_GET_REASON(err_code) == PEM_R_NO_START_LINE;
}

bool IsIpLiteral(const std::string& host)
{
    unsigned char buf[16]; 
    if (host.empty()) {
        return false;
    }
    if (inet_pton(AF_INET, host.c_str(), buf) == 1) {
        return true;
    }
    if (inet_pton(AF_INET6, host.c_str(), buf) == 1) {
        return true;
    }
    return false;
}

} 

int TlsContext::ParseCAsFromPem(
    const std::string& pem,
    std::vector<X509*>& out,
    std::string& outErr)
{
    FreeCertVector(out);
    outErr.clear();
    if (pem.empty()) {
        return 0;
    }

    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) {
        outErr = "failed to allocate BIO for caCertsPem";
        return -1;
    }

    while (true) {
        X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (cert) {
            out.push_back(cert);
            continue;
        }

        const unsigned long err_code = ERR_peek_last_error();
        if (IsPemBundleEof(err_code)) {
            ERR_clear_error();
            break;
        }

        char err_buf[256] = {0};
        ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
        outErr = std::string("failed to parse caCertsPem bundle: ") + err_buf;
        ERR_clear_error();
        BIO_free(bio);
        FreeCertVector(out);
        return -1;
    }

    BIO_free(bio);
    if (out.empty()) {
        outErr = "caCertsPem did not contain a valid certificate";
        return -1;
    }
    return 0;
}

int TlsContext::AddTrustedCAsToStore(
    X509_STORE* store,
    const std::vector<X509*>& certs,
    void* )
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
        ERR_clear_error();
        return -1;
    }
    return 0;
}

std::string TlsContext::CanonicalizeSpkiPin(const char* pin, size_t len)
{
    if (!pin) {
        return std::string();
    }

    
    
    
    
    
    
    
    
    
    
    

    
    
    size_t begin = 0;
    size_t end = len;
    while (begin < end && isspace((unsigned char)pin[begin])) {
        ++begin;
    }
    while (end > begin && isspace((unsigned char)pin[end - 1])) {
        --end;
    }

    
    static const char kPrefix[] = "sha256//";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (end - begin >= kPrefixLen
        && strncmp(pin + begin, kPrefix, kPrefixLen) == 0) {
        begin += kPrefixLen;
    }

    
    
    
    
    
    
    
    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        char c = pin[i];
        switch (c) {
            case '-': out.push_back('+'); break;   
            case '_': out.push_back('/'); break;   
            case ' ': out.push_back('+'); break;   
            case '=':                              
            case '\r':
            case '\n':
            case '\t': break;                       
            default:  out.push_back(c); break;
        }
    }
    return out;
}

std::string TlsContext::ComputeSpkiPinBase64(X509* leaf)
{
    if (!leaf) {
        return std::string();
    }
    EVP_PKEY* pkey = X509_get_pubkey(leaf);
    if (!pkey) {
        return std::string();
    }
    
    
    unsigned char* spki_der = nullptr;
    int spki_len = i2d_PUBKEY(pkey, &spki_der);
    EVP_PKEY_free(pkey);
    if (spki_len <= 0 || !spki_der) {
        return std::string();
    }
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(spki_der, (size_t)spki_len, hash);
    OPENSSL_free(spki_der);

    unsigned char b64[((SHA256_DIGEST_LENGTH + 2) / 3) * 4 + 1];
    size_t b64_len = EVP_EncodeBlock(b64, hash, SHA256_DIGEST_LENGTH);
    if (b64_len == 0) {
        return std::string();
    }
    return TlsContext::CanonicalizeSpkiPin((const char*)b64, b64_len);
}

SSL_CTX* TlsContext::Create(const TlsConfig& cfg, std::string& outErr)
{
    outErr.clear();

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        outErr = "SSL_CTX_new failed";
        return nullptr;
    }

    SSL_CTX_set_options(
        ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);
    
    
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    bool haveCa = false;
    if (!cfg.caCertsPem.empty()) {
        std::vector<X509*> cas;
        if (TlsContext::ParseCAsFromPem(cfg.caCertsPem, cas, outErr) != 0) {
            SSL_CTX_free(ctx);
            return nullptr;   
        }
        X509_STORE* store = SSL_CTX_get_cert_store(ctx);
        haveCa = (TlsContext::AddTrustedCAsToStore(store, cas, cfg.loggerCtx) == 0
                  && !cas.empty());
        FreeCertVector(cas);
    } else {
        haveCa = (SSL_CTX_set_default_verify_paths(ctx) == 1);
#ifdef OS_ANDROID
        if (SSL_CTX_load_verify_locations(
                ctx, nullptr, "/system/etc/security/cacerts") == 1) {
            haveCa = true;
        }
#endif
    }

    
    
    
    
    
    
    
    
    
    
    
    
    
    (void)haveCa;

    
    
    
    
    
    
    
    if (!cfg.clientCertFile.empty() && !cfg.clientKeyFile.empty()) {
        
        
        
        if (!cfg.clientKeyPassword.empty()) {
            SSL_CTX_set_default_passwd_cb_userdata(
                ctx, const_cast<char*>(cfg.clientKeyPassword.c_str()));
        }
        if (SSL_CTX_use_certificate_file(
                ctx, cfg.clientCertFile.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(
                ctx, cfg.clientKeyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
            outErr = "加载客户端证书/私钥失败: " + cfg.clientCertFile;
            SSL_CTX_free(ctx);
            return nullptr;
        }
    }

    SSL_CTX_set_verify(
        ctx, cfg.insecureSkipVerify ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, nullptr);

    return ctx;
}

BCRESULT TlsContext::PrepareSsl(
    SSL* ssl,
    const std::string& host,
    const TlsConfig& cfg,
    std::string& outErr)
{
    outErr.clear();
    if (!ssl) {
        outErr = "PrepareSsl: ssl is null";
        return BC_R_INVALIDARG;
    }

    const bool is_ip_literal = IsIpLiteral(host);

    
    if (!host.empty() && !is_ip_literal) {
        SSL_set_tlsext_host_name(ssl, host.c_str());
    }

    
    
    
    
    
    
    if (!cfg.insecureSkipVerify && !host.empty()) {
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
        if (param) {
            X509_VERIFY_PARAM_set1_host(param, host.c_str(), host.size());
        }
    }

    return BC_R_SUCCESS;
}

BCRESULT TlsContext::VerifyChain(
    const std::vector<X509*>& chain,
    const std::string& host,
    const TlsConfig& cfg,
    std::string& outErr)
{
    outErr.clear();
    if (chain.empty() || !chain[0]) {
        outErr = "cert verify: empty certificate chain";
        return BC_R_TLS_VERIFY_FAILED;
    }
    X509* leaf = chain[0];

    
    
    
    if (!cfg.spkiPin.empty()) {
        std::string computed = TlsContext::ComputeSpkiPinBase64(leaf);
        std::string expected = TlsContext::CanonicalizeSpkiPin(
            cfg.spkiPin.c_str(), cfg.spkiPin.size());
        if (!computed.empty() && computed == expected) {
            return BC_R_SUCCESS;
        }
        outErr = "spki pin mismatch";
        return BC_R_TLS_VERIFY_FAILED;
    }

    X509_STORE* store = X509_STORE_new();
    if (!store) {
        outErr = "cert verify: X509_STORE_new failed";
        return BC_R_TLS_VERIFY_FAILED;
    }

    std::vector<X509*> parsedCas;
    if (!cfg.caCertsPem.empty()) {
        std::string parseErr;
        if (TlsContext::ParseCAsFromPem(cfg.caCertsPem, parsedCas, parseErr) != 0) {
            outErr = "cert verify: failed to parse caCertsPem: " + parseErr;
            X509_STORE_free(store);
            return BC_R_TLS_VERIFY_FAILED;
        }
        if (TlsContext::AddTrustedCAsToStore(store, parsedCas, cfg.loggerCtx) != 0) {
            outErr = "cert verify: failed to add trusted CA to store";
            FreeCertVector(parsedCas);
            X509_STORE_free(store);
            return BC_R_TLS_VERIFY_FAILED;
        }
    } else {
        
        
        X509_STORE_set_default_paths(store);
    }

    STACK_OF(X509)* intermediates = sk_X509_new_null();
    for (size_t i = 1; i < chain.size(); ++i) {
        if (chain[i]) {
            sk_X509_push(intermediates, chain[i]);
        }
    }

    BCRESULT result = BC_R_TLS_VERIFY_FAILED;
    X509_STORE_CTX* store_ctx = X509_STORE_CTX_new();
    if (store_ctx && X509_STORE_CTX_init(store_ctx, store, leaf, intermediates) == 1) {
        if (X509_verify_cert(store_ctx) == 1) {
            const bool host_ok = !host.empty()
                && X509_check_host(leaf, host.c_str(), host.size(), 0, nullptr) == 1;
            if (host_ok) {
                result = BC_R_SUCCESS;
            } else {
                outErr = std::string("certificate hostname mismatch, expected=")
                        + (host.empty() ? "(null)" : host.c_str());
            }
        } else {
            int err_code = X509_STORE_CTX_get_error(store_ctx);
            outErr = std::string("certificate chain validation failed: ")
                    + X509_verify_cert_error_string(err_code);
        }
    } else {
        outErr = "cert verify: X509_STORE_CTX_init failed";
    }

    if (store_ctx) {
        X509_STORE_CTX_free(store_ctx);
    }
    
    
    
    
    
    
    
    
    sk_X509_free(intermediates);
    FreeCertVector(parsedCas);
    X509_STORE_free(store);
    return result;
}




