// Bambu Bridge — virtual storage tunnel for the slicer's PrinterFileSystem.
//
// The slicer's Storage tab uses libBambuSource's `Bambu_Tunnel` API to talk
// JSON-RPC to a real printer over TLS port 6000 (the BambuTunnel protocol).
// We can't speak BambuTunnel: it's proprietary, the real printer's LAN port
// is usually held by the user's other slicer, and the cloud relay (TUTK/
// Agora) needs Bambu's account credentials in a way the bridge doesn't
// participate in.
//
// Instead we expose a *non-Bambu* TLS endpoint that mimics just enough of
// the Bambu_Tunnel control channel (the JSON RPC the slicer actually
// makes — LIST_INFO / FILE_DEL / FILE_DOWNLOAD / FILE_UPLOAD / TASK_CANCEL)
// to let the slicer think it's connected. The matching slicer-side dispatch
// lives in `slic3r/GUI/Printer/VirtualBambuTunnel.{hpp,cpp}` and routes
// `bambu:///virtual/...` URLs to this server instead of libBambuSource.
//
// Wire format (per direction):
//
//     [4-byte big-endian length N][N bytes of JSON payload]
//
// One frame per JSON-RPC message. The slicer's PrinterFileSystem already
// handles a trailing `\n\n` boundary inside the JSON portion, so we don't
// need an explicit content-type framing — every frame is JSON, the slicer
// parses it as `Bambu_Sample` of `itrack=CTRL_TYPE`.
//
// Phase 2 (current): proxy the JSON-RPC through libBambuSource. Each
// accepted SSL session opens a `bambu:///local/<ip>?port=6000&...` tunnel
// against the real printer via `Bambu_Create`/`Bambu_Open`/
// `Bambu_StartStreamEx(CTRL_TYPE=0x3001)`, forwards every request frame
// in via `Bambu_SendMessage` and pumps replies back to the slicer via a
// reader thread that polls `Bambu_ReadSample`. The bridge never parses
// the JSON-RPC; the proprietary plugin and the slicer's PrinterFileSystem
// share the protocol.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_VIRTUAL_TUNNEL_SERVER_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_VIRTUAL_TUNNEL_SERVER_HPP

#ifdef _WIN32
#  error "VirtualTunnelServer is Linux-only for now"
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../tls/CertFactory.hpp"

namespace Slic3r {
namespace bridge {

class BambuSourceHandle;

namespace server {

// Storage delegate signature. The vtun session_loop hands each inbound
// JSON-RPC frame to a callable matching this shape; the GUI binds it
// to `BridgeStorageBackend::send_request`.
//
// `request_body_json` is the serialized inner `req` payload (UTF-8
// JSON text). The reply callback returns the serialized reply JSON
// the same way. We deliberately pass JSON-as-string rather than
// `nlohmann::json` because the bridge module vendors a different
// version of the nlohmann/json header than the slicer GUI does, and
// the two ABIs cannot share `basic_json<...>` types in function
// signatures (the inline `json_abi_v3_11_3` namespace on one side
// vs no namespace on the other mangles to different symbols and
// produces link-time undefined references).
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
    std::function<void(int /*rc*/, std::string /*reply_json*/)> reply_cb)>;

struct VirtualTunnelVirtualDevice {
    std::string       dev_id;
    // Address the bridge LISTENS on for this device (typically the bridge
    // LAN interface bind, e.g. 0.0.0.0). NOT the real printer's IP.
    std::string       lan_ip   = "0.0.0.0";
    uint16_t          port     = 0;       // bridge picks an ephemeral if 0
    std::string       access_code;        // also the proxied passwd
    // Real printer's LAN IPv4 — the bridge connects HERE (port 6000) when
    // proxying CTRL_TYPE=0x3001 storage JSON-RPC through libBambuSource.
    // Empty for cloud-only printers; the session_loop will refuse to open
    // the real tunnel and close the slicer connection.
    std::string       printer_lan_ip;
    // Printer's OTA firmware version (e.g. "01.08.00.00"). Embedded in
    // the BambuTunnel storage URL as `dev_ver=…`. The printer-side
    // refuses to send its first frame until this is non-empty, so the
    // bridge must push a fresh value via `update_printer_firmware_ver`
    // whenever the GUI's MachineObject.ota_version changes.
    std::string       printer_firmware_ver;
    tls::CertMaterial cert;
};

struct VirtualTunnelServerConfig {
    int accept_backlog     = 4;
    int io_timeout_seconds = 60;

    // Slicer-identity URL fields baked into the BambuTunnel storage URL.
    // The proprietary plugin reads these out of the URL query string
    // (`net_ver`, `cli_id`, `cli_ver`) and may refuse to advance
    // StartStreamEx if they're empty. The in-GUI bridge fills these
    // from GUI_App; headless mode leaves them blank (storage from
    // headless --bridge-only isn't supported anyway).
    std::string slicer_net_ver;
    std::string slicer_cli_id;
    std::string slicer_cli_ver;
};

class VirtualTunnelServer {
public:
    explicit VirtualTunnelServer(VirtualTunnelServerConfig cfg);
    ~VirtualTunnelServer();

    VirtualTunnelServer(const VirtualTunnelServer&)            = delete;
    VirtualTunnelServer& operator=(const VirtualTunnelServer&) = delete;

    void add_device   (VirtualTunnelVirtualDevice dev);
    void remove_device(const std::string& dev_id);

    // Push a fresh real-printer LAN IP into the existing device entry
    // without tearing down the listener or the accept loop. The bridge
    // calls this from `update_lan_ip_locked` when SSDP discovers (or
    // refreshes) a printer's LAN IP after the device has already been
    // added. New sessions opened after this call see the updated IP;
    // sessions that were already in flight keep the IP they captured at
    // accept time. No-op when the dev_id isn't registered.
    void update_printer_lan_ip(const std::string& dev_id,
                               const std::string& printer_lan_ip);

    // Push a fresh OTA firmware version into the existing device entry.
    // Same semantics as update_printer_lan_ip: new sessions see the
    // updated value, in-flight sessions keep what they captured.
    void update_printer_firmware_ver(const std::string& dev_id,
                                     const std::string& firmware_ver);

    // Wire the proprietary libBambuSource handle through. Each session
    // Wire the storage delegate through. Each session hands every
    // inbound JSON-RPC frame to the delegate (which pushes it through
    // `PrinterFileSystem::SendRequest` via BridgeStorageBackend — the
    // GUI's proven libBambuSource consumer). This is the only storage
    // path the bridge supports; if no delegate is attached the
    // session_loop refuses the connection.
    //
    // The delegate is captured by value into per-session closures, so
    // it must remain valid for the lifetime of every session it
    // launches. GUI_App keeps the BridgeStorageBackend alive across
    // shutdown for this reason.
    void attach_storage_delegate(StorageDelegate delegate);

    void start();
    void stop();

    bool running() const noexcept { return m_running.load(); }

    // Returns the actually-bound port for a device. 0 if not bound yet.
    uint16_t bound_port(const std::string& dev_id) const;

    struct Device;

private:
    Device* find_locked(const std::string& dev_id);
    void start_device(Device& d);
    void stop_device(Device& d);

    VirtualTunnelServerConfig                                  m_cfg;
    std::atomic<bool>                                          m_running{false};
    mutable std::mutex                                         m_mu;
    std::unordered_map<std::string, std::unique_ptr<Device>>   m_devices;
    StorageDelegate                                            m_storage_delegate;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_VIRTUAL_TUNNEL_SERVER_HPP
