// Bambu Bridge — RFC 2435 RTP/JPEG packetiser test seam.
//
// The production packetiser lives in `RtspServer.cpp` (anonymous namespace,
// inlined into the streaming loop). This header exposes the two pieces that
// are worth unit-testing in isolation:
//
//   - `RtpJpegParse parse_jpeg_frame(buf)`: parses a JFIF buffer and
//     surfaces width/height/sampling-type + concatenated quant tables +
//     entropy-coded scan-data span. The same parser that `RtspServer`
//     runs per-frame on incoming MotionJpeg `VideoFrame`s.
//
//   - `make_rtp_jpeg_packets(...)`: produces the in-memory byte form of
//     the RTP packets a single frame would generate, WITHOUT the TLS write
//     side. Tests can assert on header bytes (type, Q, dimensions, frag
//     offset, M bit) and total packet count.
//
// This header is server-internal — slicer code never includes it. The
// definitions live in `RtspServer.cpp` next to the production wire
// implementation; this header just makes them accessible to unit tests.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_RTSP_JPEG_PACKETISER_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_RTSP_JPEG_PACKETISER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace server {

struct RtpJpegParse {
    bool                  ok          = false;
    int                   width       = 0;     // pixels
    int                   height      = 0;
    uint8_t               type        = 1;     // 0=yuv422, 1=yuv420
    std::vector<uint8_t>  qtables;             // concatenated 64-byte 8-bit DQTs
    std::size_t           scan_off    = 0;     // entropy-coded scan start in input
    std::size_t           scan_len    = 0;     // entropy-coded scan length
};

// Parse a JFIF buffer. See RtspServer.cpp for details. ok=true iff
// dimensions, scan data, and at least one quant table were found.
RtpJpegParse rtp_jpeg_parse(const uint8_t* data, std::size_t n);

// Build the in-memory wire form of the RTP packets that would be sent for
// one MJPEG frame. Returned vector contains, in order, the full RTP packet
// payloads (RTP header + RTP-JPEG header + optional QT header + scan bytes)
// — same bytes as would be written after the 4-byte interleaved-frame
// preamble on the TLS socket.
//
// `seq` is taken by reference and advanced per packet (matches the
// production helper). `max_payload` caps each packet's payload (RTP-JPEG
// header + QT header + scan bytes); RTP header is added on top.
std::vector<std::vector<uint8_t>>
rtp_jpeg_build_packets(uint16_t& seq, uint32_t ts, uint32_t ssrc,
                       const RtpJpegParse& jp,
                       const uint8_t* scan, std::size_t scan_len,
                       std::size_t max_payload);

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_RTSP_JPEG_PACKETISER_HPP
