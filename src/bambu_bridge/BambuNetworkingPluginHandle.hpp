// Bambu Bridge — BambuNetworkingPluginHandle.
//
// Shared owner of the proprietary `bambu_networking` plugin agent. The
// plugin is a Bambu-Lab-produced shared library that talks to the cloud
// MQTT broker / REST APIs on our behalf. Only one agent can exist per
// process (the plugin allocates global state under the covers, and the
// host process — BambuStudio — instantiates exactly one).
//
// This class centralises:
//
//   - dlopen / LoadLibrary of the .so / .dll, with the same candidate
//     probing CloudInventory used in phase 1.
//   - Resolution of the C++-ABI-by-name function table (the symbol names
//     are unmangled C but the *calling convention* is Itanium C++ with
//     std::string / std::vector / std::function passed by value — using
//     `const char*` here corrupts the stack at runtime).
//   - The single `void* agent` returned by `bambu_network_create_agent`,
//     including the documented boot order: create → set_config_dir →
//     set_country_code → init_log → start.
//   - A thread-safe dispatcher for the cloud-side `OnMessageFn` callback:
//     the plugin fires `(dev_id, payload_string)` on one of its own
//     worker threads, and we route to a per-dev_id receiver registered
//     by `CloudUplink`. The receivers themselves run on the plugin's
//     thread (they're cheap — they just hand off into LanUplink-style
//     `DownstreamPublisher` callbacks), so no extra marshalling here.
//
// The handle is intentionally a CLASS (not an interface) but with all
// member functions made virtual so tests can subclass it and inject a
// `MockPluginHandle` that records every send/subscribe in memory. The
// `init()` member is the only one that opens the .so — subclasses
// override it to skip dlopen.
//
// Lifetime: instantiate once in BridgeService startup, share via
// `std::shared_ptr` to both `CloudInventory` and `CloudUplink`.

#ifndef SLIC3R_BAMBU_BRIDGE_PLUGIN_HANDLE_HPP
#define SLIC3R_BAMBU_BRIDGE_PLUGIN_HANDLE_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace bridge {

// Per-process plugin configuration. Mirrors the subset CloudInventoryConfig
// already exposed (with the addition of `log_dir`, which the plugin's
// `create_agent` takes as its first argument). All fields are optional;
// an empty string means "don't pass the corresponding setter".
struct PluginHandleConfig {
    std::string plugin_path;     // absolute path; empty = probe defaults
    std::string log_dir;         // -> create_agent(log_dir)
    std::string config_dir;      // -> set_config_dir(agent, ...)
    std::string country_code;    // -> set_country_code(agent, ...)

    // Headers forwarded to bambu_network_set_extra_http_header. The
    // proprietary plugin uses its own curl session for cloud REST calls,
    // so we can't override CURLOPT_USERAGENT directly — instead we
    // attach the same X-BBL-* identification fields BambuStudio's
    // NetworkAgent attaches (X-BBL-Client-Name, X-BBL-Client-Version,
    // X-BBL-OS-Type, X-BBL-OS-Version, X-BBL-Device-ID, X-BBL-Language).
    // Empty map → skip the call.
    std::map<std::string, std::string> extra_http_headers;

    // Forwarded to bambu_network_set_cert_file. The GUI passes
    // ${resources_dir}/cert + "slicer_base64.cer" — the base64-encoded
    // Bambu cloud root cert the plugin uses to verify TLS to the cloud
    // MQTT broker. Both empty → skip the call (plugin falls back to its
    // built-in cert list, which may not include Bambu's chain).
    std::string cert_dir;
    std::string cert_file;
};

class BambuNetworkingPluginHandle {
public:
    explicit BambuNetworkingPluginHandle(PluginHandleConfig cfg = {});
    virtual ~BambuNetworkingPluginHandle();

    BambuNetworkingPluginHandle(const BambuNetworkingPluginHandle&)            = delete;
    BambuNetworkingPluginHandle& operator=(const BambuNetworkingPluginHandle&) = delete;

    // Probe + dlopen + resolve + create_agent + start. Returns true on
    // success. False (and the handle stays in the "no agent" state) when
    // the .so can't be loaded, required symbols are missing, or the
    // plugin's start() refuses to come up. Idempotent.
    //
    // Test subclasses override this to skip the dlopen entirely.
    virtual bool init();

    // True iff init() has completed successfully (plugin loaded + agent
    // started). Cheap; safe to poll.
    virtual bool agent_ready() const;

