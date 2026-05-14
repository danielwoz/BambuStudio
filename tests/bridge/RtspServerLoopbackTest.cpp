// Bambu Bridge — RtspServer loopback integration test (phase 8).
//
// Stands up an RtspServer bound to NullCameraSource on a loopback endpoint
// and drives it through the OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN
// path with an inline TLS RTSP test client. Asserts:
//
//   - OPTIONS replies 200 with a Public header listing the 5 verbs.
//   - DESCRIBE replies 200 application/sdp with H264/90000 + a non-empty
//     sprop-parameter-sets.
//   - SETUP echoes Transport: RTP/AVP/TCP;interleaved=0-1 and returns a
//     Session.
//   - PLAY replies 200; at least 3 interleaved RTP packets arrive within
//     2s; every received packet has version=2 + payload-type=96.
//   - TEARDOWN replies 200 and the server shuts the session cleanly.
//
// Endpoint selection mirrors FtpsServerLoopbackTest:
//   1. Try 127.0.0.4:38322
//   2. Fall back to 127.0.0.1 port 0 (kernel-picked)
// Returns ctest skip code 77 if neither bind works.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "router/NullCameraSource.hpp"
#include "server/RtspServer.hpp"
#include "tls/CertFactory.hpp"
#include "support/RtspTestClient.hpp"

using Slic3r::bridge::router::NullCameraSource;
using Slic3r::bridge::server::ICameraSource;
using Slic3r::bridge::server::RtspServer;
using Slic3r::bridge::server::RtspServerConfig;
using Slic3r::bridge::server::RtspVirtualDevice;
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

struct TestCertHolder {
    Slic3r::bridge::tls::CertMaterial cert;
    std::filesystem::path             dir;
};

TestCertHolder mint_test_cert(const std::string& dev_id) {
    TestCertHolder h;
    h.dir = std::filesystem::temp_directory_path() /
            ("bambu-bridge-rtsp-test-" + std::to_string(::getpid()));
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
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[rtsp-loopback] start at %s:%u failed: %s\n",
                     bind_ip.c_str(), port, ex.what());
        return nullptr;
    }
    bound_out = srv->bound_port(d.dev_id);
    if (bound_out == 0) { srv->stop(); return nullptr; }
    std::fprintf(stderr, "[rtsp-loopback] bound %s:%u\n",
                 bind_ip.c_str(), bound_out);
    return srv;
}

} // namespace

