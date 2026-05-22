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
            "[cloud-camera] open dev=%s FAIL: plugin not ready (plugin=%p agent_ready=%d)\n",
            m_cfg.dev_id.c_str(), (void*)plugin.get(),
            plugin ? int(plugin->agent_ready()) : -1);
        std::fflush(stderr);
        return false;
    }
    if (!source) {
        std::fprintf(stderr,
            "[cloud-camera] open dev=%s FAIL: no BambuSource attached\n",
            m_cfg.dev_id.c_str());
        std::fflush(stderr);
        return false;
    }
    if (!source->library_ready()) source->init();
    if (!source->library_ready()) {
        std::fprintf(stderr,
            "[cloud-camera] open dev=%s FAIL: BambuSource library_ready() false\n",
            m_cfg.dev_id.c_str());
        std::fflush(stderr);
        return false;
    }

    std::string url;
    int rc = 0;
    if (!m_cfg.url_override.empty()) {
        url = m_cfg.url_override;
    } else {
        const int timeout_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                m_cfg.connect_timeout).count());
        // Mirror MediaPlayCtrl.cpp:374 — pass `<dev_id>|<dev_ver>|<protocols>`
        // not just `<dev_id>`. Without dev_ver and protocols the plugin
        // doesn't know whether the caller can accept tutk/agora and may
        // return an empty URL.
        const std::string protocols = "\"tutk\",\"agora\"";
        std::string ask = m_cfg.dev_id + "|" + m_cfg.dev_ver
                        + "|" + protocols;
        rc = plugin->get_camera_url(ask, &url, timeout_ms);
        std::fprintf(stderr,
            "[cloud-camera] get_camera_url dev=%s rc=%d url=%s\n",
            m_cfg.dev_id.c_str(), rc, url.c_str());
        std::fflush(stderr);
        if (rc != 0 || url.empty()) {
            return false;
        }
        // MediaPlayCtrl.cpp:381-385 — when the URL starts with bambu:///
        // the GUI appends `&device=&net_ver=&dev_ver=&refresh_url=
        // &cli_id=&cli_ver=`. The proprietary plugin fingerprints these.
        if (url.compare(0, 9, "bambu:///") == 0) {
            url += "&device=";  url += m_cfg.dev_id;
            url += "&net_ver="; url += m_cfg.net_ver;
            url += "&dev_ver="; url += m_cfg.dev_ver;
            url += "&cli_id=";  url += m_cfg.cli_id;
            url += "&cli_ver="; url += m_cfg.cli_ver;
            std::fprintf(stderr,
                "[cloud-camera] augmented url dev=%s url=%s\n",
                m_cfg.dev_id.c_str(), url.c_str());
            std::fflush(stderr);
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
            "[cloud-camera] open dev=%s FAIL: bambu_create rc=%d tunnel=%p\n",
            m_cfg.dev_id.c_str(), rc, tunnel);
        std::fflush(stderr);
        return false;
    }
    // Mirror wxMediaCtrl3.cpp:288 — install logger BETWEEN Create and Open.
    struct LogCtx { std::string dev_id; };
    static thread_local LogCtx s_cloud_log_ctx;
    s_cloud_log_ctx.dev_id = m_cfg.dev_id;
    source->bambu_set_logger(tunnel,
        +[](void* ctx, int level, const char* msg) {
            auto* lc = static_cast<LogCtx*>(ctx);
            std::fprintf(stderr,
                "[cloud-camera] bambu-log dev=%s lvl=%d %s\n",
                lc ? lc->dev_id.c_str() : "?", level, msg ? msg : "");
            std::fflush(stderr);
        },
        &s_cloud_log_ctx);
    rc = source->bambu_open(tunnel);
    if (rc != 0) {
        std::fprintf(stderr,
            "[cloud-camera] open dev=%s FAIL: bambu_open rc=%d\n",
            m_cfg.dev_id.c_str(), rc);
        std::fflush(stderr);
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
        }
    }
    if (rc != 0) {
        std::fprintf(stderr,
            "[cloud-camera] open dev=%s FAIL: bambu_start_stream rc=%d\n",
            m_cfg.dev_id.c_str(), rc);
        std::fflush(stderr);
        source->bambu_close(tunnel);
        source->bambu_destroy(tunnel);
        return false;
    }
    std::fprintf(stderr,
        "[cloud-camera] open dev=%s OK\n", m_cfg.dev_id.c_str());
    std::fflush(stderr);

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