    // Plugin's self-reported version string. Mirrors
    // `NetworkAgent::get_version()` — returns "00.00.00.00" when the
    // plugin isn't loaded or the symbol is missing. The GUI embeds this
    // as the `&net_ver=` query param on `bambu_create` URLs.
    virtual std::string plugin_version() const;

    // True iff the plugin reports the user is currently logged in. False
    // when the plugin isn't loaded.
    virtual bool is_user_login() const;

    // True iff the plugin's cloud-MQTT transport is currently connected.
    // Driven by the OnServerConnectedFn callback the plugin fires when
    // the broker session comes up / drops. False when the plugin isn't
    // loaded.
    virtual bool is_server_connected() const;

    // ---- CloudInventory pass-through ---------------------------------
    //
    // Calls bambu_network_get_user_print_info. Returns true on
    // success and fills `*http_code` and `*http_body` accordingly.
    // Returns false (without touching the out-params) when the plugin
    // isn't loaded or the export wasn't found.
    virtual bool get_user_print_info(unsigned int* http_code,
                                     std::string*  http_body) const;

    // ---- CloudUplink pass-through ------------------------------------
    //
    // Subscribe / unsubscribe to a single dev_id on the cloud broker.
    // The plugin's `add_subscribe` / `del_subscribe` take a vector of
    // dev_ids; this wrapper handles the one-shot common case. Returns
    // BAMBU_NETWORK_SUCCESS (0) or a negative error code. Returns
    // BAMBU_NETWORK_ERR_INVALID_HANDLE (-1) when no agent is loaded.
    virtual int subscribe_device  (const std::string& dev_id);
    virtual int unsubscribe_device(const std::string& dev_id);

    // Publish `json_payload` to the cloud broker for `dev_id`. Matches
    // the proprietary `bambu_network_send_message_to_printer` signature
    // (the proprietary forwarder routes to cloud or LAN under the hood).
    // qos is the MQTT QoS the slicer requested. Returns 0 on success,
    // negative on error.
    virtual int publish_to_device(const std::string& dev_id,
                                  const std::string& json_payload,
                                  int                qos);

    // ---- CloudUploadSink pass-through --------------------------------
    //
    // Stage a .3mf upload through the proprietary plugin's cloud OSS +
    // SD-card path. The plugin's `start_send_gcode_to_sdcard` export
    // uploads bytes to Bambu cloud OSS and signals the printer to fetch
    // the result onto its internal SD card (no print start; the slicer
    // issues the print command separately via MQTT). Same export
    // `~/BambuStudio/src/slic3r/GUI/Jobs/SendJob.cpp` calls — routing
    // through it (rather than reimplementing OSS upload) keeps cloud-
    // bound bytes byte-identical to a real BambuStudio session.
    //
    // Caller writes the upload payload to disk at `local_file_path`
    // before invoking; the plugin streams from disk.
    //
    // Returns 0 on success, negative on error:
    //   -1  no agent loaded
    //   -2  plugin doesn't expose the export (older plugin versions)
    struct CloudUploadParams {
        std::string dev_id;             // printer serial
        std::string dev_ip;             // empty for cloud-only route
        std::string access_code;        // LAN MQTT/FTPS password (plugin re-uses for cloud auth)
        std::string local_file_path;    // path to the .3mf already on disk
        std::string project_name;       // task / project label; default = filename
        std::string connection_type;    // "cloud" | "lan" — plugin routes accordingly
        bool        use_ssl_for_ftp  = true;
        bool        use_ssl_for_mqtt = true;
    };
    virtual int upload_gcode_to_sdcard(const CloudUploadParams& params);

    // ---- Receiver typedef (used by both cloud and LAN dispatch) ----------
    //
    // CloudUplink / LanUplink each register one MessageReceiver per
    // dev_id when they want to receive printer→bridge PUBLISH frames for
    // that device. The plugin fires `OnMessageFn(dev_id, payload_string)`
    // (or `OnLocalMessageFn` with the same shape) from one of its worker
    // threads; we look up the matching receiver (under a mutex) and
    // invoke it. The receiver runs on the plugin's thread — so it must be
    // cheap and non-blocking. Cloud-side and LAN-side topics are derived
    // from the dev_id: the plugin always pushes reports for
    // `device/<dev_id>/report` so we synthesise that topic at dispatch
    // time.
    using MessageReceiver =
        std::function<void(std::string topic,
                           std::vector<uint8_t> payload,
                           uint8_t qos)>;