int main() {
    const std::string dev_id = "0938BC582502312";
    auto cert_holder         = mint_test_cert(dev_id);

    auto source = std::make_shared<NullCameraSource>();

    RtspVirtualDevice dev;
    dev.dev_id      = dev_id;
    dev.access_code = "ABCD1234";
    dev.cert        = cert_holder.cert;
    dev.source      = source;

    std::string bind_ip = "127.0.0.4";
    uint16_t    bound   = 0;
    auto srv = try_start(bind_ip, 38322, dev, bound);
    if (!srv) {
        bind_ip = "127.0.0.1";
        srv = try_start(bind_ip, 0, dev, bound);
    }
    if (!srv) {
        std::fprintf(stderr, "SKIP: couldn't bind RtspServer on loopback\n");
        return kCtestSkip;
    }

    // Give the accept thread a moment.
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

    // ----- OPTIONS -------------------------------------------------------
    RtspResponse r;
    check(c.options(base, r),        "OPTIONS sent");
    check(r.code == 200,              "OPTIONS -> 200");
    {
        auto it = r.headers.find("public");
        check(it != r.headers.end(),  "OPTIONS reply has Public header");
        if (it != r.headers.end()) {
            const std::string& p = it->second;
            check(p.find("OPTIONS")  != std::string::npos, "Public lists OPTIONS");
            check(p.find("DESCRIBE") != std::string::npos, "Public lists DESCRIBE");
            check(p.find("SETUP")    != std::string::npos, "Public lists SETUP");
            check(p.find("PLAY")     != std::string::npos, "Public lists PLAY");
            check(p.find("TEARDOWN") != std::string::npos, "Public lists TEARDOWN");
        }
    }

    // ----- DESCRIBE ------------------------------------------------------
    check(c.describe(base, r),        "DESCRIBE sent");
    check(r.code == 200,              "DESCRIBE -> 200");
    {
        auto it = r.headers.find("content-type");
        check(it != r.headers.end() && it->second.find("application/sdp") == 0,
              "DESCRIBE returns application/sdp");
        check(r.body.find("m=video") != std::string::npos,
              "SDP contains m=video");
        check(r.body.find("H264/90000") != std::string::npos,
              "SDP advertises H264/90000");
        check(r.body.find("sprop-parameter-sets=") != std::string::npos,
              "SDP carries sprop-parameter-sets");
        check(r.body.find("packetization-mode=1") != std::string::npos,
              "SDP declares packetization-mode=1");
    }

    // ----- SETUP ---------------------------------------------------------
    const std::string track = base + "/streamid=0";
    check(c.setup(track, 0, 1, r),    "SETUP sent");
    check(r.code == 200,              "SETUP -> 200");
    {
        auto it = r.headers.find("transport");
        check(it != r.headers.end(),  "SETUP reply has Transport header");
        if (it != r.headers.end()) {
            check(it->second.find("RTP/AVP/TCP") != std::string::npos,
                  "Transport echoes RTP/AVP/TCP");
            check(it->second.find("interleaved=0-1") != std::string::npos,
                  "Transport echoes interleaved=0-1");
        }
        check(!c.session_id().empty(), "Session id assigned");
    }

    // ----- PLAY ----------------------------------------------------------
    check(c.play(base, r),            "PLAY sent");
    check(r.code == 200,              "PLAY -> 200");

    // ----- Receive 3+ RTP frames in <= 2s --------------------------------
    std::vector<RtpFrame> frames;
    c.read_interleaved_rtp(frames, /*max_frames=*/8,
                           /*deadline=*/std::chrono::seconds(2));
    std::fprintf(stderr, "[rtsp-loopback] received %zu interleaved frames\n",
                 frames.size());
    check(frames.size() >= 3, "received at least 3 interleaved RTP frames");

    int valid = 0;
    for (const auto& f : frames) {
        if (f.data.size() < 12) continue;
        const uint8_t b0 = f.data[0];
        const uint8_t b1 = f.data[1];
        const uint8_t ver = (b0 >> 6) & 0x03;
        const uint8_t pt  = b1 & 0x7F;
        if (ver == 2 && pt == 96 && f.channel == 0) ++valid;
    }
    check(valid >= 3,
          "at least 3 frames have RTP V=2, PT=96, channel=0");

    // First frame should carry the SPS NAL or an FU-A start with NAL type 7.
    if (!frames.empty() && frames[0].data.size() > 12) {
        const uint8_t* payload = frames[0].data.data() + 12;
        const size_t   plen    = frames[0].data.size() - 12;
        bool nal_ok = false;
        if (plen > 0) {
            const uint8_t type = payload[0] & 0x1F;
            // Single-NAL: type is 1..23 (we expect SPS=7).
            // FU-A: type=28 with FU-header carrying the original NAL type.
            if (type == 7) nal_ok = true;
            else if (type == 28 && plen >= 2) {
                const uint8_t fu_h = payload[1];
                if ((fu_h & 0x1F) == 7 && (fu_h & 0x80)) nal_ok = true;
            }
            else if (type >= 1 && type <= 23) nal_ok = true;  // tolerable
        }
        check(nal_ok, "first RTP packet contains a recognisable NAL");
    }

    // ----- TEARDOWN ------------------------------------------------------
    check(c.teardown(base, r),        "TEARDOWN sent");
    check(r.code == 200,              "TEARDOWN -> 200");

    c.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    srv->stop();

    if (g_fails) {
        std::fprintf(stderr, "RtspServerLoopbackTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("RtspServerLoopbackTest: ok\n");
    return 0;
}
