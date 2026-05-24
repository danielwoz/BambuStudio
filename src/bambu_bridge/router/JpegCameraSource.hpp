// Bambu Bridge — A1 / P1 native JPEG-on-port-6000 camera source.
//
// A1 and P1 series printers expose a TLS-on-TCP-6000 server that streams
// 1280x720 JPEG frames. The protocol is small and self-contained — no
// proprietary plugin needed. Documented in OpenBambuAPI/video.md:
//
//   TCP connect to <printer-ip>:6000, wrap with TLS 1.2 (verify=0,
//   cipher AES256-GCM-SHA384, EMPTY SNI — matches LocalControlTunnel).
//
//   Send a single 80-byte auth packet:
//     bytes 0..3  : u32 little-endian payload size (0x00000040)
//     bytes 4..7  : u32 little-endian type (0x00003000)
//     bytes 8..11 : u32 flags (0)
//     bytes 12..15: u32 reserved (0)
//     bytes 16..47: username "bblp" right-padded with NUL to 32 bytes
//     bytes 48..79: password (LAN access code) right-padded with NUL to 32 bytes
//
//   Server then streams frames as:
//     16-byte little-endian header:
//       bytes 0..3  : u32 payload size (JPEG byte count to follow)
//       bytes 4..7  : u32 itrack (0)
//       bytes 8..11 : u32 flags (1)
//       bytes 12..15: u32 reserved (0)
//     payload_size bytes of JPEG data, starting with FF D8 and ending with FF D9.
//
// X1/H2 series ALSO listen on port 6000 but speak the BambuTunnel control
// protocol (see LocalControlTunnel.hpp). A1/P1 do not; selecting the right
// source happens upstream via `is_jpeg_camera_model(model)`.
//
// Threading: same contract as LanCameraSource / NullCameraSource —
// `next_frame()` is single-threaded per source; `open` / `close` /
// `is_open` are safe to call concurrently with `next_frame`.
//
// Frame surface: `VideoFrame::nal_data` carries the RAW JPEG payload
// (not Annex-B H.264) and `StreamInfo::codec` is reported as
// `Codec::MotionJpeg`. Downstream consumers (`RtspServer`) must inspect
// `codec` before treating `nal_data` as H.264; until the RtspServer
// gains an RFC-2435 (RTP-over-JPEG) packetiser, an MJPEG source plugged
// into the H.264-only RTSP path will be advertised in SDP as JPEG and
// most slicer-side clients will refuse SETUP cleanly. The source itself
// is fully functional and unit-tested; the RTSP packetiser upgrade is
// tracked separately.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_JPEG_CAMERA_SOURCE_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_JPEG_CAMERA_SOURCE_HPP

#include "../server/ICameraSource.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

typedef struct ssl_st     SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace Slic3r {
namespace bridge {
namespace router {

struct JpegCameraSourceConfig {
    std::string dev_id;
    std::string printer_ip;
    uint16_t    printer_port = 6000;
    std::string access_code;
    std::string username = "bblp";
    // TCP+TLS connect deadline.
    std::chrono::milliseconds connect_timeout{5000};
    // Per-frame read timeout. The A1 camera emits ~10 fps; allow more for
    // a slow LAN. `next_frame(timeout_ms)` from RtspServer is consulted
    // first; this is the floor.
    std::chrono::milliseconds io_timeout{5000};
    // Optional: cap reconnect-on-failure retries inside `next_frame`. The
    // current implementation does NOT auto-reconnect — it returns nullopt
    // and lets the router teardown. Hook reserved for future use.
    int max_reconnect_attempts = 0;
};

// Free helper — returns true iff the bridge should select JpegCameraSource
// instead of LanCameraSource / CloudCameraSource for this vendor model
// string. Substring / token match, case-insensitive. See
// NativeStorageDelegate::model_is_native for the analogous storage gate.
//
// Matched models:
//   "A1", "A1 mini", "A1MINI", "N1" (A1 mini internal), "N2S" (A1 internal),
//   "P1S", "P1P", "C13" (P1P internal), "C14" (P1S internal).
//
// NOT matched (use H.264 sources instead):
//   X1*, H2*, anything containing those tokens.
bool is_jpeg_camera_model(const std::string& model);

// Free helper — short human tag ("A1", "A1mini", "P1S", "P1P") for log
// formatting. Returns the input verbatim if it doesn't normalise.
std::string jpeg_camera_model_tag(const std::string& model);

class JpegCameraSource : public server::ICameraSource {
public:
    explicit JpegCameraSource(JpegCameraSourceConfig cfg);
    ~JpegCameraSource() override;

    JpegCameraSource(const JpegCameraSource&)            = delete;
    JpegCameraSource& operator=(const JpegCameraSource&) = delete;

    // ---- ICameraSource ------------------------------------------------
    bool open() override;
    void close() override;
    bool is_open() const override;

    std::optional<server::VideoFrame> next_frame(int timeout_ms) override;
    server::ICameraSource::StreamInfo info() const override;

    const JpegCameraSourceConfig& config() const { return m_cfg; }

    // ---- Testing seams -------------------------------------------------
    //
    // Build the 80-byte auth packet from the configured `username` /
    // `access_code`. Exposed for unit tests so they can pin the byte
    // layout against OpenBambuAPI/video.md without mocking sockets.
    std::vector<uint8_t> build_auth_packet() const;

    // Parse a 16-byte frame header in-place. Returns true on success and
    // fills `out_payload_size`. Exposed for tests; the production
    // implementation calls this internally from next_frame().
    static bool parse_frame_header(const uint8_t* hdr16,
                                   uint32_t&      out_payload_size,
                                   uint32_t&      out_itrack,
                                   uint32_t&      out_flags);

protected:
    JpegCameraSourceConfig                                  m_cfg;

    mutable std::mutex                                      m_mu;
    std::atomic<bool>                                       m_open{false};

    // Owned TCP/TLS state. Held under m_mu for lifecycle ops; the streaming
    // thread reads/writes via these without holding the mutex (only one
    // next_frame() at a time per source).
    int                                                     m_fd      = -1;
    SSL_CTX*                                                m_ssl_ctx = nullptr;
    SSL*                                                    m_ssl     = nullptr;

    server::ICameraSource::StreamInfo                       m_info;

    // Scratch JPEG buffer reused across frames to avoid per-frame realloc.
    std::vector<uint8_t>                                    m_jpeg_scratch;
    int                                                     m_frames_seen = 0;
    std::chrono::steady_clock::time_point                   m_t0;

    // Internal helpers — virtual for unit-test seams. The "io_*" calls all
    // operate against m_fd / m_ssl unless overridden.
    virtual int  tcp_connect_(int timeout_ms);
    virtual int  tls_handshake_(int timeout_ms);
    virtual int  send_auth_(int timeout_ms);
    virtual int  io_read_full_(uint8_t* dst, std::size_t n, int timeout_ms);
    virtual int  io_write_full_(const uint8_t* src, std::size_t n, int timeout_ms);

    void close_locked_();
};

}  // namespace router
}  // namespace bridge
}  // namespace Slic3r

#endif  // SLIC3R_BAMBU_BRIDGE_ROUTER_JPEG_CAMERA_SOURCE_HPP
