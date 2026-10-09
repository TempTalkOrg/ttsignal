///////////////////////////////////////////////////////////////////////////////
// file : TlsContext_test.cpp
//
// Standalone unit test. Not part of any build target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp -I deps/env/src -I deps/boringssl/src/include \
//       src/cpp/TlsContext.cpp src/cpp/tests/TlsContext_test.cpp \
//       -L deps/boringssl/lib/Darwin/arm64/Debug -lssl -lcrypto \
//       -o /tmp/tlscontext_test && /tmp/tlscontext_test
//
///////////////////////////////////////////////////////////////////////////////
#include "TlsContext.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <openssl/bio.h>
#include <openssl/pem.h>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// caCerts 为空且未开 insecureSkipVerify 时，必须回落系统信任库并成功建 ctx。
// 这是与 SMPConnector 唯一有意的行为差异：SMP 为空是"跳过校验"，
// TLS 侧为空是"用系统信任库校验"。
static void test_empty_ca_falls_back_to_system_store()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    CHECK(err.empty());
    if (ctx) SSL_CTX_free(ctx);
}

// 显式传入的 CA 必须被加载，且不该报错。
static void test_explicit_ca_is_loaded()
{
    // certs/localhost.crt 是仓库里现成的自签证书
    FILE* f = fopen("certs/localhost.crt", "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::string pem;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) pem.append(buf, n);
    fclose(f);

    TlsConfig cfg;
    cfg.caCertsPem = pem;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    CHECK(err.empty());
    if (ctx) SSL_CTX_free(ctx);
}

// 畸形 PEM 必须失败并给出可读原因，不能静默降级成"不校验"。
static void test_malformed_ca_fails_loudly()
{
    TlsConfig cfg;
    cfg.caCertsPem = "-----BEGIN CERTIFICATE-----\nbm90IGEgY2VydA==\n"
                     "-----END CERTIFICATE-----\n";
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx == nullptr);
    CHECK(!err.empty());
    if (ctx) SSL_CTX_free(ctx);
}

// SPKI pin 的规范化：去空白、剥掉 "sha256//" 前缀、丢弃 base64 padding
// （与 SMPConnector 迁移前的 canonicalizeSpkiPin 语义一致，只有一套规范
// 形式，比较双方都过这个函数即可直接比较，不需要额外再 trim 一次）。
static void test_spki_pin_canonicalization()
{
    const char* raw = "  sha256//YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg=  ";
    std::string got = TlsContext::CanonicalizeSpkiPin(raw, strlen(raw));
    CHECK(got == "YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg");

    const char* bare = "YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg";
    CHECK(TlsContext::CanonicalizeSpkiPin(bare, strlen(bare)) == bare);

    CHECK(TlsContext::CanonicalizeSpkiPin(nullptr, 0).empty());
}

// IP 字面量不能设 SNI（RFC 6066 禁止），域名必须设。
static void test_sni_skipped_for_ip_literal()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    if (!ctx) return;

    SSL* ssl = SSL_new(ctx);
    CHECK(ssl != nullptr);
    if (ssl) {
        CHECK(TlsContext::PrepareSsl(ssl, "192.0.2.1", cfg, err) == BC_R_SUCCESS);
        CHECK(SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name) == nullptr);
        SSL_free(ssl);
    }

    ssl = SSL_new(ctx);
    CHECK(ssl != nullptr);
    if (ssl) {
        CHECK(TlsContext::PrepareSsl(ssl, "example.com", cfg, err) == BC_R_SUCCESS);
        const char* sni = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        CHECK(sni != nullptr && strcmp(sni, "example.com") == 0);
        SSL_free(ssl);
    }

    SSL_CTX_free(ctx);
}

// IPv6 字面量同样不设 SNI。
static void test_sni_skipped_for_ipv6_literal()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    if (!ctx) return;

    SSL* ssl = SSL_new(ctx);
    if (ssl) {
        CHECK(TlsContext::PrepareSsl(ssl, "2001:db8::1", cfg, err) == BC_R_SUCCESS);
        CHECK(SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name) == nullptr);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
}

// 最低协议版本必须是 TLS 1.2。原 WSConnector.cpp:556 设的是 TLS1_1_VERSION。
static void test_min_proto_version_is_tls12()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    if (!ctx) return;
    CHECK(SSL_CTX_get_min_proto_version(ctx) == TLS1_2_VERSION);
    SSL_CTX_free(ctx);
}

// VerifyChain 只借用 chain，不释放里面的任何元素——调用方（如
// SMPConnector::on_conn_cert_verify）在拿到返回值之后自己 X509_free。
// 这里构造一条至少 2 个元素的链（leaf + 一张"中间证书"，用同一份 PEM 各
// PEM_read 一次得到两个独立的 X509* 对象，模拟真实的 leaf+intermediate），
// 调用 VerifyChain，然后模拟调用方的 freeCertVector：对 chain 里每个元素
// 再 X509_free 一次。如果 VerifyChain 内部误释放过 chain[1..]（曾经的
// bug：把借来的 intermediates 用 sk_X509_pop_free(..., X509_free) 真的
// free 掉），这里就是双重释放，BoringSSL 的引用计数下溢检测会 abort——
// 不崩即通过。
static void test_verify_chain_does_not_free_borrowed_certs()
{
    FILE* f = fopen("certs/localhost.crt", "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::string pem;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) pem.append(buf, n);
    fclose(f);

    BIO* bio1 = BIO_new_mem_buf(pem.data(), (int)pem.size());
    X509* leaf = bio1 ? PEM_read_bio_X509(bio1, nullptr, nullptr, nullptr) : nullptr;
    if (bio1) BIO_free(bio1);

    BIO* bio2 = BIO_new_mem_buf(pem.data(), (int)pem.size());
    X509* intermediate = bio2 ? PEM_read_bio_X509(bio2, nullptr, nullptr, nullptr) : nullptr;
    if (bio2) BIO_free(bio2);

    CHECK(leaf != nullptr);
    CHECK(intermediate != nullptr);
    if (!leaf || !intermediate) {
        if (leaf) X509_free(leaf);
        if (intermediate) X509_free(intermediate);
        return;
    }

    std::vector<X509*> chain = {leaf, intermediate};

    TlsConfig cfg; // 空 cfg：这里只关心 chain 元素的所有权，不关心校验
                   // 本身成不成功（自签证书当 leaf+intermediate 大概率会
                   // 校验失败，这是预期内的，不影响本测试的判定条件）。
    std::string err;
    TlsContext::VerifyChain(chain, "example.com", cfg, err);

    for (X509* cert : chain) {
        if (cert) X509_free(cert);
    }
}

int main()
{
    test_empty_ca_falls_back_to_system_store();
    test_explicit_ca_is_loaded();
    test_malformed_ca_fails_loudly();
    test_spki_pin_canonicalization();
    test_sni_skipped_for_ip_literal();
    test_sni_skipped_for_ipv6_literal();
    test_min_proto_version_is_tls12();
    test_verify_chain_does_not_free_borrowed_certs();

    if (g_failures == 0) {
        printf("TlsContext_test: ALL PASS\n");
    } else {
        printf("TlsContext_test: %d FAILURE(S)\n", g_failures);
    }
    return g_failures != 0;
}
