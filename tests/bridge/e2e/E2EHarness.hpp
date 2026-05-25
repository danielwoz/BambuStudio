// Bambu Bridge — E2E test harness.
//
// Spawns `BambuStudio --bridge-only` as a child process and watches its
// stderr for the per-device "[bridge-app] add dev_id=... lan_ip=...
// ports=..." line. Tests poll `ports_for(model)` to learn which
// loopback ports the bridge has bound for the device, then drive a
// scripted slicer session against those ports.
//
// There is no standalone bridge daemon binary — the proprietary
// `bambu_networking` plugin fingerprints its host process and refuses
// to operate unless it's been loaded by BambuStudio. The harness reads
// $BAMBU_BRIDGE_E2E_BAMBUSTUDIO for the slicer path, falling back to a
// $PATH lookup of `BambuStudio`. Tests skip (rc=77) if neither resolves.
//
// What lives here vs. in each per-printer test:
//   - E2EHarness is shared: process spawn, stderr capture, line parsing,
//     start/stop lifecycle, timeout enforcement.
//   - Per-model tests own the slicer-shaped flow (connect / upload /
//     start / pause / cancel) via E2EPrinterClient, plus the per-model
//     status predicates.
//
// IMPORTANT:
//   - The harness flag set mirrors what an interactive operator would
//     type: `--bind 127.0.0.1`, port bases in the ephemeral range,
//     `--no-ssdp` (1900 binding is privileged), short inventory poll.
//   - It deliberately picks port bases above 40000 so a developer
//     machine that's also running a real BambuStudio (default
//     mqtt=8883 etc.) does not collide.

#ifndef SLIC3R_BAMBU_BRIDGE_E2E_HARNESS_HPP
#define SLIC3R_BAMBU_BRIDGE_E2E_HARNESS_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace e2e {

struct PrinterEndpoint {
    // Lowercase canonical model tag: "h2s" | "a1" | "a1mini" | "h2".
    std::string model;
    // LAN IPv4 of the physical printer.
    std::string ip;
    // Filled in by the harness after the daemon's first inventory
    // refresh, by reading the matching device_bindings line off stderr.
    // Empty until the daemon reports it.
    std::string dev_id;
    // LAN access code captured from the printer LCD (Settings →
    // General → LAN Mode). The slicer-impersonating MQTT/FTPS client
    // logs in with this.
    std::string access_code;
};

struct DevicePorts {
    uint16_t    mqtt    = 0;
    uint16_t    ftps    = 0;
    uint16_t    rtsp    = 0;
    std::string bind_ip;
    std::string dev_id;
    std::string lan_ip;
};

class E2EHarness {
public:
    explicit E2EHarness(std::vector<PrinterEndpoint> endpoints);
    ~E2EHarness();

    E2EHarness(const E2EHarness&)            = delete;
    E2EHarness& operator=(const E2EHarness&) = delete;

    // Spawns BambuStudio --bridge-only as a child process and waits
    // for at least one device_bindings line to appear on its stderr.
    // Returns true if the bridge came up and at least one of the
    // requested endpoints landed in the bindings table within
    // `startup_timeout`.
    //
    // Returns false (without spawning) if `endpoints` is empty. The
    // E2EHarnessSkipTest pins that contract.
    bool start(std::chrono::seconds startup_timeout = std::chrono::seconds(30));

    // Idempotent. Sends SIGTERM to the child, joins the reader thread.
    void stop();

    // Look up per-device bridge ports by model tag (matched against the
    // endpoint list). Returns nullopt if no binding has been seen yet.
    std::optional<DevicePorts> ports_for(const std::string& model) const;

    // Where the BambuStudio binary is. Reads
    // $BAMBU_BRIDGE_E2E_BAMBUSTUDIO; falls back to a $PATH lookup of
    // `BambuStudio`. (Kept as `default_daemon_path()` for historical
    // call sites; nothing else still talks about a daemon binary.)
    static std::string default_daemon_path();

    // Full captured stderr so far (for diagnostics on failure).
    std::string captured_stderr() const;

private:
    // Reader thread body: reads from m_stderr_fd in chunks, parses each
    // newline-terminated line, and updates m_bindings on
    // device_bindings lines.
    void reader_loop();

    // Parses one stderr line. Returns true if a device_bindings line
    // was consumed.
    bool parse_line(const std::string& line);

    std::vector<PrinterEndpoint>                m_endpoints;
    int                                         m_pid          = -1;
    int                                         m_stderr_fd    = -1;
    std::thread                                 m_reader;
    std::atomic<bool>                           m_stop_reader{false};

    mutable std::mutex                          m_mu;
    std::unordered_map<std::string, DevicePorts> m_bindings; // by dev_id
    std::string                                  m_stderr_log;
};

} // namespace e2e
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_E2E_HARNESS_HPP
