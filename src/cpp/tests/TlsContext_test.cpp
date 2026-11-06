


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




static void test_empty_ca_falls_back_to_system_store()
{
    TlsConfig cfg;
    std::string err;
    SSL_CTX* ctx = TlsContext::Create(cfg, err);
    CHECK(ctx != nullptr);
    CHECK(err.empty());
    if (ctx) SSL_CTX_free(ctx);
}


static void test_explicit_ca_is_loaded()
{
    
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




static void test_spki_pin_canonicalization()
{
    const char* raw = "  sha256//YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg=  ";
    std::string got = TlsContext::CanonicalizeSpkiPin(raw, strlen(raw));
    CHECK(got == "YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg");

    const char* bare = "YLh1dUR9y6Kja30RrAn7JKnbQG/uEtLMkBgFF2Fuihg";
    CHECK(TlsContext::CanonicalizeSpkiPin(bare, strlen(bare)) == bare);

    CHECK(TlsContext::CanonicalizeSpkiPin(nullptr, 0).empty());
}


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

    TlsConfig cfg; 
                   
                   
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
