// Bambu Bridge — cloud-side upload sink stub (phase 7).
//
// `CloudUploadSink` is the IUploadSink that would push a slicer upload via
// Bambu's cloud OSS (the same route the proprietary `bambu_networking`
// plugin uses for `start_send_gcode_to_sdcard`). The plugin handle we
// currently expose through `BambuNetworkingPluginHandle` does NOT yet
// surface that upload export — see the missing
// `func_start_send_gcode_to_sdcard` resolution in
// `BambuNetworkingPluginHandle.cpp`.
//
// Until the plugin handle gains that export, this sink:
//
//   - Logs every received UploadJob (dev_id, file, byte count).
//   - Returns `ok=false` with a `error_message` explaining the plugin
//     export is not yet wired.
//   - Is safe to install in the BridgeService default — phase 9's
//     SessionRouter will fall through to LanUploadSink (or NullUpload
//     Sink) when LAN reachability is up and cloud isn't.
//
// LAN is the primary path for phase 7; the cloud upload export is on the
// phase-10/13 roadmap.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_UPLOAD_SINK_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_UPLOAD_SINK_HPP

#include "../server/IUploadSink.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r {
namespace bridge {

class BambuNetworkingPluginHandle;   // forward, owned by caller as shared_ptr.

namespace router {

class CloudUploadSink : public server::IUploadSink {
public:
    CloudUploadSink() = default;
    ~CloudUploadSink() override = default;

    // Attach the shared plugin handle. May be called before or after the
    // BridgeService is started; we just hold the shared_ptr so the
    // plugin's lifetime outlives us. Nullptr detaches.
    void attach_plugin(std::shared_ptr<BambuNetworkingPluginHandle> handle);

    // Per-device hints. The plugin's start_send_gcode_to_sdcard wants
    // both dev_ip and access_code populated even when the upload is
    // routed via cloud OSS (rc=-1 silently otherwise). Same shape as
    // LanUploadSink so BridgeApp can call add_device on both with the
    // same parameters.
    struct Device {
        std::string dev_id;
        std::string printer_ip;    // real printer LAN IP
        std::string access_code;
    };
    void add_device   (Device dev);
    void remove_device(const std::string& dev_id);

    server::UploadResult deliver(server::UploadJob job) override;

private:
    std::shared_ptr<BambuNetworkingPluginHandle> m_handle;
    mutable std::mutex                           m_mu;
    std::map<std::string, Device>                m_devices;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_UPLOAD_SINK_HPP
