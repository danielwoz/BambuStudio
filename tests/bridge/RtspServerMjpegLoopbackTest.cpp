// Bambu Bridge — RtspServer MJPEG loopback integration test (ship-11d).
//
// Mirror of RtspServerLoopbackTest, but the bound ICameraSource emits raw
// JPEG frames (codec=MotionJpeg) instead of H.264 NAL units. Asserts:
//
//   - DESCRIBE SDP advertises rtpmap:26 JPEG/90000 (NOT H264/96)
//   - SETUP + PLAY drive the streaming loop into the JPEG packetiser
//   - At least 3 interleaved RTP frames arrive within 2s
//   - Each frame's RTP header has V=2, PT=26
//   - The first frame's RTP-JPEG payload header reports the source's
//     dimensions (16x16 → width=2,height=2 in /8-px units) and Q=255,
//     and the first packet carries a Quantization Table header that
//     matches the source's quant table
//
// Endpoint selection mirrors the H.264 test (127.0.0.4:38323 → 127.0.0.1:0
// → skip 77).
//
// The stub source produces the same valid-shape synthetic JFIF the
// RtspJpegPacketiserTest exercises — a 16x16 frame with one 64-byte DQT
// and 12 entropy-coded scan bytes — repeated at ~30fps.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "server/ICameraSource.hpp"
#include "server/RtspServer.hpp"
#include "tls/CertFactory.hpp"
#include "support/RtspTestClient.hpp"

using Slic3r::bridge::server::ICameraSource;
using Slic3r::bridge::server::RtspServer;
using Slic3r::bridge::server::RtspServerConfig;
using Slic3r::bridge::server::RtspVirtualDevice;
using Slic3r::bridge::server::VideoFrame;
using Slic3r::bridge::test::RtpFrame;
using Slic3r::bridge::test::RtspResponse;
using Slic3r::bridge::test::RtspTestClient;
using Slic3r::bridge::tls::CertFactory;
using Slic3r::bridge::tls::CertFactoryConfig;

namespace {

constexpr int kCtestSkip = 77;
int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// Stub MJPEG camera source — emits a fixed synthetic JFIF on every
// next_frame() call, with monotonically advancing pts.
class StubMjpegSource : public ICameraSource {
public:
    StubMjpegSource() {
        m_info.codec  = Codec::MotionJpeg;
        m_info.width  = 16;
        m_info.height = 16;
        m_info.fps    = 30;
        build_jfif();
    }
    bool open() override   { m_open.store(true); return true; }
    void close() override  { m_open.store(false); }
    bool is_open() const override { return m_open.load(); }
    std::optional<VideoFrame> next_frame(int /*timeout_ms*/) override {
        if (!m_open.load()) return std::nullopt;
        VideoFrame f;
        f.nal_data = m_jfif;
        f.pts_us   = m_pts;
        m_pts += 33000;
        return f;
    }
    StreamInfo info() const override { return m_info; }

