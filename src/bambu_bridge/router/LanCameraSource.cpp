// Bambu Bridge — LAN-direct camera source (routes through BambuSource).

#include "LanCameraSource.hpp"

#include "../BambuSourceHandle.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

// Mirror of `struct __Bambu_StreamInfo` from BambuTunnel.h. We use a
// `void*` opaque interface in BambuSourceHandle so the public bridge
// API doesn't need to spill the proprietary header; here at the call
// site we cast to / from a local mirror.
//
// Layout MUST match BambuTunnel.h byte-for-byte. Last verified against
// `src/slic3r/GUI/Printer/BambuTunnel.h` in this worktree (vendored
// read-only).
struct MirrorBambu_StreamInfo {
    int type;       // 0 = video, 1 = audio
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
    int                  flags;          // bit 0 (`f_sync`) = IDR / sync point
    unsigned char const* buffer;
    unsigned long long   decode_time;
};

// Bambu_Error values from BambuTunnel.h.
constexpr int kBambuSuccess     = 0;
constexpr int kBambuStreamEnd   = 1;
constexpr int kBambuWouldBlock  = 2;

// Bambu_StreamType: 0 = video.
constexpr int kStreamTypeVideo = 0;
// Bambu_VideoSubType: 0 = AVC1 (H.264), 1 = MJPG.
constexpr int kSubTypeAvc      = 0;

} // namespace

LanCameraSource::LanCameraSource(LanCameraSourceConfig cfg)
    : m_cfg(std::move(cfg)) {}

LanCameraSource::LanCameraSource(LanCameraSourceConfig               cfg,
                                 std::shared_ptr<BambuSourceHandle>  source)
    : m_cfg(std::move(cfg)), m_source(std::move(source)) {}

LanCameraSource::~LanCameraSource() { close(); }

void LanCameraSource::attach_source_handle(std::shared_ptr<BambuSourceHandle> handle) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_source = std::move(handle);
}

std::string LanCameraSource::url() const { return build_url(); }

std::string LanCameraSource::build_url() const {
    // When the GUI host has pre-resolved a camera URL through the
    // shared `Slic3r::GUI::build_media_live_url` helper (e.g. a
    // bambu:///tutk?... or a bambu:///local/... per the printer's own
    // protocol flags), use it verbatim. Otherwise fall back to the
    // simple LAN-direct RTSPS form that's worked for every printer
    // we've tested headless.
    if (!m_cfg.url_override.empty()) return m_cfg.url_override;

    // Format mirrors `~/BambuStudio/src/slic3r/GUI/MediaPlayCtrl.cpp:322-329`
    // byte-for-byte. The proprietary plugin fingerprints the URL — any
    // missing query param can make `bambu_start_stream` reject with
    // `rc=-107`. Always emit all 5 params, even when individual fields
    // are empty (the GUI also emits empty `&dev_ver=` etc. before its
    // get_version reply arrives).
    //
    // Note: TRIPLE underscore between `rtsps` and the credentials.
    std::string url;
    url.reserve(256 + m_cfg.printer_ip.size() + m_cfg.access_code.size()
                + m_cfg.dev_id.size() + m_cfg.slicer_net_ver.size()
                + m_cfg.slicer_cli_id.size() + m_cfg.slicer_cli_ver.size());
    url += "bambu:///rtsps___";
    url += m_cfg.username;
    url += ':';
    url += m_cfg.access_code;
    url += '@';
    url += m_cfg.printer_ip;
    url += "/streaming/live/1?proto=rtsps";
    url += "&device=";  url += m_cfg.dev_id;
    url += "&net_ver="; url += m_cfg.slicer_net_ver;
    url += "&dev_ver="; url += m_cfg.slicer_dev_ver;
    url += "&cli_id=";  url += m_cfg.slicer_cli_id;
    url += "&cli_ver="; url += m_cfg.slicer_cli_ver;
    return url;
}

