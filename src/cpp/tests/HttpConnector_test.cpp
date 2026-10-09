///////////////////////////////////////////////////////////////////////////////
// file : HttpConnector_test.cpp
//
// HttpConnector 里不依赖网络的部分：URL 解析、请求组装、配置解析。
// 不进任何构建目标，手动编译运行。
//
// 真实网络路径（收响应、maxResponseBytes 截断、超时、TLS 校验、并发关闭）
// 在 HttpConnector_integration_test.cpp 里，那个才是主战场 —— 前几轮的教训是
// "没有真实路径测试覆盖的分支必然有 bug"。
//
// ---------------------------------------------------------------------------
// 怎么跑（macOS arm64；Linux 见文件末尾的替换说明）
// ---------------------------------------------------------------------------
//
//   cd /Users/antonio/chative/ttsignal
//
//   # llhttp 是纯 C，必须单独按 C 编（跟着 -std=c++17 走会因为 int->enum 的
//   # 隐式转换编不过）
//   for f in api http llhttp; do \
//       cc -c -g -fsanitize=address -I deps/llhttp/include \
//          deps/llhttp/src/$f.c -o /tmp/llhttp_$f.o || break; done
//
//   c++ -std=c++17 -g -Wall -fsanitize=address -fno-omit-frame-pointer \
//       -I src/cpp -I deps/env/src -I deps/jquic/include \
//       -I deps/boringssl/src/include -I deps/llhttp/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       src/cpp/TlsContext.cpp src/cpp/SSLayer.cpp src/cpp/Runtime.cpp \
//       src/cpp/NetworkRouteLookup.cpp src/cpp/apple/AppleRouteLookup.cpp \
//       src/cpp/TcpChannel.cpp src/cpp/Utils.cpp \
//       src/cpp/LLHTTPParser.cpp src/cpp/HttpConnector.cpp \
//       /tmp/llhttp_api.o /tmp/llhttp_http.o /tmp/llhttp_llhttp.o \
//       src/cpp/tests/HttpConnector_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libssl.a \
//       deps/boringssl/lib/Darwin/arm64/Debug/libcrypto.a \
//       -o /tmp/httpconnector_ut
//   /tmp/httpconnector_ut
//
// （Linux 把 apple/AppleRouteLookup.cpp 换成 linux/LinuxRouteLookup.cpp，
//   库路径换成 deps/*/lib/Linux/x86_64/Debug/*.a。）
//
// 本文件一行网络代码都没有，也不需要 Runtime::Initialize —— 上面那一大串源文件
// 只是为了满足链接器（HttpConnector.cpp 里引用了 TcpChannel / Runtime / LogQ）。
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// ParseUrl
///////////////////////////////////////////////////////////////////////////////

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
    // 没写路径时补 "/"，否则请求行会是 "GET  HTTP/1.1"
    CHECK_EQ_STR(p.target, "/");
    CHECK_EQ_STR(p.hostHeader, "example.com");

    CHECK(HttpConnector::ParseUrl("https://api.example.com:8443/v1/x?a=1&b=2",
                                  p, err));
    CHECK_EQ_STR(p.host, "api.example.com");
    CHECK(p.port == 8443);
    CHECK_EQ_STR(p.target, "/v1/x?a=1&b=2");
    // 非默认端口必须进 Host 头，否则虚拟主机分流会出错
    CHECK_EQ_STR(p.hostHeader, "api.example.com:8443");

    // 显式写出默认端口时不重复进 Host 头
    CHECK(HttpConnector::ParseUrl("https://example.com:443/x", p, err));
    CHECK_EQ_STR(p.hostHeader, "example.com");
    CHECK(HttpConnector::ParseUrl("http://example.com:80/x", p, err));
    CHECK_EQ_STR(p.hostHeader, "example.com");

    // scheme 大小写不敏感
    CHECK(HttpConnector::ParseUrl("HTTPS://Example.COM/A", p, err));
    CHECK_EQ_STR(p.scheme, "https");
    CHECK(p.tls);
    // 主机名保持原样（SNI 与 Host 头都不该被我们改写大小写）
    CHECK_EQ_STR(p.host, "Example.COM");
    CHECK_EQ_STR(p.target, "/A");
}

