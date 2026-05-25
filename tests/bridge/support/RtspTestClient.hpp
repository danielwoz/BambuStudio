// Bambu Bridge — minimal TLS RTSP client for tests (phase 8).
//
// Hand-rolled OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN issuance over
// an OpenSSL TLS socket. Drives the interleaved RTP-over-RTSP transport
// (RTP/AVP/TCP;interleaved=0-1) and parses incoming `$<chan><len><payload>`
// frames into a queue the test can assert on.
//
// Not a general-purpose RTSP client — it does exactly what
// `tests/bridge/RtspServerLoopbackTest.cpp` needs:
//   - Implicit TLS 1.2 connect on the RTSP port (real Bambu cameras are
//     RTSPS — TLS on first byte, no upgrade).
//   - One-track session only.
//   - RTSP requests issued with a monotonic CSeq; responses parsed by
//     reading until "\r\n\r\n" plus Content-Length body.
//   - read_interleaved_rtp() drains zero or more frames off the socket
//     up to a deadline.

#ifndef SLIC3R_BAMBU_BRIDGE_TEST_RTSP_CLIENT_HPP
#define SLIC3R_BAMBU_BRIDGE_TEST_RTSP_CLIENT_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace test {

struct RtpFrame {
    uint8_t                channel = 0;
    std::vector<uint8_t>   data;   // raw 12-byte RTP header + payload
};

struct RtspResponse {
    int                                            code = -1;
    std::string                                    status;
    std::unordered_map<std::string, std::string>   headers; // lower-cased keys
    std::string                                    body;
};

class RtspTestClient {
public:
    RtspTestClient();
    ~RtspTestClient();

    RtspTestClient(const RtspTestClient&)            = delete;
    RtspTestClient& operator=(const RtspTestClient&) = delete;

    // TCP + implicit-TLS connect on (host, port). Returns false on failure.
    bool connect(const std::string& host, uint16_t port,
                 std::chrono::seconds timeout = std::chrono::seconds(5));

    // Issue an RTSP request. `extra_headers` is one "Key: Value\r\n" line
    // per entry (no trailing CRLF — the client appends). Returns false on
    // socket / TLS error; on success, fills `resp`.
    bool request(const std::string& verb,
                 const std::string& target,
                 const std::vector<std::string>& extra_headers,
                 RtspResponse& resp);

    // Convenience wrappers. Each bumps CSeq and uses the current session
    // (after SETUP) when needed.
    bool options    (const std::string& target, RtspResponse& resp);
    bool describe   (const std::string& target, RtspResponse& resp);
    bool setup      (const std::string& target,
                     int interleaved_lo, int interleaved_hi,
                     RtspResponse& resp);
    bool play       (const std::string& target, RtspResponse& resp);
    bool teardown   (const std::string& target, RtspResponse& resp);

    // After PLAY, drain RTP frames off the socket until `deadline` elapses
    // OR `max_frames` have been received. RTSP responses interleaved in
    // the stream are silently swallowed (ffplay does the same).
    int read_interleaved_rtp(std::vector<RtpFrame>& out,
                             int max_frames,
                             std::chrono::seconds deadline);

    void close();

    const std::string& session_id()  const { return m_session_id; }
    const std::string& last_error()  const { return m_last_error; }

private:
    // Read at least one byte into m_recv with `timeout`. Returns false on
    // EOF / TLS error / timeout.
    bool read_some(std::chrono::seconds timeout);

    // Parse and consume one full RTSP response off m_recv. Returns true
    // on success.
    bool consume_one_response(RtspResponse& out);

    int          m_fd  = -1;
    void*        m_ssl = nullptr;
    void*        m_ctx = nullptr;
    std::vector<uint8_t> m_recv;
    int          m_cseq      = 1;
    std::string  m_session_id;
    std::string  m_last_error;
};

} // namespace test
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_TEST_RTSP_CLIENT_HPP
