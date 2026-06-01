// Bambu Bridge — MQTT session router implementation (phase 9).

#include "SessionRouter.hpp"

#include "CloudUplink.hpp"
#include "LanUplink.hpp"
#include "UplinkHealth.hpp"

#include <cstdio>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

const char* route_name(SessionRouter::Route r) {
    switch (r) {
    case SessionRouter::Route::None:  return "None";
    case SessionRouter::Route::Lan:   return "Lan";
    case SessionRouter::Route::Cloud: return "Cloud";
    }
    return "?";
}

} // namespace

SessionRouter::SessionRouter()  = default;
SessionRouter::~SessionRouter() = default;

void SessionRouter::set_lan_uplink(std::shared_ptr<LanUplink> lan) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_lan = std::move(lan);
    // Invalidate the cache so the next call re-picks against the new uplink.
    m_cache.clear();
}

void SessionRouter::set_cloud_uplink(std::shared_ptr<CloudUplink> cloud) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_cloud = std::move(cloud);
    m_cache.clear();
}

void SessionRouter::set_health_monitor(std::shared_ptr<UplinkHealthMonitor> monitor) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_health = std::move(monitor);
    m_cache.clear();
}

void SessionRouter::set_policy(Policy p) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_policy = p;
    m_cache.clear();
}

SessionRouter::Route SessionRouter::pick_route(const std::string& dev_id) const {
    // Sample everything under the lock first.
    std::shared_ptr<LanUplink>           lan;
    std::shared_ptr<CloudUplink>         cloud;
    std::shared_ptr<UplinkHealthMonitor> health;
    Policy                               policy;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan    = m_lan;
        cloud  = m_cloud;
        health = m_health;
        policy = m_policy;

        // Honour the debounce window if we have a fresh cache entry.
        auto it = m_cache.find(dev_id);
        if (it != m_cache.end()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - it->second.at < policy.rerouting_debounce) {
                return it->second.route;
            }
        }
    }

    // Health monitor is unreliable for the embedded-bridge case (LAN's
    // is_connected is true iff the plugin's connect_printer has been
    // swapped to this dev_id, but the swap-on-subscribe path doesn't
    // run for headless probes). Fall back to "uplink exists -> route
    // available" — let each sub-uplink reject if it can't actually
    // deliver. Same pattern as CameraSourceRouter.
    const bool lan_ok   = static_cast<bool>(lan);
    const bool cloud_ok = static_cast<bool>(cloud);
    (void)health;

    Route chosen = Route::None;
    if (policy.prefer_lan) {
        if      (lan_ok)   chosen = Route::Lan;
        else if (cloud_ok) chosen = Route::Cloud;
    } else {
        if      (cloud_ok) chosen = Route::Cloud;
        else if (lan_ok)   chosen = Route::Lan;
    }

    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto& slot = m_cache[dev_id];
        if (slot.route != chosen) {
        }
        slot.route = chosen;
        slot.at    = std::chrono::steady_clock::now();
    }

    if (chosen == Route::None) {
    }
    return chosen;
}

SessionRouter::Route SessionRouter::current_route(const std::string& dev_id) const {
    return pick_route(dev_id);
}

void SessionRouter::on_subscribe(const std::string& dev_id, std::string topic) {
    // Mirror attach_downstream: register the subscription with BOTH
    // sub-uplinks. If we only subscribe via the currently-healthy uplink
    // and later flip routes (LAN went down → Cloud), the new uplink never
    // gets its subscribe call and inbound traffic vanishes. Each uplink
    // dedupes via topic_refs (CloudUplink) / similar, so calling both is
    // idempotent. on_publish still picks a single route to avoid sending
    // the slicer's command to the printer twice.
    std::shared_ptr<LanUplink>   lan;
    std::shared_ptr<CloudUplink> cloud;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan   = m_lan;
        cloud = m_cloud;
    }
    (void)pick_route(dev_id); // keep cache + log warm so on_publish picks right
    if (lan)   lan  ->on_subscribe(dev_id, topic);
    if (cloud) cloud->on_subscribe(dev_id, std::move(topic));
}

void SessionRouter::on_publish(const std::string& dev_id, std::string topic,
                               std::vector<uint8_t> payload, uint8_t qos) {
    Route r = pick_route(dev_id);
    std::shared_ptr<LanUplink>   lan;
    std::shared_ptr<CloudUplink> cloud;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan   = m_lan;
        cloud = m_cloud;
    }
    if      (r == Route::Lan   && lan)
        lan  ->on_publish(dev_id, std::move(topic), std::move(payload), qos);
    else if (r == Route::Cloud && cloud)
        cloud->on_publish(dev_id, std::move(topic), std::move(payload), qos);
    // None: drop with the warning already logged inside pick_route.
}

void SessionRouter::on_unsubscribe(const std::string& dev_id, std::string topic) {
    // Mirror on_subscribe: unsubscribe from BOTH so refcounts stay balanced
    // (we subscribed to both regardless of route, so unsubscribe both).
    std::shared_ptr<LanUplink>   lan;
    std::shared_ptr<CloudUplink> cloud;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan   = m_lan;
        cloud = m_cloud;
    }
    if (lan)   lan  ->on_unsubscribe(dev_id, topic);
    if (cloud) cloud->on_unsubscribe(dev_id, std::move(topic));
}

void SessionRouter::on_disconnect(const std::string& dev_id) {
    // Slicer detached. Forward to BOTH sub-uplinks so each can drop its
    // downstream publisher; the printer-side sessions stay up (each
    // sub-uplink's on_disconnect is documented as "do not tear down").
    std::shared_ptr<LanUplink>   lan;
    std::shared_ptr<CloudUplink> cloud;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan   = m_lan;
        cloud = m_cloud;
        m_cache.erase(dev_id);
    }
    if (lan)   lan  ->on_disconnect(dev_id);
    if (cloud) cloud->on_disconnect(dev_id);
}

void SessionRouter::attach_downstream(const std::string& dev_id,
                                      uint64_t            session_id,
                                      DownstreamPublisher publisher) {
    // Critical: register with BOTH sub-uplinks. Either side may receive
    // a printer-state report independently — for example, a print started
    // on LAN doesn't stop the cloud broker from pushing the same
    // device/<dev_id>/report stream. The slicer's broker session is the
    // single point that fans both back together, so we wire both ends.
    std::shared_ptr<LanUplink>   lan;
    std::shared_ptr<CloudUplink> cloud;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan   = m_lan;
        cloud = m_cloud;
    }
    if (lan)   lan  ->attach_downstream(dev_id, session_id, publisher);
    if (cloud) cloud->attach_downstream(dev_id, session_id, publisher);
}

void SessionRouter::detach_downstream(const std::string& dev_id,
                                      uint64_t            session_id) {
    std::shared_ptr<LanUplink>   lan;
    std::shared_ptr<CloudUplink> cloud;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan   = m_lan;
        cloud = m_cloud;
    }
    if (lan)   lan  ->detach_downstream(dev_id, session_id);
    if (cloud) cloud->detach_downstream(dev_id, session_id);
}

void SessionRouter::attach_downstream(const std::string& dev_id,
                                      DownstreamPublisher publisher) {
    std::fprintf(stderr,
        "[session-router] WARN: deprecated 2-arg attach_downstream(dev=%s)\n",
        dev_id.c_str());
    std::fflush(stderr);
    if (publisher) attach_downstream(dev_id, /*session_id=*/0, std::move(publisher));
    else           detach_downstream(dev_id, /*session_id=*/0);
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
