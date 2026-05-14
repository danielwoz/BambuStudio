// Bambu Bridge — per-device uplink health view (phase 9).
//
// See UplinkHealth.hpp for the rationale. Implementation is just shared_ptr
// holds + two `is_connected(dev_id)` calls + a tiny last-change cache.

#include "UplinkHealth.hpp"

#include "LanUplink.hpp"
#include "CloudUplink.hpp"

#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

UplinkHealthMonitor::UplinkHealthMonitor()  = default;
UplinkHealthMonitor::~UplinkHealthMonitor() = default;

void UplinkHealthMonitor::set_lan_uplink(std::shared_ptr<LanUplink> lan) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_lan = std::move(lan);
}

void UplinkHealthMonitor::set_cloud_uplink(std::shared_ptr<CloudUplink> cloud) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_cloud = std::move(cloud);
}

UplinkHealth UplinkHealthMonitor::snapshot(const std::string& dev_id) const {
    std::shared_ptr<LanUplink>   lan;
    std::shared_ptr<CloudUplink> cloud;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan   = m_lan;
        cloud = m_cloud;
    }
    const bool lan_ok   = lan   ? lan  ->is_connected(dev_id) : false;
    const bool cloud_ok = cloud ? cloud->is_connected(dev_id) : false;

    UplinkHealth out;
    out.lan_connected   = lan_ok;
    out.cloud_connected = cloud_ok;

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_cache_mu);
        auto& c = m_cache[dev_id];
        if (!c.seen || c.lan != lan_ok || c.cloud != cloud_ok) {
            c.lan   = lan_ok;
            c.cloud = cloud_ok;
            c.at    = now;
            c.seen  = true;
        }
        out.last_change = c.at;
    }
    return out;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
