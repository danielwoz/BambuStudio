// Bambu Bridge — CloudCameraSource plugin-route test.
//
// Drives CloudCameraSource against:
//   * a MockPluginHandle subclass that fakes `get_camera_url`,
//   * a MockSource (mock BambuSourceHandle) that records the URL the
//     source asked it to Bambu_Create.
//
// Pins:
//   * open() asks the plugin for the URL (via get_camera_url).
//   * Whatever URL the plugin returns is handed verbatim to Bambu_Create.
//   * Frames pumped by the mock surface in next_frame() with pts/keyframe
//     populated from Bambu_Sample.
//   * close() drives Bambu_Close + Bambu_Destroy.
//   * Failure modes:
//     - Plugin not loaded / not ready → open() refuses, no Bambu_Create.
//     - get_camera_url returns nonzero rc → open() refuses.
//     - BambuSource not loaded → open() refuses, no Bambu_Create.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "BambuNetworkingPluginHandle.hpp"
#include "BambuSourceHandle.hpp"
#include "router/CloudCameraSource.hpp"

using Slic3r::bridge::BambuNetworkingPluginHandle;
using Slic3r::bridge::BambuSourceConfig;
using Slic3r::bridge::BambuSourceHandle;
using Slic3r::bridge::PluginHandleConfig;
using Slic3r::bridge::router::CloudCameraSource;
using Slic3r::bridge::router::CloudCameraSourceConfig;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// Mirror of __Bambu_Sample from BambuTunnel.h.
struct MirrorBambu_Sample {
    int                  itrack;
    int                  size;
    int                  flags;
    unsigned char const* buffer;
    unsigned long long   decode_time;
};

class MockPlugin : public BambuNetworkingPluginHandle {
public:
    mutable std::mutex                                  mu;
    std::map<std::string, std::string>                  urls;     // dev_id -> URL
    int                                                 rc_override = 0;   // 0 = success
    int                                                 calls       = 0;

    MockPlugin() : BambuNetworkingPluginHandle({}) {
        set_agent_ready_for_test(true);
    }
    bool init() override          { return true; }
    bool agent_ready() const override { return true; }

    int get_camera_url(const std::string& dev_id,
                       std::string*       url_out,
                       int /*timeout_ms*/) override {
        std::lock_guard<std::mutex> lk(mu);
        ++calls;
        if (!url_out) return -3;
        if (rc_override != 0) { url_out->clear(); return rc_override; }
        auto it = urls.find(dev_id);
        if (it == urls.end()) { url_out->clear(); return -3; }
        *url_out = it->second;
        return 0;
    }
};

class MockSource : public BambuSourceHandle {
public:
    struct CreateCall { std::string url; };
    struct ScriptedFrame {
        std::vector<uint8_t> bytes;
        int                  flags = 0;
        unsigned long long   decode_time = 0;
    };

    mutable std::mutex             mu;
    std::vector<CreateCall>        creates;
    int                            opens          = 0;
    int                            start_streams  = 0;
    int                            closes         = 0;
    int                            destroys       = 0;
    int                            read_samples   = 0;
    void*                          tunnel         = reinterpret_cast<void*>(0xBEEF);

    int create_rc = 0;
    int open_rc   = 0;
    int start_rc  = 0;
    std::deque<ScriptedFrame>      frames;

    MockSource() : BambuSourceHandle(BambuSourceConfig{}) {
        set_library_ready_for_test(true);
    }
    bool init() override          { return true; }
    bool library_ready() const override { return true; }

    int bambu_create(void** out, const std::string& url) override {
        std::lock_guard<std::mutex> lk(mu);
        creates.push_back({url});
        if (create_rc != 0) { if (out) *out = nullptr; return create_rc; }
        if (out) *out = tunnel;
        return 0;
    }
    void bambu_destroy(void* /*t*/) override {
        std::lock_guard<std::mutex> lk(mu);
        ++destroys;
    }
    int bambu_open(void* /*t*/) override {
        std::lock_guard<std::mutex> lk(mu);
        ++opens;
        return open_rc;
    }
    void bambu_close(void* /*t*/) override {
        std::lock_guard<std::mutex> lk(mu);
        ++closes;
    }
    int bambu_start_stream(void* /*t*/, bool /*video*/) override {
        std::lock_guard<std::mutex> lk(mu);
        ++start_streams;
        return start_rc;
    }
    int bambu_get_stream_count(void* /*t*/) override { return 0; }
    int bambu_get_stream_info(void* /*t*/, int /*i*/, void* /*o*/) override { return -1; }
    int bambu_read_sample(void* /*t*/, void* sample_out) override {
        std::lock_guard<std::mutex> lk(mu);
        ++read_samples;
        if (!sample_out) return -1;
        auto* s = static_cast<MirrorBambu_Sample*>(sample_out);
        if (frames.empty()) { std::memset(s, 0, sizeof(*s)); return 1; }
        last = std::move(frames.front()); frames.pop_front();
        s->itrack      = 0;
        s->size        = static_cast<int>(last.bytes.size());
        s->flags       = last.flags;
        s->buffer      = last.bytes.data();
        s->decode_time = last.decode_time;
        return 0;
    }

private:
    ScriptedFrame last;
};

} // namespace

