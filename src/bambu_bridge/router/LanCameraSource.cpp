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

    // Format mirrors `~/BambuStudio/src/slic3r/GUI/MediaPlayCtrl.cpp:322`:
    //   "bambu:///rtsps___" + user + ":" + pw + "@" + ip +
    //   "/streaming/live/1?proto=rtsps"
    //
    // Note: TRIPLE underscore between `rtsps` and the credentials.
    std::string url;
    url.reserve(64 + m_cfg.printer_ip.size() + m_cfg.access_code.size());
    url += "bambu:///rtsps___";
    url += m_cfg.username;
    url += ':';
    url += m_cfg.access_code;
    url += '@';
    url += m_cfg.printer_ip;
    url += "/streaming/live/1?proto=rtsps";
    return url;
}

bool LanCameraSource::open() {
    std::shared_ptr<BambuSourceHandle> handle;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        handle = m_source;
    }

    if (!handle) {
        return false;
    }
    // Idempotent init() — BambuSourceHandle guards with a once_flag.
    if (!handle->library_ready()) handle->init();
    if (!handle->library_ready()) {
        return false;
    }

    const std::string u = build_url();

    void* tunnel = nullptr;
    int rc = handle->bambu_create(&tunnel, u);
    if (rc != 0 || !tunnel) {
        return false;
    }
    rc = handle->bambu_open(tunnel);
    if (rc != 0) {
        handle->bambu_destroy(tunnel);
        return false;
    }
    // The printer often returns Bambu_would_block (rc=2) for the first
    // few hundred ms after the tunnel comes up — the camera takes a
    // moment to wake. PrinterFileSystem handles this with a 3s retry
    // loop in Reconnect(); mirror that here. Without the loop the
    // first-attempt liveview always fails on the H2S/H2D, even though
    // the printer is perfectly happy to stream once primed.
    {
        const auto start = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::seconds(3);
        int loops = 0;
        do {
            rc = handle->bambu_start_stream(tunnel, /*video=*/true);
            if (rc != kBambuWouldBlock) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++loops;
        } while (std::chrono::steady_clock::now() - start < timeout);
        if (loops > 0) {
        }
    }
    if (rc != 0) {
        handle->bambu_close(tunnel);
        handle->bambu_destroy(tunnel);
        return false;
    }

    // Pull stream info for the SDP advertising. Find the first VIDEO
    // stream and cache its dimensions / fps. The `format_buffer` holds
    // SPS/PPS for H.264 in Annex-B form; we don't parse it out here
    // (the RtspServer fills sprop-parameter-sets from a separate path),
    // but we keep the cached info around for `info()`.
    server::ICameraSource::StreamInfo si;
    si.fps = 30; // sensible default
    const int count = handle->bambu_get_stream_count(tunnel);
    for (int i = 0; i < count; ++i) {
        MirrorBambu_StreamInfo bi{};
        if (handle->bambu_get_stream_info(tunnel, i, &bi) != 0) continue;
        if (bi.type != kStreamTypeVideo) continue;
        si.width  = bi.format.video.width;
        si.height = bi.format.video.height;
        si.fps    = bi.format.video.frame_rate > 0 ? bi.format.video.frame_rate : si.fps;
        // Stash the library-owned format_buffer bytes (SPS+PPS Annex-B)
        // into our own scratch so the consumer can read them safely after
        // ReadSample invalidates the library's pointer. We split out raw
        // SPS / PPS lazily in info() if needed; for now stash both lumps
        // joined in `sps` (the SDP packetiser expects raw NAL form, and
        // for a non-AVC1 stream this won't matter).
        if (bi.format_size > 0 && bi.format_buffer) {
            si.sps.assign(bi.format_buffer,
                          bi.format_buffer + bi.format_size);
        }
        // Pre-allocate scratch so next_frame() doesn't malloc per call.
        // The library only guarantees `max_frame_size` is a hint; round
        // up generously.
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
