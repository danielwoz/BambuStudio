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

    // Hook the uplink uses to push a printer-side message back to a
    // connected slicer. The broker installs one publisher PER SESSION
    // when each slicer connects, and detaches that specific session's
    // entry on session end. Multiple sessions for the same dev_id all
    // receive the same printer-side traffic (multi-subscriber fan-out).
    //
    // Implementations also maintain a small retained-message cache per
    // dev_id (typically the last 2 inbound messages) and replay it
    // synchronously into the new publisher inside attach_downstream so
    // a freshly-connected slicer's UI has push_status to render without
    // waiting for the next printer push.
    using DownstreamPublisher = std::function<void(std::string topic,
                                                   std::vector<uint8_t> payload,
                                                   uint8_t qos)>;

    // Add (or replace, for the same {dev_id, session_id}) a downstream
    // subscriber. The broker generates session_id via its own counter
    // — uniqueness is the caller's responsibility.
    virtual void attach_downstream(const std::string& dev_id,
                                   uint64_t            session_id,
                                   DownstreamPublisher publisher) = 0;

    // Remove just the specified session's publisher. Other sessions
    // attached to the same dev_id continue to receive traffic.
    virtual void detach_downstream(const std::string& dev_id,
                                   uint64_t            session_id) = 0;

    // Deprecated 2-arg overload kept for backward compatibility with
    // existing tests and SessionRouter call paths that haven't yet
    // adopted session_id. Implementations synthesise a session_id
    // internally and log a warning. Prefer the 3-arg form.
    virtual void attach_downstream(const std::string& dev_id,
                                   DownstreamPublisher publisher) = 0;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_IUPLINK_HPP