    // Test-accessible quant table bytes for byte-level assertions.
    static std::vector<uint8_t> expected_qtable() {
        std::vector<uint8_t> q;
        for (int i = 0; i < 64; ++i) q.push_back(static_cast<uint8_t>(i + 1));
        return q;
    }

private:
    void build_jfif() {
        auto u8    = [&](uint8_t v){ m_jfif.push_back(v); };
        auto u16be = [&](uint16_t v){
            m_jfif.push_back(static_cast<uint8_t>(v >> 8));
            m_jfif.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        u8(0xFF); u8(0xD8);                       // SOI
        u8(0xFF); u8(0xDB); u16be(67);            // DQT
        u8(0x00);
        for (int i = 0; i < 64; ++i) u8(static_cast<uint8_t>(i + 1));
        u8(0xFF); u8(0xC0); u16be(11);            // SOF0
        u8(8); u16be(16); u16be(16); u8(1);
        u8(1); u8(0x21); u8(0);                    // C=1, H=2 V=1 (yuv422)
        u8(0xFF); u8(0xDA); u16be(8);             // SOS
        u8(1); u8(1); u8(0x00); u8(0); u8(63); u8(0);
        // 12 entropy-coded bytes.
        u8(0x01); u8(0x02); u8(0x03); u8(0x04); u8(0x05); u8(0x06);
        u8(0xFF); u8(0x00); u8(0xFF); u8(0xD0); u8(0x07); u8(0x08);
        u8(0xFF); u8(0xD9);                        // EOI
    }
    std::atomic<bool>     m_open{false};
    std::vector<uint8_t>  m_jfif;
    int64_t               m_pts = 0;
    StreamInfo            m_info;
};

struct TestCertHolder {
    Slic3r::bridge::tls::CertMaterial cert;
    std::filesystem::path             dir;
};

TestCertHolder mint_test_cert(const std::string& dev_id) {
    TestCertHolder h;
    h.dir = std::filesystem::temp_directory_path() /
            ("bambu-bridge-rtsp-mjpeg-" + std::to_string(::getpid()));
    std::filesystem::create_directories(h.dir);
    CertFactoryConfig cfg;
    cfg.cache_dir = h.dir;
    CertFactory factory(cfg);
    h.cert = factory.get_or_create(dev_id);
    return h;
}

std::unique_ptr<RtspServer> try_start(const std::string& bind_ip,
                                      uint16_t port,
                                      const RtspVirtualDevice& dev,
                                      uint16_t& bound_out) {
    RtspServerConfig cfg;
    cfg.io_timeout_seconds       = 30;
    cfg.rtp_max_payload          = 1400;
    cfg.max_sessions_per_device  = 4;
    auto srv = std::make_unique<RtspServer>(cfg);
    RtspVirtualDevice d = dev;
    d.lan_ip = bind_ip;
    d.port   = port;
    try {
        srv->add_device(d);
        srv->start();
    } catch (const std::exception&) {
        return nullptr;
    }
    bound_out = srv->bound_port(d.dev_id);
    if (bound_out == 0) { srv->stop(); return nullptr; }
    return srv;
}

} // namespace

int main() {
    const std::string dev_id = "0938BC582502MJP";
    auto cert_holder         = mint_test_cert(dev_id);
    auto source              = std::make_shared<StubMjpegSource>();

    RtspVirtualDevice dev;
    dev.dev_id      = dev_id;
    dev.access_code = "MJPEG999";
    dev.cert        = cert_holder.cert;
    dev.source      = source;

    std::string bind_ip = "127.0.0.4";
    uint16_t    bound   = 0;
    auto srv = try_start(bind_ip, 38323, dev, bound);
    if (!srv) {
        bind_ip = "127.0.0.1";
        srv = try_start(bind_ip, 0, dev, bound);
    }
    if (!srv) {
        std::fprintf(stderr, "SKIP: couldn't bind RtspServer on loopback\n");
        return kCtestSkip;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    RtspTestClient c;
    if (!c.connect(bind_ip, bound)) {
        std::fprintf(stderr, "SKIP: TLS connect failed: %s\n",
                     c.last_error().c_str());
        srv->stop();
        return kCtestSkip;
    }

    const std::string base = "rtsp://" + bind_ip + ":" +
                             std::to_string(bound) + "/streaming/live/1";

    // ----- DESCRIBE: SDP must advertise JPEG, not H264 -------------------
    RtspResponse r;
    check(c.describe(base, r),        "DESCRIBE sent");
    check(r.code == 200,              "DESCRIBE -> 200");
    {
        check(r.body.find("m=video 0 RTP/AVP 26") != std::string::npos,
              "SDP m=video uses PT 26 (JPEG)");
        check(r.body.find("rtpmap:26 JPEG/90000") != std::string::npos,
              "SDP advertises JPEG/90000");
        check(r.body.find("H264") == std::string::npos,
              "SDP does NOT mention H264");
    }

    // ----- SETUP / PLAY -------------------------------------------------
    const std::string track = base + "/streamid=0";
    check(c.setup(track, 0, 1, r),    "SETUP sent");
    check(r.code == 200,              "SETUP -> 200");
    check(c.play(base, r),            "PLAY sent");
    check(r.code == 200,              "PLAY -> 200");

    // ----- Drain RTP frames ---------------------------------------------
    std::vector<RtpFrame> frames;
    c.read_interleaved_rtp(frames, /*max_frames=*/8,
                           /*deadline=*/std::chrono::seconds(2));
    check(frames.size() >= 3,         "received at least 3 MJPEG RTP frames");

    // RTP-level: every frame must be V=2, PT=26 on channel 0.
    int rtp_ok = 0;
    for (const auto& f : frames) {
        if (f.data.size() < 12) continue;
        const uint8_t ver = (f.data[0] >> 6) & 0x03;
        const uint8_t pt  = f.data[1] & 0x7F;
        if (ver == 2 && pt == 26 && f.channel == 0) ++rtp_ok;
    }
    check(rtp_ok >= 3,                "RTP V=2, PT=26 on >=3 frames");

    // ----- Inspect the first frame's RTP-JPEG header --------------------
    // Test JPEG is small (<200 B incl all overhead) so it fits in a single
    // RTP packet under the 1400-byte budget. Header layout after the 12-B
    // RTP header:
    //   off 12: type-specific (0)
    //   off 13..15: fragment offset (0)
    //   off 16: type (0 == yuv422)
    //   off 17: Q (255)
    //   off 18: width / 8 (=2 for 16px)
    //   off 19: height / 8 (=2 for 16px)
    //   off 20..23: QT header (MBZ, precision=0, length BE=64)
    //   off 24..87: 64 quant table bytes (1..64)
    //   off 88..99: 12 scan bytes
    if (!frames.empty() && frames[0].data.size() >= 100) {
        const auto& p = frames[0].data;
        check(p[12] == 0x00,          "type-specific = 0");
        check(p[13] == 0 && p[14] == 0 && p[15] == 0,
                                       "fragment offset = 0");
        check(p[16] == 0,             "type = 0 (yuv422)");
        check(p[17] == 255,           "Q = 255");
        check(p[18] == 2,             "width field = 2");
        check(p[19] == 2,             "height field = 2");
        check(p[20] == 0 && p[21] == 0,
                                       "QT MBZ=0, precision=0");
        check(p[22] == 0 && p[23] == 64,
                                       "QT length = 64");
        auto q = StubMjpegSource::expected_qtable();
        bool qt_ok = true;
        for (int i = 0; i < 64; ++i) {
            if (p[24 + i] != q[i]) { qt_ok = false; break; }
        }
        check(qt_ok,                  "QT data matches source DQT");
        // Marker bit MUST be set on the last (and here only) packet of frame.
        check((p[1] & 0x80) != 0,     "M bit set on single-packet frame");
    } else {
        check(false,                  "first frame too short to inspect");
    }

    // ----- TEARDOWN -----------------------------------------------------
    check(c.teardown(base, r),        "TEARDOWN sent");
    check(r.code == 200,              "TEARDOWN -> 200");

    c.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    srv->stop();

    if (g_fails) {
        std::fprintf(stderr, "RtspServerMjpegLoopbackTest: %d failure(s)\n",
                     g_fails);
        return 1;
    }
    std::printf("RtspServerMjpegLoopbackTest: ok\n");
    return 0;
}
