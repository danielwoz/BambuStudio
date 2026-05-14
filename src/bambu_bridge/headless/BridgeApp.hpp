// Bambu Bridge — headless multi-device orchestrator (phase 10).
//
// `BridgeApp` is the long-lived daemon process. Unlike phase 9's
// `bridge-cli proxy` (which is a single-printer command), this owns N
// virtual devices and keeps the (LAN, cloud) split coherent across all
// of them with a single shared `BambuNetworkingPluginHandle`.
//
// Lifecycle of `run()`:
//
//   1. Construct the plugin handle. If init() fails, return non-zero
//      immediately (refusing to start). We deliberately do this BEFORE
//      any server binds so a misconfigured operator gets a clean
//      "couldn't load the plugin" error, not half-bound listeners.
//   2. Construct CloudInventory + all routers + all bottom-tier
//      LAN/cloud uplinks/sinks/sources. Wire them together.
//   3. Construct the four servers (SSDP/MQTT/FTPS/RTSP) based on the
//      enable_* flags in the config. Servers that are disabled simply
//      aren't created — their `start()` won't be called and nothing
//      binds on their ports.
//   4. Start every server (each can already host N devices via its own
//      `add_device` API).
//   5. Spawn the inventory poll thread. Every `inventory_poll` seconds:
//        - call inventory.refresh() + probe_lan_reachability()
//        - for each new device: mint cert, add to all servers with
//          ascending ports, configure LAN uplink/sink/source with the
//          LAN IP.
//        - for each disappeared device: remove from all servers, tear
//          down per-device state.
//        - for each device whose lan_ip changed: re-add the LAN
//          uplink/sink/camera-source so its connection target follows.
//   6. Block on the stop condvar.
//   7. On shutdown(): set the stop flag, wake the condvar, join the
//      poll thread, then stop the servers in order
//      RTSP -> FTPS -> MQTT -> SSDP. Tear down the plugin handle last.
//
// Per-device port assignment policy:
//
//   The N-th distinct device sees ports
//     (mqtt_port_base + N,
//      ftps_port_base + N,
//      rtsp_port_base + N).
//   N starts at 0 and grows monotonically; ports for a removed device
//   are NOT reclaimed in phase 10 (it would race with re-add of the
//   same dev_id while the OS still holds TIME_WAIT on the old port).
//
// Test hooks:
//   - `set_plugin_handle_for_test()` lets a test inject a
//     MockPluginHandle before `run()` (so we never dlopen). The mock's
//     `get_user_print_info` decides what `poll_inventory_once()` sees.
//   - `poll_inventory_once()` runs one pass of the poll loop
//     synchronously. The multi-device test uses this to drive the
//     orchestration without spinning a worker thread.

#ifndef SLIC3R_BAMBU_BRIDGE_HEADLESS_BRIDGE_APP_HPP
#define SLIC3R_BAMBU_BRIDGE_HEADLESS_BRIDGE_APP_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <vector>

