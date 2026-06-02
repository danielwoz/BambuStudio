// Bambu Bridge — passthrough camera-source wrapper (impl).

#include "PassthroughCameraSource.hpp"

#include <cstdio>

namespace Slic3r {
namespace bridge {
namespace server {

PassthroughCameraSource::PassthroughCameraSource(std::shared_ptr<ICameraSource> inner)
    : m_inner(std::move(inner)) {}

PassthroughCameraSource::~PassthroughCameraSource() { close(); }

bool PassthroughCameraSource::open() {
    if (!m_inner || !m_inner->open()) return false;
    m_out_info = m_inner->info();
    m_open.store(true);
    std::fprintf(stderr,
        "[camera-passthrough] open codec=%s %dx%d@%dfps\n",
        m_out_info.codec == Codec::H264_AnnexB ? "H.264" : "MJPEG",
        m_out_info.width, m_out_info.height, m_out_info.fps);
    std::fflush(stderr);
    return true;
}

void PassthroughCameraSource::close() {
    if (m_inner) m_inner->close();
    m_open.store(false);
}

bool PassthroughCameraSource::is_open() const {
    return m_open.load() && m_inner && m_inner->is_open();
}

std::optional<VideoFrame> PassthroughCameraSource::next_frame(int timeout_ms) {
    if (!m_open.load() || !m_inner) return std::nullopt;
    return m_inner->next_frame(timeout_ms);
}

ICameraSource::StreamInfo PassthroughCameraSource::info() const {
    return m_out_info;
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