    // ---- LAN-side pass-through -------------------------------------------
    //
    // The proprietary plugin owns the LAN MQTT-over-TLS transport too.
    // Mirroring what real BambuStudio does via NetworkAgent::connect_printer
    // / disconnect_printer / send_message_to_printer / set_on_local_*,
    // the bridge routes its LAN traffic through these wrappers instead of
    // hand-rolling a TLS+MQTT client. This is the only way to keep the
    // bytes-on-the-wire (TLS cipher choice, MQTT client_id format, etc.)
    // byte-identical to a native client.
    //
    // *Important serialisation*: the plugin only supports ONE LAN
    // connection at a time (single global `connect_printer` state). The
    // bridge serialises bring-up per-dev_id in `LanUplink`; see that
    // header for the cooperative scheduling notes.

    // Establish the LAN MQTT-over-TLS connection to `dev_ip` for `dev_id`.
    // Username is conventionally "bblp"; password is the printer's LAN
    // access code. `use_ssl=true` matches what real BambuStudio passes
    // (printers ship a self-signed cert; plugin uses SSL_VERIFY_NONE).
    // Returns the plugin's int rc (0 = success). Returns -1 (no agent)
    // or -2 (missing export) when the plugin isn't loaded.
    virtual int connect_printer(const std::string& dev_id,
                                const std::string& dev_ip,
                                const std::string& username,
                                const std::string& password,
                                bool               use_ssl);

    // Mark `dev_id` the plugin's active machine. The enc_msg gate and
    // the cert handshake are keyed off this selection. Returns plugin's
    // rc (0 = success), -1/-2 for no-agent / missing-export.
    virtual int set_user_selected_machine(const std::string& dev_id);

    // Trigger the device-cert handshake (cert_request → printer's
    // cert_report reply) that populates the plugin's device_pub_key_map
    // for `dev_id`. Until that map entry exists, the enc_msg gate refuses
    // to sign print.* and send_message_to_printer drops the publish with
    // rc=-4. Must be called AFTER connect_printer establishes the LAN
    // session so the cert_request rides a live socket. No return (the
    // plugin export is void).
    virtual void install_device_cert(const std::string& dev_id, bool lan_only);

    // Tear down the current LAN MQTT-over-TLS connection. Global (the
    // plugin only holds one). Returns plugin's rc (0 = success), -1/-2
    // for the no-agent / missing-export degraded states.
    virtual int disconnect_printer();

    // Send an MQTT PUBLISH to the currently-LAN-connected printer.
    // Symmetry with `publish_to_device` for the cloud side; the upstream
    // export is the same `send_message_to_printer` symbol — when there's
    // a live `connect_printer` session, the plugin routes the payload to
    // the LAN broker instead of the cloud one. Returns 0 on success,
    // negative on error.
    virtual int send_message_to_printer(const std::string& dev_id,
                                        const std::string& json_payload,
                                        int                qos);

    // Stage a LAN print via `start_local_print_with_record`. The plugin
    // handles the underlying transport choice (FTPS-990 vs port-6000
    // BambuTunnel) per printer model. Returns 0 on success, negative on
    // error (-1 no agent, -2 missing export, plugin's rc otherwise).
    struct LocalPrintParams {
        std::string dev_id;             // printer serial
        std::string dev_ip;             // printer IPv4
        std::string access_code;        // LAN auth password
        std::string local_file_path;    // path to the .3mf already on disk
        std::string project_name;       // user-facing label; default = filename
        std::string connection_type;    // "lan" for the LAN route
        bool        use_ssl_for_ftp  = true;
        bool        use_ssl_for_mqtt = true;
    };
    virtual int start_local_print_with_record(const LocalPrintParams& params);

    // True iff the LAN connection driven by `connect_printer` is currently
    // up. Maintained via the OnLocalConnectedFn callback the plugin fires
    // when the LAN MQTT session lands / drops. False when no LAN session
    // has been brought up (or the plugin isn't loaded).
    virtual bool is_local_connected() const;

    // ---- LAN → bridge message dispatch -----------------------------------
    //
    // Same pattern as `register_receiver` for cloud, but registered against
    // the plugin's `OnLocalMessageFn` (set_on_local_message_fn). The
    // plugin's callback signature is `(dev_id, payload_string)` — identical
    // to the cloud-side OnMessageFn.

    virtual void register_local_message_receiver  (const std::string& dev_id,
                                                   MessageReceiver    cb);
    virtual void unregister_local_message_receiver(const std::string& dev_id);

