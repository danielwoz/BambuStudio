// Bambu Bridge — minimal TLS MQTT client for tests (phase 4).
//
// Hand-rolled CONNECT/PUBLISH/SUBSCRIBE/PINGREQ/DISCONNECT issuance over an
// OpenSSL TLS socket. Reuses the broker's own MqttFraming encoders for the
// outbound side and decode_packet() for the inbound side, so the client and
// server stay symmetric.
//
// Not a general-purpose MQTT client — it does only what the loopback test
// (and any later wire-diff harness) needs:
//   - TLS 1.2 connect, no cert verification (the broker's cert is the
//     bridge's per-device self-signed cert).
//   - One pending in-flight write at a time; QoS 1 PUBACKs are read but
//     not matched.
//   - recv_publish() blocks until a PUBLISH arrives or a timeout elapses.
//
// Linux-only.

#ifndef SLIC3R_BAMBU_BRIDGE_TEST_MQTT_CLIENT_HPP
#define SLIC3R_BAMBU_BRIDGE_TEST_MQTT_CLIENT_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "server/MqttFraming.hpp"

namespace Slic3r {
namespace bridge {
namespace test {

struct ReceivedPublish {
    std::string          topic;
    std::vector<uint8_t> payload;
    uint8_t              qos = 0;
};

class MqttTestClient {
public:
    MqttTestClient();
    ~MqttTestClient();

    MqttTestClient(const MqttTestClient&) = delete;
    MqttTestClient& operator=(const MqttTestClient&) = delete;

    // TCP+TLS connect. Returns false on error; the caller can call
    // last_error() for a human-readable reason. Pass `verify=false`
    // (the only mode we support) — the broker uses a self-signed cert.
    bool tcp_tls_connect(const std::string& host, uint16_t port,
                         std::chrono::milliseconds timeout =
                             std::chrono::seconds(5));

    // Send a CONNECT, then read CONNACK. Returns the CONNACK return code
    // (0 = accepted) or -1 if the read failed.
    int connect_mqtt(const std::string& client_id,
                     const std::string& username,
                     const std::string& password,
                     bool clean_session = true,
                     uint16_t keep_alive = 30);

    // Publish helpers. send_publish() returns true on a successful socket
    // write (caller can then poll for the PUBACK if qos==1).
    bool send_publish(const std::string& topic,
                      const std::vector<uint8_t>& payload,
                      uint8_t qos = 0,
                      uint16_t packet_id = 0);

    // Subscribe to a single topic and wait for SUBACK with matching pid.
    bool subscribe_one(const std::string& topic, uint16_t pid, uint8_t qos = 0);

    // Block until the next PUBLISH arrives, or the timeout expires.
    // Any non-PUBLISH packets seen along the way (PUBACK, PINGRESP, ...)
    // are silently consumed.
    std::optional<ReceivedPublish> recv_publish(std::chrono::milliseconds timeout);

    // Read any pending PUBACK with the given packet id, returning true if
    // seen within `timeout`.
    bool wait_for_puback(uint16_t pid, std::chrono::milliseconds timeout);

    // Tear down TLS + TCP. Idempotent.
    void close();

    const std::string& last_error() const { return m_last_error; }

private:
    // Returns true iff at least one byte was successfully read into the
    // internal recv buffer before the deadline.
    bool pump_in(std::chrono::steady_clock::time_point deadline);

    // Decode whatever's currently buffered. Caller pre-loops on
    // pump_in() to ensure progress.
    std::optional<server::mqtt::MqttPacket> try_decode();

    int   m_fd  = -1;
    void* m_ssl = nullptr;   // SSL*
    void* m_ctx = nullptr;   // SSL_CTX*
    std::vector<uint8_t> m_recv;
    std::string          m_last_error;
};

} // namespace test
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_TEST_MQTT_CLIENT_HPP
