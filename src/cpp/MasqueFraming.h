///////////////////////////////////////////////////////////////////////////////
// file : MasqueFraming.h
// author : anto
//
// RFC 9298 (Proxying UDP in HTTP) + RFC 9297 (HTTP Datagrams) wire framing
// helpers used by the MASQUE CONNECT-UDP proxy path. These are deliberately
// self-contained (no xquic / BC dependency) so they can be unit-tested in
// isolation.
//
// On the wire, every UDP packet tunnelled through a CONNECT-UDP request is
// carried as the payload of a QUIC DATAGRAM frame. xquic exposes the raw
// DATAGRAM payload to us untouched, so we are responsible for the RFC 9297
// HTTP Datagram framing ourselves:
//
//   HTTP/3 Datagram payload (what we hand to xqc_h3_ext_datagram_send):
//     Quarter Stream ID (varint)   -- CONNECT request stream id / 4
//     HTTP Datagram Payload:
//       Context ID (varint)        -- 0 = UDP packet  (RFC 9298 §5)
//       UDP Proxying Payload       -- the inner QUIC packet bytes
//
///////////////////////////////////////////////////////////////////////////////
#ifndef MASQUE_FRAMING_H_INCLUDED__
#define MASQUE_FRAMING_H_INCLUDED__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace masque {

// Context ID 0 designates a UDP packet payload (RFC 9298 §5). The base
// CONNECT-UDP protocol only ever uses context 0; other contexts require the
// (unsupported here) capsule registration extension.
static const uint64_t kUdpContextId = 0;

///////////////////////////////////////////////////////////////////////////////
// QUIC variable-length integers (RFC 9000 §16).
///////////////////////////////////////////////////////////////////////////////

// Number of bytes the varint encoding of `value` occupies (1, 2, 4 or 8).
size_t VarintLen(uint64_t value);

// Encode `value` into `out` (capacity `cap`). Returns the number of bytes
// written, or 0 if `cap` is too small.
size_t VarintEncode(uint8_t* out, size_t cap, uint64_t value);

// Decode a varint from `in` (length `len`). On success returns the number of
// bytes consumed and writes the decoded value to *out_value. Returns 0 if the
// buffer is too short for the encoded length.
size_t VarintDecode(const uint8_t* in, size_t len, uint64_t* out_value);

///////////////////////////////////////////////////////////////////////////////
// Quarter Stream ID (RFC 9297 §2.1): the CONNECT request runs on a
// client-initiated bidirectional stream, whose id is always a multiple of 4.
///////////////////////////////////////////////////////////////////////////////
inline uint64_t QuarterStreamId(uint64_t stream_id) { return stream_id / 4; }
inline uint64_t StreamIdFromQuarter(uint64_t quarter) { return quarter * 4; }

///////////////////////////////////////////////////////////////////////////////
// CONNECT-UDP request target.
///////////////////////////////////////////////////////////////////////////////

// Percent-encode a single path segment, escaping everything outside the
// unreserved set (RFC 3986 §2.3). Used for the host/port segments of the
// connect-udp URI template.
std::string PercentEncodeSegment(const std::string& s);

// Build the :path pseudo-header value for a CONNECT-UDP request, per the
// RFC 9298 §3 URI template /.well-known/masque/udp/{target_host}/{target_port}/.
// `host` may be a reg-name, IPv4 literal, or bare IPv6 literal (no brackets).
std::string BuildConnectUdpPath(const std::string& host, uint16_t port);

///////////////////////////////////////////////////////////////////////////////
// HTTP Datagram payload framing for CONNECT-UDP (context 0).
///////////////////////////////////////////////////////////////////////////////

// Byte overhead added in front of the inner UDP payload for a given quarter
// stream id: varint(qsid) + varint(context-id 0).
size_t DatagramOverhead(uint64_t quarter_stream_id);

// Encode [Quarter-Stream-ID][Context-ID=0][payload] into `out` (resized to
// fit). Returns true on success.
bool EncodeDatagram(uint64_t quarter_stream_id,
                    const uint8_t* payload,
                    size_t payload_len,
                    std::vector<uint8_t>& out);

// Decode an inbound HTTP Datagram payload. On success sets *quarter_stream_id
// and *context_id, points *payload at the start of the UDP payload inside
// `in`, sets *payload_len, and returns true. Returns false on a malformed /
// truncated header.
bool DecodeDatagram(const uint8_t* in,
                    size_t len,
                    uint64_t* quarter_stream_id,
                    uint64_t* context_id,
                    const uint8_t** payload,
                    size_t* payload_len);

} // namespace masque

#endif // MASQUE_FRAMING_H_INCLUDED__

///////////////////////////////////////////////////////////////////////////////
// End of file : MasqueFraming.h
///////////////////////////////////////////////////////////////////////////////
