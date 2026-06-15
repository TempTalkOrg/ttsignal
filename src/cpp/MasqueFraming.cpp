///////////////////////////////////////////////////////////////////////////////
// file : MasqueFraming.cpp
// author : anto
///////////////////////////////////////////////////////////////////////////////
#include "MasqueFraming.h"

#include <cstdio>
#include <cstring>

namespace masque {

///////////////////////////////////////////////////////////////////////////////
// QUIC varint (RFC 9000 §16)
///////////////////////////////////////////////////////////////////////////////

size_t VarintLen(uint64_t value)
{
    if (value <= 0x3F) {
        return 1;
    } else if (value <= 0x3FFF) {
        return 2;
    } else if (value <= 0x3FFFFFFF) {
        return 4;
    }
    return 8;
}

size_t VarintEncode(uint8_t* out, size_t cap, uint64_t value)
{
    size_t len = VarintLen(value);
    if (cap < len) {
        return 0;
    }
    switch (len) {
    case 1:
        out[0] = (uint8_t)value;                 // 00xxxxxx
        break;
    case 2:
        out[0] = (uint8_t)(0x40 | (value >> 8)); // 01xxxxxx
        out[1] = (uint8_t)value;
        break;
    case 4:
        out[0] = (uint8_t)(0x80 | (value >> 24)); // 10xxxxxx
        out[1] = (uint8_t)(value >> 16);
        out[2] = (uint8_t)(value >> 8);
        out[3] = (uint8_t)value;
        break;
    default: // 8
        out[0] = (uint8_t)(0xC0 | (value >> 56)); // 11xxxxxx
        out[1] = (uint8_t)(value >> 48);
        out[2] = (uint8_t)(value >> 40);
        out[3] = (uint8_t)(value >> 32);
        out[4] = (uint8_t)(value >> 24);
        out[5] = (uint8_t)(value >> 16);
        out[6] = (uint8_t)(value >> 8);
        out[7] = (uint8_t)value;
        break;
    }
    return len;
}

size_t VarintDecode(const uint8_t* in, size_t len, uint64_t* out_value)
{
    if (len < 1) {
        return 0;
    }
    // The two most-significant bits of the first byte encode the length:
    // 00 -> 1 byte, 01 -> 2, 10 -> 4, 11 -> 8.
    size_t enc_len = (size_t)1 << (in[0] >> 6);
    if (len < enc_len) {
        return 0;
    }
    uint64_t value = in[0] & 0x3F;
    for (size_t i = 1; i < enc_len; i++) {
        value = (value << 8) | in[i];
    }
    if (out_value) {
        *out_value = value;
    }
    return enc_len;
}

///////////////////////////////////////////////////////////////////////////////
// CONNECT-UDP request target
///////////////////////////////////////////////////////////////////////////////

static inline bool IsUnreserved(char c)
{
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '.' || c == '_' || c == '~';
}

std::string PercentEncodeSegment(const std::string& s)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (IsUnreserved((char)c)) {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

std::string BuildConnectUdpPath(const std::string& host, uint16_t port)
{
    std::string h = host;
    // Accept a bracketed IPv6 literal "[::1]" and strip the brackets: the URI
    // template expects the bare address with ':' percent-encoded.
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') {
        h = h.substr(1, h.size() - 2);
    }
    char port_buf[8];
    snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)port);
    std::string path = "/.well-known/masque/udp/";
    path += PercentEncodeSegment(h);
    path += "/";
    path += PercentEncodeSegment(port_buf);
    path += "/";
    return path;
}

///////////////////////////////////////////////////////////////////////////////
// HTTP Datagram payload framing (RFC 9297 + RFC 9298, context 0)
///////////////////////////////////////////////////////////////////////////////

size_t DatagramOverhead(uint64_t quarter_stream_id)
{
    return VarintLen(quarter_stream_id) + VarintLen(kUdpContextId);
}

bool EncodeDatagram(uint64_t quarter_stream_id,
                    const uint8_t* payload,
                    size_t payload_len,
                    std::vector<uint8_t>& out)
{
    size_t overhead = DatagramOverhead(quarter_stream_id);
    out.resize(overhead + payload_len);
    uint8_t* p = out.data();
    size_t n = VarintEncode(p, overhead, quarter_stream_id);
    if (n == 0) {
        return false;
    }
    p += n;
    size_t m = VarintEncode(p, overhead - n, kUdpContextId);
    if (m == 0) {
        return false;
    }
    p += m;
    if (payload_len > 0 && payload != nullptr) {
        memcpy(p, payload, payload_len);
    }
    return true;
}

bool DecodeDatagram(const uint8_t* in,
                    size_t len,
                    uint64_t* quarter_stream_id,
                    uint64_t* context_id,
                    const uint8_t** payload,
                    size_t* payload_len)
{
    uint64_t qsid = 0;
    size_t n = VarintDecode(in, len, &qsid);
    if (n == 0) {
        return false;
    }
    uint64_t ctx = 0;
    size_t m = VarintDecode(in + n, len - n, &ctx);
    if (m == 0) {
        return false;
    }
    if (quarter_stream_id) {
        *quarter_stream_id = qsid;
    }
    if (context_id) {
        *context_id = ctx;
    }
    if (payload) {
        *payload = in + n + m;
    }
    if (payload_len) {
        *payload_len = len - n - m;
    }
    return true;
}

} // namespace masque

///////////////////////////////////////////////////////////////////////////////
// End of file : MasqueFraming.cpp
///////////////////////////////////////////////////////////////////////////////
