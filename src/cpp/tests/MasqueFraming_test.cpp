///////////////////////////////////////////////////////////////////////////////
// file : MasqueFraming_test.cpp
//
// Standalone unit test for the MASQUE framing helpers. Not part of any build
// target; compile & run manually:
//
//   c++ -std=c++17 -I src/cpp src/cpp/MasqueFraming.cpp \
//       src/cpp/tests/MasqueFraming_test.cpp -o /tmp/masque_test && /tmp/masque_test
//
///////////////////////////////////////////////////////////////////////////////
#include "MasqueFraming.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace masque;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void test_varint_roundtrip()
{
    const uint64_t vals[] = {
        0, 1, 0x3F, 0x40, 0x3FFF, 0x4000, 0x3FFFFFFF, 0x40000000,
        0x3FFFFFFFFFFFFFFFULL, 151288809941952652ULL,
    };
    const size_t expect_len[] = {1, 1, 1, 2, 2, 4, 4, 8, 8, 8};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        uint8_t buf[8];
        size_t n = VarintEncode(buf, sizeof(buf), vals[i]);
        CHECK(n == expect_len[i]);
        uint64_t out = ~0ULL;
        size_t m = VarintDecode(buf, n, &out);
        CHECK(m == n);
        CHECK(out == vals[i]);
    }
}

static void test_varint_known_vectors()
{
    // RFC 9000 Appendix A.1 sample encodings.
    uint8_t b1[] = {0xC2, 0x19, 0x7C, 0x5E, 0xFF, 0x14, 0xE8, 0x8C};
    uint64_t v = 0;
    size_t n = VarintDecode(b1, sizeof(b1), &v);
    CHECK(n == 8);
    CHECK(v == 151288809941952652ULL);

    uint8_t b2[] = {0x40, 0x25};
    n = VarintDecode(b2, sizeof(b2), &v);
    CHECK(n == 2);
    CHECK(v == 37);

    uint8_t b3[] = {0x25};
    n = VarintDecode(b3, sizeof(b3), &v);
    CHECK(n == 1);
    CHECK(v == 37);
}

static void test_varint_truncated()
{
    uint8_t buf[] = {0x80, 0x00}; // claims 4 bytes, only 2 present
    uint64_t v = 0;
    CHECK(VarintDecode(buf, sizeof(buf), &v) == 0);
    CHECK(VarintDecode(buf, 0, &v) == 0);
}

static void test_datagram_roundtrip()
{
    const uint8_t inner[] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22};
    uint64_t stream_id = 0;       // first client bidi stream
    uint64_t qsid = QuarterStreamId(stream_id);

    std::vector<uint8_t> wire;
    CHECK(EncodeDatagram(qsid, inner, sizeof(inner), wire));
    // qsid=0 -> 1 byte, context=0 -> 1 byte.
    CHECK(wire.size() == 2 + sizeof(inner));
    CHECK(wire[0] == 0x00);
    CHECK(wire[1] == 0x00);

    uint64_t got_qsid = ~0ULL, got_ctx = ~0ULL;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    CHECK(DecodeDatagram(wire.data(), wire.size(), &got_qsid, &got_ctx,
                         &payload, &payload_len));
    CHECK(got_qsid == qsid);
    CHECK(got_ctx == kUdpContextId);
    CHECK(payload_len == sizeof(inner));
    CHECK(memcmp(payload, inner, sizeof(inner)) == 0);
}

static void test_datagram_large_qsid()
{
    uint64_t stream_id = 4 * 0x4000; // qsid 0x4000 -> 4-byte varint
    uint64_t qsid = QuarterStreamId(stream_id);
    CHECK(DatagramOverhead(qsid) == 4 /*qsid*/ + 1 /*ctx*/);

    const uint8_t inner[] = {0x01, 0x02};
    std::vector<uint8_t> wire;
    CHECK(EncodeDatagram(qsid, inner, sizeof(inner), wire));
    uint64_t got_qsid = 0, got_ctx = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    CHECK(DecodeDatagram(wire.data(), wire.size(), &got_qsid, &got_ctx,
                         &payload, &payload_len));
    CHECK(got_qsid == qsid);
    CHECK(payload_len == sizeof(inner));
}

static void test_connect_udp_path()
{
    CHECK(BuildConnectUdpPath("example.com", 443) ==
          "/.well-known/masque/udp/example.com/443/");
    CHECK(BuildConnectUdpPath("192.0.2.1", 4433) ==
          "/.well-known/masque/udp/192.0.2.1/4433/");
    // bare IPv6 literal: ':' is not unreserved -> percent-encoded.
    CHECK(BuildConnectUdpPath("[2001:db8::1]", 443) ==
          "/.well-known/masque/udp/2001%3Adb8%3A%3A1/443/");
}

int main()
{
    test_varint_roundtrip();
    test_varint_known_vectors();
    test_varint_truncated();
    test_datagram_roundtrip();
    test_datagram_large_qsid();
    test_connect_udp_path();
    if (g_failures == 0) {
        printf("MasqueFraming: all tests passed\n");
        return 0;
    }
    printf("MasqueFraming: %d failure(s)\n", g_failures);
    return 1;
}