namespace Slic3r {
namespace bridge {

class BambuNetworkingPluginHandle;
class BambuSourceHandle;
class CloudInventory;

namespace tls    { class CertFactory;   }
namespace server { class SsdpResponder; }
namespace server { class SsdpListener;  }
namespace server { class MqttBroker;    }
namespace server { class FtpsServer;    }
namespace server { class RtspServer;    }
namespace server { class VirtualTunnelServer; }
namespace router { class LanUplink;          }
namespace router { class CloudUplink;        }
namespace router { class LanUploadSink;      }
namespace router { class CloudUploadSink;    }
namespace router { class LanCameraSource;    }
namespace router { class CloudCameraSource;  }
namespace router { class NullCameraSource;   }
namespace router { class SessionRouter;      }
namespace router { class UploadSinkRouter;   }
namespace router { class CameraSourceRouter; }
namespace router { class UplinkHealthMonitor;}

namespace headless {

// One printer the host has handed us to mirror as a `${name}-virtual`
// LAN device. The host typically constructs these from its own
// DeviceManager snapshot (BambuStudio's `Slic3r::DeviceManager`) so
// the bridge sees exactly the printers the GUI is aware of, in the
// same names/states.
struct VirtualPrinter {
    std::string dev_id;        // serial
    std::string dev_name;      // user-facing name; bridge appends "-virtual"
    std::string lan_ip;        // empty if the printer is cloud-only today
    std::string access_code;
    std::string model;         // e.g. "H2S"; empty -> falls back to ssdp_default_model
    std::string firmware;      // empty -> falls back to ssdp_default_firmware
    // Pre-resolved camera URL from Slic3r::GUI::build_media_live_url
    // (the same ladder MediaPlayCtrl uses). Optional — when empty the
    // bridge's camera sources fall back to their built-in URL form.
    std::string camera_url;
};

struct BridgeAppConfig {
    // Path to the proprietary `libbambu_networking.so`. Empty = let the
    // plugin handle's default probe list resolve it. An empty / missing
    // plugin makes `run()` fail with a non-zero return code.
    std::string  plugin_path;

    // Path to the proprietary `libBambuSource.so`. Empty = let the
    // BambuSourceHandle's default probe list resolve it. Missing
    // BambuSource does NOT fail run() — LAN/Cloud camera sources just
    // refuse to open(), and `CameraSourceRouter` falls back to
    // NullCameraSource (or refuses outright if the policy disallows
    // that). The MQTT / FTPS / SSDP paths are unaffected.
    std::string  bambu_source_path;

    // Pass-through to bambu_network_set_config_dir / set_country_code.
    std::string  config_dir;
    std::string  country_code;

    // Identification headers forwarded to bambu_network_set_extra_http_header.
    // Drives the X-BBL-Client-Name / X-BBL-Client-Version fingerprint
    // BambuStudio's cloud uses to filter requests. Callers should at
    // minimum populate X-BBL-Client-Name and X-BBL-Client-Version with
    // the build's SLIC3R_APP_NAME / SLIC3R_VERSION; the GUI normally
    // also fills OS-Type / OS-Version / Device-ID / Language.
    std::map<std::string, std::string> http_extra_headers;

    // ${resources_dir}/cert + "slicer_base64.cer" — the base64-encoded
    // Bambu cloud root cert the plugin needs to verify TLS to the cloud
    // MQTT broker. Both empty → skip set_cert_file.
    std::string cert_dir;
    std::string cert_file;

    // How long a device's lan_ip stays valid after the last SSDP
    // NOTIFY from that printer. Real Bambu firmware announces every
    // ~30 s; we give 4× that before assuming the printer left the LAN
    // (or rebooted to cloud-only) and clear the field. Cleared lan_ips
    // come back to non-empty the next time the listener hears the
    // device.
    std::chrono::seconds lan_ip_stale_after{120};

    // If non-empty, reconcile_once silently drops every printer whose
    // dev_id isn't in this set. Useful for debugging — bind a single
    // printer on port 8883 without the 8884/8885 sibling listeners
    // muddying the SSDP advertise list, so other slicers only see one
    // virtual entry to chase.
    std::vector<std::string> only_dev_ids;

    // Bind address for every per-device server. Default is 0.0.0.0
    // (every NIC). Tests use 127.0.0.1 to stay on loopback.
    std::string  lan_iface_bind = "0.0.0.0";

    // Cadence for CloudInventory refresh. The walker thread also handles
    // device add/remove/lan_ip-change. Tests can set this very long and
    // drive `poll_inventory_once()` themselves.
    std::chrono::seconds inventory_poll{60};

    // Cert cache. Empty = CertFactory's default ($XDG_CONFIG_HOME/...).
    std::filesystem::path cert_cache_dir;