static void test_parse_url_query_and_fragment()
{
    printf("[url] query / fragment\n");

    HttpUrlParts p;
    std::string  err;

    // 没有路径只有 query：request-target 必须补上前导 "/"
    CHECK(HttpConnector::ParseUrl("http://h/?a=1", p, err));
    CHECK_EQ_STR(p.target, "/?a=1");
    CHECK(HttpConnector::ParseUrl("http://h?a=1", p, err));
    CHECK_EQ_STR(p.target, "/?a=1");

    // fragment 绝不能发上线
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
    // host 交给 TcpChannel 做 IP 字面量解析，必须是去掉方括号的裸地址
    CHECK_EQ_STR(p.host, "2001:db8::1");
    CHECK(p.port == 443);
    // Host 头按 RFC 3986 必须保留方括号
    CHECK_EQ_STR(p.hostHeader, "[2001:db8::1]");

    CHECK(HttpConnector::ParseUrl("http://[::1]:8080/", p, err));
    CHECK_EQ_STR(p.host, "::1");
    CHECK(p.port == 8080);
    CHECK_EQ_STR(p.hostHeader, "[::1]:8080");

    // 裸写的 IPv6（没有方括号）必须拒绝，否则会被当成 host:port 切错
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
    CHECK(!HttpConnector::ParseUrl("example.com/x", p, err));       // 无 scheme
    CHECK(!HttpConnector::ParseUrl("://example.com", p, err));      // 空 scheme
    CHECK(!HttpConnector::ParseUrl("ftp://example.com/", p, err));
    CHECK(!HttpConnector::ParseUrl("ws://example.com/", p, err));   // WS 归 Task 7
    CHECK(!HttpConnector::ParseUrl("http:///x", p, err));           // 空 host
    CHECK(!HttpConnector::ParseUrl("http://u:p@example.com/", p, err)); // userinfo
    CHECK(!HttpConnector::ParseUrl("http://example.com:0/", p, err));
    CHECK(!HttpConnector::ParseUrl("http://example.com:70000/", p, err));
    CHECK(!HttpConnector::ParseUrl("http://example.com:80a/", p, err));
    // 路径里的裸空格 / 裸换行是请求走私的经典入口
    CHECK(!HttpConnector::ParseUrl("http://example.com/a b", p, err));
    CHECK(!HttpConnector::ParseUrl("http://example.com/a\r\nX: 1", p, err));
    CHECK(!HttpConnector::ParseUrl("http://exa mple.com/", p, err));

    // 失败时 outErr 必须有内容，否则调用方没法给用户任何提示
    CHECK(!err.empty());

    // 空端口（"host:"）按 RFC 3986 3.2.3 是合法的，含义是"用 scheme 默认端口"，
    // curl 也这么处理。这里刻意接受而不是拒绝。
    CHECK(HttpConnector::ParseUrl("http://example.com:/", p, err));
    CHECK(p.port == 80);
    CHECK_EQ_STR(p.hostHeader, "example.com");
}

///////////////////////////////////////////////////////////////////////////////
// BuildRequestText
///////////////////////////////////////////////////////////////////////////////

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
    // GET 没有 body，不该出现 Content-Length
    CHECK(!Contains(text, "Content-Length"));
    // 报文必须以空行结尾
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
    // 业务给了 User-Agent 就不再叠默认值
    CHECK(Contains(text, "User-Agent: myapp/2.0\r\n"));
    CHECK(!Contains(text, "ttsignal/1.0"));

    // Content-Length / Connection / Transfer-Encoding 由本模块权威决定，
    // 业务给的同名项必须被丢掉 —— 让调用方同时控制 Content-Length 与 body
    // 等于把请求走私的钥匙交出去
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

    // Host 允许业务覆盖（有网关按 Host 分流）
    text.clear();
    HttpRequest req3;
    req3.headers["Host"] = "virtual.example.com";
    CHECK(HttpConnector::BuildRequestText(req3, p, text, err));
    CHECK(Contains(text, "Host: virtual.example.com\r\n"));
    // 覆盖之后不能再冒出第二个 Host
    CHECK(text.find("Host:") == text.rfind("Host:"));
}

