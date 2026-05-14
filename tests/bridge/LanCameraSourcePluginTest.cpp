// Bambu Bridge — LanCameraSource plugin-route test.
//
// Drives `LanCameraSource` against a mock `BambuSourceHandle` subclass
// that records every bambu_* call in memory. Pins:
//
//   * open() builds the documented `bambu:///rtsps___bblp:<code>@<ip>
//     /streaming/live/1?proto=rtsps` URL (triple underscore, query suffix)
//     and hands it to Bambu_Create.
//   * open() then calls Bambu_Open + Bambu_StartStream(video=true).
//   * next_frame() copies out the bytes the mock emits, sets pts/keyframe
//     from the mock's Bambu_Sample fields, returns nullopt at EOS.
//   * close() drives Bambu_Close + Bambu_Destroy with the tunnel handle
//     Bambu_Create produced.
//   * Failure paths: missing-handle → open() returns false without any
//     calls; mock returning non-zero from Bambu_Create / _Open also
//     returns false (with appropriate cleanup).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "BambuSourceHandle.hpp"
#include "router/LanCameraSource.hpp"

using Slic3r::bridge::BambuSourceHandle;
using Slic3r::bridge::BambuSourceConfig;
using Slic3r::bridge::router::LanCameraSource;
using Slic3r::bridge::router::LanCameraSourceConfig;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// Mirror of __Bambu_Sample from BambuTunnel.h — same layout
// LanCameraSource.cpp uses internally.
struct MirrorBambu_Sample {
    int                  itrack;
    int                  size;
    int                  flags;
    unsigned char const* buffer;
    unsigned long long   decode_time;
};

class MockSource : public BambuSourceHandle {
public:
    struct CreateCall { std::string url; };
    struct ScriptedFrame {
        std::vector<uint8_t> bytes;
        int                  flags = 0;          // bit0 = sync (IDR)
        unsigned long long   decode_time = 0;    // ms
    };

    mutable std::mutex             mu;
    std::vector<CreateCall>        creates;
    int                            opens          = 0;
    std::vector<bool>              start_streams; // {video,...}
    int                            closes         = 0;
    int                            destroys       = 0;
    int                            read_samples   = 0;
    void*                          tunnel         = reinterpret_cast<void*>(0xCAFE);

    // Scripted return codes.
    int create_rc       = 0;
    int open_rc         = 0;
    int start_rc        = 0;
    // Queue of frames; once empty next read returns end-of-stream (rc=1).
    std::deque<ScriptedFrame>      frames;
    // Optional override: return value from bambu_read_sample.
    int read_rc_override = 0; // 0 = no override (use queue semantics)

    MockSource() : BambuSourceHandle(BambuSourceConfig{}) {
        set_library_ready_for_test(true);
    }

    bool init() override          { return true; }
    bool library_ready() const override { return true; }

    int bambu_create(void** out, const std::string& url) override {
        std::lock_guard<std::mutex> lk(mu);
        creates.push_back({url});
        if (create_rc != 0) {
            if (out) *out = nullptr;
            return create_rc;
        }
        if (out) *out = tunnel;
        return 0;
    }
    void bambu_destroy(void* t) override {
        std::lock_guard<std::mutex> lk(mu);
        (void)t;
        ++destroys;
    }
    int bambu_open(void* t) override {
        std::lock_guard<std::mutex> lk(mu);
        (void)t;
        ++opens;
        return open_rc;
    }
    void bambu_close(void* t) override {
        std::lock_guard<std::mutex> lk(mu);
        (void)t;
        ++closes;
    }
    int bambu_start_stream(void* t, bool video) override {
        std::lock_guard<std::mutex> lk(mu);
        (void)t;
        start_streams.push_back(video);
        return start_rc;
    }
    int bambu_get_stream_count(void* /*t*/) override { return 0; }
    int bambu_get_stream_info(void* /*t*/, int /*i*/, void* /*out*/) override { return -1; }
    int bambu_read_sample(void* /*t*/, void* sample_out) override {
        std::lock_guard<std::mutex> lk(mu);
        ++read_samples;
        if (!sample_out) return -1;
        auto* s = static_cast<MirrorBambu_Sample*>(sample_out);
        if (read_rc_override != 0) {
            std::memset(s, 0, sizeof(*s));
            return read_rc_override;
        }
        if (frames.empty()) {
            std::memset(s, 0, sizeof(*s));
            return 1; // Bambu_stream_end
        }
        // Pop one frame; the bytes stay alive via the queue entry (we
        // *don't* pop until after the caller copies, but LanCameraSource
        // copies inside next_frame before returning so this is safe).
        // For test determinism we keep a stable backing vector below by
        // moving the front entry into a member buffer first.
        last_frame_buf = std::move(frames.front());
        frames.pop_front();
        s->itrack      = 0;
        s->size        = static_cast<int>(last_frame_buf.bytes.size());
        s->flags       = last_frame_buf.flags;
        s->buffer      = last_frame_buf.bytes.data();
        s->decode_time = last_frame_buf.decode_time;
        return 0;
    }

private:
    ScriptedFrame last_frame_buf;
};

} // namespace

