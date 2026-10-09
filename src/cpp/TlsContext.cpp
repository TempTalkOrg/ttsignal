///////////////////////////////////////////////////////////////////////////////
// file   : TlsContext.cpp
// author : anto
//
// 见 TlsContext.h 顶部关于依赖边界的说明：本文件刻意不 #include "Utils.h"、
// 不调用 BC 的 LogQ 日志族（会拖进 xquic 头文件 + 需要链接 libenv，与
// "不依赖 xquic 的独立模块" 的设计目标冲突）。所有失败详情通过 outErr 参数
// 返回，调用方自行决定怎么落地日志。
///////////////////////////////////////////////////////////////////////////////

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

// BC/Config.h（经由 TlsContext.h 引入）已经按平台 #include 了
// <arpa/inet.h> / <winsock2.h>+<ws2tcpip.h>，这里直接用 inet_pton。

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

// 区分"PEM bundle 正常读完"（PEM_R_NO_START_LINE）与"内容确实解析失败"，
// 逻辑与旧版 SMPConnector.cpp 里的 isPemBundleEof 完全一致。
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
    unsigned char buf[16]; // sizeof(struct in6_addr)
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

} // namespace

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
    void* /*loggerCtx*/)
{
    // loggerCtx 暂时未使用——保留在签名里只是为了跟调用方（SMPConnector
    // 等）对齐；本模块不接 BC 的日志子系统，理由见文件头注释。
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

    // ⚠️ 相对迁移前（SMPConnector::canonicalizeSpkiPin，ad202d5）的行为
    // 变更，直接影响 SMP 已上线的 spki_pin 比对：新增了下面 1)、2) 两步
    // （去首尾空白、剥 "sha256//" 前缀），迁移前没有这两步。
    // 影响面：迁移前如果配置的 spki_pin 带首尾空白或 "sha256//" 前缀，会
    // 因为规范化后仍带着这些字符而跟计算出来的指纹比对失败（安全上是"误
    // 拒"，不是"误受"）；现在这些写法会被规范化掉，从而匹配成功。计算侧
    // （SHA-256(SPKI) 本身）没有变松——要伪造匹配仍然需要哈希碰撞，这两步
    // 只是放宽了对配置字符串本身写法的容忍度。这个变更是刻意保留的（详见
    // TlsContext_test.cpp 的 test_spki_pin_canonicalization），因为给
    // SMP 和 TCP 分叉两套规范化正是抽 TlsContext 想消除的漂移；但既然是
    // 已上线路径的行为变化，这里必须留痕，不能只写在 commit message 里。

    // 1) 去首尾空白：pin 常见来源是配置文件 / 命令行 / HTTP header，容易
    //    带换行或空格。
    size_t begin = 0;
    size_t end = len;
    while (begin < end && isspace((unsigned char)pin[begin])) {
        ++begin;
    }
    while (end > begin && isspace((unsigned char)pin[end - 1])) {
        --end;
    }

    // 2) 剥掉 "sha256//" 前缀——curl --pinnedpubkey / HPKP 的常见写法。
    static const char kPrefix[] = "sha256//";
    static const size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (end - begin >= kPrefixLen
        && strncmp(pin + begin, kPrefix, kPrefixLen) == 0) {
        begin += kPrefixLen;
    }

    // 3) 逐字符规范化：与旧版 SMPConnector::canonicalizeSpkiPin（迁移前，
    //    见 git show ad202d5:src/cpp/SMPConnector.cpp:162-180）的 switch
    //    逐字对齐——url-safe base64 -> 标准 base64（兼容 Android 侧 share
    //    link 的 tr '+/' '-_'），'+' 在传输中偶尔被解码成空格的情况也一并
    //    纠正，'=' 当 padding 统一丢弃。只有这一套规范形式：computed 和
    //    expected 都过这个函数，天然可比，不需要调用方另外再 trim 一次
    //    padding。
    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
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

std::string TlsContext::ComputeSpkiPinBase64(X509* leaf)
{
    if (!leaf) {
        return std::string();
    }
    EVP_PKEY* pkey = X509_get_pubkey(leaf);
    if (!pkey) {
        return std::string();
    }
    // i2d_PUBKEY marshals the public key as a DER SubjectPublicKeyInfo —
    // 与服务器端 `openssl pkey -pubin -outform der` 生成的一致。
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
    // 最低协议版本钉在 TLS 1.2。原 WSConnector.cpp:556 设的是
    // TLS1_1_VERSION，是待修的历史遗留 bug——这里直接用正确值，不重犯。
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    bool haveCa = false;
    if (!cfg.caCertsPem.empty()) {
        std::vector<X509*> cas;
        if (TlsContext::ParseCAsFromPem(cfg.caCertsPem, cas, outErr) != 0) {
            SSL_CTX_free(ctx);
            return nullptr;   // 畸形 PEM 必须响亮失败，不能静默降级
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

    // 拿不到 CA 时不让 Create() 失败：
    //
    // "绝不静默地不校验"这个保证来自下面的 SSL_VERIFY_PEER —— 空 store 下
    // 握手必然以 "unable to get local issuer certificate" 失败，不存在静默
    // 放行。所以这里再加一道硬失败守卫是多余的，而且有假阴性风险：
    // BoringSSL 的 X509_STORE 在部分平台是惰性加载，判"空"会把本来能连的
    // https 全部拒掉。
    //
    // 本模块不接 BC 的日志子系统（见文件头注释），所以这里没有 WARN 落地；
    // 需要现场排查时握手失败本身的错误信息已经足够定位。
    //
    // 实测（macOS 15 / BoringSSL）：set_default_verify_paths 返回 1，store
    // 里实际加载了 128 个 CA，来源 /etc/ssl/cert.pem。
    (void)haveCa;

    // 客户端证书（双向 TLS，可选）。
    //
    // 顺手修掉两个从 WSConnector.cpp 带过来、原本要在 Task 7 挪回来的 bug：
    // 1) WSConnector.cpp:558-561 的条件把 certificate_file 判了两次，
    //    第二次本意是 private_key_file；private_key_file 为 NULL 时
    //    strlen() 会解引用空指针崩溃。这里换成分别判断各自的
    //    std::string 是否为空，天然不会有这个问题。
    if (!cfg.clientCertFile.empty() && !cfg.clientKeyFile.empty()) {
        // 2) WSConnector.cpp:562 写的是
        //    `if (!config_.private_key_password)`——判反了，变成只在密码
        //    为空时才设密码回调。这里改为非空时才设。
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

    // IP 字面量不设 SNI —— RFC 6066 明确禁止 SNI 携带 IP 字面量。
    if (!host.empty() && !is_ip_literal) {
        SSL_set_tlsext_host_name(ssl, host.c_str());
    }

    // hostname 校验期望值：无论是否 IP 字面量都要设（IP 字面量走
    // X509_check_host 的 IP 匹配分支）。insecureSkipVerify 时跳过——纯自签
    // 调试场景不校验 host。
    //
    // 这台 BoringSSL 没有 OpenSSL 1.1 的 SSL_set1_host 便捷封装，用底层的
    // SSL_get0_param + X509_VERIFY_PARAM_set1_host 达到同样效果。
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

    // SPKI-pin 路径（外层自建代理跳，design §9.6）：只比对 leaf 的公钥
    // 指纹，不做链校验——与 SMPConnector 原 on_conn_cert_verify 的
    // spki_pin 分支语义完全一致。
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
        // 与 Create() 一致：caCertsPem 为空回落系统信任库（X509_STORE 版本
        // 的 SSL_CTX_set_default_verify_paths）。
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
    // 只释放 intermediates 这个 STACK_OF(X509) 容器本身，不碰里面的元素：
    // 它们是从调用方传入的 chain 借来的指针（chain 的所有权在调用方，见
    // 本函数上方的所有权约定注释），chain[0..] 由调用方在拿到返回值之后
    // 自己 X509_free。用 sk_X509_pop_free(intermediates, X509_free) 会把
    // 这些借来的对象真的释放掉，导致调用方那边的 freeCertVector 再释放
    // 一次（双重释放），以及本函数上面的诊断分支（若移到这里）读到已释放
    // 内存——这是曾经真实发生过的一次 use-after-free / double-free 崩溃，
    // 别再犯。
    sk_X509_free(intermediates);
    FreeCertVector(parsedCas);
    X509_STORE_free(store);
    return result;
}

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
