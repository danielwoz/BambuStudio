#include "NetworkAgentBridgeHooks.hpp"

#include "NetworkAgent.hpp"
#include "bambu_virtual_client/VirtualMqttClient.hpp"
#include "bambu_virtual_client/VirtualFtpsClient.hpp"

#include <mutex>
#include <utility>

namespace Slic3r {
namespace bridge_hooks {

// ----- Callback wrappers ---------------------------------------------------

OnMessageFn Dispatcher::make_on_message_wrapper(NetworkAgent* agent,
                                                OnMessageFn fn)
{
    // Plugin fires for ANY dev_id; we forward unconditionally to the
    // slicer, then fan out to the bridge tap for non-virtual dev_ids
    // only. Tap is sampled under m_bridge_tap_mu at fire time so it
    // can be detached at runtime without recompiling the wrapper.
    return [agent, fn](std::string dev_id, std::string msg) {
        if (fn) fn(dev_id, msg);
        BridgeMessageTap tap;
        {
            std::lock_guard<std::mutex> lk(agent->m_bridge_tap_mu);
            tap = agent->m_bridge_tap;
        }
        if (tap && !NetworkAgent::is_virtual_dev_id(dev_id))
            tap(dev_id, msg, /*is_local=*/false);
    };
}

OnLocalConnectedFn Dispatcher::make_on_local_connect_wrapper(
    OnLocalConnectedFn fn)
{
    // Plugin-side SSDP auto-discovery tries to LAN-MQTT-connect to our
    // bridge's broadcast (DevConnect: lan), fails cert verification
    // (Bambu CA chain), then fires this callback with state=Failed for
    // the virtual dev_id. GUI_App's handler sees Failed +
    // is_lan_mode_printer() and calls erase_local_machine — which yanks
    // our entry out of the UI a few seconds after we add it.
    //
    // Filter: drop plugin-originating callbacks for virtual dev_ids.
    // VirtualMqttClient::session_loop fires the same user callback
    // directly for virtual sessions, with state derived from its own
    // MQTT layer (which uses verify=false and actually connects).
    return [fn](int state, std::string dev_id, std::string msg) {
        if (NetworkAgent::is_virtual_dev_id(dev_id)) return;
        if (fn) fn(state, dev_id, msg);
    };
}

OnMessageFn Dispatcher::make_on_local_message_wrapper(NetworkAgent* agent,
                                                      OnMessageFn fn)
{
    // Same filter as make_on_local_connect_wrapper: drop any plugin-
    // originating messages for virtual dev_ids. VirtualMqttClient owns
    // the inbound path for those. After dispatching to the slicer, fan
    // a copy out to the in-GUI bridge tap if attached.
    return [agent, fn](std::string dev_id, std::string msg) {
        if (NetworkAgent::is_virtual_dev_id(dev_id)) return;
        if (fn) fn(dev_id, msg);
        BridgeMessageTap tap;
        {
            std::lock_guard<std::mutex> lk(agent->m_bridge_tap_mu);
            tap = agent->m_bridge_tap;
        }
        if (tap) tap(dev_id, msg, /*is_local=*/true);
    };
}

// ----- Captured-callback setters ------------------------------------------

void Dispatcher::capture_local_connect_cb(NetworkAgent* agent,
                                          OnLocalConnectedFn fn)
{
    agent->m_local_connect_cb = std::move(fn);
}

void Dispatcher::capture_local_message_cb(NetworkAgent* agent,
                                          OnMessageFn fn)
{
    agent->m_local_message_cb = std::move(fn);
}

// ----- Bridge tap setter --------------------------------------------------

void Dispatcher::set_bridge_message_tap(NetworkAgent* agent,
                                        BridgeMessageTap tap)
{
    std::lock_guard<std::mutex> lk(agent->m_bridge_tap_mu);
    agent->m_bridge_tap = std::move(tap);
}

// ----- Per-method virtual-path dispatchers --------------------------------

bool Dispatcher::try_connect_printer(NetworkAgent* agent,
                                     const std::string& dev_id,
                                     const std::string& dev_ip,
                                     const std::string& /*username*/,
                                     const std::string& password,
                                     int* out_rc)
{
    if (!NetworkAgent::is_virtual_dev_id(dev_id)) return false;
    auto& vc = ::Slic3r::VirtualMqttClient::instance();
    vc.set_on_local_connect(agent->m_local_connect_cb);
    vc.set_on_message      (agent->m_local_message_cb);
    int rc = vc.connect_printer(dev_id, dev_ip, /*access_code=*/password);
    if (rc == 0) agent->m_current_local_dev_id = dev_id;
    if (out_rc) *out_rc = rc;
    return true;
}

bool Dispatcher::try_disconnect_printer(NetworkAgent* agent, int* out_rc)
{
    // disconnect_printer is dev-id-less (the plugin only holds one LAN
    // session at a time). Route based on the dev_id we recorded at the
    // most-recent connect_printer.
    if (!NetworkAgent::is_virtual_dev_id(agent->m_current_local_dev_id))
        return false;
    const std::string id = agent->m_current_local_dev_id;
    agent->m_current_local_dev_id.clear();
    const int rc = ::Slic3r::VirtualMqttClient::instance().disconnect_printer(id);
    if (out_rc) *out_rc = rc;
    return true;
}

void Dispatcher::note_plugin_connect_success(NetworkAgent* agent,
                                             const std::string& dev_id)
{
    agent->m_current_local_dev_id = dev_id;
}

void Dispatcher::note_plugin_disconnect_success(NetworkAgent* agent)
{
    agent->m_current_local_dev_id.clear();
}

bool Dispatcher::try_send_message_to_printer(const std::string& dev_id,
                                             const std::string& json_str,
                                             int qos,
                                             int* out_rc)
{
    if (!NetworkAgent::is_virtual_dev_id(dev_id)) return false;
    const int rc = ::Slic3r::VirtualMqttClient::instance()
        .send_message(dev_id, json_str, qos);
    if (out_rc) *out_rc = rc;
    return true;
}

bool Dispatcher::try_start_send_gcode_to_sdcard(const PrintParams& params,
                                                OnUpdateStatusFn   update_fn,
                                                WasCancelledFn     cancel_fn,
                                                int*               out_rc)
{
    if (!NetworkAgent::is_virtual_dev_id(params.dev_id)) return false;

    ::Slic3r::virtual_ftps::UploadParams up;
    up.host        = params.dev_ip;
    // Default high port — slicer never sees this; the bridge picks it.
    // Keep in sync with BridgeAppConfig::ftps_port_base.
    up.port        = 39990;
    up.user        = params.username.empty() ? "bblp" : params.username;
    up.pass        = params.password;
    up.local_path  = params.filename;
    up.remote_name = params.ftp_file.empty() ? params.dst_file
                                             : params.ftp_file;

    ::Slic3r::virtual_ftps::ProgressFn  prog = nullptr;
    ::Slic3r::virtual_ftps::CancelledFn canc = nullptr;
    if (update_fn) {
        prog = [update_fn](int pct, std::string msg) {
            // Plugin signature is (status, code, msg). Slicer reads
            // `status` as percent and `msg` as label; `code` is a
            // sub-status not relevant for virtual uploads.
            update_fn(pct, /*code=*/0, msg);
        };
    }
    if (cancel_fn) {
        canc = [cancel_fn]() -> bool { return cancel_fn(); };
    }
    const int rc = ::Slic3r::virtual_ftps::upload(up, prog, canc);
    if (out_rc) *out_rc = rc;
    return true;
}

} // namespace bridge_hooks
} // namespace Slic3r
