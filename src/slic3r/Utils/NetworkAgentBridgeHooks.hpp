#ifndef SLIC3R_UTILS_NETWORK_AGENT_BRIDGE_HOOKS_HPP
#define SLIC3R_UTILS_NETWORK_AGENT_BRIDGE_HOOKS_HPP

// ----------------------------------------------------------------------------
// NetworkAgentBridgeHooks
//
// Out-of-line glue that NetworkAgent uses to route virtual-printer
// (FFFF-prefix dev_id) traffic through the open-source
// `bambu_virtual_client` instead of the proprietary `bambu_networking`
// plugin, and to fan plugin-side message callbacks out to an in-GUI
// bridge tap.
//
// Existed inline in NetworkAgent.cpp until phase3b; extracted to keep
// the diff against upstream bambulab/BambuStudio small.
//
// Behaviour-preserving wrapper: every entry point below has 1:1
// correspondence with the inline block it replaces. Side effects fire
// in the same order they did before.
// ----------------------------------------------------------------------------

#include "bambu_networking.hpp"
#include "libslic3r/ProjectTask.hpp"

#include <functional>
#include <string>

// NetworkAgent.hpp pulls BBL into the global namespace via
// `using namespace BBL`; we do the same so the OnMessageFn /
// OnLocalConnectedFn / OnUpdateStatusFn / WasCancelledFn typedefs
// resolve here without qualification.
using namespace BBL;

namespace Slic3r {

// Real fwd decl in the enclosing namespace so the `friend struct
// bridge_hooks::Dispatcher;` clause inside NetworkAgent.hpp resolves
// to this type rather than introducing a new name via elaborated-
// type-specifier (cf. phase3a bug class #1).
namespace bridge_hooks {
struct Dispatcher;
} // namespace bridge_hooks

class NetworkAgent;

namespace bridge_hooks {

// Dispatcher is the *only* type in this TU that touches NetworkAgent's
// private state. NetworkAgent grants it friendship, so every helper
// here can read/write the captured callbacks, m_current_local_dev_id,
// and the bridge tap members.
//
// All methods are static; the struct exists solely as a friendship
// anchor. No instances are ever constructed.
struct Dispatcher {

    using BridgeMessageTap =
        std::function<void(const std::string& dev_id,
                           const std::string& payload,
                           bool               is_local)>;

    // ---- Callback wrappers ------------------------------------------------
    // Each factory produces the std::function that NetworkAgent hands to
    // the proprietary plugin's `bambu_network_set_on_*` slot. The
    // wrappers (a) drop plugin-originating events for virtual dev_ids
    // because the VirtualMqttClient owns that side of the wire, and
    // (b) fan a copy of non-virtual events out to the in-GUI bridge tap.

    // Wrap an OnMessageFn for cloud (`set_on_message_fn`). Plugin fires
    // for ANY dev_id; we forward unconditionally to the slicer, then
    // fan out to the bridge tap for non-virtual dev_ids only.
    static OnMessageFn make_on_message_wrapper(
        NetworkAgent* agent, OnMessageFn fn);

    // Wrap an OnLocalConnectedFn for `set_on_local_connect_fn`. The
    // plugin's SSDP discovery fires this with state=Failed for our
    // FFFF dev_ids because our self-signed cert fails Bambu's CA
    // chain; the wrapper swallows those so GUI_App's handler does not
    // call erase_local_machine on virtual entries. Real LAN session
    // events fire through the VirtualMqttClient session loop.
    static OnLocalConnectedFn make_on_local_connect_wrapper(
        OnLocalConnectedFn fn);

    // Wrap an OnMessageFn for `set_on_local_message_fn`. Same virtual-
    // dev_id filter as the local-connect wrapper, plus the bridge-tap
    // fanout (is_local=true).
    static OnMessageFn make_on_local_message_wrapper(
        NetworkAgent* agent, OnMessageFn fn);

    // ---- Captured-callback setters ---------------------------------------
    // The `set_on_local_*_fn` entry points in NetworkAgent also need to
    // stash the user's raw callback so VirtualMqttClient can fire it on
    // virtual sessions. NetworkAgent's setters call these one-liners
    // before installing the wrapped fn with the plugin.

    static void capture_local_connect_cb(NetworkAgent* agent,
                                         OnLocalConnectedFn fn);
    static void capture_local_message_cb(NetworkAgent* agent,
                                         OnMessageFn fn);

    // ---- Bridge tap setter -----------------------------------------------
    static void set_bridge_message_tap(NetworkAgent* agent,
                                       BridgeMessageTap tap);

    // ---- Per-method virtual-path dispatchers -----------------------------
    // Each returns `true` and writes *out_rc when the virtual path took
    // responsibility for the call. Returns `false` to mean "fall through
    // to the proprietary plugin".

    // connect_printer: routes FFFF dev_ids into VirtualMqttClient, plus
    // records the dev_id as the current LAN-session target so a later
    // disconnect_printer can route correctly (the plugin's
    // disconnect_printer is dev-id-less).
    static bool try_connect_printer(NetworkAgent* agent,
                                    const std::string& dev_id,
                                    const std::string& dev_ip,
                                    const std::string& username,
                                    const std::string& password,
                                    int* out_rc);

    // disconnect_printer: consumes the recorded m_current_local_dev_id
    // and routes accordingly. Returns true ONLY if the recorded dev_id
    // was a virtual one.
    static bool try_disconnect_printer(NetworkAgent* agent,
                                       int* out_rc);

    // Mirror of the post-success bookkeeping that the plugin-side
    // branch of connect_printer / disconnect_printer used to do inline.
    // Called by NetworkAgent on the success path of the plugin call.
    static void note_plugin_connect_success(NetworkAgent* agent,
                                            const std::string& dev_id);
    static void note_plugin_disconnect_success(NetworkAgent* agent);

    // send_message_to_printer: routes FFFF dev_ids into VirtualMqttClient.
    static bool try_send_message_to_printer(const std::string& dev_id,
                                            const std::string& json_str,
                                            int qos,
                                            int* out_rc);

    // start_send_gcode_to_sdcard: routes FFFF dev_ids into
    // virtual_ftps::upload, translating PrintParams -> UploadParams and
    // wrapping update_fn / cancel_fn for the plugin-style ABI.
    static bool try_start_send_gcode_to_sdcard(
        const PrintParams& params,
        OnUpdateStatusFn   update_fn,
        WasCancelledFn     cancel_fn,
        int*               out_rc);
};

} // namespace bridge_hooks
} // namespace Slic3r

#endif // SLIC3R_UTILS_NETWORK_AGENT_BRIDGE_HOOKS_HPP
