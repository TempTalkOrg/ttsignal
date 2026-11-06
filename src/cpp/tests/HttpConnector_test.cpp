



#include "HttpConnector.h"

#include <BC/BCFCodec.h>

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                         \
    do {                                                                       \
        std::string _a(actual);                                                \
        std::string _e(expected);                                              \
        if (_a != _e) {                                                        \
            printf("  FAIL %s:%d: 期望 \"%s\"，实际 \"%s\"\n",                 \
                   __FILE__, __LINE__, _e.c_str(), _a.c_str());                \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static bool Contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}





static void test_parse_url_basic()
{
    printf("[url] 基本形态\n");

    HttpUrlParts p;
    std::string  err;

    CHECK(HttpConnector::ParseUrl("http://example.com/", p, err));
    CHECK_EQ_STR(p.scheme, "http");
    CHECK_EQ_STR(p.host, "example.com");
    CHECK(p.port == 80);
    CHECK(!p.tls);
    CHECK_EQ_STR(p.target, "/");
    CHECK_EQ_STR(p.hostHeader, "example.com");

    CHECK(HttpConnector::ParseUrl("https://example.com", p, err));
    CHECK(p.port == 443);
    CHECK(p.tls);
    
    CHECK_EQ_STR(p.target, "/");
    CHECK_EQ_STR(p.hostHeader, "example.com");

    CHECK(HttpConnector::ParseUrl("https://api.example.com:8443/v1/x?a=1&b=2",
                                  p, err));
    CHECK_EQ_STR(p.host, "api.example.com");
    CHECK(p.port == 8443);
    CHECK_EQ_STR(p.target, "/v1/x?a=1&b=2");
    
    CHECK_EQ_STR(p.hostHeader, "api.example.com:8443");

    
    CHECK(HttpConnector::ParseUrl("https://example.com:443/x", p, err));
    CHECK_EQ_STR(p.hostHeader, "example.com");
    CHECK(HttpConnector::ParseUrl("http://example.com:80/x", p, err));
    CHECK_EQ_STR(p.hostHeader, "example.com");

    
    CHECK(HttpConnector::ParseUrl("HTTPS://Example.COM/A", p, err));
    CHECK_EQ_STR(p.scheme, "https");
    CHECK(p.tls);
    
    CHECK_EQ_STR(p.host, "Example.COM");
    CHECK_EQ_STR(p.target, "/A");
}

static void test_parse_url_query_and_fragment()
{
    printf("[url] query / fragment\n");

    HttpUrlParts p;
    std::string  err;

    
    CHECK(HttpConnector::ParseUrl("http://h/?a=1", p, err));
    CHECK_EQ_STR(p.target, "/?a=1");
    CHECK(HttpConnector::ParseUrl("http://h?a=1", p, err));
    CHECK_EQ_STR(p.target, "/?a=1");

    
    CHECK(HttpConnector::ParseUrl("http://h/p?a=1#frag", p, err));
    CHECK_EQ_STR(p.target, "/p?a=1");
    CHECK(HttpConnector::ParseUrl("http://h/p#frag", p, err));
    CHECK_EQ_STR(p.target, "/p");
    CHECK(HttpConnector::ParseUrl("http://h#frag", p, err));
    CHECK_EQ_STR(p.target, "/");
    CHECK_EQ_STR(p.host, "h");
}

static void test_parse_url_ipv6()
{
    printf("[url] IPv6 字面量\n");

    HttpUrlParts p;
    std::string  err;

    CHECK(HttpConnector::ParseUrl("https://[2001:db8::1]/x", p, err));
    
    CHECK_EQ_STR(p.host, "2001:db8::1");
    CHECK(p.port == 443);
    
    CHECK_EQ_STR(p.hostHeader, "[2001:db8::1]");

    CHECK(HttpConnector::ParseUrl("http://[::1]:8080/", p, err));
    CHECK_EQ_STR(p.host, "::1");
    CHECK(p.port == 8080);
    CHECK_EQ_STR(p.hostHeader, "[::1]:8080");

    
    CHECK(!HttpConnector::ParseUrl("http://::1/", p, err));
    CHECK(!HttpConnector::ParseUrl("http://[::1/", p, err));
    CHECK(!HttpConnector::ParseUrl("http://[]/", p, err));
    CHECK(!HttpConnector::ParseUrl("http://[::1]x/", p, err));
}

static void test_parse_url_rejects()
{
    printf("[url] 非法输入\n");

    HttpUrlParts p;
    std::string  err;

    CHECK(!HttpConnector::ParseUrl("", p, err));
    CHECK(!HttpConnector::ParseUrl("example.com/x", p, err));       
    CHECK(!HttpConnector::ParseUrl("://example.com", p, err));      
    CHECK(!HttpConnector::ParseUrl("ftp://example.com/", p, err));
    CHECK(!HttpConnector::ParseUrl("ws://example.com/", p, err));   
    CHECK(!HttpConnector::ParseUrl("http:///x", p, err));           
    CHECK(!HttpConnector::ParseUrl("http://u:p@example.com/", p, err)); 
    CHECK(!HttpConnector::ParseUrl("http://example.com:0/", p, err));
    CHECK(!HttpConnector::ParseUrl("http://example.com:70000/", p, err));
    CHECK(!HttpConnector::ParseUrl("http://example.com:80a/", p, err));
    
    CHECK(!HttpConnector::ParseUrl("http://example.com/a b", p, err));
    CHECK(!HttpConnector::ParseUrl("http://example.com/a\r\nX: 1", p, err));
    CHECK(!HttpConnector::ParseUrl("http://exa mple.com/", p, err));

    
    CHECK(!err.empty());

    
    
    CHECK(HttpConnector::ParseUrl("http://example.com:/", p, err));
    CHECK(p.port == 80);
    CHECK_EQ_STR(p.hostHeader, "example.com");
}





static void test_build_request_basic()
{
    printf("[req] 基本组装\n");

    HttpUrlParts p;
    std::string  err, text;
    CHECK(HttpConnector::ParseUrl("https://api.example.com:8443/v1/x?a=1", p, err));

    HttpRequest req;
    req.url = "https://api.example.com:8443/v1/x?a=1";

    CHECK(HttpConnector::BuildRequestText(req, p, text, err));
    CHECK(text.compare(0, 34, "GET /v1/x?a=1 HTTP/1.1\r\nHost: api.") == 0);
    CHECK(Contains(text, "Host: api.example.com:8443\r\n"));
    CHECK(Contains(text, "User-Agent: ttsignal/1.0\r\n"));
    CHECK(Contains(text, "Connection: close\r\n"));
    
    CHECK(!Contains(text, "Content-Length"));
    
    CHECK(text.size() >= 4 && text.compare(text.size() - 4, 4, "\r\n\r\n") == 0);
}

static void test_build_request_headers()
{
    printf("[req] 自定义头 / 覆盖 / 忽略\n");

    HttpUrlParts p;
    std::string  err, text;
    CHECK(HttpConnector::ParseUrl("http://h/x", p, err));

    HttpRequest req;
    req.headers["Accept"]     = "application/json";
    req.headers["User-Agent"] = "myapp/2.0";
    CHECK(HttpConnector::BuildRequestText(req, p, text, err));
    CHECK(Contains(text, "Accept: application/json\r\n"));
    
    CHECK(Contains(text, "User-Agent: myapp/2.0\r\n"));
    CHECK(!Contains(text, "ttsignal/1.0"));

    
    
    
    text.clear();
    HttpRequest req2;
    req2.method                       = "POST";
    req2.body                         = "hello";
    req2.headers["Content-Length"]    = "99999";
    req2.headers["Connection"]        = "keep-alive";
    req2.headers["Transfer-Encoding"] = "chunked";
    CHECK(HttpConnector::BuildRequestText(req2, p, text, err));
    CHECK(Contains(text, "Content-Length: 5\r\n"));
    CHECK(!Contains(text, "99999"));
    CHECK(Contains(text, "Connection: close\r\n"));
    CHECK(!Contains(text, "keep-alive"));
    CHECK(!Contains(text, "chunked"));
    CHECK(text.size() >= 5 && text.compare(text.size() - 5, 5, "hello") == 0);

    
    text.clear();
    HttpRequest req3;
    req3.headers["Host"] = "virtual.example.com";
    CHECK(HttpConnector::BuildRequestText(req3, p, text, err));
    CHECK(Contains(text, "Host: virtual.example.com\r\n"));
    
    CHECK(text.find("Host:") == text.rfind("Host:"));
}

static void test_build_request_body_semantics()
{
    printf("[req] body 与 Content-Length\n");

    HttpUrlParts p;
    std::string  err, text;
    CHECK(HttpConnector::ParseUrl("http://h/x", p, err));

    
    
    HttpRequest post;
    post.method = "POST";
    CHECK(HttpConnector::BuildRequestText(post, p, text, err));
    CHECK(Contains(text, "Content-Length: 0\r\n"));

    
    text.clear();
    HttpRequest post2;
    post2.method = "post";
    CHECK(HttpConnector::BuildRequestText(post2, p, text, err));
    CHECK(Contains(text, "Content-Length: 0\r\n"));

    
    text.clear();
    HttpRequest del;
    del.method = "DELETE";
    CHECK(HttpConnector::BuildRequestText(del, p, text, err));
    CHECK(!Contains(text, "Content-Length"));

    
    
    text.clear();
    HttpRequest head;
    head.method = "HEAD";
    CHECK(HttpConnector::BuildRequestText(head, p, text, err));
    CHECK(text.compare(0, 5, "HEAD ") == 0);
    CHECK(!Contains(text, "Content-Length"));

    
    text.clear();
    HttpRequest bin;
    bin.method = "PUT";
    bin.body.assign("ab\0cd", 5);
    CHECK(HttpConnector::BuildRequestText(bin, p, text, err));
    CHECK(Contains(text, "Content-Length: 5\r\n"));
    CHECK(text.size() >= 5 && text.compare(text.size() - 5, 5,
                                           std::string("ab\0cd", 5)) == 0);

    
    text.clear();
    HttpRequest def;
    def.method.clear();
    CHECK(HttpConnector::BuildRequestText(def, p, text, err));
    CHECK(text.compare(0, 4, "GET ") == 0);
}

static void test_build_request_injection()
{
    printf("[req] 头注入必须被拒\n");

    HttpUrlParts p;
    std::string  err, text;
    CHECK(HttpConnector::ParseUrl("http://h/x", p, err));

    
    HttpRequest r1;
    r1.headers["X-Bad"] = "a\r\nX-Injected: 1";
    CHECK(!HttpConnector::BuildRequestText(r1, p, text, err));
    CHECK(!err.empty());

    HttpRequest r2;
    r2.headers["X-Bad"] = std::string("a\0b", 3);
    CHECK(!HttpConnector::BuildRequestText(r2, p, text, err));

    
    HttpRequest r3;
    r3.headers["X Bad"] = "1";
    CHECK(!HttpConnector::BuildRequestText(r3, p, text, err));

    HttpRequest r4;
    r4.headers["X:Bad"] = "1";
    CHECK(!HttpConnector::BuildRequestText(r4, p, text, err));

    HttpRequest r5;
    r5.headers[""] = "1";
    CHECK(!HttpConnector::BuildRequestText(r5, p, text, err));

    
    HttpRequest r6;
    r6.method = "GET /evil HTTP/1.1\r\nX:";
    CHECK(!HttpConnector::BuildRequestText(r6, p, text, err));

    
    HttpRequest r7;
    r7.headers["Host"] = "h\r\nX: 1";
    CHECK(!HttpConnector::BuildRequestText(r7, p, text, err));
}





static void test_config_defaults()
{
    printf("[cfg] 默认值\n");

    HttpConnector c;
    CHECK(c.Create(NULL, NULL) == BC_R_SUCCESS);
    CHECK(c.MaxResponseBytes() == 8u * 1024 * 1024);
    CHECK(c.TlsSettings().caCertsPem.empty());
    CHECK(c.TlsSettings().spkiPin.empty());
    CHECK(!c.TlsSettings().insecureSkipVerify);
    CHECK(c.DnsSettings().servers.empty());
    CHECK(c.DnsSettings().androidNetHandle == 0);
    
    CHECK(c.EffectivePolicy() == tt_vpn_policy_platform_default());
    
    
    CHECK(c.DnsSettings().policy == c.EffectivePolicy());

    
    CHECK(c.Create(NULL, NULL) == BC_R_ALREADYRUNNING);
}

static void test_config_vpn_policy()
{
    printf("[cfg] vpnPolicy / bypassVpn 合并\n");

    {
        BCFObject cfg;
        cfg.PutString("vpnPolicy", "force-physical");
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.EffectivePolicy() == TT_VPN_POLICY_FORCE_PHYSICAL);
        CHECK(c.DnsSettings().policy == TT_VPN_POLICY_FORCE_PHYSICAL);
    }
    {
        BCFObject cfg;
        cfg.PutInt("androidNetHandle", 123456);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DnsSettings().androidNetHandle == 123456);
    }
    {
        BCFObject cfg;
        cfg.PutString("vpnPolicy", "os");
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.EffectivePolicy() == TT_VPN_POLICY_OS);
    }
    {
        
        BCFObject cfg;
        cfg.PutBool("bypassVpn", true);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.EffectivePolicy() == TT_VPN_POLICY_PREFER_PHYSICAL);
    }
    {
        BCFObject cfg;
        cfg.PutBool("bypassVpn", false);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.EffectivePolicy() == TT_VPN_POLICY_OS);
    }
    {
        
        BCFObject cfg;
        cfg.PutString("vpnPolicy", "os");
        cfg.PutBool("bypassVpn", true);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.EffectivePolicy() == TT_VPN_POLICY_OS);
    }
    {
        
        BCFObject cfg;
        cfg.PutString("vpnPolicy", "physical-force");
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.EffectivePolicy() == tt_vpn_policy_platform_default());
    }
}

