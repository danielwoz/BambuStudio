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
class EncMsgEnvelope;                 // forward; Ship 7 native enc_msg.

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

    // Per-printer mTLS material extracted from the slicer/plugin heap
    // by `install_device_cert()`. When both are non-empty,
    // `LanUplink::on_publish` bypasses the plugin path for
    // `print.command=*` payloads and spawns the paho-python subprocess
    // helper (raw_mqtt_publish.py) — the plugin silently drops control
    // commands from non-UI contexts, but the printer's LAN MQTT
    // broker accepts any publish that presents a valid client cert in
    // the TLS handshake. See DISCOVERY-2026-05-22.md for the gate
    // mechanics.
    std::string  mtls_cert_path;                  // /path/to/<dev>_chain.pem
    std::string  mtls_key_path;                   // /path/to/<dev>_key.pem
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

    // Ship 7 — install a native enc_msg envelope wrapper. When installed,
    // `on_publish` will wrap print.* payloads with the plugin-compatible
    // RSA-SHA256-signed envelope BEFORE handing them to the cert+key
    // paho publish helper. Without an envelope wrapper, print.* payloads
    // are still published via the cert+key path but unsigned — newer
    // firmware may reject those.
    //
    // Pass nullptr to detach. Caller retains ownership.
    void attach_enc_msg_envelope(std::shared_ptr<EncMsgEnvelope> envelope);
    std::shared_ptr<EncMsgEnvelope> enc_msg_envelope() const;

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
                           uint64_t            session_id,
                           DownstreamPublisher publisher) override;
    void detach_downstream(const std::string& dev_id,
                           uint64_t            session_id) override;
    // Deprecated single-publisher overload. Logs a warning. Synthesises
    // session_id = 0. Kept so existing tests and the legacy
    // SessionRouter call path keep compiling.
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
