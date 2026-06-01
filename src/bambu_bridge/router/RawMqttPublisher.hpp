// Bambu Bridge — minimal raw-OpenSSL MQTT publisher for control commands.
//
// Workaround for the proprietary plugin's send_message_to_printer
// silently rejecting `print.command=*` control payloads from non-UI
// contexts (regression introduced by commit 318a73092). The plugin
// path is preferred for status reads, camera, and uploads; this raw
// publisher is the documented fallback for control commands. See
// `feedback_proprietary_lib.md` in the project memory for the full
// background.
//
// Single-shot publish per call: connect TLS → MQTT CONNECT → CONNACK →
// PUBLISH (QoS 1, wait for PUBACK) → DISCONNECT → close. No persistent
// session; we pay the TLS-handshake cost (~50–150 ms on LAN) for each
// control command. Control commands are user-initiated and infrequent
// enough that this trade-off is fine; status streams stay on the
// plugin's persistent session.
//
// TLS profile mirrors the pre-refactor MqttUplinkSession:
//   TLS 1.2 only, SSL_VERIFY_NONE, BUT with `SSL_CTX_set_ecdh_auto(1)`
//   so the OpenSSL build accepts the printer's ECDHE-only cipher list.
// MQTT framing reuses `server::mqtt::encode_publish` /
// `append_mqtt_string` / `encode_varint` — no duplicate encoder code.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_RAW_MQTT_PUBLISHER_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_RAW_MQTT_PUBLISHER_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace router {

struct RawMqttPublishConfig {
    std::string dev_id;              // real printer SN — used for topic + client_id
    std::string printer_ip;          // e.g. "192.168.1.209"
    uint16_t    printer_port = 8883; // LAN MQTT TLS
    std::string username = "bblp";   // BambuLab LAN MQTT username
    std::string access_code;         // 8-char printer access code (password)
    std::chrono::seconds connect_timeout{5};
    std::chrono::seconds io_timeout{5};
    // Optional: client cert + key for mTLS. Both empty = plain TLS without
    // client cert (current behaviour, fine for printer-side read-only LAN
    // broker). For control commands (`print.command=*`) the printer's
    // firmware ENFORCES client-cert auth — see feedback_proprietary_lib.md
    // in project memory — and `raw_mqtt_publish_oneshot` is the entry
    // point the adapter uses to bypass the proprietary plugin's gate.
    std::string mtls_cert_path;
    std::string mtls_key_path;
};

// Synchronous one-shot publish. Returns 0 on success, negative on
// failure (-1 TCP, -2 TLS handshake, -3 MQTT CONNECT/CONNACK, -4
// PUBLISH write, -5 PUBACK timeout).
//
// `topic` is typically `device/<real_sn>/request`. `payload` is the
// raw JSON the slicer already shaped — no rewriting here.
// `qos` is 0 or 1 (we always wait for PUBACK when qos>=1).
int raw_mqtt_publish_oneshot(const RawMqttPublishConfig& cfg,
                             const std::string&          topic,
                             const std::vector<uint8_t>& payload,
                             uint8_t                     qos);

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_RAW_MQTT_PUBLISHER_HPP
