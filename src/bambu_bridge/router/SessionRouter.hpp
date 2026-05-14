// Bambu Bridge — MQTT session router (phase 9).
//
// `SessionRouter` is the IUplink the MqttBroker is permanently bound to. It
// owns two sub-uplinks (LAN, cloud) and, per dev_id, picks one to retarget
// IUplink calls to. The broker never sees a different IUplink — the
// slicer's broker session is never dropped just because the uplink flipped.
//
// Three properties of MQTT routing motivate this design:
//
//   * Switching is cheap. Both sub-uplinks already maintain hot connections
//     to the printer (LAN) and the cloud (CloudUplink via the proprietary
//     plugin). Picking one is a memcpy.
//   * Either path can be a failover. Operators have configs where LAN-MQTT
//     drops but cloud-MQTT is healthy, and vice-versa.
//   * Downstream traffic can arrive on EITHER path independently. A print
//     may have started on LAN but the cloud broker still pushes state
//     reports (the printer mirrors). So `attach_downstream` registers the
//     SAME publisher with BOTH sub-uplinks — we don't gate downstream by
//     current_route. Anything that arrives goes through.
//
// Camera (phase 8) and upload (phase 7) are separate decisions; see
// CameraSourceRouter / UploadSinkRouter. Each has different switching
// costs and lifecycle.
//
// Debounce: `current_route` is cached per dev_id for `policy.rerouting_
// debounce`. A flapping LAN doesn't cause us to flip back and forth on
// every PUBLISH. Inside the debounce window we route to the cached choice
// (or drop if the cache says None and the cache is still warm).
//
// Threading: the broker calls `on_publish` / `on_subscribe` etc from its
// per-connection I/O thread. The router takes a short mutex to look up
// the cached route (and conditionally re-pick), then dispatches to the
// chosen sub-uplink. Sub-uplinks are themselves thread-safe.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_SESSION_ROUTER_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_SESSION_ROUTER_HPP

#include "../server/IUplink.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace router {

class LanUplink;
class CloudUplink;
class UplinkHealthMonitor;

class SessionRouter : public server::IUplink {
public:
    enum class Route { None, Lan, Cloud };

    struct Policy {
        // Default-on: real users overwhelmingly want LAN if reachable
        // because it's lower latency and doesn't traverse the WAN.
        bool                       prefer_lan = true;
        // Re-pick cadence ceiling. Inside this window after the last
        // pick, repeated IUplink calls re-use the cached choice without
        // re-evaluating health. Real-world LAN flaps are bursty (a few
        // seconds of churn while DHCP renegotiates) and we don't want
        // every PUBLISH to rip the route back and forth.
        std::chrono::milliseconds  rerouting_debounce{500};
    };

    SessionRouter();
    ~SessionRouter() override;

    SessionRouter(const SessionRouter&)            = delete;
    SessionRouter& operator=(const SessionRouter&) = delete;

    void set_lan_uplink   (std::shared_ptr<LanUplink>           lan);
    void set_cloud_uplink (std::shared_ptr<CloudUplink>         cloud);
    void set_health_monitor(std::shared_ptr<UplinkHealthMonitor> monitor);
    void set_policy(Policy p);

    // ---- IUplink --------------------------------------------------------
    void on_subscribe  (const std::string& dev_id, std::string topic) override;
    void on_publish    (const std::string& dev_id, std::string topic,
                        std::vector<uint8_t> payload, uint8_t qos) override;
    void on_unsubscribe(const std::string& dev_id, std::string topic) override;
    void on_disconnect (const std::string& dev_id) override;
    void attach_downstream(const std::string& dev_id,
                           DownstreamPublisher publisher) override;

    // Inspection: what would the router pick *right now* for `dev_id`?
    // Honours the debounce cache. Useful for tests + status logging.
    Route current_route(const std::string& dev_id) const;

private:
    struct CachedRoute {
        Route                                  route = Route::None;
        std::chrono::steady_clock::time_point  at{};
    };

    // Pick (and cache) the route for dev_id. Must be called with m_mu
    // unlocked — this method takes m_mu itself.
    Route pick_route(const std::string& dev_id) const;

    mutable std::mutex                          m_mu;
    std::shared_ptr<LanUplink>                  m_lan;
    std::shared_ptr<CloudUplink>                m_cloud;
    std::shared_ptr<UplinkHealthMonitor>        m_health;
    Policy                                      m_policy;
    mutable std::unordered_map<std::string,
                               CachedRoute>     m_cache;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_SESSION_ROUTER_HPP