int main() {
    // ---- Happy path: open() → URL pinned, frames flow, close() teardown ----
    {
        auto mock = std::make_shared<MockSource>();
        LanCameraSourceConfig cfg;
        cfg.dev_id      = "0938X1";
        cfg.printer_ip  = "192.0.2.42";
        cfg.access_code = "ABCDEF";
        // username left at its default ("bblp") to pin the slicer-matching form.

        LanCameraSource src(cfg, mock);
        check(src.url() == "bambu:///rtsps___bblp:ABCDEF@192.0.2.42/streaming/live/1?proto=rtsps",
              "URL has triple underscore + creds + ?proto=rtsps");

        // Pre-queue two frames the mock will emit.
        MockSource::ScriptedFrame f1;
        f1.bytes       = {0x00,0x00,0x00,0x01,0x65,0xAA};  // Annex-B IDR-ish
        f1.flags       = 1;     // f_sync = IDR
        f1.decode_time = 100;   // ms
        MockSource::ScriptedFrame f2;
        f2.bytes       = {0x00,0x00,0x00,0x01,0x41,0xBB};  // Annex-B P-slice
        f2.flags       = 0;
        f2.decode_time = 133;
        {
            std::lock_guard<std::mutex> lk(mock->mu);
            mock->frames.push_back(std::move(f1));
            mock->frames.push_back(std::move(f2));
        }

        check(src.open(), "open() returns true");
        {
            std::lock_guard<std::mutex> lk(mock->mu);
            check(mock->creates.size() == 1, "Bambu_Create called once");
            if (!mock->creates.empty()) {
                check(mock->creates[0].url
                      == "bambu:///rtsps___bblp:ABCDEF@192.0.2.42/streaming/live/1?proto=rtsps",
                      "Create URL matches the build_url() output");
            }
            check(mock->opens         == 1, "Bambu_Open called once");
            check(mock->start_streams.size() == 1, "Bambu_StartStream called once");
            if (!mock->start_streams.empty())
                check(mock->start_streams[0] == true, "StartStream video=true");
        }

        auto fa = src.next_frame(0);
        check(fa.has_value(), "next_frame() #1 returned a frame");
        if (fa) {
            check(fa->nal_data.size() == 6,         "frame #1 size = 6");
            check(fa->is_keyframe,                  "frame #1 keyframe");
            check(fa->pts_us == 100 * 1000,         "frame #1 pts in us");
            check(fa->nal_data[4] == 0x65,          "frame #1 IDR header byte");
        }
        auto fb = src.next_frame(0);
        check(fb.has_value(), "next_frame() #2 returned a frame");
        if (fb) {
            check(!fb->is_keyframe,                 "frame #2 not keyframe");
            check(fb->pts_us == 133 * 1000,         "frame #2 pts in us");
        }
        // Queue empty → mock returns Bambu_stream_end (rc=1). Source flips
        // closed and next_frame returns nullopt.
        auto fc = src.next_frame(0);
        check(!fc.has_value(), "next_frame() nullopt at EOS");
        check(!src.is_open(),  "is_open() false after EOS");

        // close() still drives the teardown calls even after the source
        // auto-closed on EOS.
        src.close();
        {
            std::lock_guard<std::mutex> lk(mock->mu);
            check(mock->closes   == 1, "Bambu_Close called once");
            check(mock->destroys == 1, "Bambu_Destroy called once");
        }
    }

    // ---- No source handle: open() refuses without any calls -----------
    {
        LanCameraSourceConfig cfg;
        cfg.dev_id     = "0938X1";
        cfg.printer_ip = "10.0.0.1";
        cfg.access_code = "ZZZZ";
        LanCameraSource src(cfg);
        check(!src.open(),    "open() false with no BambuSourceHandle");
        check(!src.is_open(), "is_open() false");
    }

    // ---- Bambu_Create fails → open() returns false, no leak ----------
    {
        auto mock = std::make_shared<MockSource>();
        mock->create_rc = -7;
        LanCameraSourceConfig cfg;
        cfg.dev_id     = "0938X1";
        cfg.printer_ip = "10.0.0.1";
        cfg.access_code = "ZZZZ";
        LanCameraSource src(cfg, mock);
        check(!src.open(), "open() false when Bambu_Create fails");
        {
            std::lock_guard<std::mutex> lk(mock->mu);
            check(mock->opens == 0,    "Bambu_Open NOT called after Create failed");
            check(mock->destroys == 0, "Bambu_Destroy NOT called (Create gave us nothing)");
        }
    }

    // ---- Bambu_Open fails → Bambu_Destroy is called to clean up -------
    {
        auto mock = std::make_shared<MockSource>();
        mock->open_rc = -3;
        LanCameraSourceConfig cfg;
        cfg.dev_id     = "0938X1";
        cfg.printer_ip = "10.0.0.1";
        cfg.access_code = "ZZZZ";
        LanCameraSource src(cfg, mock);
        check(!src.open(), "open() false when Bambu_Open fails");
        {
            std::lock_guard<std::mutex> lk(mock->mu);
            check(mock->destroys == 1, "Bambu_Destroy called to clean up tunnel");
            check(mock->closes   == 0, "Bambu_Close NOT called (open never succeeded)");
        }
    }

    // ---- Bambu_StartStream fails → close + destroy both called --------
    {
        auto mock = std::make_shared<MockSource>();
        mock->start_rc = -9;
        LanCameraSourceConfig cfg;
        cfg.dev_id     = "0938X1";
        cfg.printer_ip = "10.0.0.1";
        cfg.access_code = "ZZZZ";
        LanCameraSource src(cfg, mock);
        check(!src.open(), "open() false when Bambu_StartStream fails");
        {
            std::lock_guard<std::mutex> lk(mock->mu);
            check(mock->closes   == 1, "Bambu_Close called to clean up");
            check(mock->destroys == 1, "Bambu_Destroy called to clean up");
        }
    }

    if (g_fails) {
        std::fprintf(stderr, "LanCameraSourcePluginTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("LanCameraSourcePluginTest: ok\n");
    return 0;
}
