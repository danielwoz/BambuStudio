// Bambu Bridge — camera source router implementation (phase 9).

#include "CameraSourceRouter.hpp"

#include "CloudCameraSource.hpp"
#include "JpegCameraSource.hpp"
#include "LanCameraSource.hpp"
#include "NullCameraSource.hpp"
#include "UplinkHealth.hpp"

#include <cstdio>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

const char* choice_name(CameraSourceRouter::Choice c) {
    switch (c) {
    case CameraSourceRouter::Choice::None:  return "None";
    case CameraSourceRouter::Choice::Lan:   return "Lan";
    case CameraSourceRouter::Choice::Cloud: return "Cloud";
    case CameraSourceRouter::Choice::Jpeg:  return "Jpeg";
    case CameraSourceRouter::Choice::Null:  return "Null";
    }
    return "?";
}

} // namespace

CameraSourceRouter::CameraSourceRouter(std::string dev_id)
    : m_dev_id(std::move(dev_id)) {}

CameraSourceRouter::~CameraSourceRouter() {
    // Best-effort close on destruction.
    close();
}

void CameraSourceRouter::set_lan_source(std::shared_ptr<LanCameraSource> lan) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_lan = std::move(lan);
}

void CameraSourceRouter::set_cloud_source(std::shared_ptr<CloudCameraSource> cloud) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_cloud = std::move(cloud);
}

void CameraSourceRouter::set_jpeg_source(std::shared_ptr<JpegCameraSource> jpeg) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_jpeg = std::move(jpeg);
}

void CameraSourceRouter::set_null_source(std::shared_ptr<NullCameraSource> null) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_null = std::move(null);
}

void CameraSourceRouter::set_health_monitor(std::shared_ptr<UplinkHealthMonitor> monitor) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_health = std::move(monitor);
}

void CameraSourceRouter::set_policy(Policy p) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_policy = p;
}

std::shared_ptr<server::ICameraSource>
CameraSourceRouter::pick_locked(Choice c) const {
    switch (c) {
    case Choice::Lan:   return std::static_pointer_cast<server::ICameraSource>(m_lan);
    case Choice::Cloud: return std::static_pointer_cast<server::ICameraSource>(m_cloud);
    case Choice::Jpeg:  return std::static_pointer_cast<server::ICameraSource>(m_jpeg);
    case Choice::Null:  return std::static_pointer_cast<server::ICameraSource>(m_null);
    case Choice::None:  break;
    }
    return nullptr;
}

bool CameraSourceRouter::open() {
    // Sample everything under lock first.
    std::shared_ptr<LanCameraSource>     lan;
    std::shared_ptr<CloudCameraSource>   cloud;
    std::shared_ptr<JpegCameraSource>    jpeg;
    std::shared_ptr<NullCameraSource>    null;
    std::shared_ptr<UplinkHealthMonitor> health;
    Policy                               policy;
    std::string                          dev_id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_open) return true; // already open
        lan    = m_lan;
        cloud  = m_cloud;
        jpeg   = m_jpeg;
        null   = m_null;
        health = m_health;
        policy = m_policy;
        dev_id = m_dev_id;
    }

    // Always attempt every configured source, in preference order.
    // The health monitor's `lan_connected` / `cloud_connected` reflect
    // the MQTT session status — useful for routing MQTT traffic, but
    // not the right gate for camera. For camera, the printer can have
    // RTSPS disabled and TUTK disabled (newer firmware default) yet
    // still serve frames via cloud relay. Let each source's open()
    // decide for itself; we just fall through on failure.
    const bool lan_ok   = static_cast<bool>(lan);
    const bool cloud_ok = static_cast<bool>(cloud);
    const bool jpeg_ok  = static_cast<bool>(jpeg);
    (void)health;

    // Build the preference order. JPEG (A1/P1 native) always wins when
    // `prefer_jpeg` is set AND a JPEG source is configured — for those
    // models the H.264 LanCameraSource path is wrong (printer's port 322
    // isn't an RTSPS server) and cloud path requires TUTK plugin. JPEG
    // failing falls back to LAN→Cloud→Null in the usual order so the
    // bridge degrades gracefully (e.g. printer offline).
    Choice order[4] = { Choice::None, Choice::None, Choice::None, Choice::None };
    int    n        = 0;
    if (policy.prefer_jpeg && jpeg_ok && jpeg) {
        order[n++] = Choice::Jpeg;
    }
    if (policy.prefer_lan) {
        if (lan_ok   && lan)   order[n++] = Choice::Lan;
        if (cloud_ok && cloud) order[n++] = Choice::Cloud;
    } else {
        if (cloud_ok && cloud) order[n++] = Choice::Cloud;
        if (lan_ok   && lan)   order[n++] = Choice::Lan;
    }
    if (policy.allow_null_fallback && null) order[n++] = Choice::Null;

    for (int i = 0; i < n; ++i) {
        std::shared_ptr<server::ICameraSource> src;
        switch (order[i]) {
        case Choice::Lan:   src = lan;   break;
        case Choice::Cloud: src = cloud; break;
        case Choice::Jpeg:  src = jpeg;  break;
        case Choice::Null:  src = null;  break;
        default: break;
        }
        if (!src) continue;
        if (src->open()) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_choice = order[i];
            m_chosen = src;
            m_open   = true;
            std::fprintf(stderr,
                "[camera-router] dev=%s opened via %s\n",
                dev_id.c_str(), choice_name(order[i]));
            std::fflush(stderr);
            return true;
        }
    }

    std::lock_guard<std::mutex> lk(m_mu);
    m_choice = Choice::None;
    m_chosen.reset();
    m_open   = false;
    return false;
}

void CameraSourceRouter::close() {
    std::shared_ptr<server::ICameraSource> src;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_open) return;
        src      = m_chosen;
        m_chosen.reset();
        m_choice = Choice::None;
        m_open   = false;
    }
    if (src) src->close();
}

bool CameraSourceRouter::is_open() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_open;
}

std::optional<server::VideoFrame> CameraSourceRouter::next_frame(int timeout_ms) {
    std::shared_ptr<server::ICameraSource> src;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_open) return std::nullopt;
        src = m_chosen;
    }
    if (!src) return std::nullopt;
    // Sticky semantics: do NOT switch sources on nullopt from the
    // underlying stream. The RtspServer will TEARDOWN the session and the
    // slicer will reconnect, which calls open() again and re-picks fresh.
    return src->next_frame(timeout_ms);
}

server::ICameraSource::StreamInfo CameraSourceRouter::info() const {
    std::shared_ptr<server::ICameraSource> src;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        src = m_chosen;
    }
    if (!src) return server::ICameraSource::StreamInfo{};
    return src->info();
}

CameraSourceRouter::Choice CameraSourceRouter::current_choice() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_choice;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
