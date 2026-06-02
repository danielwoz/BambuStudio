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

    // Printer model — propagated from `--model` on the CLI / from the
    // cloud inventory's `model_code` when --bridge-multi spawns this
    // worker. Used by the sink to set `try_emmc_print` (X1C/P1S only)
    // and to gate the port-6000 BambuTunnel preflight probe (the
    // BambuTunnel server is only present on firmware that supports it —
    // H2/H2S/H2D unconditionally, X1C/P1S when the user has flashed
    // recent firmware, A1 never).
    std::string             printer_model;
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

    // Dispatch a slicer-originated `print.command=gcode_file` MQTT
    // payload. Called by MqttBroker as soon as it sees the slicer publish
    // a print command on `device/<sn>/request`. Looks up the spool
    // recorded by the matching `deliver()` for this (dev_id, filename),
    // constructs a full LocalPrintParams from the MQTT JSON (ams_mapping,
    // task flags, plate index, …) and asks the plugin to start the
    // print via `start_local_print_with_record` — which handles the
    // upload-to-real-printer AND the equivalent MQTT print command in
    // one transaction. On A1 / FTPS-less printers the plugin's own
    // fallback to `start_print` (cloud-relay) covers the case.
    //
    // Returns 0 on dispatch success (plugin accepted the job), nonzero
    // on lookup or plugin failure. MqttBroker uses the return code only
    // to decide whether to SUPPRESS the forward to the printer's MQTT
    // (success → suppress; failure → fall back to verbatim forward so
    // a broken interceptor degrades gracefully).
    int dispatch_print_command(const std::string& dev_id,
                               const std::string& virtual_dev_id,
                               const std::string& mqtt_payload_json);

private:
    mutable std::mutex                                    m_mu;
    std::shared_ptr<BambuNetworkingPluginHandle>          m_handle;
    std::unordered_map<std::string, LanUploadSinkDevice>  m_devices;

    // Per-device spool registry. Each successful `deliver()` records its
    // spooled tempfile path here keyed by (dev_id, basename of the STOR
    // remote name the slicer sent), along with the matching settings-
    // only `.3mf` sidecar (built by `make_settings_only_zip` from the
    // main upload). The matching `dispatch_print_command` looks both
    // up by the `print.param` field of the MQTT JSON and passes them
    // to the plugin's start_local_print_with_record as
    // (local_file_path, config_filename). The plugin requires BOTH —
    // an empty config_filename causes a -3070 / -2030 cascade because
    // the plugin can't upload the OSS config sidecar.
    //
    // Same (dev_id, filename) on a re-print overwrites — last upload wins.
    struct SpoolEntry {
        std::string main_path;     // /tmp/bridge-spool/<dev>/<basename>
        std::string config_path;   // /tmp/bridge-spool/<dev>/<stem>_config.3mf
                                   // empty if make_settings_only_zip failed
    };
    std::unordered_map<std::string,
        std::unordered_map<std::string, SpoolEntry>>      m_spool_paths;

    // Per-device port-6000 BambuTunnel reachability cache. Populated by
    // a one-shot TCP-connect probe on the first upload for a dev_id and
    // refreshed at most once per `kTunnelProbeTtl`. Used to gate the
    // `try_emmc_print` hint we pass to the plugin: on X1C/P1S the plugin
    // honours the hint by routing through the BambuTunnel on port 6000
    // (eMMC target) instead of legacy FTPS on 990 (SD-card target).
    struct TunnelProbeResult {
        bool                                  reachable = false;
        std::chrono::steady_clock::time_point probed_at{};
    };
    std::unordered_map<std::string, TunnelProbeResult>    m_tunnel_probes;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_UPLOAD_SINK_HPP