static void test_build_request_body_semantics()
{
    printf("[req] body 与 Content-Length\n");

    HttpUrlParts p;
    std::string  err, text;
    CHECK(HttpConnector::ParseUrl("http://h/x", p, err));

    // POST 即使 body 为空也要显式给 Content-Length: 0，
    // 否则服务端在 "Connection: close" 下无从判断请求体是否结束
    HttpRequest post;
    post.method = "POST";
    CHECK(HttpConnector::BuildRequestText(post, p, text, err));
    CHECK(Contains(text, "Content-Length: 0\r\n"));

    // 大小写不敏感
    text.clear();
    HttpRequest post2;
    post2.method = "post";
    CHECK(HttpConnector::BuildRequestText(post2, p, text, err));
    CHECK(Contains(text, "Content-Length: 0\r\n"));

    // DELETE 没有 body 时不加 Content-Length
    text.clear();
    HttpRequest del;
    del.method = "DELETE";
    CHECK(HttpConnector::BuildRequestText(del, p, text, err));
    CHECK(!Contains(text, "Content-Length"));

    // HEAD 同理。（HEAD 响应"没有 body"这件事必须另外告诉 llhttp，
    //  否则它会去等 Content-Length 声明的字节数 —— 见集成测试 case U。）
    text.clear();
    HttpRequest head;
    head.method = "HEAD";
    CHECK(HttpConnector::BuildRequestText(head, p, text, err));
    CHECK(text.compare(0, 5, "HEAD ") == 0);
    CHECK(!Contains(text, "Content-Length"));

    // 带二进制 body（含 \0）时长度按字节算，不能被 strlen 截断
    text.clear();
    HttpRequest bin;
    bin.method = "PUT";
    bin.body.assign("ab\0cd", 5);
    CHECK(HttpConnector::BuildRequestText(bin, p, text, err));
    CHECK(Contains(text, "Content-Length: 5\r\n"));
    CHECK(text.size() >= 5 && text.compare(text.size() - 5, 5,
                                           std::string("ab\0cd", 5)) == 0);

    // method 缺省为 GET
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

    // 头值里塞 CRLF：不拦的话调用方可以往报文里插任意行
    HttpRequest r1;
    r1.headers["X-Bad"] = "a\r\nX-Injected: 1";
    CHECK(!HttpConnector::BuildRequestText(r1, p, text, err));
    CHECK(!err.empty());

    HttpRequest r2;
    r2.headers["X-Bad"] = std::string("a\0b", 3);
    CHECK(!HttpConnector::BuildRequestText(r2, p, text, err));

    // 头名不是 token
    HttpRequest r3;
    r3.headers["X Bad"] = "1";
    CHECK(!HttpConnector::BuildRequestText(r3, p, text, err));

    HttpRequest r4;
    r4.headers["X:Bad"] = "1";
    CHECK(!HttpConnector::BuildRequestText(r4, p, text, err));

    HttpRequest r5;
    r5.headers[""] = "1";
    CHECK(!HttpConnector::BuildRequestText(r5, p, text, err));

    // method 不是 token
    HttpRequest r6;
    r6.method = "GET /evil HTTP/1.1\r\nX:";
    CHECK(!HttpConnector::BuildRequestText(r6, p, text, err));

    // 覆盖 Host 时同样要过注入检查
    HttpRequest r7;
    r7.headers["Host"] = "h\r\nX: 1";
    CHECK(!HttpConnector::BuildRequestText(r7, p, text, err));
}

///////////////////////////////////////////////////////////////////////////////
// 配置解析
///////////////////////////////////////////////////////////////////////////////

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
    // 两个键都没给时回落平台默认值
    CHECK(c.EffectivePolicy() == tt_vpn_policy_platform_default());
    // 策略必须原样透传给 DNS —— 只绑 TCP 不绑 DNS 在 TUN 全局模式下会拿到
    // fake-IP，等于白绑（见 DnsResolver.h 顶部注释）
    CHECK(c.DnsSettings().policy == c.EffectivePolicy());

    // Create 只能调一次
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
        // 旧键单独给出时走兼容映射：true -> prefer-physical
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
        // 两个键同时给出：新键胜出，旧键被忽略
        BCFObject cfg;
        cfg.PutString("vpnPolicy", "os");
        cfg.PutBool("bypassVpn", true);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.EffectivePolicy() == TT_VPN_POLICY_OS);
    }
    {
        // 无法识别的取值：回落平台默认值，不能崩也不能当成 os
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
        // 逗号分隔的字符串写法（命令行工具用）
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
        // maxResponseBytes = 0 视为没配，保持默认；否则一切响应都会被截断
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
        CHECK(c.DrainTimeoutMs() == 30000);      // 默认
    }
    {
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 1500);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 1500);
    }
    {
        // 0 是合法值：不等，掐断所有回调后立即返回
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 0);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 0);
    }
    {
        // ⚠️ 关键用例：GET_BCF_INT 返回 uint64_t，传 -1 会变成
        // 0xFFFFFFFFFFFFFFFF；直接窄化成 uint32_t 就是 0xFFFFFFFF ≈ 49.7 天，
        // 正好把"带上限"想避免的事实上永久阻塞重新引回来。必须被夹住。
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", (uint64_t)-1);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 300000);     // 夹到 5 分钟
    }
    {
        // 超过 2^32 的值以前会被静默截断成一个毫不相干的数字
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", (uint64_t)0x100000001ULL);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 300000);
    }
    {
        // 边界：正好等于上限，不该被夹（判断是 > 不是 >=）
        BCFObject cfg;
        cfg.PutInt("drainTimeoutMs", 300000);
        HttpConnector c;
        CHECK(c.Create(&cfg, NULL) == BC_R_SUCCESS);
        CHECK(c.DrainTimeoutMs() == 300000);
    }
    {
        // 边界：上限 + 1，必须被夹
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
    // 还没 Create
    CHECK(c.Request(req, NULL) == BC_R_NOTCONNECTED);

    CHECK(c.Create(NULL, NULL) == BC_R_SUCCESS);

    // handler 为空
    CHECK(c.Request(req, NULL) == BC_R_INVALIDARG);
}

///////////////////////////////////////////////////////////////////////////////
// main
///////////////////////////////////////////////////////////////////////////////

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

///////////////////////////////////////////////////////////////////////////////
// End of file
///////////////////////////////////////////////////////////////////////////////