    // Enable flags for each server. A disabled server is not instantiated
    // at all — `start()` won't be called and no port will be bound.
    //
    // Defaults: full proxy (SSDP + MQTT + FTPS + RTSP). The bridge mints
    // a per-device cert, binds private high ports for each server, and
    // advertises every virtual printer to the LAN with a mangled USN
    // (`BR-${serial}`) so the host slicer's own SSDP listener does NOT
    // attribute the broadcast back to its real cloud-bound printer —
    // that collision used to wedge the GUI's device list.
    bool         enable_ssdp  = true;
    bool         enable_mqtt  = true;
    bool         enable_ftps  = true;
    bool         enable_rtsp  = true;
    bool         enable_vtun  = true;  // virtual storage tunnel (Phase 1)

    // Per-device port bases. The N-th device (in add order) gets
    // (mqtt_port_base+N, ftps_port_base+N, rtsp_port_base+N). MQTT
    // defaults to 8883 — the port Bambu slicers (BambuStudio, Orca,
    // forks) hardcode for LAN MQTT-over-TLS — so the FIRST device
    // added is reachable without slicer-side port config. The other
    // two land on 8884/8885 which slicers won't try; multi-printer
    // requires the single-port multiplexer (planned). FTPS/RTSP stay
    // on high ports for now because 990/322 are privileged and
    // current builds run unprivileged.
    uint16_t     mqtt_port_base = 8883;
    uint16_t     ftps_port_base = 39990;
    uint16_t     rtsp_port_base = 38322;
    // Virtual storage tunnel (Phase 1 / our own non-Bambu protocol over
    // TLS). The slicer's PrinterFileSystem opens a `bambu:///virtual/...`
    // URL pointed at this port. Picked high to avoid clashing with the
    // real BambuTunnel port 6000.
    uint16_t     vtun_port_base = 39998;

    // SSDP fallback values, used only for the small set of fields the
    // host's DeviceManager might not have filled in on a given
    // VirtualPrinter (typically model/firmware on a freshly-discovered
    // device). The DEVICE name itself is per-printer; see
    // `BridgeApp::set_virtual_printers`.
    std::string  ssdp_default_name     = "Bambu Bridge";
    std::string  ssdp_default_model    = "H2S";
    std::string  ssdp_default_firmware = "01.02.00.00";

    // Slicer-identity fields that the BambuTunnel storage URL embeds.
    // The slicer's native MediaFilePanel populates them with real
    // values (`agent->get_version()`, `app_config->get("slicer_uuid")`,
    // `SLIC3R_VERSION`, the printer's OTA firmware version). The
    // proprietary plugin appears to refuse to advance `StartStreamEx`
    // for storage when these are empty — see
    // `VirtualTunnelServer::session_loop`'s URL builder. The in-GUI
    // bridge fills these from GUI_App at construction time; leave
    // them empty in headless mode (the proprietary plugin still
    // tolerates that for non-storage paths).
    std::string  slicer_net_ver;       // e.g. "1.13.04.x"
    std::string  slicer_cli_id;        // app_config slicer_uuid
    std::string  slicer_cli_ver;       // SLIC3R_VERSION

    // If true, BridgeApp does NOT instantiate its own
    // `BambuNetworkingPluginHandle`. This is the contract for
    // running INSIDE BambuStudio's GUI: the slicer's NetworkAgent is
    // already the (one and only) plugin agent in the process, and the
    // bridge must not race against it. The GUI pushes its DeviceManager
    // snapshot via `BridgeApp::set_virtual_printers`.
    //
    // Default true so the GUI worker thread is safe by construction.
    // The `--bridge-only` headless mode flips this to false because
    // there is no GUI and no NetworkAgent — we're the only plugin
    // consumer.
    bool         host_drives_inventory = true;

