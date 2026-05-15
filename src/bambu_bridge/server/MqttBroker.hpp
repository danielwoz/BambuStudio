// Bambu Bridge — TLS MQTT broker (phase 4b).
//
// One MqttBroker instance can host many virtual devices, each bound to its
// own (ip, port) endpoint. For each device:
//
//   - TLS 1.2 ONLY (no TLS 1.3). Real Bambu printers reject TLS 1.3 and
//     `LanMqttSession` pins to MQTT_SSL_VERSION_TLS_1_2; the bridge must
//     match so a slicer's TLS handshake fingerprint is identical.
//   - Cert + key driven by CertMaterial pulled from CertFactory.
//   - No client-cert verification (real LAN printers don't ask).
//   - MQTT 3.1.1 protocol level (CONNECT protocol name "MQTT", level 4).
//   - Auth: username "bblp", password = device access code. Anything else
//     gets CONNACK return code 5 (Not Authorized).
//   - One concurrent client per device (matches real-printer behaviour).
//     A second connect attempt with a session already attached gets the
//     socket closed mid-handshake.
//
// Routing:
//   PUBLISH / SUBSCRIBE / UNSUBSCRIBE / DISCONNECT events bubble up to
//   the configured IUplink. The uplink's DownstreamPublisher is wired
//   into the session at CONNACK time so phase-5/6/9 can push printer
//   messages back to the slicer.
//
// Threading model (deliberately simple, not high-perf):
//   - One accept thread per `(ip, port)` listener.
//   - One I/O thread per accepted connection.
//   - Per-connection I/O is single-threaded; the only cross-thread call
//     is `inject_downstream` / the uplink's DownstreamPublisher, which
//     hops through a mutex-guarded send queue inside the session.
//
// The broker isn't designed for thousands of concurrent virtual devices
// — Bambu users typically own 1–4 printers, so 4 accept threads + 4 I/O
// threads is fine.
//
// Networking is built on boost::asio for portability; the TLS layer is
// still raw OpenSSL (SSL/SSL_CTX) because:
//   - we need TLS-1.2-only pinning + in-memory PEM loading, which is
//     simpler with the OpenSSL API than asio::ssl::context's wrapper;
//   - the existing session I/O uses blocking SSL_read/SSL_write driven
//     by select(), and we keep that loop intact by giving SSL the
//     asio socket's native_handle().
// The accept loop and the underlying TCP socket are asio, so the
// Linux-only listen+accept+select scaffolding is gone.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_MQTT_BROKER_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_MQTT_BROKER_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../tls/CertFactory.hpp"

namespace Slic3r {
namespace bridge {
namespace server {

class IUplink;

struct MqttBrokerVirtualDevice {
    std::string         dev_id;          // real serial — used upstream
                                          // (uplink dev_id, cert CN, plugin
                                          // routing). Internal routing key.
    std::string         virtual_dev_id;  // serial we advertise via SSDP
                                          // (typically real_sn + 1). Slicers
                                          // use this in MQTT topics. Empty =
                                          // no translation (slicer uses real
                                          // serial in topics — covers tests).
    std::string         lan_ip = "0.0.0.0"; // bind address
    uint16_t            port   = 8883;
    std::string         access_code;     // bblp password
    tls::CertMaterial   cert;             // PEM cert + key for TLS
};

struct MqttBrokerConfig {
    // Pluggable uplink. If null, the broker creates an internal stub at
    // start() time that just logs.
    std::shared_ptr<IUplink> uplink;

    // listen(2) backlog.
    int  accept_backlog          = 8;

    // Max concurrent clients per dev_id. Real Bambu printers allow 1;
    // we mirror that behaviour by default.
    int  max_clients_per_device  = 1;

    // I/O read timeout in seconds. Used as the recv() SO_RCVTIMEO so a
    // stuck client doesn't pin an I/O thread forever.
    int  io_timeout_seconds      = 90;
};

class MqttBroker {
public:
    explicit MqttBroker(MqttBrokerConfig cfg);
    ~MqttBroker();

    MqttBroker(const MqttBroker&) = delete;
    MqttBroker& operator=(const MqttBroker&) = delete;

    // Devices may be added before start() (they'll be brought up by
    // start()) or after (they're brought up immediately). remove_device
    // closes the listener and tears down any active session.
    void add_device   (MqttBrokerVirtualDevice dev);
    void remove_device(const std::string& dev_id);

    // Idempotent lifecycle.
    void start();
    void stop();

    bool running() const noexcept { return m_running.load(); }

    // Replace the uplink. Must be called before start() — the accept
    // thread captures the uplink pointer as it brings each device up,
    // so swapping mid-run is racy. Returns silently in that case.
    // Phase 5 uses this to wire a `LanUplink` into a broker that was
    // constructed with a default `NullUplink`.
    void set_uplink(std::shared_ptr<IUplink> uplink);

    // Test hook: explicitly push a printer-side message into the
    // downstream pipe. No-op if the device has no active session.
    void inject_downstream(const std::string& dev_id,
                           std::string topic,
                           std::vector<uint8_t> payload,
                           uint8_t qos);

    // Reports the actually-bound port for a device. Useful when callers
    // pass port=0 (kernel picks an ephemeral port — handy in tests).
    // Returns 0 if the device isn't bound.
    uint16_t bound_port(const std::string& dev_id) const;

    // Internal per-device state. Public in name only — the definition
    // lives in MqttBroker.cpp so callers can forward-declare but not
    // poke at the fields. Exposing the symbol publicly avoids friending
    // every helper in the implementation file.
    struct Device;

private:

    // Look up by dev_id. Caller must hold m_devices_mu.
    Device* find_locked(const std::string& dev_id);

    // Bring a single device's listener up. Throws on bind/listen failure.
    void start_device(Device& d);

    // Tear down one device's listener + any active session.
    void stop_device(Device& d);

    MqttBrokerConfig                                m_cfg;
    std::atomic<bool>                               m_running{false};

    mutable std::mutex                              m_devices_mu;
    std::unordered_map<std::string,
                       std::unique_ptr<Device>>     m_devices;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_MQTT_BROKER_HPP
