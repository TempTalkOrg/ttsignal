



















#ifndef MASQUE_FRAMING_H_INCLUDED__
#define MASQUE_FRAMING_H_INCLUDED__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace masque {




static const uint64_t kUdpContextId = 0;






size_t VarintLen(uint64_t value);



size_t VarintEncode(uint8_t* out, size_t cap, uint64_t value);




size_t VarintDecode(const uint8_t* in, size_t len, uint64_t* out_value);





inline uint64_t QuarterStreamId(uint64_t stream_id) { return stream_id / 4; }
inline uint64_t StreamIdFromQuarter(uint64_t quarter) { return quarter * 4; }








std::string PercentEncodeSegment(const std::string& s);




std::string BuildConnectUdpPath(const std::string& host, uint16_t port);







size_t DatagramOverhead(uint64_t quarter_stream_id);



bool EncodeDatagram(uint64_t quarter_stream_id,
                    const uint8_t* payload,
                    size_t payload_len,
                    std::vector<uint8_t>& out);





bool DecodeDatagram(const uint8_t* in,
                    size_t len,
                    uint64_t* quarter_stream_id,
                    uint64_t* context_id,
                    const uint8_t** payload,
                    size_t* payload_len);

} 

#endif 




