// Bambu Bridge — native storage delegate.
//
// A `VirtualTunnelServer::StorageDelegate` implementation that talks to the
// real printer's TLS port 6000 directly (no proprietary plugin) using
// `LocalControlTunnel`. Activates only for printer models known to speak the
// legacy BambuTunnel storage protocol (X1C / P1S / X1 / X1E). For every
// other model (H2S / H2D / A1 / unknown) it falls through to the fallback
// delegate the caller installed (the GUI's PrinterFileSystem-via-plugin
// path).
//
// Lifecycle:
//   - `register_device(dev_id, model)` from BridgeApp's `add_device_locked`
//     populates the model registry; subsequent storage requests for that
//     dev_id consult the registry to decide native-vs-fallback routing.
//   - `unregister_device(dev_id)` drops the registry entry AND any open
//     LocalControlTunnel for that dev_id.
//   - `make_delegate(...)` returns a `StorageDelegate` callable that
//     captures a shared_ptr to this instance and is safe to install on
//     VirtualTunnelServer.
//
// Currently implemented FTCmds: 1 (LIST_INFO) and 2 (SUB_FILE). Other
// cmdtypes (3=DELETE, 4=DOWNLOAD, 5=UPLOAD, 7=MEDIA_ABILITY) are
// forwarded through the tunnel as raw JSON envelopes — they don't need
// any bridge-side state machine, just the wire framing the tunnel
// already provides. Multi-frame uploads/downloads with binary tails are
// NOT implemented yet (the FtpsServer handles uploads via a separate
// path), so cmdtype 4/5 will return whatever the printer replies with
// for a single-frame exchange.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_NATIVE_STORAGE_DELEGATE_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_NATIVE_STORAGE_DELEGATE_HPP

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Slic3r {
namespace bridge {
namespace router {

class LocalControlTunnel;

// StorageDelegate signature must match `server::StorageDelegate`. Repeated
// here so this header doesn't pull in VirtualTunnelServer.hpp.
using StorageDelegateFn = std::function<void(
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

class NativeStorageDelegate
    : public std::enable_shared_from_this<NativeStorageDelegate> {
public:
    NativeStorageDelegate() = default;
    ~NativeStorageDelegate();

    NativeStorageDelegate(const NativeStorageDelegate&)            = delete;
    NativeStorageDelegate& operator=(const NativeStorageDelegate&) = delete;

    // Models the native delegate handles directly. Anything else falls
    // through to the fallback delegate. Case-insensitive; matches any
    // model string containing these substrings:
    //   "X1"  -> X1, X1C, X1E, "3DPrinter-X1"
    //   "P1"  -> P1S, P1P
    static bool model_is_native(const std::string& model);

    // Returns "X1C", "P1S", "H2S", "A1", or empty for unknown.
    // Just for log line formatting.
    static std::string normalize_model_tag(const std::string& model);

    // Register a device. Subsequent storage requests for `dev_id` will
    // route to the native tunnel iff `model_is_native(model)`.
    void register_device(const std::string& dev_id,
                         const std::string& model);

    // Drop registry entry + close any open tunnel for `dev_id`.
    void unregister_device(const std::string& dev_id);

    // Install a fallback delegate to use when a device isn't in the
    // native-eligible set, OR when the native tunnel fails to open.
    void set_fallback(StorageDelegateFn fallback);

    // Return a delegate callable suitable for
    // `VirtualTunnelServer::attach_storage_delegate`. The callable
    // captures a shared_ptr to `this` so it remains valid even after the
    // owning unique_ptr drops (the vtun server's per-session threads can
    // outlive teardown briefly).
    StorageDelegateFn make_delegate();

private:
    struct DeviceEntry {
        std::string                         model;        // raw vendor string
        bool                                native = false;
        // Lazily opened; null until first request. We open per-call (NOT
        // long-lived) because the printer eats inactive tunnels after a
        // short timeout. Native-storage operations are bursty (slicer
        // opens the Storage tab, does a LIST, closes), so per-request
        // overhead is acceptable.
        std::unique_ptr<LocalControlTunnel> tunnel;
        std::mutex                          tunnel_mu;
    };

    // Returns the registry entry or nullptr. Holds m_mu only briefly;
    // the caller can then lock the per-device tunnel_mu without holding
    // the registry mutex.
    std::shared_ptr<DeviceEntry> find_(const std::string& dev_id);

    void handle_native_(
        const std::shared_ptr<DeviceEntry>& entry,
        const std::string& dev_id,
        const std::string& real_lan_ip,
        const std::string& access_code,
        int                cmdtype,
        std::string        request_body_json,
        std::function<void(int, std::string)> reply_cb);

    void handle_fallback_(
        const std::string& real_dev_id,
        const std::string& real_lan_ip,
        const std::string& access_code,
        const std::string& dev_ver,
        const std::string& net_ver,
        const std::string& cli_id,
        const std::string& cli_ver,
        int                cmdtype,
        std::string        request_body_json,
        std::function<void(int, std::string)> reply_cb);

    std::mutex                                              m_mu;
    std::unordered_map<std::string, std::shared_ptr<DeviceEntry>> m_devices;
    StorageDelegateFn                                       m_fallback;
};

}  // namespace router
}  // namespace bridge
}  // namespace Slic3r

#endif  // SLIC3R_BAMBU_BRIDGE_ROUTER_NATIVE_STORAGE_DELEGATE_HPP
