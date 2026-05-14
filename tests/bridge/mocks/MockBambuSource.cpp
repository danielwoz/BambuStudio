// Bambu Bridge — MockBambuSource implementation (harness).

#include "MockBambuSource.hpp"

#include <cstring>
#include <memory>

namespace Slic3r {
namespace bridge {
namespace mocks {

namespace {

// Mirror of __Bambu_Sample from BambuTunnel.h. Same layout
// LanCameraSourcePluginTest's mock uses — keep in sync.
struct MirrorBambu_Sample {
    int                  itrack;
    int                  size;
    int                  flags;
    unsigned char const* buffer;
    unsigned long long   decode_time;
};

// Single fake MJPG-ish frame. The bytes don't decode — they're enough
// to prove the create/open/start_stream/read/close lifecycle wires up.
const unsigned char kFrame[] = {
    0xFF, 0xD8,                            // SOI
    0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00,
    0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
    0xFF, 0xD9,                            // EOI
};

} // namespace

const std::vector<unsigned char>& MockBambuSource::mjpg_sample() {
    static const std::vector<unsigned char> bytes(
        std::begin(kFrame), std::end(kFrame));
    return bytes;
}

MockBambuSource::MockBambuSource()
    : BambuSourceHandle(BambuSourceConfig{}) {
    set_library_ready_for_test(true);
}

int MockBambuSource::bambu_create(void** out_tunnel, const std::string& url) {
    if (!out_tunnel) return -1;
    std::lock_guard<std::mutex> lk(m_mu);
    auto state = std::make_unique<TunnelState>();
    state->url = url;
    *out_tunnel = state.get();
    m_tunnels.push_back(std::move(state));
    return 0;
}

void MockBambuSource::bambu_destroy(void* tunnel) {
    if (!tunnel) return;
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto it = m_tunnels.begin(); it != m_tunnels.end(); ++it) {
        if (it->get() == tunnel) {
            m_tunnels.erase(it);
            return;
        }
    }
}

int MockBambuSource::bambu_open(void* tunnel) {
    if (!tunnel) return -1;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto* s = static_cast<TunnelState*>(tunnel);
        s->open = true;
    }
    m_open_count.fetch_add(1);
    return 0;
}

void MockBambuSource::bambu_close(void* tunnel) {
    if (!tunnel) return;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto* s = static_cast<TunnelState*>(tunnel);
        s->open = false;
        s->started = false;
    }
    m_close_count.fetch_add(1);
}

int MockBambuSource::bambu_start_stream(void* tunnel, bool /*video*/) {
    if (!tunnel) return -1;
    std::lock_guard<std::mutex> lk(m_mu);
    auto* s = static_cast<TunnelState*>(tunnel);
    s->started = true;
    return 0;
}

int MockBambuSource::bambu_get_stream_count(void* tunnel) {
    if (!tunnel) return 0;
    return 1; // one video stream
}

int MockBambuSource::bambu_get_stream_info(void* tunnel, int /*index*/,
                                           void* info_out) {
    if (!tunnel || !info_out) return -1;
    // Caller (RtspServer / LanCameraSource) reads only a small subset
    // of fields — those it reads in this codepath are size-checked. For
    // the harness we zero the buffer and return success; consumers that
    // care about exact stream info should not exercise this mock.
    std::memset(info_out, 0, 64);
    return 0;
}

int MockBambuSource::bambu_read_sample(void* tunnel, void* sample_out) {
    if (!tunnel || !sample_out) return -1;
    std::lock_guard<std::mutex> lk(m_mu);
    auto* s = static_cast<TunnelState*>(tunnel);
    if (!s->started || !s->open) return 2;  // would_block
    const auto& frame = mjpg_sample();
    auto* out = static_cast<MirrorBambu_Sample*>(sample_out);
    out->itrack      = 0;
    out->size        = static_cast<int>(frame.size());
    out->flags       = 1; // sync (IDR equivalent)
    out->buffer      = frame.data();
    out->decode_time = static_cast<unsigned long long>(++s->read_cursor) * 33;
    m_read_count.fetch_add(1);
    // After 60 frames pretend EOS so loops terminate.
    if (s->read_cursor >= 60) return 1;
    return 0;
}

} // namespace mocks
} // namespace bridge
} // namespace Slic3r
