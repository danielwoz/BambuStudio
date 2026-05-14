// Bambu Bridge — cloud-relay camera source (URL via plugin → BambuSource).

#include "CloudCameraSource.hpp"

#include "../BambuNetworkingPluginHandle.hpp"
#include "../BambuSourceHandle.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

// Same mirror structs as LanCameraSource — duplicated here so the cloud
// source TU doesn't have to peek into LanCameraSource.cpp. Both layouts
// must match BambuTunnel.h byte-for-byte.
struct MirrorBambu_StreamInfo {
    int type;
    int sub_type;
    union {
        struct { int width; int height; int frame_rate; }              video;
        struct { int sample_rate; int channel_count; int sample_size; } audio;
    } format;
    int                  format_type;
    int                  format_size;
    int                  max_frame_size;
    unsigned char const* format_buffer;
};

struct MirrorBambu_Sample {
    int                  itrack;
    int                  size;
    int                  flags;
    unsigned char const* buffer;
    unsigned long long   decode_time;
};

constexpr int kBambuSuccess    = 0;
constexpr int kBambuStreamEnd  = 1;
constexpr int kBambuWouldBlock = 2;
constexpr int kStreamTypeVideo = 0;

} // namespace

CloudCameraSource::CloudCameraSource(CloudCameraSourceConfig cfg)
    : m_cfg(std::move(cfg)) {}

CloudCameraSource::CloudCameraSource(
    CloudCameraSourceConfig                       cfg,
    std::shared_ptr<BambuNetworkingPluginHandle>  plugin,
    std::shared_ptr<BambuSourceHandle>            source)
    : m_cfg(std::move(cfg))
    , m_handle(std::move(plugin))
    , m_source(std::move(source)) {}

CloudCameraSource::~CloudCameraSource() { close(); }

void CloudCameraSource::attach_plugin(
    std::shared_ptr<BambuNetworkingPluginHandle> handle) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_handle = std::move(handle);
}

void CloudCameraSource::attach_source_handle(
    std::shared_ptr<BambuSourceHandle> handle) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_source = std::move(handle);
}

