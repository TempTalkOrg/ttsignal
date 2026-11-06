


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


static const uint8_t kResponseExampleCom[] = {
    
    0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
    0x00, 0x01, 0x00, 0x01,
    
    0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C,
    0x00, 0x04, 0x5D, 0xB8, 0xD8, 0x22,
};

static void test_build_query_encodes_labels()
{
    std::vector<uint8_t> q = DnsMessage::BuildQuery("example.com", 1, 0x1234);

    
    CHECK(q.size() == 29);
    CHECK(q[0] == 0x12 && q[1] == 0x34);        
    CHECK(q[2] == 0x01 && q[3] == 0x00);        
    CHECK(q[4] == 0x00 && q[5] == 0x01);        
    CHECK(q[6] == 0x00 && q[7] == 0x00);        
    CHECK(q[12] == 0x07);                       
    CHECK(memcmp(&q[13], "example", 7) == 0);
    CHECK(q[20] == 0x03);                       
    CHECK(memcmp(&q[21], "com", 3) == 0);
    CHECK(q[24] == 0x00);                       
    CHECK(q[25] == 0x00 && q[26] == 0x01);      
    CHECK(q[27] == 0x00 && q[28] == 0x01);      
}


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


static void test_parse_response_detects_pointer_loop()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    
    msg[29] = 0xC0;
    msg[30] = 0x1D;   

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());
}


static void test_parse_response_detects_out_of_bounds_pointer()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[29] = 0xC0;
    msg[30] = 0xFF;   

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());
}


static void test_parse_response_rejects_truncated_message()
{
    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kResponseExampleCom, 5,
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());

    
    CHECK(!DnsMessage::ParseResponse(kResponseExampleCom,
                                     sizeof(kResponseExampleCom) - 2,
                                     0x1234, addrs, ttl, err));
}


static void test_parse_response_reports_rcode()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[3] = 0x83;   

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(err.find("3") != std::string::npos);
}


static void test_parse_response_skips_non_address_records()
{
    
    std::vector<uint8_t> msg = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        
        0xC0, 0x0C, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C,
        0x00, 0x06, 0x03, 'w', 'w', 'w', 0xC0, 0x0C,
        
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
        0x00, 0x04, 0x01, 0x02, 0x03, 0x04,
    };

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(DnsMessage::ParseResponse(msg.data(), msg.size(),
                                    0x1234, addrs, ttl, err));
    CHECK(addrs.size() == 1);       
    CHECK(ttl == 60);               
}


static void test_parse_response_rejects_bad_rdlength()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[39] = 0x00;
    msg[40] = 0x10;   

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    DnsMessage::ParseResponse(msg.data(), msg.size(), 0x1234, addrs, ttl, err);
    CHECK(addrs.empty());
}








static void test_parse_response_rejects_qr_zero()
{
    std::vector<uint8_t> msg(kResponseExampleCom,
                             kResponseExampleCom + sizeof(kResponseExampleCom));
    msg[2] = 0x01;   

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(msg.data(), msg.size(),
                                     0x1234, addrs, ttl, err));
    CHECK(!err.empty());
    CHECK(addrs.empty());
}




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




static void test_parse_response_clears_addrs_on_failure()
{
    static const uint8_t kMsg[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C,
        0x00, 0x04, 0x5D, 0xB8, 0xD8, 0x22,
        
        0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0A,
        0x00, 0x10,
    };

    std::vector<BCSockAddrS> addrs;
    uint32_t ttl = 0;
    std::string err;

    CHECK(!DnsMessage::ParseResponse(kMsg, sizeof(kMsg), 0x1234, addrs, ttl, err));
    CHECK(!err.empty());
    CHECK(addrs.empty());   
}











static void test_resolve_force_physical_requires_interface()
{
    DnsConfig cfg;
    cfg.policy  = TT_VPN_POLICY_FORCE_PHYSICAL;
    cfg.ifIndex = 0;   

    DnsResult r = DnsResolver::Resolve("example.com", cfg);

    CHECK(r.result == BC_R_NO_PHYSICAL_INTERFACE);
    CHECK(r.addrs.empty());
    CHECK(!r.errMessage.empty());
}






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
