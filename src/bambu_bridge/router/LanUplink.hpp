// Bambu Bridge — direct-LAN uplink.
//
// `LanUplink` is the IUplink implementation that bridges MqttBroker
// events to the REAL printer over the LAN. It does NOT speak MQTT
// directly — every byte goes through the proprietary `bambu_networking`
// plugin's `connect_printer` / `send_message_to_printer` /
// `set_on_local_message_fn` exports. That's how real BambuStudio talks
// to LAN printers, so by construction our wire bytes match a native
// session (TLS cipher, MQTT client_id format, keepalive cadence — all
// owned by the plugin, not by us).
//
//   slicer  →  MqttBroker  →  LanUplink::on_publish   →  plugin (LAN)
//                                                          ↑ real TLS+MQTT
//                                                          ↓
//   slicer  ←  DownstreamPublisher  ←  on_local_message_fn ←  plugin (LAN)
//
// Symmetry with CloudUplink: per-device state shrinks to the config +
// the registered DownstreamPublisher + topic refcount. The plugin owns
// the transport.
//
// Plugin LAN serialisation:
//   The proprietary plugin only holds ONE LAN connection at a time
//   (one `connect_printer` -> `disconnect_printer` cycle). Multiple
//   registered devices coexist in the LanUplink's device map, but the
//   plugin is bound to whichever was most recently `connect_printer`'d.
//   The bridge serialises bring-up on a first-come basis — see
//   `add_device` notes below. Multiple LAN-reachable printers mirrored
//   simultaneously is a known limitation; phase 12 E2E will exercise
//   this against two real printers.
//
// Tested by:
//   - `tests/bridge/LanUplinkUnitTest.cpp`   (mock-plugin refcount +
//                                              dispatch state machine).
//   - `tests/bridge/LanUplinkPluginTest.cpp` (mock-plugin loopback test
//                                              with byte-level publish/
//                                              receive assertions).

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_UPLINK_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_UPLINK_HPP

#include "../server/IUplink.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace Slic3r {
namespace bridge {

class BambuNetworkingPluginHandle;   // forward, owned by caller as shared_ptr.

namespace router {

struct LanUplinkConfig {
    std::string  dev_id;                          // must match the broker's dev_id
    std::string  printer_ip;                      // e.g. "192.168.1.209"
    std::string  access_code;                     // bblp password
    bool         use_ssl = true;                  // matches NetworkAgent::connect_printer
    // Kept for source compatibility with phase-5 callers. The plugin
    // owns the actual port choice; we don't surface it on the wire.
    uint16_t                printer_port = 8883;
    std::chrono::seconds    connect_timeout{10};
    std::chrono::seconds    keepalive{30};
};

class LanUplink : public server::IUplink {
public:
    LanUplink();
    ~LanUplink() override;

    LanUplink(const LanUplink&)            = delete;
    LanUplink& operator=(const LanUplink&) = delete;

    // Plumb in the shared proprietary plugin agent. Same shape as
    // CloudUplink::attach_plugin — the agent is owned externally (the
    // BridgeService / BridgeApp holds the shared_ptr). Pass nullptr to
    // detach. Existing devices will start working as soon as a non-null
    // handle is attached.
    void attach_plugin(std::shared_ptr<BambuNetworkingPluginHandle> handle);

    // Returns the currently-attached handle, or nullptr. Useful for the
    // health monitor.
    std::shared_ptr<BambuNetworkingPluginHandle> plugin_handle() const;

    // Register the device the bridge mirrors. Triggers `connect_printer`
    // on the plugin for this dev_id immediately. Idempotent on the same
    // dev_id (re-adding replaces the config and forces a fresh connect
    // attempt). When multiple devices are registered, only the most
    // recently-added one is actually connected on the plugin side at any
    // given moment (single-LAN-connection limitation noted above).
    void add_device   (LanUplinkConfig cfg);
    void remove_device(const std::string& dev_id);

    // True if the plugin reports its LAN MQTT session is up AND it's the
    // session we asked for (matches `dev_id`).
    bool is_connected(const std::string& dev_id) const;

    // ---- IUplink ----
    void on_subscribe  (const std::string& dev_id, std::string topic) override;
    void on_publish    (const std::string& dev_id, std::string topic,
                        std::vector<uint8_t> payload, uint8_t qos) override;
    void on_unsubscribe(const std::string& dev_id, std::string topic) override;
    void on_disconnect (const std::string& dev_id) override;
    void attach_downstream(const std::string& dev_id,
                           DownstreamPublisher publisher) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_UPLINK_HPP
