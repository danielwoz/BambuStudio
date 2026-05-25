// Bambu Bridge — per-device uplink health view (phase 9).
//
// `UplinkHealthMonitor` is the tiny per-device aggregate of (LAN connected,
// cloud connected) that SessionRouter / UploadSinkRouter / CameraSourceRouter
// each consult before picking a route. It does NOT keep its own watchdog
// thread — both underlying uplinks already track liveness internally
// (`LanUplink::is_connected(dev_id)` is driven by the per-device printer
// session's CONNACK + reader-loop state; `CloudUplink::is_connected(dev_id)`
// folds in the plugin's `OnServerConnectedFn`). The monitor just polls them
// when asked.
//
// Why a separate class instead of inlining the two `is_connected` calls in
// each router: the three routers want the SAME view (so a flip-the-LAN-off
// test exercises all three identically), and a future "real watchdog with
// hysteresis" implementation can drop in here without rewriting every
// router. Today it's millisecond-cheap mutex-light polling. No threads.
//
// The monitor holds the LanUplink / CloudUplink via `std::shared_ptr`
// (matching how BridgeService owns them) so its lifetime strictly tracks
// the BridgeService's. Detaching is `nullptr`.
//
// Threading: routers may call `snapshot()` from their own I/O threads while
// the underlying uplinks' connection state changes from theirs. Both
// `LanUplink::is_connected` and `CloudUplink::is_connected` are documented
// thread-safe; this class adds one mutex around the shared_ptr swaps and
// one (separate) mutex around the per-dev_id last-change cache.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_UPLINK_HEALTH_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_UPLINK_HEALTH_HPP

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Slic3r {
namespace bridge {
namespace router {

class LanUplink;
class CloudUplink;

struct UplinkHealth {
    bool lan_connected   = false;
    bool cloud_connected = false;
    // The steady_clock instant either flag last changed FROM THIS
    // monitor's perspective. Used by SessionRouter's debounce to decide
    // whether a route flip is allowed. The default-constructed (epoch)
    // value means "never observed" — first snapshot stamps it.
    std::chrono::steady_clock::time_point last_change{};
};

class UplinkHealthMonitor {
public:
    UplinkHealthMonitor();
    ~UplinkHealthMonitor();

    UplinkHealthMonitor(const UplinkHealthMonitor&)            = delete;
    UplinkHealthMonitor& operator=(const UplinkHealthMonitor&) = delete;

    // Attach / detach the underlying uplinks. May be called before or
    // after `snapshot()`. Passing nullptr detaches.
    void set_lan_uplink  (std::shared_ptr<LanUplink>   lan);
    void set_cloud_uplink(std::shared_ptr<CloudUplink> cloud);

    // Pull the current (cheap) connection state for `dev_id` from both
    // uplinks. Returns {false, false, last_change} if neither uplink is
    // attached or if neither has a record of the device. `last_change`
    // is the instant *this monitor* most recently observed a flip for
    // this dev_id.
    UplinkHealth snapshot(const std::string& dev_id) const;

private:
    struct Cache {
        bool                                   lan   = false;
        bool                                   cloud = false;
        std::chrono::steady_clock::time_point  at{};
        bool                                   seen  = false;
    };

    mutable std::mutex                                m_mu;
    std::shared_ptr<LanUplink>                        m_lan;
    std::shared_ptr<CloudUplink>                      m_cloud;

    mutable std::mutex                                m_cache_mu;
    mutable std::unordered_map<std::string, Cache>    m_cache;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_UPLINK_HEALTH_HPP