int main() {
    const std::string dev_id      = "0938X1";
    const std::string agora_url   = "bambu:///agora/abcd1234?token=xyz";
    const std::string rtsps_url   = "bambu:///rtsps___bblp:CODE@10.0.0.5/streaming/live/1?proto=rtsps";

    // ---- Happy path #1: cloud plugin hands back an agora URL ----------
    //
    // The whole point of the BambuSource detour is that the bridge
    // doesn't have to understand the scheme — whatever the plugin says,
    // BambuSource takes care of. This subtest pins that the URL flows
    // through verbatim.
    {
        auto plugin = std::make_shared<MockPlugin>();
        auto src    = std::make_shared<MockSource>();
        {
            std::lock_guard<std::mutex> lk(plugin->mu);
            plugin->urls[dev_id] = agora_url;
        }
        // Pre-queue one frame.
        MockSource::ScriptedFrame f1;
        f1.bytes       = {0x00,0x00,0x00,0x01,0x65,0x10,0x20};
        f1.flags       = 1;
        f1.decode_time = 200;
        {
            std::lock_guard<std::mutex> lk(src->mu);
            src->frames.push_back(std::move(f1));
        }

        CloudCameraSourceConfig cfg;
        cfg.dev_id          = dev_id;
        cfg.connect_timeout = std::chrono::seconds(1);
        CloudCameraSource cam(cfg, plugin, src);

        check(cam.open(), "open() returns true (agora URL path)");
        check(cam.last_url() == agora_url, "last_url() == plugin-returned agora URL");
        {
            std::lock_guard<std::mutex> lk(src->mu);
            check(src->creates.size() == 1, "Bambu_Create called once");
            if (!src->creates.empty()) {
                check(src->creates[0].url == agora_url,
                      "Create URL == plugin URL (no rewriting in cloud source)");
            }
            check(src->opens == 1,         "Bambu_Open called");
            check(src->start_streams == 1, "Bambu_StartStream called");
        }
        {
            std::lock_guard<std::mutex> lk(plugin->mu);
            check(plugin->calls == 1, "plugin->get_camera_url called once");
        }

        auto frame = cam.next_frame(0);
        check(frame.has_value(),         "next_frame served a frame");
        if (frame) {
            check(frame->is_keyframe,                "frame is keyframe");
            check(frame->pts_us == 200 * 1000,       "pts in microseconds");
            check(frame->nal_data.size() == 7,       "payload size = 7");
        }

        cam.close();
        {
            std::lock_guard<std::mutex> lk(src->mu);
            check(src->closes   == 1, "Bambu_Close on tunnel teardown");
            check(src->destroys == 1, "Bambu_Destroy on tunnel teardown");
        }
    }

    // ---- Happy path #2: cloud plugin gives back a bambu:///rtsps___ URL
    // (the LAN-discoverable path; cloud just relays it). Same behaviour.
    {
        auto plugin = std::make_shared<MockPlugin>();
        auto src    = std::make_shared<MockSource>();
        {
            std::lock_guard<std::mutex> lk(plugin->mu);
            plugin->urls[dev_id] = rtsps_url;
        }

        CloudCameraSourceConfig cfg; cfg.dev_id = dev_id;
        cfg.connect_timeout = std::chrono::seconds(1);
        CloudCameraSource cam(cfg, plugin, src);

        check(cam.open(),                  "open() succeeds with rtsps URL");
        check(cam.last_url() == rtsps_url, "last_url() preserved");
        {
            std::lock_guard<std::mutex> lk(src->mu);
            check(src->creates.size() == 1,           "Create called");
            if (!src->creates.empty())
                check(src->creates[0].url == rtsps_url, "Create URL == rtsps URL");
        }
        cam.close();
    }

    // ---- Failure: plugin not attached → open() refuses, no Bambu_Create
    {
        auto src = std::make_shared<MockSource>();
        CloudCameraSourceConfig cfg; cfg.dev_id = dev_id;
        cfg.connect_timeout = std::chrono::seconds(1);
        CloudCameraSource cam(cfg, /*plugin=*/nullptr, src);
        check(!cam.open(), "open() refuses with no plugin");
        {
            std::lock_guard<std::mutex> lk(src->mu);
            check(src->creates.empty(), "Bambu_Create NOT called");
        }
    }

    // ---- Failure: BambuSource not attached → open() refuses ----------
    {
        auto plugin = std::make_shared<MockPlugin>();
        {
            std::lock_guard<std::mutex> lk(plugin->mu);
            plugin->urls[dev_id] = agora_url;
        }
        CloudCameraSourceConfig cfg; cfg.dev_id = dev_id;
        cfg.connect_timeout = std::chrono::seconds(1);
        CloudCameraSource cam(cfg, plugin, /*src=*/nullptr);
        check(!cam.open(), "open() refuses with no BambuSourceHandle");
        {
            std::lock_guard<std::mutex> lk(plugin->mu);
            // We DON'T strictly require the plugin to be untouched; current
            // behaviour: it's queried before the source is checked, but
            // even if that changes the test should still hold by virtue
            // of open() returning false.
            (void)plugin->calls;
        }
    }

    // ---- Failure: get_camera_url returns nonzero → open() refuses ----
    {
        auto plugin = std::make_shared<MockPlugin>();
        auto src    = std::make_shared<MockSource>();
        plugin->rc_override = -99;
        CloudCameraSourceConfig cfg; cfg.dev_id = dev_id;
        cfg.connect_timeout = std::chrono::seconds(1);
        CloudCameraSource cam(cfg, plugin, src);
        check(!cam.open(), "open() refuses when get_camera_url errors");
        {
            std::lock_guard<std::mutex> lk(src->mu);
            check(src->creates.empty(), "Bambu_Create NOT called on URL fetch failure");
        }
    }

    if (g_fails) {
        std::fprintf(stderr, "CloudCameraSourcePluginTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("CloudCameraSourcePluginTest: ok\n");
    return 0;
}