bool CloudCameraSource::open() {
    std::shared_ptr<BambuNetworkingPluginHandle> plugin;
    std::shared_ptr<BambuSourceHandle>           source;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        plugin = m_handle;
        source = m_source;
        m_last_url.clear();
    }

    if (!plugin || !plugin->agent_ready()) {
        std::fprintf(stderr,
            "[cloud-camera-source] open(dev=%s) failed: no plugin handle "
            "(get_camera_url requires the proprietary bambu_networking plugin)\n",
            m_cfg.dev_id.c_str());
        return false;
    }
    if (!source) {
        std::fprintf(stderr,
            "[cloud-camera-source] open(dev=%s) failed: no BambuSourceHandle\n",
            m_cfg.dev_id.c_str());
        return false;
    }
    if (!source->library_ready()) source->init();
    if (!source->library_ready()) {
        std::fprintf(stderr,
            "[cloud-camera-source] open(dev=%s) failed: BambuSource not loaded\n",
            m_cfg.dev_id.c_str());
        return false;
    }

    std::string url;
    int rc = 0;
    if (!m_cfg.url_override.empty()) {
        // GUI host pre-resolved the URL via the shared
        // build_media_live_url helper — use it verbatim instead of
        // hitting the plugin's get_camera_url endpoint again.
        url = m_cfg.url_override;
        std::fprintf(stderr,
            "[cloud-camera-source] dev=%s using GUI-provided url_override\n",
            m_cfg.dev_id.c_str());
    } else {
        const int timeout_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                m_cfg.connect_timeout).count());
        rc = plugin->get_camera_url(m_cfg.dev_id, &url, timeout_ms);
        if (rc != 0 || url.empty()) {
            std::fprintf(stderr,
                "[cloud-camera-source] dev=%s get_camera_url rc=%d url='%s'\n",
                m_cfg.dev_id.c_str(), rc, url.c_str());
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_last_url = url;
    }

    void* tunnel = nullptr;
    rc = source->bambu_create(&tunnel, url);
    if (rc != 0 || !tunnel) {
        std::fprintf(stderr,
            "[cloud-camera-source] dev=%s Bambu_Create rc=%d\n",
            m_cfg.dev_id.c_str(), rc);
        return false;
    }
    rc = source->bambu_open(tunnel);
    if (rc != 0) {
        std::fprintf(stderr,
            "[cloud-camera-source] dev=%s Bambu_Open rc=%d\n",
            m_cfg.dev_id.c_str(), rc);
        source->bambu_destroy(tunnel);
        return false;
    }
    // Same Bambu_would_block retry pattern as LanCameraSource (see
    // there for rationale) — the printer takes a moment to wake the
    // camera. Mirrors PrinterFileSystem::Reconnect's 3s loop.
    {
        const auto start = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(3);
        int loops = 0;
        do {
            rc = source->bambu_start_stream(tunnel, /*video=*/true);
            if (rc != kBambuWouldBlock) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++loops;
        } while (std::chrono::steady_clock::now() - start < timeout);
        if (loops > 0) {
            std::fprintf(stderr,
                "[cloud-camera-source] dev=%s StartStream settled after "
                "%d retries (final rc=%d)\n",
                m_cfg.dev_id.c_str(), loops, rc);
        }
    }
    if (rc != 0) {
        std::fprintf(stderr,
            "[cloud-camera-source] dev=%s Bambu_StartStream rc=%d (gave up "
            "after retry loop)\n",
            m_cfg.dev_id.c_str(), rc);
        source->bambu_close(tunnel);
        source->bambu_destroy(tunnel);
        return false;
    }

    server::ICameraSource::StreamInfo si;
    si.fps = 30;
    const int count = source->bambu_get_stream_count(tunnel);
    for (int i = 0; i < count; ++i) {
        MirrorBambu_StreamInfo bi{};
        if (source->bambu_get_stream_info(tunnel, i, &bi) != 0) continue;
        if (bi.type != kStreamTypeVideo) continue;
        si.width  = bi.format.video.width;
        si.height = bi.format.video.height;
        si.fps    = bi.format.video.frame_rate > 0 ? bi.format.video.frame_rate : si.fps;
        if (bi.format_size > 0 && bi.format_buffer) {
            si.sps.assign(bi.format_buffer,
                          bi.format_buffer + bi.format_size);
        }
        const int scratch = bi.max_frame_size > 0
                            ? bi.max_frame_size + 64
                            : 256 * 1024;
        m_scratch.reserve(static_cast<std::size_t>(scratch));
        break;
    }

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_tunnel = tunnel;
        m_info   = si;
    }
    m_open.store(true);
    return true;
}

void CloudCameraSource::close() {
    void* tunnel = nullptr;
    std::shared_ptr<BambuSourceHandle> source;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        tunnel  = m_tunnel;
        source  = m_source;
        m_tunnel = nullptr;
    }
    m_open.store(false);
    if (tunnel && source) {
        source->bambu_close(tunnel);
        source->bambu_destroy(tunnel);
    }
}

bool CloudCameraSource::is_open() const { return m_open.load(); }

std::optional<server::VideoFrame>
CloudCameraSource::next_frame(int /*timeout_ms*/) {
    if (!m_open.load()) return std::nullopt;
    void* tunnel = nullptr;
    std::shared_ptr<BambuSourceHandle> source;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        tunnel = m_tunnel;
        source = m_source;
    }
    if (!tunnel || !source) return std::nullopt;

    MirrorBambu_Sample sample{};
    int rc = source->bambu_read_sample(tunnel, &sample);
    if (rc == kBambuWouldBlock) return std::nullopt;
    if (rc != kBambuSuccess) {
        m_open.store(false);
        return std::nullopt;
    }
    if (sample.size <= 0 || !sample.buffer) return std::nullopt;

    server::VideoFrame f;
    f.nal_data.assign(sample.buffer,
                      sample.buffer + static_cast<std::size_t>(sample.size));
    f.pts_us      = static_cast<int64_t>(sample.decode_time) * 1000;
    f.is_keyframe = (sample.flags & 1) != 0;
    return f;
}

server::ICameraSource::StreamInfo CloudCameraSource::info() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_info;
}

std::string CloudCameraSource::last_url() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_last_url;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