// One pass: try a single URL through bambu_create / open / start_stream.
// On success returns {tunnel, info}; on failure logs and returns nullptr
// tunnel + best-effort cleanup. `tag` is a short label ("rtsps", "local")
// included in log lines so cross-URL fallback transitions are clear.
//
// Encodes the same per-call invariants the GUI's wxMediaCtrl3 path
// requires (set_logger between Create and Open, 3 s would_block grace
// window before treating bambu_start_stream as a hard failure).
//
// Returns the new tunnel on success, nullptr on failure. The stream
// info is written into `out_si` on success only.
static void* try_open_url(BambuSourceHandle& handle,
                          const std::string& dev_id,
                          const std::string& tag,
                          const std::string& url,
                          server::ICameraSource::StreamInfo& out_si,
                          std::vector<uint8_t>& out_scratch,
                          int& out_last_rc) {
    out_last_rc = 0;
    std::fprintf(stderr,
        "[lan-camera] open dev=%s tag=%s url=%s\n",
        dev_id.c_str(), tag.c_str(), url.c_str());
    std::fflush(stderr);

    void* tunnel = nullptr;
    int rc = handle.bambu_create(&tunnel, url);
    if (rc != 0 || !tunnel) {
        std::fprintf(stderr,
            "[lan-camera] open dev=%s tag=%s FAIL: bambu_create rc=%d tunnel=%p\n",
            dev_id.c_str(), tag.c_str(), rc, tunnel);
        std::fflush(stderr);
        out_last_rc = rc;
        return nullptr;
    }

    // The GUI (wxMediaCtrl3.cpp:288) sets a logger BETWEEN Create and
    // Open. Without it the plugin appears to fingerprint the caller as
    // unauthenticated and `Bambu_StartStream` later returns -107.
    struct LogCtx { std::string dev_id; std::string tag; };
    static thread_local LogCtx s_log_ctx;
    s_log_ctx.dev_id = dev_id;
    s_log_ctx.tag    = tag;
    handle.bambu_set_logger(tunnel,
        +[](void* ctx, int level, const char* msg) {
            auto* lc = static_cast<LogCtx*>(ctx);
            std::fprintf(stderr,
                "[lan-camera] bambu-log dev=%s tag=%s lvl=%d %s\n",
                lc ? lc->dev_id.c_str() : "?",
                lc ? lc->tag.c_str()    : "?",
                level, msg ? msg : "");
            std::fflush(stderr);
        },
        &s_log_ctx);

    rc = handle.bambu_open(tunnel);
    if (rc != 0) {
        std::fprintf(stderr,
            "[lan-camera] open dev=%s tag=%s FAIL: bambu_open rc=%d\n",
            dev_id.c_str(), tag.c_str(), rc);
        std::fflush(stderr);
        handle.bambu_destroy(tunnel);
        out_last_rc = rc;
        return nullptr;
    }
    // The printer often returns Bambu_would_block (rc=2) for the first
    // few hundred ms after the tunnel comes up — the camera takes a
    // moment to wake. PrinterFileSystem handles this with a 3s retry
    // loop in Reconnect(); mirror that here. Without the loop the
    // first-attempt liveview always fails on the H2S/H2D, even though
    // the printer is perfectly happy to stream once primed.
    int start_loops = 0;
    {
        const auto start = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(3);
        do {
            rc = handle.bambu_start_stream(tunnel, /*video=*/true);
            if (rc != kBambuWouldBlock) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++start_loops;
        } while (std::chrono::steady_clock::now() - start < timeout);
    }
    if (rc != 0) {
        std::fprintf(stderr,
            "[lan-camera] open dev=%s tag=%s FAIL: bambu_start_stream rc=%d (after %d would_block retries)\n",
            dev_id.c_str(), tag.c_str(), rc, start_loops);
        std::fflush(stderr);
        handle.bambu_close(tunnel);
        handle.bambu_destroy(tunnel);
        out_last_rc = rc;
        return nullptr;
    }
    std::fprintf(stderr,
        "[lan-camera] open dev=%s tag=%s OK (bambu_start_stream succeeded after %d would_block retries)\n",
        dev_id.c_str(), tag.c_str(), start_loops);
    std::fflush(stderr);

    // Pull stream info for the SDP advertising. Find the first VIDEO
    // stream and cache its dimensions / fps. The `format_buffer` holds
    // SPS/PPS for H.264 in Annex-B form.
    out_si      = server::ICameraSource::StreamInfo{};
    out_si.fps  = 30;
    const int count = handle.bambu_get_stream_count(tunnel);
    for (int i = 0; i < count; ++i) {
        MirrorBambu_StreamInfo bi{};
        if (handle.bambu_get_stream_info(tunnel, i, &bi) != 0) continue;
        if (bi.type != kStreamTypeVideo) continue;
        out_si.width  = bi.format.video.width;
        out_si.height = bi.format.video.height;
        out_si.fps    = bi.format.video.frame_rate > 0 ? bi.format.video.frame_rate : out_si.fps;
        if (bi.format_size > 0 && bi.format_buffer) {
            out_si.sps.assign(bi.format_buffer,
                              bi.format_buffer + bi.format_size);
        }
        const int scratch = bi.max_frame_size > 0
                            ? bi.max_frame_size + 64
                            : 256 * 1024;
        out_scratch.reserve(static_cast<std::size_t>(scratch));
        break;
    }
    return tunnel;
}