static void test_config_dns_tls_limits()
{
    printf("[cfg] dns / tls / maxResponseBytes\n");

    {
        BCFArray* pServers = new BCFArray();
        pServers->PushString("223.5.5.5");
        pServers->PushString("119.29.29.29");

        BCFObject cfg;
        cfg.Put("dnsServers", pServers);
        cfg.PutInt("dnsTimeoutMs", 777);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DnsSettings().servers.size() == 2);
        if (c.DnsSettings().servers.size() == 2)
        {
            CHECK_EQ_STR(c.DnsSettings().servers[0], "223.5.5.5");
            CHECK_EQ_STR(c.DnsSettings().servers[1], "119.29.29.29");
        }
        CHECK(c.DnsSettings().timeoutMs == 777);
    }
    {
        
        BCFObject cfg;
        cfg.PutString("dnsServers", "1.1.1.1,8.8.8.8");
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DnsSettings().servers.size() == 2);
        if (c.DnsSettings().servers.size() == 2)
        {
            CHECK_EQ_STR(c.DnsSettings().servers[0], "1.1.1.1");
            CHECK_EQ_STR(c.DnsSettings().servers[1], "8.8.8.8");
        }
    }
    {
        BCFObject cfg;
        cfg.PutString("caCerts", "-----BEGIN CERTIFICATE-----\nx\n");
        cfg.PutString("spkiPin", "AAAA");
        cfg.PutBool("insecureSkipVerify", true);
        cfg.PutInt("maxResponseBytes", 4096);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(Contains(c.TlsSettings().caCertsPem, "BEGIN CERTIFICATE"));
        CHECK_EQ_STR(c.TlsSettings().spkiPin, "AAAA");
        CHECK(c.TlsSettings().insecureSkipVerify);
        CHECK(c.MaxResponseBytes() == 4096);
    }
    {
        
        BCFObject cfg;
        cfg.PutInt("maxResponseBytes", 0);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.MaxResponseBytes() == 8u * 1024 * 1024);
    }
}

