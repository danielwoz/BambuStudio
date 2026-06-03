// Bambu Bridge — per-device FTPS server for slicer .3mf uploads (phase 7).
//
// One FtpsServer instance can host many virtual devices, each bound to its
// own (ip, port) endpoint. For each device:
//
//   - Implicit TLS on port 990 (the socket goes straight into SSL_accept;
//     no AUTH TLS upgrade). Verified against the real-printer behaviour
//     in `~/BambuStudio/src/bambu_net_oss/core/LocalPrintOrchestrator.cpp`
//     (kFtpPort=990, FtpsClient calls curl with ftps:// URL → implicit
//     TLS).
//   - TLS 1.2 pinned (same as MqttBroker / LanMqttSession). No client
//     cert verification — slicers use `verify=0` against the printer's
//     self-signed cert.
//   - Cert + key from CertMaterial (same `CertFactory` output the MQTT
//     broker uses, so the wire fingerprint is identical).
//   - Auth: username `bblp`, password = device access code. Anything
//     else returns 530 and closes the connection cleanly.
//   - FTP subset implemented: USER PASS FEAT PBSZ PROT TYPE PWD CWD MKD
//     CDUP LIST NLST PASV STOR DELE QUIT NOOP SYST.
//   - PASV-only (no PORT / no EPRT). EPSV is also supported because
//     master's FtpsClient uses `CURLOPT_FTP_USE_EPSV=1`.
//   - PASV picks a free port in the configured ephemeral range, binds,
//     and announces it in the 227 reply. The data channel is its own
//     SSL_accept using the same SSL_CTX as the control channel.
//   - STOR reads the whole file into memory (capped at max_upload_bytes)
//     then synchronously hands it to the configured IUploadSink. The
//     226/551 reply mirrors the sink's success/failure.
//   - LIST/NLST return an empty body — the bridge doesn't maintain a
//     virtual printer FS, and real slicers `LIST` mainly to confirm
//     `/model/` exists before STOR.
//
// Threading model (deliberately simple):
//   - One accept thread per (ip, port) listener.
//   - One I/O thread per accepted control connection.
//   - The PASV data port pool is shared across a device's sessions; it's
//     guarded by a mutex and reaps closed listeners on each new PASV.
//
// The server isn't designed for thousands of concurrent uploads — Bambu
// users own 1–4 printers and queue uploads serially, so a per-device
// accept thread + per-connection I/O thread is plenty.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_FTPS_SERVER_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_FTPS_SERVER_HPP

// Windows port: socket types/calls route through platform/WinsockShim.hpp.
#include "../platform/WinsockShim.hpp"

#include <atomic>
#include <cstddef>
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

class IUploadSink;

struct FtpsVirtualDevice {
    std::string         dev_id;       // serial — must match cert CN
    std::string         lan_ip = "0.0.0.0"; // bind address
    uint16_t            port   = 990; // real printers use 990 (implicit TLS)
    std::string         access_code;  // bblp password
    tls::CertMaterial   cert;          // PEM cert + key for TLS
};

struct FtpsServerConfig {
    // Where uploads go. NullUploadSink by default if left unset (the
    // server tolerates a null sink for tests by treating it as drop).
    std::shared_ptr<IUploadSink> sink;

    // Ephemeral port range for PASV data channels. Defaults match the
    // IANA dynamic/private range. Tests can narrow this to an unused
    // range to avoid clashes.
    uint16_t pasv_port_min = 49152;
    uint16_t pasv_port_max = 65535;

    // Hard cap on STOR upload size. 200 MB is well above a typical 3MF
    // (a few MB at most) but small enough to bound memory.
    std::size_t max_upload_bytes = 200u * 1024u * 1024u;

    // The IP we advertise in the PASV 227 reply. If empty, we use the
    // device's `lan_ip`. Useful when the server binds 0.0.0.0 but needs
    // to tell the client a specific reachable address.
    std::string pasv_advertise_ip;

    // listen(2) backlog.
    int accept_backlog = 4;

    // recv timeout for the control connection (seconds).
    int io_timeout_seconds = 90;
};

class FtpsServer {
public:
    explicit FtpsServer(FtpsServerConfig cfg);
    ~FtpsServer();

    FtpsServer(const FtpsServer&)            = delete;
    FtpsServer& operator=(const FtpsServer&) = delete;

    // Devices may be added before start() (brought up by start()) or
    // after (brought up immediately). remove_device closes the listener
    // and tears down any active session.
    void add_device   (FtpsVirtualDevice dev);
    void remove_device(const std::string& dev_id);

    // Idempotent lifecycle.
    void start();
    void stop();

    bool running() const noexcept { return m_running.load(); }

    // Reports the actually-bound port for a device. Useful when callers
    // pass port=0 (kernel picks an ephemeral port — handy in tests).
    // Returns 0 if the device isn't bound.
    uint16_t bound_port(const std::string& dev_id) const;

    // Replace the upload sink. Must be called BEFORE start() — the accept
    // thread captures the raw IUploadSink* pointer at start_device() time
    // and changing the underlying shared_ptr mid-run would be a data
    // race. Returns silently if start() has already been called.
    // Phase 9 uses this to slot in the UploadSinkRouter.
    void set_sink(std::shared_ptr<IUploadSink> sink);

    // Internal per-device state. The definition lives in FtpsServer.cpp.
    struct Device;

private:
    Device* find_locked(const std::string& dev_id);
    void start_device(Device& d);
    void stop_device (Device& d);

    FtpsServerConfig                          m_cfg;
    std::atomic<bool>                         m_running{false};

    mutable std::mutex                        m_devices_mu;
    std::unordered_map<std::string,
                       std::unique_ptr<Device>> m_devices;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_FTPS_SERVER_HPP
