///////////////////////////////////////////////////////////////////////////////
// file : DnsResolver_test.cpp
//
// Standalone unit test for DNS message encode/decode. Not part of any build
// target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp -I deps/env/src -I deps/jquic/include \
//       src/cpp/VpnPolicy.cpp src/cpp/SocketPinner.cpp src/cpp/DnsResolver.cpp \
//       src/cpp/Utils.cpp deps/env/src/BC/BCSockAddr.cpp \
//       src/cpp/tests/DnsResolver_test.cpp \
//       deps/env/lib/Darwin/arm64/Debug/libenv.a \
//       -o /tmp/dnsresolver_test && /tmp/dnsresolver_test
//
// DnsResolver.cpp 和 SocketPinner.cpp 一样通过 StdAfx.h/Utils.h 调用真实的
// LogQ（不是桩实现），所以除了 VpnPolicy.cpp 之外还要把 Utils.cpp 一起编译，
// 并链接 env 库（LogQ 内部依赖 BCPString::Format / LogCustomV 等符号，只编译
// Utils.cpp 不链接 libenv.a 会在链接期报符号缺失）。-I deps/jquic/include 是
// 因为 Utils.h 传递性 include 了 <xquic/xquic.h>。非 Darwin/arm64 宿主换成
// 对应平台下的 deps/env/lib/<平台>/<架构>/Debug/libenv.a 即可。
//
// 只测报文编解码，不发真实网络请求。
///////////////////////////////////////////////////////////////////////////////
#include "DnsResolver.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

// example.com 的标准 A 应答，answer 的 NAME 用压缩指针 0xC00C 指回 question。
static const uint8_t kResponseExampleCom[] = {
    // Header: ID=0x1234, Flags=0x8180 (QR|RD|RA), QD=1, AN=1, NS=0, AR=0
    0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    // Question: 7"example" 3"com" 0, QTYPE=A(1), QCLASS=IN(1)
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
    0x00, 0x01, 0x00, 0x01,
    // Answer: NAME=ptr->12, TYPE=A, CLASS=IN, TTL=300, RDLEN=4, RDATA=93.184.216.34
    0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C,
    0x00, 0x04, 0x5D, 0xB8, 0xD8, 0x22,
};

static void test_build_query_encodes_labels()
{
    std::vector<uint8_t> q = DnsMessage::BuildQuery("example.com", 1, 0x1234);

    // 12 字节 header + 13 字节 QNAME + 4 字节 QTYPE/QCLASS
    CHECK(q.size() == 29);
    CHECK(q[0] == 0x12 && q[1] == 0x34);        // txid
    CHECK(q[2] == 0x01 && q[3] == 0x00);        // flags: RD=1
    CHECK(q[4] == 0x00 && q[5] == 0x01);        // QDCOUNT=1
    CHECK(q[6] == 0x00 && q[7] == 0x00);        // ANCOUNT=0
    CHECK(q[12] == 0x07);                       // label len "example"
    CHECK(memcmp(&q[13], "example", 7) == 0);
    CHECK(q[20] == 0x03);                       // label len "com"
    CHECK(memcmp(&q[21], "com", 3) == 0);
    CHECK(q[24] == 0x00);                       // root label
    CHECK(q[25] == 0x00 && q[26] == 0x01);      // QTYPE=A
    CHECK(q[27] == 0x00 && q[28] == 0x01);      // QCLASS=IN
}

// 超长 label（>63）和超长域名（>253）必须被拒绝，返回空。
static void test_build_query_rejects_oversized_names()
{
    std::string longLabel(64, 'a');
    CHECK(DnsMessage::BuildQuery(longLabel + ".com", 1, 1).empty());

    std::string longName;
    for (int i = 0; i < 40; i++) longName += "abcdef.";
    longName += "com";
    CHECK(longName.size() > 253);
    CHECK(DnsMessage::BuildQuery(longName, 1, 1).empty());

    CHECK(DnsMessage::BuildQuery("", 1, 1).empty());
}

static void test_parse_response_extracts_a_record()
{
    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(DnsMessage::ParseResponse(kResponseExampleCom,
                                    sizeof(kResponseExampleCom),
                                    0x1234, addrs, ttl, err));
    CHECK(err.empty());
    CHECK(addrs.size() == 1);
    CHECK(ttl == 300);

    char text[64] = {0};
    bc_sockaddr_format(&addrs[0], text, sizeof(text));
    CHECK(strstr(text, "93.184.216.34") != nullptr);
}

