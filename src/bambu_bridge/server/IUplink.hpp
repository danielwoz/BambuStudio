// Bambu Bridge — uplink interface (phase 4).
//
// The MqttBroker terminates downstream-slicer MQTT and hands the resulting
// events off to an `IUplink`. The uplink decides where the traffic actually
// goes:
//
//   - phase 5: `LanUplink`    -- relays to the real printer over LAN MQTT
//   - phase 6: `CloudUplink`  -- relays to Bambu's cloud MQTT
//   - phase 9: `SessionRouter` -- picks LAN vs cloud per session
//
// Phase 4 only ships `NullUplink` (drops + logs), which is enough to bring
// up the broker and round-trip CONNECT/PUBLISH/SUBSCRIBE in a test.
//
// Threading: the broker invokes these callbacks from the I/O thread that
// owns the connection. Implementations must be thread-safe across
// concurrent devices (different dev_ids) but a single dev_id is only ever
// touched from one I/O thread at a time.
//
// `DownstreamPublisher` is the injection hook: the uplink calls it (from
// any thread) to push a printer-side message back to the connected
// slicer. The broker synchronises sends internally so the publisher is
// safe to invoke concurrently.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_IUPLINK_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_IUPLINK_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace server {

class IUplink {
public:
    virtual ~IUplink() = default;

    // The broker notifies the uplink whenever the connected slicer
    // subscribes/unsubscribes/publishes/disconnects.
    virtual void on_subscribe  (const std::string& dev_id, std::string topic) = 0;
    virtual void on_publish    (const std::string& dev_id, std::string topic,
                                std::vector<uint8_t> payload, uint8_t qos) = 0;
    virtual void on_unsubscribe(const std::string& dev_id, std::string topic) = 0;
    virtual void on_disconnect (const std::string& dev_id) = 0;

    // Hook the uplink uses to push a printer-side message back to the
    // currently-connected slicer. The broker installs the publisher when a
    // session starts and tears it down (passes an empty function) when the
    // session ends. Implementations should store the latest publisher per
    // dev_id and ignore calls when none is installed.
    using DownstreamPublisher = std::function<void(std::string topic,
                                                   std::vector<uint8_t> payload,
                                                   uint8_t qos)>;
    virtual void attach_downstream(const std::string& dev_id,
                                   DownstreamPublisher publisher) = 0;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_IUPLINK_HPP
