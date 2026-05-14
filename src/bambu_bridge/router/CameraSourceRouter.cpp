// Bambu Bridge — camera source router implementation (phase 9).

#include "CameraSourceRouter.hpp"

#include "CloudCameraSource.hpp"
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
    case Choice::Null:  return std::static_pointer_cast<server::ICameraSource>(m_null);
    case Choice::None:  break;
    }
    return nullptr;
}

bool CameraSourceRouter::open() {
    // Sample everything under lock first.
    std::shared_ptr<LanCameraSource>     lan;
    std::shared_ptr<CloudCameraSource>   cloud;
    std::shared_ptr<NullCameraSource>    null;
    std::shared_ptr<UplinkHealthMonitor> health;
    Policy                               policy;
    std::string                          dev_id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_open) return true; // already open
        lan    = m_lan;
        cloud  = m_cloud;
        null   = m_null;
        health = m_health;
        policy = m_policy;
        dev_id = m_dev_id;
    }

    bool lan_ok   = false;
    bool cloud_ok = false;
    if (health) {
        auto snap = health->snapshot(dev_id);
        lan_ok    = snap.lan_connected;
        cloud_ok  = snap.cloud_connected;
    } else {
        // No health monitor → assume available iff the source pointer is set.
        lan_ok   = static_cast<bool>(lan);
        cloud_ok = static_cast<bool>(cloud);
    }

    // Build the preference order.
    Choice order[3] = { Choice::None, Choice::None, Choice::None };
    int    n        = 0;
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
        case Choice::Null:  src = null;  break;
        default: break;
        }
        if (!src) continue;
        if (src->open()) {
            std::lock_guard<std::mutex> lk(m_mu);
            m_choice = order[i];
            m_chosen = src;
            m_open   = true;
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