    // The bridge's only plugin-touching surface is whatever the GUI's
    // NetworkAgent exposes; everything goes through this callback so
    // the bridge module doesn't take a compile-time dep on
    // NetworkAgent.hpp. The host (BambuStudio's GUI worker thread)
    // implements it as something like:
    //
    //   cfg.printer_source = [this] {
    //       std::vector<VirtualPrinter> ps;
    //       if (auto* dm = this->getDeviceManager()) {
    //           for (auto& [id, mo] : dm->get_user_machinelist()) {
    //               ps.push_back({id, mo->dev_name, mo->dev_ip,
    //                             mo->access_code, mo->printer_type, ""});
    //           }
    //       }
    //       return ps;
    //   };
    //
    // The bridge invokes it on every inventory_poll tick (and the
    // first time during run() startup). Empty / unset means "use the
    // bridge's own CloudInventory poll" (which only kicks in when
    // host_drives_inventory == false, i.e. --bridge-only mode).
    std::function<std::vector<VirtualPrinter>()> printer_source;
};

class BridgeApp {
public:
    explicit BridgeApp(BridgeAppConfig cfg);
    ~BridgeApp();

    BridgeApp(const BridgeApp&)            = delete;
    BridgeApp& operator=(const BridgeApp&) = delete;

    // Blocks until shutdown(). Returns 0 on clean exit, non-zero on init
    // failure (plugin couldn't be loaded, cert factory blew up, etc.).
    int run();

    // Idempotent. Safe to call from a signal handler thread / another
    // thread. Wakes the poll thread + the run() main loop.
    void shutdown();

    // Host-driven inventory entry point. Replaces the bridge's current
    // device set with `printers`, doing the minimum diff (add new ones
    // via SSDP, remove ones that vanished, update lan_ip flips). Safe
    // to call from any thread; the bridge serialises through its own
    // device mutex. Use this from the slicer's GUI tick (every couple
    // of seconds) to keep the SSDP advertisements in step with the
    // DeviceManager.
    //
    // Only the SSDP layer reads these in passive advertise-only mode
    // (the default). When proxy-mode flags are on, the LanUplink /
    // CloudUplink etc. also consume them — but you should NOT enable
    // proxy mode in the same process as the GUI because the proxy path
    // calls connect_printer on the shared plugin and would tear down
    // the GUI's session.
    void set_virtual_printers(std::vector<VirtualPrinter> printers);

    // Test-only injection: replaces the auto-constructed plugin handle
    // BEFORE run()/poll_inventory_once() builds anything that depends on
    // it. Mutually exclusive with the default dlopen path. Pass nullptr
    // to restore the default behaviour.
    void set_plugin_handle_for_test(
        std::shared_ptr<BambuNetworkingPluginHandle> handle);

    // Production injection point for the in-GUI bridge. The slicer's
    // GUI_App constructs a `NetworkAgentPluginAdapter` wrapping its
    // already-running NetworkAgent and hands it in here BEFORE
    // launching the bridge worker thread. The bridge then skips its
    // own dlopen of libbambu_networking.so (which would crash the
    // plugin's global state — see BridgeApp.cpp:133) and instead
    // routes every plugin call through the slicer's existing agent.
    // Calls to this and to `set_plugin_handle_for_test` are the same
    // operation; the test name is kept for backwards compat.
    void attach_plugin_handle(
        std::shared_ptr<BambuNetworkingPluginHandle> handle) {
        set_plugin_handle_for_test(std::move(handle));
    }

    // Look up the real printer's LAN IP and real dev_id for a (possibly
    // FFFF-mangled) dev_id. The bridge learns the LAN IP from SSDP
    // NOTIFYs the printer broadcasts. For virtual `FFFF…` dev_ids the
    // slicer-side `MediaFilePanel` uses this to build a
    // `bambu:///local/<real-ip>…&device=<real-id>…` storage URL that
    // bypasses the bridge entirely — the proprietary plugin opens the
    // tunnel directly to the real printer over port 6000, exactly as
    // it would for a non-virtual LAN printer.
    //
    // Pass either the real `03900D…` dev_id or the virtual `FFFF…`
    // form. Both real_dev_id and lan_ip come back empty when the
    // dev_id isn't tracked.
    struct RealDeviceInfo {
        std::string real_dev_id;
        std::string lan_ip;
    };
    RealDeviceInfo lookup_real_device(const std::string& dev_id) const;

