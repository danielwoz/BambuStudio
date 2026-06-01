// Bambu Bridge — NetworkAgent → bridge plugin handle adapter.
//
// Background. The proprietary `bambu_networking` plugin allows ONE
// agent per process (see BridgeApp.cpp:133-138). In headless
// `--bridge-only` mode the bridge owns that one agent. In GUI mode
// the slicer's `Slic3r::NetworkAgent` already owns it, which is
// why `BridgeApp` constructed inside the GUI is host-driven and
// previously left m_plugin null — the uplinks then short-circuit
// on every is_connected() check.
//
// This adapter is a thin BambuNetworkingPluginHandle subclass that
// forwards every plugin-call the bridge issues at runtime
// (connect_printer, send_message_to_printer, subscribe_device,
// publish_to_device, is_user_login, …) to the slicer's existing
// NetworkAgent instead of dlopening the plugin a second time.
// It installs a "bridge message tap" on NetworkAgent so incoming
// cloud/LAN reports fan out to the per-dev_id receivers the bridge
// registers via register_receiver / register_local_message_receiver.
//
// Lifetime: the adapter is non-owning. The slicer's GUI_App owns
// the NetworkAgent and tears the bridge down before the network
// agent. Construction is cheap (no I/O); init() is a no-op because
// the slicer already brought the plugin up.

#ifndef SLIC3R_NETWORK_AGENT_PLUGIN_ADAPTER_HPP
#define SLIC3R_NETWORK_AGENT_PLUGIN_ADAPTER_HPP

#include "../../bambu_bridge/BambuNetworkingPluginHandle.hpp"
#include "PrintDispatcher.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace Slic3r {

class NetworkAgent;

class NetworkAgentPluginAdapter : public bridge::BambuNetworkingPluginHandle {
public:
    explicit NetworkAgentPluginAdapter(NetworkAgent* agent);
    ~NetworkAgentPluginAdapter() override;

    // No dlopen happens — the slicer already loaded the plugin. We
    // still return true so the bridge's "is the handle usable?"
    // checks pass.
    bool init() override;

    bool agent_ready()        const override;
    bool is_user_login()      const override;
    bool is_server_connected() const override;
    bool is_local_connected() const override;

    bool get_user_print_info(unsigned int* http_code,
                             std::string*  http_body) const override;

    int subscribe_device  (const std::string& dev_id) override;
    int unsubscribe_device(const std::string& dev_id) override;
    int publish_to_device (const std::string& dev_id,
                           const std::string& json_payload,
                           int                qos) override;

    int upload_gcode_to_sdcard(const CloudUploadParams& params) override;

    int connect_printer(const std::string& dev_id,
                        const std::string& dev_ip,
                        const std::string& username,
                        const std::string& password,
                        bool               use_ssl) override;
    int disconnect_printer() override;
    // Delegate the enc_msg gate-open primitives to the wrapped GUI
    // NetworkAgent (which owns the live plugin agent). The base
    // BambuNetworkingPluginHandle impls check their own null m_impl->agent
    // and would no-op here, so LanUplink's post-connect cert re-fire only
    // works if these route through the real agent.
    int  set_user_selected_machine(const std::string& dev_id) override;
    void install_device_cert(const std::string& dev_id, bool lan_only) override;
    int send_message_to_printer(const std::string& dev_id,
                                const std::string& json_payload,
                                int                qos) override;

    int start_local_print_with_record(const LocalPrintParams& params) override;
    int start_local_print            (const LocalPrintParams& params) override;
    int start_sdcard_print           (const LocalPrintParams& params) override;

    int get_camera_url(const std::string& dev_id,
                       std::string*       url_out,
                       int                timeout_ms = 10000) override;

    // Source-of-truth bridge for `PrintDispatcher::Inputs`. The adapter
    // calls this immediately before invoking the dispatcher; the
    // installer is responsible for reading from the live MachineObject
    // (or whatever per-printer capability source is available). See
    // PrintDispatcherInputs.hpp for the reusable from_dev_id() helper
    // that reads exactly what the GUI's PrintJob reads.
    //
    // If no resolver is installed, the adapter falls back to safe
    // defaults (cloud_print_only=false, has_sdcard=false,
    // could_emmc_print=false; ftp_folder=""). That matches what the
    // GUI sees for a brand-new printer before pushall has run.
    using DispatcherInputsResolver = std::function<void(
        const std::string&         dev_id,
        PrintDispatcher::Inputs&   inputs_out,
        std::string&               ftp_folder_out)>;
    void set_dispatcher_inputs_resolver(DispatcherInputsResolver r);

    // Per-printer mTLS lookup. Used by `send_message_to_printer` as a
    // FALLBACK when the proprietary plugin's cloud + LAN paths both
    // fail (rc_cloud=-2 / rc_lan=-4 — the long-known
    // "plugin won't send from non-UI contexts" issue documented in
    // memory feedback_proprietary_lib.md). With these fields the
    // adapter can dial the printer's LAN broker directly using raw
    // OpenSSL + client cert, which the printer firmware accepts for
    // every payload class (status reads, `print.command=*`, tier2, …).
    //
    // Resolver returns true and fills `out` when info is available for
    // `dev_id`; returns false to skip the fallback (and the plugin's
    // rc is propagated as-is).
    struct MtlsTarget {
        std::string printer_ip;     // e.g. "192.168.1.209"
        std::string access_code;    // 8-char LAN access code (broker password)
        std::string cert_path;      // absolute path to client cert chain (PEM)
        std::string key_path;       // absolute path to client private key (PEM)
    };
    using MtlsResolver = std::function<bool(const std::string& dev_id,
                                            MtlsTarget&        out)>;
    void set_mtls_resolver(MtlsResolver r);

private:
    NetworkAgent*       m_agent { nullptr };  // non-owning, GUI_App owns it
    std::atomic<bool>   m_local_connected { false };

    mutable std::mutex          m_resolver_mu;
    DispatcherInputsResolver    m_inputs_resolver; // optional; nullptr-safe
    MtlsResolver                m_mtls_resolver;   // optional; nullptr-safe
};

} // namespace Slic3r

#endif // SLIC3R_NETWORK_AGENT_PLUGIN_ADAPTER_HPP
