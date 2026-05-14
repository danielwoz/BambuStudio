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

#include <atomic>
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
    int send_message_to_printer(const std::string& dev_id,
                                const std::string& json_payload,
                                int                qos) override;

    int start_local_print_with_record(const LocalPrintParams& params) override;

    int get_camera_url(const std::string& dev_id,
                       std::string*       url_out,
                       int                timeout_ms = 10000) override;

private:
    NetworkAgent*       m_agent { nullptr };  // non-owning, GUI_App owns it
    std::atomic<bool>   m_local_connected { false };
};

} // namespace Slic3r

#endif // SLIC3R_NETWORK_AGENT_PLUGIN_ADAPTER_HPP
