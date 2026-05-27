// Bambu Bridge — CameraSourceRouter unit test (phase 9).
//
// Pure unit test: no sockets, no real RTSP. We subclass each of the three
// camera sources (Lan / Cloud / Null) so open() is toggleable and
// next_frame() can be driven from the test. The router is then exercised
// against scenarios that prove:
//
//   1. Sticky open(): the first open() picks one source and subsequent
//      next_frame() calls only flow through that source.
//   2. Fallback-on-open(): if the preferred source's open() returns
//      false, the router falls through to the next preference.
//   3. No mid-stream failover: once open() succeeded against a source,
//      a next_frame() returning nullopt is propagated to the caller —
//      we do NOT try to silently swap to another source.
//   4. NullCameraSource fallback gated by Policy::allow_null_fallback.

#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "router/CameraSourceRouter.hpp"
#include "router/CloudCameraSource.hpp"
#include "router/JpegCameraSource.hpp"
#include "router/LanCameraSource.hpp"
#include "router/NullCameraSource.hpp"
#include "router/UplinkHealth.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

using Slic3r::bridge::router::CameraSourceRouter;
using Slic3r::bridge::router::CloudCameraSource;
using Slic3r::bridge::router::CloudCameraSourceConfig;
using Slic3r::bridge::router::JpegCameraSource;
using Slic3r::bridge::router::JpegCameraSourceConfig;
using Slic3r::bridge::router::LanCameraSource;
using Slic3r::bridge::router::LanCameraSourceConfig;
using Slic3r::bridge::router::NullCameraSource;
using Slic3r::bridge::router::UplinkHealthMonitor;
using Slic3r::bridge::server::VideoFrame;

// Stub subclasses. Each tracks whether it was opened, count of frames
// served, and whether to fail open() / fail next_frame mid-stream.

class StubLan : public LanCameraSource {
public:
    StubLan() : LanCameraSource(LanCameraSourceConfig{}) {}
    std::atomic<bool> open_succeeds{true};
    std::atomic<bool> stream_alive{true};
    std::atomic<int>  open_calls{0};
    std::atomic<int>  frame_calls{0};

    bool open() override {
        ++open_calls;
        if (!open_succeeds.load()) return false;
        m_open.store(true);
        return true;
    }
    void close() override { m_open.store(false); }
    bool is_open() const override { return m_open.load(); }

    std::optional<VideoFrame> next_frame(int /*timeout_ms*/) override {
        ++frame_calls;
        if (!m_open.load() || !stream_alive.load()) return std::nullopt;
        VideoFrame f;
        f.nal_data    = {0x00,0x00,0x00,0x01,0x65}; // 1-byte IDR-like marker
        f.is_keyframe = true;
        f.pts_us      = frame_calls.load() * 33333;
        return f;
    }
    Slic3r::bridge::server::ICameraSource::StreamInfo info() const override {
        Slic3r::bridge::server::ICameraSource::StreamInfo si;
        si.width = 320; si.height = 240; si.fps = 30;
        si.sps = {0x67}; si.pps = {0x68};
        return si;
    }
private:
    std::atomic<bool> m_open{false};
};

class StubCloud : public CloudCameraSource {
public:
    StubCloud() : CloudCameraSource(CloudCameraSourceConfig{}) {}
    std::atomic<bool> open_succeeds{true};
    std::atomic<int>  open_calls{0};
    std::atomic<int>  frame_calls{0};

    bool open() override {
        ++open_calls;
        if (!open_succeeds.load()) return false;
        m_open.store(true);
        return true;
    }
    void close() override { m_open.store(false); }
    bool is_open() const override { return m_open.load(); }
    std::optional<VideoFrame> next_frame(int) override {
        ++frame_calls;
        if (!m_open.load()) return std::nullopt;
        VideoFrame f; f.nal_data = {0xCC}; f.is_keyframe = false;
        return f;
    }
    Slic3r::bridge::server::ICameraSource::StreamInfo info() const override {
        Slic3r::bridge::server::ICameraSource::StreamInfo si;
        si.width = 640; si.height = 480; si.fps = 24;
        return si;
    }
private:
    std::atomic<bool> m_open{false};
};

class StubJpeg : public JpegCameraSource {
public:
    StubJpeg() : JpegCameraSource(JpegCameraSourceConfig{}) {}
    std::atomic<bool> open_succeeds{true};
    std::atomic<int>  open_calls{0};
    std::atomic<int>  frame_calls{0};

