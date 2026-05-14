// Bambu Bridge — E2E slicer-shaped client (phase 12).
//
// Speaks Bambu's LAN protocols (MQTT 8883, FTPS 990, RTSP 322) against
// the BRIDGE — never against the real printer directly. Reuses the
// phase-4 MqttTestClient and phase-7 FtpsTestClient harnesses; the
// RtspTestClient is available too but the per-printer flow tests don't
// exercise the camera path in phase 12 (RtspServerLoopbackTest already
// pins it).
//
// Design notes:
//   - Connection ordering mirrors what BambuStudio's slicer does:
//     subscribe to `device/<dev_id>/report` first, THEN publish to
//     `device/<dev_id>/request`. That way a fast printer's status
//     bursts don't get dropped on the floor.
//   - `wait_for_status` polls inbound PUBLISHes for a JSON predicate.
//     We do NOT parse the JSON here — the per-model tests pass simple
//     substring predicates that pin known status keywords (RUNNING,
//     PAUSED, IDLE, FINISHED). This stays free of a JSON dep while
//     remaining easy to extend.
//   - Auth: USERname is always "bblp" (Bambu LAN protocol literal);
//     password is the printer's LCD-displayed access code. Same string
//     is used for MQTT username/password AND FTP USER/PASS.
//
// Threading: one-shot per print run. Not thread-safe across operations
// on the same instance.

#ifndef SLIC3R_BAMBU_BRIDGE_E2E_PRINTER_CLIENT_HPP
#define SLIC3R_BAMBU_BRIDGE_E2E_PRINTER_CLIENT_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {

namespace test { class MqttTestClient; class FtpsTestClient; }

namespace e2e {

class E2EPrinterClient {
public:
    E2EPrinterClient();
    ~E2EPrinterClient();

    // TLS+MQTT CONNECT to the bridge's per-device MQTT port. The
    // `dev_id` is what the bridge uses as the SSL cert CN; user/pass
    // pair is ("bblp", access_code). Returns true on a 0x00 CONNACK.
    bool connect_mqtt(const std::string& host, uint16_t port,
                      const std::string& dev_id,
                      const std::string& access_code);

    // SUBSCRIBE to `device/<dev_id>/report`. Required before
    // start_print() so we don't miss the first RUNNING status burst.
    bool subscribe_report(const std::string& dev_id);

    // Poll inbound PUBLISHes until either the predicate returns true on
    // one's payload or the timeout expires. Non-matching messages are
    // silently consumed. Returns the matching payload or empty string
    // on timeout.
    std::string wait_for_status(
        const std::function<bool(const std::string&)>& predicate,
        std::chrono::seconds timeout);

    // Drive the FTPS upload path. Connects on (host, port), logs in as
    // ("bblp", access_code), STORs the file at `path_to_3mf` as
    // `/cache/<basename>` on the bridge. Returns true on a 226 close.
    bool upload_3mf(const std::string& host, uint16_t port,
                    const std::string& dev_id,
                    const std::string& access_code,
                    const std::string& path_to_3mf,
                    std::string&       uploaded_filename_out);

    // PUBLISH a JSON print/pause/cancel command to
    // `device/<dev_id>/request`. The bridge routes it to the LAN or
    // cloud uplink. Returns true on a successful socket write.
    bool start_print(const std::string& dev_id,
                     const std::string& filename,
                     int                plate_idx);
    bool pause_print(const std::string& dev_id);
    bool cancel_print(const std::string& dev_id);

    // Send MQTT DISCONNECT and tear down the socket. Idempotent.
    bool disconnect();

    const std::string& last_error() const { return m_last_error; }

private:
    bool publish_request(const std::string& dev_id,
                         const std::string& payload);

    std::unique_ptr<test::MqttTestClient> m_mqtt;
    std::string                           m_last_error;
};

} // namespace e2e
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_E2E_PRINTER_CLIENT_HPP
