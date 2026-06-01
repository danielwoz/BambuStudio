// Bambu Bridge — default uplink stub (phase 4).
//
// Drops every event from the broker after writing a one-line breadcrumb
// to stderr. Useful for:
//   - integration tests that want a real broker but don't care where the
//     traffic goes (RecordingUplink is the assertion variant);
//   - bridge-cli's `broker` subcommand, so a developer can connect with
//     mosquitto_pub to confirm the server is up;
//   - the BridgeService default before phase 5's LanUplink replaces it.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_UPLINK_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_UPLINK_HPP

#include "../server/IUplink.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace router {

class NullUplink final : public server::IUplink {
public:
    NullUplink() = default;
    ~NullUplink() override = default;

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
    void attach_downstream(const std::string& dev_id,
                           DownstreamPublisher publisher) override;

private:
    struct Subscriber {
        uint64_t            session_id;
        DownstreamPublisher publisher;
    };
    // Stored only so callers can later send back via the publisher if
    // they want; NullUplink itself never pushes anything downstream.
    std::mutex                                                   m_mu;
    std::unordered_map<std::string, std::vector<Subscriber>>     m_pub;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_UPLINK_HPP