    bool open() override {
        ++open_calls;
        if (!open_succeeds.load()) return false;
        m_open.store(true);
        return true;
    }
    void close() override { m_open.store(false); }
    bool is_open() const override { return m_open.load(); }
    std::optional<VideoFrame> next_frame(int) override {
        ++frame_calls;
        if (!m_open.load()) return std::nullopt;
        VideoFrame f; f.nal_data = {0xFF,0xD8,0xFF,0xD9}; f.is_keyframe = true;
        return f;
    }
    Slic3r::bridge::server::ICameraSource::StreamInfo info() const override {
        Slic3r::bridge::server::ICameraSource::StreamInfo si;
        si.width = 1280; si.height = 720; si.fps = 30;
        return si;
    }
private:
    std::atomic<bool> m_open{false};
};

class StubNull : public NullCameraSource {
public:
    std::atomic<int> open_calls{0};
    std::atomic<int> frame_calls{0};
    std::atomic<bool> open_succeeds{true};

    bool open() override {
        ++open_calls;
        if (!open_succeeds.load()) return false;
        // Use base behaviour to populate fixture state.
        return NullCameraSource::open();
    }
    std::optional<VideoFrame> next_frame(int t) override {
        ++frame_calls;
        return NullCameraSource::next_frame(t);
    }
};

} // namespace

