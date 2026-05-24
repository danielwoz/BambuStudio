// Bambu Bridge — JpegCameraSource unit test.
//
// Pins the OpenBambuAPI/video.md wire format and JpegCameraSource's
// ICameraSource contract:
//
//   1. build_auth_packet() emits 80 bytes matching the documented layout
//      (4-byte payload_size=0x40, 4-byte type=0x3000, 8 reserved, 32-byte
//      username NUL-padded, 32-byte password NUL-padded).
//   2. parse_frame_header() decodes a 16-byte LE header into payload_size /
//      itrack / flags and rejects oversize / zero payloads.
//   3. is_jpeg_camera_model() returns true for A1 / P1 variants and false
//      for X1 / H2 variants and unknown strings.
//   4. End-to-end via a fake IO seam: feeds two scripted JPEG frames into
//      JpegCameraSource and verifies open + two next_frame() calls + close
//      + lifecycle accounting.

#include "router/JpegCameraSource.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <vector>

using Slic3r::bridge::router::JpegCameraSource;
using Slic3r::bridge::router::JpegCameraSourceConfig;
using Slic3r::bridge::router::is_jpeg_camera_model;
using Slic3r::bridge::router::jpeg_camera_model_tag;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// Test subclass: bypass real sockets / TLS by overriding tcp_connect_,
// tls_handshake_, and io_read_full_ / io_write_full_. The test drives
// bytes into the read queue and inspects the write log.
class FakeJpegSource : public JpegCameraSource {
public:
    using JpegCameraSource::JpegCameraSource;

    std::vector<uint8_t>             writes;
    std::deque<uint8_t>              read_queue;
    int                              tcp_calls = 0;
    int                              tls_calls = 0;
    int                              auth_calls = 0;

    // Inject a frame: header + payload.
    void inject_frame(const std::vector<uint8_t>& jpeg_payload) {
        uint32_t sz = static_cast<uint32_t>(jpeg_payload.size());
        // 16-byte header little-endian.
        read_queue.push_back(static_cast<uint8_t>(sz & 0xff));
        read_queue.push_back(static_cast<uint8_t>((sz >> 8) & 0xff));
        read_queue.push_back(static_cast<uint8_t>((sz >> 16) & 0xff));
        read_queue.push_back(static_cast<uint8_t>((sz >> 24) & 0xff));
        // itrack = 0, flags = 1, reserved = 0
        for (int i = 0; i < 4; ++i) read_queue.push_back(0);      // itrack
        read_queue.push_back(1); for (int i = 0; i < 3; ++i) read_queue.push_back(0); // flags
        for (int i = 0; i < 4; ++i) read_queue.push_back(0);      // reserved
        // Payload.
        for (uint8_t b : jpeg_payload) read_queue.push_back(b);
    }

protected:
    int tcp_connect_(int /*timeout_ms*/) override {
        ++tcp_calls;
        return 1; // pretend success; we never read from this fd
    }
    int tls_handshake_(int /*timeout_ms*/) override {
        ++tls_calls;
        return 0;
    }
    int send_auth_(int timeout_ms) override {
        // Use the real build_auth_packet but capture writes locally.
        ++auth_calls;
        auto pkt = build_auth_packet();
        for (uint8_t b : pkt) writes.push_back(b);
        (void)timeout_ms;
        return 0;
    }
    int io_read_full_(uint8_t* dst, std::size_t n, int /*timeout_ms*/) override {
        if (read_queue.size() < n) return -1;
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = read_queue.front();
            read_queue.pop_front();
        }
        return 0;
    }
    int io_write_full_(const uint8_t* src, std::size_t n, int /*timeout_ms*/) override {
        for (std::size_t i = 0; i < n; ++i) writes.push_back(src[i]);
        return 0;
    }
};

std::vector<uint8_t> tiny_jpeg() {
    // 8-byte fake JPEG payload (just SOI + EOI + filler). Real JPEGs are
    // hundreds-of-kB, but the source only inspects the SOI markers.
    return { 0xFF, 0xD8, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xD9 };
}

}  // namespace

