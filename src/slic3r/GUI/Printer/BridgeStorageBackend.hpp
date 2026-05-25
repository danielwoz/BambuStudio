// BridgeStorageBackend — delegate virtual-storage JSON-RPC to PFS.
//
// The bridge's `VirtualTunnelServer` accepts JSON-RPC frames from a
// "virtual" slicer process and used to translate them to libBambuSource
// calls itself (Bambu_Create/Open/StartStreamEx/SendMessage/ReadSample
// against the real printer's TLS port 6000). That replication kept
// misbehaving for reasons we can't pin down without reverse-engineering
// the proprietary protocol, which is explicitly off-limits.
//
// Instead, this class delegates the bridge's per-session JSON-RPC to
// the slicer's `PrinterFileSystem` (PFS) — the GUI's *proven*
// libBambuSource consumer. PFS already owns the entire Bambu_Tunnel
// lifecycle (RecvMessageThread, sequence allocation, reconnect,
// notifies). We open one PFS per real_dev_id, route every inbound
// request through `fs->SendRequest(cmdtype, req, cb)`, and shuttle the
// reply JSON back to the virtual slicer over the same SSL session.
//
// Threading:
//
//   - `send_request` is callable from any thread. It marshals PFS
//     construction / Bind / SetUrl / Start to the wx main thread via
//     `wxGetApp().CallAfter(...)`.
//   - The status listener (bound on the PFS) fires on the wx main
//     thread; it drives `ready` state and drains the pending-request
//     queue.
//   - PFS' RecvMessageThread invokes the per-request callback on its
//     own thread. The callback is provided by `VirtualTunnelServer`'s
//     session_loop and writes the response envelope back to the
//     slicer; it uses a shared mutex + alive-flag so it bails if the
//     session has already torn down.

#ifndef SLIC3R_GUI_PRINTER_BRIDGE_STORAGE_BACKEND_HPP
#define SLIC3R_GUI_PRINTER_BRIDGE_STORAGE_BACKEND_HPP

#include <boost/smart_ptr/shared_ptr.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"

class PrinterFileSystem;

namespace Slic3r {
namespace bridge {

class BridgeStorageBackend {
public:
    using ReplyCallback =
        std::function<void(int /*rc*/, nlohmann::json /*reply*/)>;

    BridgeStorageBackend();
    ~BridgeStorageBackend();

    BridgeStorageBackend(const BridgeStorageBackend&)            = delete;
    BridgeStorageBackend& operator=(const BridgeStorageBackend&) = delete;

    // Queue a single JSON-RPC request to be sent through the per-dev_id
    // PFS instance. Creates the PFS lazily on first call; reuses it for
    // every subsequent request on the same `real_dev_id`. The reply
    // callback fires on the PFS recv thread.
    //
    // `request_body` is the inner `req` payload (everything inside the
    // slicer-virtual envelope's `req` field). `cmdtype` is the
    // PrinterFileSystem opcode (LIST_INFO, FILE_DOWNLOAD, …). PFS
    // assigns its own outbound sequence; the wire-level sequence
    // remapping is the caller's job.
    void send_request(const std::string& real_dev_id,
                      const std::string& real_lan_ip,
                      const std::string& access_code,
                      const std::string& dev_ver,
                      const std::string& net_ver,
                      const std::string& cli_id,
                      const std::string& cli_ver,
                      int                cmdtype,
                      nlohmann::json     request_body,
                      ReplyCallback      cb);

    // Tear down the PFS for a single dev_id (called when the device
    // disappears from the bridge's table). Must be runnable from any
    // thread — internally CallAfter()s the wx main thread.
    void release(const std::string& real_dev_id);

    // Tear down every PFS. Safe to call multiple times. Used by
    // GUI_App::OnExit before the BridgeApp is reset.
    void shutdown();

private:
    struct PfsEntry {
        boost::shared_ptr<PrinterFileSystem>      fs;
        bool                                      ready = false;
        std::vector<std::function<void()>>        pending_sends;
        std::string                               url;
    };

    // URL construction lives in Slic3r::GUI::build_media_storage_url
    // (see MediaUrlBuilder.hpp). Both BridgeStorageBackend and the
    // GUI's MediaFilePanel call it to keep the local/tutk/agora
    // selection ladder in one place.

    std::mutex                                            m_mu;
    std::map<std::string, std::shared_ptr<PfsEntry>>      m_by_dev;
};

}} // namespace Slic3r::bridge

#endif // SLIC3R_GUI_PRINTER_BRIDGE_STORAGE_BACKEND_HPP