    // Look up the bridge's MQTT broker port for a (possibly FFFF-mangled)
    // dev_id. Each virtual printer gets its own port (mqtt_port_base +
    // index); the slicer's VirtualMqttClient needs this so it can
    // connect to the correct broker socket. Returns 0 if the dev_id
    // isn't tracked.
    uint16_t mqtt_port_for_dev_id(const std::string& dev_id) const;

    // Test-only injection of the BambuSourceHandle. Mirrors
    // `set_plugin_handle_for_test`. When injected, BridgeApp skips its
    // own dlopen of libBambuSource.so.
    void set_bambu_source_handle_for_test(
        std::shared_ptr<BambuSourceHandle> handle);

    // Wire the GUI-owned storage delegate through to the virtual tunnel
    // server. When set, vtun sessions hand every JSON-RPC frame to the
    // delegate (which in turn routes via PrinterFileSystem -> the
    // GUI's proven libBambuSource consumer) instead of calling
    // libBambuSource directly.
    //
    // The optional release_cb is invoked when a device is removed from
    // BridgeApp's tracking table (so the GUI side can drop its
    // per-dev PFS). BridgeApp can't include the GUI headers needed to
    // hop to the wx main thread itself, so GUI_App supplies the lambda
    // that marshals the call to wx via CallAfter. Leave both empty in
    // headless mode.
    // Request/reply body is JSON-serialized text. We don't put
    // `nlohmann::json` in the signature because the bridge and the
    // slicer GUI vendor different versions of the header (different
    // inline namespaces => different mangled symbols).
    using StorageDelegate = std::function<void(
        const std::string& real_dev_id,
        const std::string& real_lan_ip,
        const std::string& access_code,
        const std::string& dev_ver,
        const std::string& net_ver,
        const std::string& cli_id,
        const std::string& cli_ver,
        int                cmdtype,
        std::string        request_body_json,
        std::function<void(int, std::string)> reply_cb)>;

    void attach_storage_delegate(
        StorageDelegate                         delegate,
        std::function<void(const std::string&)> release_cb = {});

    // Test-only single-iteration pump of what the poll thread does.
    // Returns true if the iteration ran (inventory was queried), false
    // if the bridge wasn't initialised. Used by the multi-device test to
    // drive orchestration deterministically without a real timer.
    bool poll_inventory_once();

    // For inspection / tests / phase-11 wire-diff: every device the
    // BridgeApp currently has in its per-device table, plus the ports it
    // bound on each server. Returned snapshot is a copy.
    struct DeviceBinding {
        std::string dev_id;
        std::string lan_ip;
        uint16_t    mqtt_port = 0;
        uint16_t    ftps_port = 0;
        uint16_t    rtsp_port = 0;
        uint16_t    vtun_port = 0;
    };
    std::vector<DeviceBinding> device_bindings() const;

private:
    struct DeviceState; // defined below as a nested type
    // Build the plugin / inventory / routers / servers. Returns true on
    // success. Called once at the top of run() (or, in tests, before the
    // first poll_inventory_once()).
    bool initialise();

    // Tear down everything initialise() set up. Idempotent. Servers
    // stopped in reverse order (RTSP -> FTPS -> MQTT -> SSDP). The
    // plugin handle is reset last.
    void teardown();

    // Body of one poll iteration: refresh inventory, diff vs. our table,
    // apply add/remove/lan_ip-change.
    void reconcile_once();

    // The thread function.
    void poll_loop();

    // Helpers used by reconcile_once / set_virtual_printers. Assume
    // `m_devices_mu` held.
    void add_device_locked(const VirtualPrinter& vp);
    void update_lan_ip_locked(DeviceState&        state,
                              const std::string&  lan_ip);
    void remove_device_locked(const std::string& dev_id);

    BridgeAppConfig                                       m_cfg;