static void test_config_drain_timeout()
{
    printf("[cfg] drainTimeoutMs 的取值校验\n");

    {
        HttpConnector c;
        CHECK(c.Create(NULL, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 30000);      
    }
    {
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 1500);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 1500);
    }
    {
        
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 0);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 0);
    }
    {
        
        
        
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", (uint64_t)-1);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 300000);     
    }
    {
        
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", (uint64_t)0x100000001ULL);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 300000);
    }
    {
        
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 300000);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 300000);
    }
    {
        
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 300001);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 300000);
    }
}

static void test_request_argument_validation()
{
    printf("[cfg] Request 的同步参数校验\n");

    HttpConnector c;

    HttpRequest req;
    req.url = "http://example.com/";
    
    CHECK(c.Request(req, NULL) == BC_R_NOTCONNECTED);

    CHECK(c.Create(NULL, NULL) == BC_R_SUCCESS);

    
    CHECK(c.Request(req, NULL) == BC_R_INVALIDARG);
}





int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);

    test_parse_url_basic();
    test_parse_url_query_and_fragment();
    test_parse_url_ipv6();
    test_parse_url_rejects();

    test_build_request_basic();
    test_build_request_headers();
    test_build_request_body_semantics();
    test_build_request_injection();

    test_config_defaults();
    test_config_vpn_policy();
    test_config_dns_tls_limits();
    test_config_drain_timeout();
    test_request_argument_validation();

    if (g_failures == 0)
    {
        printf("HttpConnector_test: ALL PASSED\n");
        return 0;
    }
    printf("HttpConnector_test: %d FAILURE(S)\n", g_failures);
    return 1;
}




