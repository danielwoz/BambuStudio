// Bambu Bridge — LAN-side upload sink.
//
// `LanUploadSink` is the IUploadSink that forwards a slicer upload to
// the REAL printer's LAN endpoint via the proprietary `bambu_networking`
// plugin's `start_local_print_with_record` export. Mirror of
// CloudUploadSink (which uses `start_send_gcode_to_sdcard`).
//
// Routing through the plugin keeps LAN-bound bytes byte-identical to a
// real BambuStudio session — the plugin internally picks the right
// transport for the printer model (FTPS-990 for X1/P1, BambuTunnel on
// port 6000 for H2/H2S/A1). The bridge does NOT need to know which is
// in play.
//
// On success, `UploadResult.remote_url` is set to a virtual marker:
//   `bambu-lan:///model/<filename>`
// Nothing parses this; logs only.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_UPLOAD_SINK_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_UPLOAD_SINK_HPP

#include "../server/IUploadSink.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Slic3r {
namespace bridge {

class BambuNetworkingPluginHandle;   // forward, owned by caller as shared_ptr.

namespace router {

// Per-device routing parameters. The plugin needs both dev_id and dev_ip
// to drive `start_local_print_with_record`; access_code is the LAN auth
// password.
struct LanUploadSinkDevice {
    std::string dev_id;
    std::string printer_ip;
    std::string access_code;
    // Legacy field kept for source-compat with phase-7 callers; ignored
    // (the plugin owns transport choice).
    uint16_t                printer_port    = 990;
    std::chrono::seconds    connect_timeout{10};
    std::chrono::seconds    io_timeout{120};
};

class LanUploadSink : public server::IUploadSink {
public:
    LanUploadSink() = default;
    ~LanUploadSink() override = default;

    // Attach the shared plugin handle (same pattern as CloudUploadSink).
    // Pass nullptr to detach.
    void attach_plugin(std::shared_ptr<BambuNetworkingPluginHandle> handle);

    // Register a forwarding target. Adding the same dev_id twice replaces.
    void add_device   (LanUploadSinkDevice dev);
    void remove_device(const std::string& dev_id);

    server::UploadResult deliver(server::UploadJob job) override;

private:
    mutable std::mutex                                    m_mu;
    std::shared_ptr<BambuNetworkingPluginHandle>          m_handle;
    std::unordered_map<std::string, LanUploadSinkDevice>  m_devices;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_UPLOAD_SINK_HPP