    // The local-connect callback fires with the plugin's int rc when the
    // LAN session lands or drops. rc == 0 means "now connected". The
    // upstream signature is `(int status, std::string dev_id, std::string msg)`
    // but we project to `(int return_code, int reason_code)` for symmetry
    // with the cloud-side register_*_callback (reason_code is unused for
    // LAN today — set to 0).
    virtual void register_local_connected_callback(
        std::function<void(int return_code, int reason_code)> cb);

    // For tests: simulate an incoming LAN message and the LAN-connected
    // callback firing. Production code never invokes these.
    void deliver_local_message_for_test(const std::string& dev_id,
                                        const std::string& payload);
    void deliver_local_connected_for_test(bool connected);

    // ---- Cloud → bridge message dispatch -----------------------------
    //
    // CloudUplink registers one MessageReceiver per dev_id (see typedef
    // above) for cloud-side reports. The plugin fires `OnMessageFn` for
    // these; we dispatch under a mutex and synthesise the report topic.

    virtual void register_receiver  (const std::string& dev_id,
                                     MessageReceiver    cb);
    virtual void unregister_receiver(const std::string& dev_id);

    // For tests: synthesise an incoming cloud message as if the plugin
    // itself had called us. Production code never invokes this — the
    // plugin's worker thread does, via the registered OnMessageFn — but
    // mock subclasses and test code use it to drive `CloudUplink`
    // without bringing up a real plugin.
    void deliver_message_for_test(const std::string& dev_id,
                                  const std::string& payload);

    // ---- Camera URL retrieval ---------------------------------------------
    //
    // Wraps `bambu_network_get_camera_url`. The upstream signature is:
    //
    //   int (*func_get_camera_url)(void* agent, std::string dev_id,
    //                              std::function<void(std::string)> callback);
    //
    // i.e. the plugin returns a synchronous int rc and fires `callback`
    // with the resolved URL *asynchronously* (sometimes inline if the URL
    // is cached, sometimes from a worker thread after a cloud round-trip).
    // See `~/BambuStudio/src/slic3r/GUI/MediaPlayCtrl.cpp` for the slicer's
    // call pattern. We expose that as a synchronous "wait on a condvar
    // with timeout" API so CloudCameraSource doesn't have to handle the
    // callback dance itself.
    //
    // `dev_id` may be the bare serial OR the slicer's composite
    // `"<dev_id>|<dev_ver>|<protocols>"` token (see MediaPlayCtrl line
    // 373). The bridge uses the bare serial — the plugin treats either
    // form the same way for the rtsps_ path.
    //
    // Returns 0 on success and fills `*url_out` with the resolved URL.
    // Returns:
    //   -1  no agent loaded
    //   -2  plugin doesn't expose the export
    //   -3  plugin returned non-zero (URL retrieval failed)
    //   -4  timeout elapsed before the plugin fired the callback
    //
    // The URL format mirrors what the slicer's `MediaPlayCtrl` would have
    // received: `bambu:///rtsps___<user>:<pw>@<ip>/streaming/live/1?proto=rtsps`
    // for the LAN-discovered path, or `bambu:///agora/...` / `bambu:///tutk/...`
    // for the cloud-tunnel path. The bridge hands the URL verbatim to
    // `BambuSourceHandle::bambu_create` — BambuSource dispatches by scheme.
    virtual int get_camera_url(const std::string& dev_id,
                               std::string*       url_out,
                               int                timeout_ms = 10000);

    // For tests: simulate the OnServerConnected callback firing.
    void deliver_server_connected_for_test(bool connected);

protected:
    // Subclasses (mocks) can flip this to pretend they have an agent.
    void set_agent_ready_for_test(bool ready);

    // Subclass-visible internal: dispatch a payload to the receiver for
    // `dev_id`. Used by both the production OnMessageFn pump and
    // `deliver_message_for_test`.
    void dispatch_message(const std::string& dev_id,
                          const std::string& payload);

    // LAN counterpart of `dispatch_message`. Looks up the LAN-side
    // receiver registered via `register_local_message_receiver` and
    // invokes it on the caller's thread.
    void dispatch_local_message(const std::string& dev_id,
                                const std::string& payload);

    // Drive the local-connected flag + fire the user's callback (if set).
    // Called by the plugin's OnLocalConnectedFn pump and by
    // `deliver_local_connected_for_test`.
    void dispatch_local_connected(bool connected);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_PLUGIN_HANDLE_HPP