int main() {
    const std::string dev_id = "DEV-1";

    // ---- Scenario 1: LAN healthy + opens successfully → stick to LAN ----
    {
        auto lan   = std::make_shared<StubLan>();
        auto cloud = std::make_shared<StubCloud>();
        auto nul   = std::make_shared<StubNull>();

        CameraSourceRouter router(dev_id);
        router.set_lan_source(lan);
        router.set_cloud_source(cloud);
        router.set_null_source(nul);
        // No health monitor → router falls back to "shared_ptr presence
        // == healthy" view.
        CameraSourceRouter::Policy pol;
        pol.prefer_lan          = true;
        pol.allow_null_fallback = false;
        router.set_policy(pol);

        check(router.open(), "S1: router opens");
        check(router.current_choice() == CameraSourceRouter::Choice::Lan,
              "S1: chosen source is LAN");
        check(lan->open_calls.load()   == 1, "S1: LAN opened once");
        check(cloud->open_calls.load() == 0, "S1: cloud not touched");
        check(nul->open_calls.load()   == 0, "S1: null not touched");

        // Drive frames; they should all come from LAN.
        for (int i = 0; i < 5; ++i) {
            auto f = router.next_frame(0);
            check(f.has_value(), "S1: next_frame returned a frame");
        }
        check(lan->frame_calls.load()   == 5, "S1: 5 frames came from LAN");
        check(cloud->frame_calls.load() == 0, "S1: cloud never asked");

        router.close();
        check(!router.is_open(), "S1: close → not open");
        check(router.current_choice() == CameraSourceRouter::Choice::None,
              "S1: post-close choice is None");
    }

    // ---- Scenario 2: LAN open() fails → fall back to cloud ----
    {
        auto lan   = std::make_shared<StubLan>();
        auto cloud = std::make_shared<StubCloud>();
        lan->open_succeeds.store(false);
        cloud->open_succeeds.store(true);

        CameraSourceRouter router(dev_id);
        router.set_lan_source(lan);
        router.set_cloud_source(cloud);
        CameraSourceRouter::Policy pol;
        pol.prefer_lan = true;
        router.set_policy(pol);

        check(router.open(), "S2: router opens via fallback");
        check(router.current_choice() == CameraSourceRouter::Choice::Cloud,
              "S2: chosen source is Cloud after LAN open() failure");
        check(lan->open_calls.load()   == 1, "S2: LAN was tried first");
        check(cloud->open_calls.load() == 1, "S2: cloud was tried after LAN failed");

        auto f = router.next_frame(0);
        check(f.has_value(),                       "S2: frame served from cloud");
        check(cloud->frame_calls.load() == 1,      "S2: cloud counted");
        check(lan->frame_calls.load()   == 0,      "S2: LAN not asked");

        router.close();
    }

    // ---- Scenario 3: Both LAN+cloud fail, null fallback disabled ----
    {
        auto lan   = std::make_shared<StubLan>();
        auto cloud = std::make_shared<StubCloud>();
        auto nul   = std::make_shared<StubNull>();
        lan->open_succeeds.store(false);
        cloud->open_succeeds.store(false);

        CameraSourceRouter router(dev_id);
        router.set_lan_source(lan);
        router.set_cloud_source(cloud);
        router.set_null_source(nul);
        CameraSourceRouter::Policy pol;
        pol.allow_null_fallback = false;
        router.set_policy(pol);

        check(!router.open(), "S3: router open() returns false (no source)");
        check(router.current_choice() == CameraSourceRouter::Choice::None,
              "S3: choice = None when nothing brought up");
        check(nul->open_calls.load() == 0,
              "S3: null source NOT tried (allow_null_fallback=false)");
    }

    // ---- Scenario 4: All others fail, null fallback enabled → Null wins --
    {
        auto lan   = std::make_shared<StubLan>();
        auto cloud = std::make_shared<StubCloud>();
        auto nul   = std::make_shared<StubNull>();
        lan->open_succeeds.store(false);
        cloud->open_succeeds.store(false);

        CameraSourceRouter router(dev_id);
        router.set_lan_source(lan);
        router.set_cloud_source(cloud);
        router.set_null_source(nul);
        CameraSourceRouter::Policy pol;
        pol.allow_null_fallback = true;
        router.set_policy(pol);

        check(router.open(), "S4: router opens via null fallback");
        check(router.current_choice() == CameraSourceRouter::Choice::Null,
              "S4: choice is Null");

        auto f = router.next_frame(0);
        check(f.has_value(),                  "S4: null source serves a frame");
        check(nul->frame_calls.load() == 1,   "S4: null counted");
    }

    // ---- Scenario 5: mid-stream failure → propagate nullopt, no failover --
    {
        auto lan   = std::make_shared<StubLan>();
        auto cloud = std::make_shared<StubCloud>();
        // Both healthy at open(); LAN wins.
        CameraSourceRouter router(dev_id);
        router.set_lan_source(lan);
        router.set_cloud_source(cloud);
        check(router.open(), "S5: open succeeded");
        check(router.current_choice() == CameraSourceRouter::Choice::Lan,
              "S5: opened LAN");

        // First frame: ok.
        auto f1 = router.next_frame(0);
        check(f1.has_value(), "S5: first frame ok");

        // Simulate the LAN stream dying mid-session.
        lan->stream_alive.store(false);
        auto f2 = router.next_frame(0);
        check(!f2.has_value(),
              "S5: nullopt propagated (no sneaky swap to cloud)");
        check(cloud->open_calls.load()  == 0,
              "S5: cloud NOT opened on mid-stream failure");
        check(cloud->frame_calls.load() == 0,
              "S5: cloud NOT asked after mid-stream failure");
        check(router.current_choice() == CameraSourceRouter::Choice::Lan,
              "S5: choice still LAN after mid-stream nullopt");
    }

    // ---- Scenario 6: JPEG demoted to trailing fallback (the native-aligned
    // local path). prefer_lan + prefer_jpeg=false with a JPEG source wired:
    // the libBambuSource LAN source must lead; JPEG is tried ONLY after LAN
    // (and cloud) fail to open. This is the A1 bambu:///local case where we
    // drive the same lib native does, with JpegCameraSource as the net. ----
    {
        // 6a: LAN healthy → LAN wins, JPEG never touched even though wired.
        auto lan  = std::make_shared<StubLan>();
        auto jpeg = std::make_shared<StubJpeg>();
        CameraSourceRouter router(dev_id);
        router.set_lan_source(lan);
        router.set_jpeg_source(jpeg);
        CameraSourceRouter::Policy pol;
        pol.prefer_lan  = true;
        pol.prefer_jpeg = false;   // JPEG is fallback-only
        router.set_policy(pol);

        check(router.open(), "S6a: router opens");
        check(router.current_choice() == CameraSourceRouter::Choice::Lan,
              "S6a: LAN(lib) leads when prefer_jpeg=false");
        check(jpeg->open_calls.load() == 0,
              "S6a: JPEG NOT preempting LAN");
    }
    {
        // 6b: LAN open() fails (lib's headless local path can't open) →
        // router falls through to the JPEG trailing fallback.
        auto lan  = std::make_shared<StubLan>();
        auto jpeg = std::make_shared<StubJpeg>();
        lan->open_succeeds.store(false);
        CameraSourceRouter router(dev_id);
        router.set_lan_source(lan);
        router.set_jpeg_source(jpeg);
        CameraSourceRouter::Policy pol;
        pol.prefer_lan  = true;
        pol.prefer_jpeg = false;
        router.set_policy(pol);

        check(router.open(), "S6b: router opens via JPEG fallback");
        check(router.current_choice() == CameraSourceRouter::Choice::Jpeg,
              "S6b: JPEG chosen after LAN open() failure");
        check(lan->open_calls.load()  == 1, "S6b: LAN tried first");
        check(jpeg->open_calls.load() == 1, "S6b: JPEG tried after LAN failed");

        auto f = router.next_frame(0);
        check(f.has_value(),                 "S6b: frame served from JPEG");
        check(jpeg->frame_calls.load() == 1, "S6b: JPEG counted");
    }

    if (g_fails) {
        std::fprintf(stderr, "CameraSourceRouterTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("CameraSourceRouterTest: ok\n");
    return 0;
}
