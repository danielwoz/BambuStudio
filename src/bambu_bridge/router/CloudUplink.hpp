// Bambu Bridge — cloud-fallback uplink (phase 6).
//
// `CloudUplink` is the IUplink implementation that, per `dev_id`, bridges
// MqttBroker events to Bambu's cloud MQTT broker via the proprietary
// `bambu_networking` plugin's send/recv exports. It does NOT speak MQTT
// directly — the plugin's authenticated forwarder is in front of us, so
// our job is just to plumb the right calls into the shared
// BambuNetworkingPluginHandle and route incoming messages back through
// the broker's DownstreamPublisher.
//
// Symmetry with LanUplink (phase 5):
//
//   slicer  →  MqttBroker  →  CloudUplink::on_publish   →  plugin (cloud)
//                                                            ↑
//   slicer  ←  DownstreamPublisher  ←  on_message_fn      ←  plugin (cloud)
//
// Topic shape: Bambu's cloud broker uses the same topic naming as the
// LAN broker — `device/<dev_id>/{request,report}`. The plugin's
// `bambu_network_send_message_to_printer` and `bambu_network_add_subscribe`
// exports take `dev_id` (not topic) so CloudUplink doesn't have to
// rebuild a topic string itself; we just hand the dev_id to the plugin.
// When a cloud message arrives via `OnMessageFn`, we synthesise the
// `/report` topic locally so the downstream slicer sees the same shape
// it would from a real LAN printer.
//
// Threading: the plugin fires `OnMessageFn` from its own worker thread.
// The handle's dispatcher takes a short mutex to look up the receiver
// for the dev_id, then invokes it on that same plugin thread. The
// receiver itself just calls into the broker's DownstreamPublisher,
// which is documented (see IUplink.hpp) as thread-safe.
//
// Connection state: the plugin's `OnServerConnectedFn` callback drives
// the handle's `is_server_connected()` flag. `CloudUplink::is_connected()`
// reports the AND of (plugin loaded, user logged in, server connected).
// Phase-9 SessionRouter uses this to decide between LAN and cloud.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_UPLINK_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_UPLINK_HPP

#include "../server/IUplink.hpp"

#include <memory>
#include <string>

namespace Slic3r {
namespace bridge {

class BambuNetworkingPluginHandle;

namespace router {

struct CloudUplinkConfig {
    std::string dev_id;
    std::string access_code;     // not strictly needed for cloud MQTT
                                 // writes (the plugin's auth token is)
                                 // but some downstream API calls take it
};

class CloudUplink : public server::IUplink {
public:
    CloudUplink();
    ~CloudUplink() override;

    CloudUplink(const CloudUplink&)            = delete;
    CloudUplink& operator=(const CloudUplink&) = delete;

    // Plumb in the shared proprietary plugin agent. Owned externally
    // (typically by BridgeService). Pass nullptr to detach. Calling
    // before any add_device() is fine; calling after is fine too —
    // existing devices will simply start working as soon as the handle
    // is attached.
    void attach_plugin(std::shared_ptr<BambuNetworkingPluginHandle> handle);

    // Returns the currently-attached handle, or nullptr.
    std::shared_ptr<BambuNetworkingPluginHandle> plugin_handle() const;

    // Caller registers each cloud-bound device. Idempotent (re-adding the
    // same dev_id overrides the previous config and re-subscribes).
    void add_device   (CloudUplinkConfig cfg);
    void remove_device(const std::string& dev_id);

    // True iff (plugin loaded) AND (user logged in) AND (cloud server
    // connected) AND (we have a config for this dev_id). False
    // otherwise. SessionRouter checks this before picking the cloud
    // path; if it returns false, fall back to LAN.
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
    // an internal session_id (0). Provided so existing tests and the
    // legacy SessionRouter call path keep compiling.
    void attach_downstream(const std::string& dev_id,
                           DownstreamPublisher publisher) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_UPLINK_HPP