    // Construction-time inputs / test injection.
    std::shared_ptr<BambuNetworkingPluginHandle>          m_plugin;
    bool                                                  m_plugin_injected = false;
    std::shared_ptr<BambuSourceHandle>                    m_bambu_source;
    bool                                                  m_bambu_source_injected = false;

    // GUI-supplied storage delegate + release callback.
    StorageDelegate                                       m_storage_delegate;
    std::function<void(const std::string&)>               m_storage_release_cb;

    // Initialised in initialise().
    std::unique_ptr<CloudInventory>                       m_inventory;
    std::unique_ptr<tls::CertFactory>                     m_cert_factory;
    std::shared_ptr<router::LanUplink>                    m_lan_uplink;
    std::shared_ptr<router::CloudUplink>                  m_cloud_uplink;
    std::shared_ptr<router::LanUploadSink>                m_lan_sink;
    std::shared_ptr<router::CloudUploadSink>              m_cloud_sink;
    std::shared_ptr<router::UplinkHealthMonitor>          m_health;
    std::shared_ptr<router::SessionRouter>                m_session_router;
    std::shared_ptr<router::UploadSinkRouter>             m_upload_router;
    std::shared_ptr<router::NullCameraSource>             m_null_camera;

    std::unique_ptr<server::SsdpResponder>                m_ssdp;
    std::unique_ptr<server::SsdpListener>                 m_ssdp_listener;
    std::unique_ptr<server::MqttBroker>                   m_mqtt;
    std::unique_ptr<server::FtpsServer>                   m_ftps;
    std::unique_ptr<server::RtspServer>                   m_rtsp;
    std::unique_ptr<server::VirtualTunnelServer>          m_vtun;

    // Per-device table. The N-th device added (alphabetical by dev_id is
    // NOT guaranteed — it's insertion order) gets ports
    // (mqtt_port_base+N, ftps_port_base+N, rtsp_port_base+N).
    struct DeviceState {
        std::string                                       dev_id;
        std::string                                       lan_ip;
        std::string                                       access_code;
        // Printer's OTA firmware version (e.g. "01.08.00.00"). Pushed
        // in via the GUI's wxTimer from MachineObject::get_ota_version().
        // The storage tunnel URL embeds this as `dev_ver=…` and the
        // printer-side waits for it to be non-empty before sending its
        // first frame.
        std::string                                       firmware_ver;
        // Pre-resolved camera URL from the GUI's MediaUrlBuilder.
        // Empty in headless mode; camera sources fall back to their
        // built-in `bambu:///rtsps___...` form when this is unset.
        std::string                                       camera_url;
        std::size_t                                       index = 0;
        uint16_t                                          mqtt_port = 0;
        uint16_t                                          ftps_port = 0;
        uint16_t                                          rtsp_port = 0;
        uint16_t                                          vtun_port = 0;
        // When the SsdpListener last heard a NOTIFY for this dev_id.
        // reconcile_once expires lan_ip after lan_ip_stale_after of
        // silence (the printer left the LAN or rebooted to cloud-only).
        std::chrono::steady_clock::time_point             lan_ip_last_seen{};
        std::shared_ptr<router::CameraSourceRouter>       cam_router;
        std::shared_ptr<router::LanCameraSource>          lan_cam;
        std::shared_ptr<router::CloudCameraSource>        cloud_cam;
    };
    mutable std::mutex                                    m_devices_mu;
    std::map<std::string, DeviceState>                    m_devices;
    std::size_t                                           m_next_index = 0;
    // Resolved IP we emit in the SSDP LOCATION header. Filled the
    // first time we add a device; empty until then. See
    // detect_primary_lan_ip() in the .cpp.
    std::string                                           m_ssdp_advertise_ip;

    // Lifecycle / stop signalling.
    std::atomic<bool>                                     m_initialised{false};
    std::atomic<bool>                                     m_stop{false};
    std::condition_variable                               m_stop_cv;
    std::mutex                                            m_stop_mu;
    std::thread                                           m_poll_thread;
};

} // namespace headless
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_HEADLESS_BRIDGE_APP_HPP