// txid 不匹配必须拒绝 —— 防 off-path 伪造应答。
static void test_parse_response_rejects_txid_mismatch()
{
    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kResponseExampleCom,
                                     sizeof(kResponseExampleCom),
                                     0x9999, addrs, ttl, err));
    CHECK(!err.empty());
    CHECK(addrs.empty());
}

// 压缩指针成环必须被检出，不能死循环。
static void test_parse_response_detects_pointer_loop()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    // answer 的 NAME 在偏移 29；改成指向它自己，构造自环
    msg[29] = 0xC0;
    msg[30] = 0x1D;   // 0x1D == 29

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());
}

// 越界的压缩指针必须被检出。
static void test_parse_response_detects_out_of_bounds_pointer()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[29] = 0xC0;
    msg[30] = 0xFF;   // 偏移 255 > 报文长度 45

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());
}

// 截断的报文（不足 header 长度）必须被拒绝，不能越界读。
static void test_parse_response_rejects_truncated_message()
{
    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kResponseExampleCom, 5,
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());

    // RDLENGTH 声称 4 字节但报文在此截断
    CHECK(!DnsMessage::ParseResponse(kResponseExampleCom,
                                     sizeof(kResponseExampleCom) - 2,
                                     0x1234, addrs, ttl, err));
}

// RCODE 非 0（如 NXDOMAIN=3）必须失败并在 errMessage 里说明。
static void test_parse_response_reports_rcode()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[3] = 0x83;   // RCODE = 3 (NXDOMAIN)

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(err.find("3") != std::string::npos);
}

// 非 A/AAAA 的 answer（如 CNAME）必须被跳过而不是当成地址。
static void test_parse_response_skips_non_address_records()
{
    // Header: AN=2。第一条 CNAME，第二条 A。
    std::vector<uint8_t> msg = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        // Answer 1: CNAME(5)，RDATA 是 3"www" + ptr->12
        0xC0, 0x0C, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C,
        0x00, 0x06, 0x03, 'w', 'w', 'w', 0xC0, 0x0C,
        // Answer 2: A，TTL=60
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04, 0x01, 0x02, 0x03, 0x04,
    };

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(DnsMessage::ParseResponse(msg.data(), msg.size(),
                                    0x1234, addrs, ttl, err));
    CHECK(addrs.size() == 1);       // 只认 A，CNAME 被跳过
    CHECK(ttl == 60);               // 取所有记录 TTL 的最小值
}

// RDLENGTH 与记录类型不符（A 记录声称 16 字节）必须被跳过，不能当成地址。
static void test_parse_response_rejects_bad_rdlength()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[39] = 0x00;
    msg[40] = 0x10;   // RDLENGTH 改成 16，与 A 记录的 4 不符

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    DnsMessage::ParseResponse(msg.data(), msg.size(), 0x1234, addrs, ttl, err);
    CHECK(addrs.empty());
}

///////////////////////////////////////////////////////////////////////////////
// Fix round：以下测试是 review 之后新增的回归测试，对应 Critical + Important
// 各项发现。旧测试一字未改。
///////////////////////////////////////////////////////////////////////////////

// QR=0（这是一条"查询"而不是"应答"）必须被拒——典型场景是我们自己发出的
// 查询被同一台主机原样环回（txid 天然相同），不加这条会被当成合法的空应答。
static void test_parse_response_rejects_qr_zero()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[2] = 0x01;   // 清掉 QR 位（0x80），保留 RD（0x01），伪装成一条"查询"

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());
    CHECK(addrs.empty());
}

// txid 对上了，但 question 问的是 "evil" 而不是我们查询的 "example.com"——
// off-path 攻击者哪怕猜中 16 位 txid，指定 expectHost 后依然会被这条挡住，
// 不会把 6.6.6.6 当成 example.com 的地址采纳。
static void test_parse_response_rejects_forged_question_host()
{
    static const uint8_t kEvil[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x04, 'e', 'v', 'i', 'l', 0x00, 0x00, 0x01, 0x00, 0x01,
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04, 6, 6, 6, 6,
    };

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kEvil, sizeof(kEvil), 0x1234, addrs, ttl,
                                     err, "example.com"));
    CHECK(!err.empty());
    CHECK(addrs.empty());
}

// 正面对照：question 域名确实与 expectHost 一致（大小写不敏感、忽略尾部
// '.'）时，加上 expectHost 参数不能破坏正常解析。
static void test_parse_response_accepts_matching_expect_host()
{
    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(DnsMessage::ParseResponse(kResponseExampleCom,
                                    sizeof(kResponseExampleCom), 0x1234, addrs,
                                    ttl, err, "EXAMPLE.COM."));
    CHECK(err.empty());
    CHECK(addrs.size() == 1);
}