int main() {
    // ---- 1. build_auth_packet bytes pinned against OpenBambuAPI/video.md
    {
        JpegCameraSourceConfig cfg;
        cfg.dev_id      = "TESTDEV";
        cfg.printer_ip  = "192.168.1.247";
        cfg.access_code = "0123456789AB";   // 12 chars; fits in 32-byte field
        cfg.username    = "bblp";
        FakeJpegSource src(cfg);
        auto pkt = src.build_auth_packet();
        check(pkt.size() == 80,           "auth packet is 80 bytes");
        // bytes 0..3: little-endian 0x00000040
        check(pkt[0] == 0x40 && pkt[1] == 0x00 && pkt[2] == 0x00 && pkt[3] == 0x00,
              "auth payload_size = 0x40 little-endian");
        // bytes 4..7: little-endian 0x00003000
        check(pkt[4] == 0x00 && pkt[5] == 0x30 && pkt[6] == 0x00 && pkt[7] == 0x00,
              "auth type = 0x3000 little-endian");
        // bytes 8..15: zero (flags + reserved)
        bool zeros = true;
        for (int i = 8; i < 16; ++i) if (pkt[i] != 0) { zeros = false; break; }
        check(zeros, "auth bytes 8..15 are zero (flags+reserved)");
        // bytes 16..19: 'bblp'
        check(pkt[16] == 'b' && pkt[17] == 'b' && pkt[18] == 'l' && pkt[19] == 'p',
              "username starts with 'bblp' at offset 16");
        // bytes 20..47: NUL padding for the rest of the 32-byte username field
        bool user_pad = true;
        for (int i = 20; i < 48; ++i) if (pkt[i] != 0) { user_pad = false; break; }
        check(user_pad, "username NUL-padded to 32 bytes");
        // bytes 48..59: access code, then NUL padding
        check(pkt[48] == '0' && pkt[49] == '1' && pkt[58] == 'A' && pkt[59] == 'B',
              "access code starts at offset 48");
        bool pass_pad = true;
        for (int i = 60; i < 80; ++i) if (pkt[i] != 0) { pass_pad = false; break; }
        check(pass_pad, "access code NUL-padded to 32 bytes");
    }

    // ---- 2. parse_frame_header decodes / rejects ----
    {
        uint8_t hdr[16] = {0};
        // payload_size = 0x12345678 little-endian
        hdr[0] = 0x78; hdr[1] = 0x56; hdr[2] = 0x34; hdr[3] = 0x12;
        // itrack = 0x10
        hdr[4] = 0x10;
        // flags = 0x01
        hdr[8] = 0x01;
        uint32_t sz = 0, it = 0, fl = 0;
        // 0x12345678 == 305419896 < 8 MB? 305 MB > 8 MB → should be rejected.
        // Use a smaller value that's under the 8 MB cap.
        hdr[0] = 0x00; hdr[1] = 0x00; hdr[2] = 0x02; hdr[3] = 0x00; // 0x00020000 = 128 KB
        bool ok1 = JpegCameraSource::parse_frame_header(hdr, sz, it, fl);
        check(ok1,                 "parse_frame_header: 128KB payload accepted");
        check(sz == 0x20000,       "parse_frame_header: payload size decoded");
        check(it == 0x10,          "parse_frame_header: itrack decoded");
        check(fl == 0x01,          "parse_frame_header: flags decoded");

        // Zero payload → reject (server should never send empty frame)
        uint8_t hdr0[16] = {0};
        ok1 = JpegCameraSource::parse_frame_header(hdr0, sz, it, fl);
        check(!ok1,                "parse_frame_header: zero size rejected");

        // 32 MB payload → reject (over 8 MB cap)
        uint8_t hdr_big[16] = {0};
        hdr_big[3] = 0x02;  // 0x02000000 = 32 MB
        ok1 = JpegCameraSource::parse_frame_header(hdr_big, sz, it, fl);
        check(!ok1,                "parse_frame_header: oversize payload rejected");
    }

    // ---- 3. is_jpeg_camera_model gating ----
    {
        check( is_jpeg_camera_model("A1"),         "A1 gates to JPEG");
        check( is_jpeg_camera_model("A1 mini"),    "A1 mini gates to JPEG");
        check( is_jpeg_camera_model("A1mini"),     "A1mini gates to JPEG");
        check( is_jpeg_camera_model("P1S"),        "P1S gates to JPEG");
        check( is_jpeg_camera_model("P1P"),        "P1P gates to JPEG");
        check( is_jpeg_camera_model("N1"),         "N1 (A1 mini code) gates to JPEG");
        check( is_jpeg_camera_model("N2S"),        "N2S (A1 code) gates to JPEG");
        check( is_jpeg_camera_model("C13"),        "C13 (P1P code) gates to JPEG");
        check( is_jpeg_camera_model("C14"),        "C14 (P1S code) gates to JPEG");

        check(!is_jpeg_camera_model(""),           "empty model does NOT gate");
        check(!is_jpeg_camera_model("X1C"),        "X1C does NOT gate");
        check(!is_jpeg_camera_model("X1"),         "X1 does NOT gate");
        check(!is_jpeg_camera_model("X1E"),        "X1E does NOT gate");
        check(!is_jpeg_camera_model("H2S"),        "H2S does NOT gate");
        check(!is_jpeg_camera_model("H2D"),        "H2D does NOT gate");
        check(!is_jpeg_camera_model("C11"),        "C11 (X1) does NOT gate");
        check(!is_jpeg_camera_model("C18"),        "C18 (H2S) does NOT gate");
        check(!is_jpeg_camera_model("Unknown"),    "unknown does NOT gate");
        check(!is_jpeg_camera_model("3DPrinter-X1-Carbon"),
              "X1-Carbon long form does NOT gate");

        check(jpeg_camera_model_tag("A1")       == "A1",      "tag A1");
        check(jpeg_camera_model_tag("A1mini")   == "A1mini",  "tag A1mini");
        check(jpeg_camera_model_tag("P1S")      == "P1S",     "tag P1S");
        check(jpeg_camera_model_tag("P1P")      == "P1P",     "tag P1P");
    }

    // ---- 4. End-to-end frame flow via fake IO seam ----
    {
        JpegCameraSourceConfig cfg;
        cfg.dev_id      = "TESTDEV";
        cfg.printer_ip  = "1.2.3.4";
        cfg.access_code = "abcdef";
        FakeJpegSource src(cfg);

        // Queue 2 frames BEFORE open() so the first next_frame doesn't EOF.
        src.inject_frame(tiny_jpeg());
        src.inject_frame(tiny_jpeg());

        check(!src.is_open(),                  "starts closed");
        check(src.open(),                      "open() succeeds");
        check(src.is_open(),                   "is_open after open");
        check(src.tcp_calls == 1,              "tcp_connect called once");
        check(src.tls_calls == 1,              "tls_handshake called once");
        check(src.auth_calls == 1,             "auth packet sent once");
        check(src.writes.size() == 80,         "80 bytes written (auth packet only)");

        auto info = src.info();
        check(info.width  == 1280,             "info.width = 1280");
        check(info.height == 720,              "info.height = 720");
        check(info.codec  == Slic3r::bridge::server::ICameraSource::Codec::MotionJpeg,
              "info.codec = MotionJpeg");
        check(info.sps.empty(),                "info.sps empty for MJPEG");
        check(info.pps.empty(),                "info.pps empty for MJPEG");

        auto f1 = src.next_frame(1000);
        check(f1.has_value(),                  "next_frame #1 returns frame");
        if (f1) {
            check(f1->nal_data.size() == 8,    "frame #1 payload is 8 bytes");
            check(f1->nal_data[0] == 0xFF && f1->nal_data[1] == 0xD8,
                  "frame #1 starts with JPEG SOI (FF D8)");
            check(f1->is_keyframe,             "MJPEG frame flagged keyframe");
        }

        auto f2 = src.next_frame(1000);
        check(f2.has_value(),                  "next_frame #2 returns frame");

        // Queue exhausted → next read returns -1 → source flips closed.
        auto f3 = src.next_frame(1000);
        check(!f3.has_value(),                 "next_frame #3 returns nullopt at EOF");
        check(!src.is_open(),                  "is_open false after EOF close");

        src.close();
        check(!src.is_open(),                  "close() idempotent / clean");
    }

    // ---- 5. open() fails fast on empty printer_ip ----
    {
        JpegCameraSourceConfig cfg;
        cfg.access_code = "x";
        FakeJpegSource src(cfg);
        check(!src.open(),                     "open() fails with empty printer_ip");
        check(src.tcp_calls == 0,              "tcp_connect NOT called on empty ip");
    }

    // ---- 6. Bad SOI in payload → source closes ----
    {
        JpegCameraSourceConfig cfg;
        cfg.printer_ip  = "1.2.3.4";
        cfg.access_code = "x";
        FakeJpegSource src(cfg);
        // Inject a frame whose payload doesn't start with FF D8.
        src.inject_frame({0xDE, 0xAD, 0xBE, 0xEF});
        check(src.open(),                      "open() ok");
        auto f = src.next_frame(1000);
        check(!f.has_value(),                  "bad-SOI frame → nullopt");
        check(!src.is_open(),                  "source closed after bad SOI");
    }

    if (g_fails) {
        std::fprintf(stderr, "JpegCameraSourceTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("JpegCameraSourceTest: ok\n");
    return 0;
}