bool LanCameraSource::open() {
    std::shared_ptr<BambuSourceHandle> handle;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        handle = m_source;
    }

    if (!handle) {
        std::fprintf(stderr,
            "[lan-camera] open dev=%s FAIL: no plugin handle attached\n",
            m_cfg.dev_id.c_str());
        std::fflush(stderr);
        return false;
    }
    // Idempotent init() — BambuSourceHandle guards with a once_flag.
    if (!handle->library_ready()) handle->init();
    if (!handle->library_ready()) {
        std::fprintf(stderr,
            "[lan-camera] open dev=%s FAIL: BambuSource library_ready() false after init\n",
            m_cfg.dev_id.c_str());
        std::fflush(stderr);
        return false;
    }

    // Two-step LAN ladder: prefer the primary URL (typically RTSPS via
    // port 322), but fall back to the printer's local-protocol port
    // 6000 stream when the primary fails at start_stream. On modern H2
    // firmware where the user has not enabled LAN RTSPS via the
    // touchscreen, port 322 is closed and the plugin's live555 client
    // returns -107 within a few hundred ms; port 6000 stays open and
    // serves the same video over the bambu:///local/...?port=6000 form.
    //
    // We only fall back when the primary tag is "rtsps" (or a custom
    // override that ISN'T already a local URL) AND a fallback URL was
    // supplied by the caller. If the caller didn't bother, keep the
    // historical single-attempt behaviour so a misconfigured deploy
    // fails loud instead of silently re-trying.
    const std::string primary_url = build_url();
    const bool primary_is_local =
        primary_url.find("bambu:///local/") != std::string::npos;
    const std::string primary_tag = primary_is_local ? "local" : "rtsps";

    server::ICameraSource::StreamInfo si;
    int last_rc = 0;
    void* tunnel = try_open_url(*handle, m_cfg.dev_id,
                                primary_tag, primary_url,
                                si, m_scratch, last_rc);

    if (!tunnel
        && !primary_is_local
        && !m_cfg.local_fallback_url.empty()) {
        // Fall back to LAN port-6000 local form. -107 from live555 is
        // the typical "TCP refused / firmware disabled RTSP" code; we
        // also retry on any non-zero rc out of paranoia, since the
        // worst case is a single extra create/open round-trip.
        std::fprintf(stderr,
            "[lan-camera] open dev=%s primary tag=%s rc=%d failed, "
            "trying local fallback url=%s\n",
            m_cfg.dev_id.c_str(), primary_tag.c_str(), last_rc,
            m_cfg.local_fallback_url.c_str());
        std::fflush(stderr);
        tunnel = try_open_url(*handle, m_cfg.dev_id,
                              "local-fallback",
                              m_cfg.local_fallback_url,
                              si, m_scratch, last_rc);
    }

    if (!tunnel) return false;

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_tunnel = tunnel;
        m_info   = si;
    }
    m_open.store(true);
    return true;
}

void LanCameraSource::close() {
    void* tunnel = nullptr;
    std::shared_ptr<BambuSourceHandle> handle;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        tunnel  = m_tunnel;
        handle  = m_source;
        m_tunnel = nullptr;
    }
    m_open.store(false);
    if (tunnel && handle) {
        handle->bambu_close(tunnel);
        handle->bambu_destroy(tunnel);
    }
}

bool LanCameraSource::is_open() const { return m_open.load(); }

std::optional<server::VideoFrame>
LanCameraSource::next_frame(int /*timeout_ms*/) {
    if (!m_open.load()) return std::nullopt;
    void* tunnel = nullptr;
    std::shared_ptr<BambuSourceHandle> handle;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        tunnel = m_tunnel;
        handle = m_source;
    }
    if (!tunnel || !handle) return std::nullopt;

    MirrorBambu_Sample sample{};
    int rc = handle->bambu_read_sample(tunnel, &sample);
    if (rc == kBambuWouldBlock) {
        // Caller asked for a frame but none ready. Per ICameraSource's
        // contract this is "timeout, not EOS".
        return std::nullopt;
    }
    if (rc != kBambuSuccess) {
        // 1 = stream_end, 3 = buffer_limit (treated as EOS for now), or
        // negative from our handle (library not loaded — shouldn't happen
        // post-open). Flip closed so is_open() reflects the new state.
        m_open.store(false);
        return std::nullopt;
    }
    if (sample.size <= 0 || !sample.buffer) return std::nullopt;

    server::VideoFrame f;
    f.nal_data.assign(sample.buffer,
                      sample.buffer + static_cast<std::size_t>(sample.size));
    // `decode_time` is in milliseconds in BambuSource (see the slicer's
    // `gstbambusrc.c` for the gstreamer-side conversion). Multiply to
    // microseconds for our PTS contract.
    f.pts_us      = static_cast<int64_t>(sample.decode_time) * 1000;
    f.is_keyframe = (sample.flags & 1) != 0;
    return f;
}

server::ICameraSource::StreamInfo LanCameraSource::info() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_info;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