// answer 1 是合法 A 记录（会被解析、push 进 outAddrs），answer 2 的 RDLENGTH
// 声称 16 字节但报文里一个字节都没有（非 TC 截断）——整条解析必须失败，且
// 失败时 outAddrs 必须清空，不能把 answer 1 的半成品结果留给调用方。
static void test_parse_response_clears_addrs_on_failure()
{
    static const uint8_t kMsg[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        // answer 1：合法 A，TTL=300
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C,
        0x00, 0x04, 0x5D, 0xB8, 0xD8, 0x22,
        // answer 2：RDLENGTH=16，但报文到此为止，一个 RDATA 字节都没有
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A,
        0x00, 0x10,
    };

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kMsg, sizeof(kMsg), 0x1234, addrs, ttl, err));
    CHECK(!err.empty());
    CHECK(addrs.empty());   // 不能残留 answer 1 的地址
}

// Critical 回归测试：force-physical 下如果没有任何可绑定的物理网卡标识
// （ifIndex 与 androidNetHandle 均为 0），必须直接失败，绝不能让 socket 悄悄
// 不绑定就把查询发出去、还谎报成功。
//
// 修复前：这里会 result=BC_R_SUCCESS 且真的解析出地址——因为
// TT_PIN_NOT_NEEDED 被当成"绑定 OK"放行了，查询走的是系统默认路由（TUN
// 全局模式下就是隧道本身），"绕过 VPN"的承诺是假的。
// 修复后：必须是 BC_R_NO_PHYSICAL_INTERFACE（值 64，deps/env/src/BC/Config.h
// 提供），且不发出任何网络请求（这条测试不需要网络也能跑通、跑得很快，
// 就是因为它在 Resolve() 里最早的位置就短路返回了）。
static void test_resolve_force_physical_requires_interface()
{
    DnsConfig cfg;
    cfg.policy  = TT_VPN_POLICY_FORCE_PHYSICAL;
    cfg.ifIndex = 0;   // 故意不给网卡：模拟"探测不到物理网卡"

    DnsResult r = DnsResolver::Resolve("example.com", cfg);

    CHECK(r.result == BC_R_NO_PHYSICAL_INTERFACE);
    CHECK(r.addrs.empty());
    CHECK(!r.errMessage.empty());
}

// Re-review 发现的 I3 剩余绕过：question 域名校验写在 "for (i < qdcount)"
// 循环体内，QDCOUNT=0 时循环一次都不执行，expectHost 形同虚设。攻击者猜中
// txid 后只要把 question 段整个删掉（QDCOUNT 置 0），answer 就照单全收——
// 这份报文的 "answer" NAME 字段实际上是明文 "evil"（未压缩），紧跟着
// TYPE=A/RDLEN=4/RDATA=6.6.6.6，QDCOUNT=0、ANCOUNT=1。
static void test_parse_response_rejects_qdcount_zero_bypass()
{
    static const uint8_t kMsg[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
        0x04, 'e', 'v', 'i', 'l', 0x00, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x3C, 0x00, 0x04, 0x06, 0x06, 0x06, 0x06,
    };

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kMsg, sizeof(kMsg), 0x1234, addrs, ttl,
                                     err, "example.com"));
    CHECK(!err.empty());
    CHECK(addrs.empty());
}

int main()
{
    test_build_query_encodes_labels();
    test_build_query_rejects_oversized_names();
    test_parse_response_extracts_a_record();
    test_parse_response_rejects_txid_mismatch();
    test_parse_response_detects_pointer_loop();
    test_parse_response_detects_out_of_bounds_pointer();
    test_parse_response_rejects_truncated_message();
    test_parse_response_reports_rcode();
    test_parse_response_skips_non_address_records();
    test_parse_response_rejects_bad_rdlength();
    test_parse_response_rejects_qr_zero();
    test_parse_response_rejects_forged_question_host();
    test_parse_response_accepts_matching_expect_host();
    test_parse_response_clears_addrs_on_failure();
    test_resolve_force_physical_requires_interface();
    test_parse_response_rejects_qdcount_zero_bypass();

    if (g_failures == 0) {
        printf("DnsResolver_test: ALL PASS\n");
    } else {
        printf("DnsResolver_test: %d FAILURE(S)\n", g_failures);
    }
    return g_failures != 0;
}
