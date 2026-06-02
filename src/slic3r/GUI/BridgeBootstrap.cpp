// Bambu Bridge — bootstrap & teardown for GUI_App.
//
// All the bridge-related modifications scattered across GUI_App.cpp's
// `OnInit` / `OnExit` / `on_init_inner` / `on_init_network` / ctor used
// to live here as inline `#ifdef BAMBU_BRIDGE` blocks. They are now in
// this TU. GUI_App.cpp keeps only short call-site glue. See
// BridgeBootstrap.hpp for the public surface.

#include "slic3r/GUI/BridgeBootstrap.hpp"

#if defined(BAMBU_BRIDGE)

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceManager.hpp" // MachineObject + DeviceManager full definition
#include "slic3r/GUI/DeviceCore/DevManager.h"

#include "slic3r/GUI/BridgeOnlyFlag.hpp"
#include "slic3r/GUI/Printer/BridgeStorageBackend.hpp"
#include "slic3r/GUI/Printer/MediaUrlBuilder.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentPluginAdapter.hpp"
#include "slic3r/Utils/PrintDispatcherInputs.hpp"
#include "slic3r/Utils/bambu_virtual_client/VirtualLanPrinterStore.hpp"
#include "slic3r/Utils/bambu_virtual_client/VirtualMqttClient.hpp"

#include "bambu_bridge/headless/BridgeApp.hpp"
#include "bambu_bridge/headless/SignalHandler.hpp"
#include "bambu_bridge/CloudSession.hpp"
#include "bambu_bridge/CloudDeviceList.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r_version.h" // SLIC3R_VERSION

#include <wx/app.h>
#include <wx/event.h>
#include <wx/timer.h>
#include <wx/window.h>

#include <boost/log/trivial.hpp>
#include "nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {
namespace BridgeBootstrap {

// Install the mTLS fallback resolver on a freshly-constructed adapter.
// Lets `send_message_to_printer` dial the printer's LAN broker directly
// when the proprietary plugin's cloud + LAN paths both fail (the long-
// known "plugin won't send from non-UI contexts" issue documented in
// project memory feedback_proprietary_lib.md). Sources lan_ip +
// access_code + cert paths from BridgeApp's m_devices table.
static void install_mtls_resolver(
        Slic3r::NetworkAgentPluginAdapter& adapter,
        Slic3r::bridge::headless::BridgeApp* bridge_app) {
    if (!bridge_app) return;
    adapter.set_mtls_resolver(
        [bridge_app](const std::string&                                 dev_id,
                     Slic3r::NetworkAgentPluginAdapter::MtlsTarget&     out)
            -> bool {
            Slic3r::bridge::headless::BridgeApp::MtlsInfo info;
            if (!bridge_app->mtls_info_for(dev_id, info)) return false;
            out.printer_ip  = info.lan_ip;
            out.access_code = info.access_code;
            out.cert_path   = info.cert_path;
            out.key_path    = info.key_path;
            return true;
        });
}

// Install the per-printer capability resolver on a freshly-constructed
// NetworkAgentPluginAdapter. Three sites in this file construct an
// adapter (and one more in BridgeOnlyConsoleApp.cpp); each needs to
// install the same resolver so the bridge sees what the GUI sees.
//
// Helper kept here rather than on the adapter itself because it
// reaches into GUI_App / wxGetApp().app_config / DeviceManager — none
// of which the adapter (which lives in slic3r/Utils) should depend on.
static void install_print_dispatcher_resolver(
        Slic3r::NetworkAgentPluginAdapter& adapter,
        GUI_App*                           app) {
    if (!app) return;
    adapter.set_dispatcher_inputs_resolver(
        [app](const std::string&                       dev_id,
              Slic3r::PrintDispatcher::Inputs&         inputs_out,
              std::string&                              ftp_folder_out) {
            // Per-printer caps from the live MachineObject. Same call
            // paths as SelectMachine.cpp:3070, 3113, 3114.
            auto* dm = app->getDeviceManager();
            inputs_out = Slic3r::PrintDispatcherInputsFromMachineObject::
                from_dev_id(
                    dm,
                    dev_id,
                    /*app_lan_mode_only=*/
                        app->app_config &&
                        !app->app_config->get("lan_mode_only").empty() &&
                        app->app_config->get("lan_mode_only") == "1",
                    /*verify_temp_path=*/
                        Slic3r::resources_dir() + "/check_access_code.txt");

            // Per-printer ftp_folder from the model JSON. Same call
            // path as SelectMachine.cpp:3060 (via
            // obj->get_ftp_folder()).
            if (dm) {
                auto list = dm->get_user_machinelist();
                auto it = list.find(dev_id);
                if (it != list.end() && it->second) {
                    ftp_folder_out =
                        Slic3r::PrintDispatcherInputsFromMachineObject::
                        get_ftp_folder_for_model(it->second->printer_type);
                }
            }
        });
}

// ============================================================================
// is_bridge_only — cheap shim around the CLI-prepass global.
// ============================================================================

bool is_bridge_only()
{
    return Slic3r::GUI::g_bridge_only;
}

// ============================================================================
// is_invisible_gui — BAMBU_BRIDGE_INVISIBLE_GUI=1: run the full GUI process
// without showing windows or popping startup modals. Sole source of truth so
// every site (MainFrame::Show, on_select_default_preset, etc.) reads the same
// boolean. Cached on first call; safe to call before wx is up.
// ============================================================================

bool is_invisible_gui()
{
    static const bool cached = [] {
        const char* e = std::getenv("BAMBU_BRIDGE_INVISIBLE_GUI");
        return e && *e && (*e == '1' || *e == 't' || *e == 'T' || *e == 'y' || *e == 'Y');
    }();
    return cached;
}

// ============================================================================
// Proof-of-life: per-dev_id inbound-message observation.
//
// The bridge's on_message / on_local_message handlers fan inbound MQTT
// reports into DeviceManager. We add a sibling tap that records the
// last-seen timestamp per dev_id so the cascade's tail can synchronously
// wait for proof that a printer's cloud tunnel is actually alive
// (vs. plugin sessions that report login=1/server=1 but in fact have
// no usable route to the printer — see project_bridge_cloud_tunnel).
// ============================================================================

namespace {
std::mutex                                                       g_inbound_mu;
std::map<std::string, std::chrono::steady_clock::time_point>     g_inbound_at;

void note_inbound(const std::string& dev_id)
{
    std::lock_guard<std::mutex> lk(g_inbound_mu);
    g_inbound_at[dev_id] = std::chrono::steady_clock::now();
}

// Returns true if note_inbound(dev_id) was called at-or-after `since`,
// polling every 100ms until `timeout` elapses.
bool wait_for_inbound(const std::string&                         dev_id,
                      std::chrono::steady_clock::time_point      since,
                      std::chrono::milliseconds                  timeout)
{
    const auto deadline = since + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(g_inbound_mu);
            auto it = g_inbound_at.find(dev_id);
            if (it != g_inbound_at.end() && it->second >= since) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}
} // namespace

// ============================================================================
// register_app_factory — IMPLEMENT_APP replacement.
//
// IMPLEMENT_APP(GUI_App) normally expands to:
//   wxIMPLEMENT_WX_THEME_SUPPORT   // empty on non-Universal
//   wxIMPLEMENT_WXWIN_MAIN         // int main(){ return wxEntry(...); }
//   wxIMPLEMENT_APP_NO_MAIN(appname):
//     appname& wxGetApp()      // returns the typed app instance
//     wxAppConsole* wxCreateApp(){ return new appname; }
//     wxAppInitializer wxTheAppInitializer((...) wxCreateApp);
//
// For the bridge build we substitute our own factory function so the
// CLI prepass that set `g_bridge_only` can route into a different app
// type. The factory currently always returns `GUI_App` — the former
// BridgeOnlyConsoleApp path was abandoned (DeviceManager hard-depends
// on wxGetApp() being a GUI_App for parse_user_print_info etc.). Run
// monitor-less servers via `xvfb-run -a bambu-studio --bridge-only` or
// `Xvfb :99 -screen 0 1x1x8 & DISPLAY=:99 bambu-studio --bridge-only`.
//
// Lives at file scope inside `Slic3r::GUI` to match the namespace the
// IMPLEMENT_APP macro originally expanded in; the resulting symbols
// (including `main`) are effectively private — wx only consults
// `wxTheAppInitializer`'s static ctor to look up the factory function
// pointer, regardless of which namespace it lives in.
// ============================================================================

// Keep wxIMPLEMENT_WX_THEME_SUPPORT + wxIMPLEMENT_WXWIN_MAIN from the
// macro alongside our own factory. `wxGetApp()` still returns `GUI_App&`
// because every caller of it in the slicer is a GUI-only code path.
wxIMPLEMENT_WX_THEME_SUPPORT
wxIMPLEMENT_WXWIN_MAIN
} // namespace BridgeBootstrap

GUI_App& wxGetApp() { return *static_cast<GUI_App*>(wxApp::GetInstance()); }

namespace BridgeBootstrap {

// Custom factory. Always returns GUI_App today; the dispatch hook is kept
// so future bridge-only-only modes can plug in without touching GUI_App.
static wxAppConsole* slic3r_create_app()
{
    wxAppConsole::CheckBuildOptions(WX_BUILD_OPTIONS_SIGNATURE,
                                    "BambuStudio");
    return new Slic3r::GUI::GUI_App();
}

static wxAppInitializer
    slic3r_app_initializer(reinterpret_cast<wxAppInitializerFunction>(slic3r_create_app));

void register_app_factory()
{
    // The static-initialiser side effect of `slic3r_app_initializer`
    // already registered the factory by the time main() runs. This
    // function exists purely so GUI_App.cpp can take its address to
    // force the linker to keep this TU.
    (void)slic3r_app_initializer;
}

// ============================================================================
// run_headless — headless --bridge-only OnInit branch.
//
// Mirrors the in-GUI bridge bootstrap (install_gui_worker, below) so
// the bridge runs on the EXACT same code path as the GUI worker thread
// (BridgeStorageBackend delegating to PrinterFileSystem,
// NetworkAgentPluginAdapter wrapping the slicer's NetworkAgent, the
// wxTimer push pump feeding DeviceManager snapshots into
// BridgeApp::set_virtual_printers). Deltas vs. the GUI path:
//   * no MainFrame
//   * ExitOnFrameDelete(false)  (so the loop stays up with zero windows)
//   * SIGINT/SIGTERM route to ExitMainLoop
// ============================================================================

bool run_headless(GUI_App* app)
{
    BOOST_LOG_TRIVIAL(info)
        << "BridgeBootstrap::run_headless: starting headless --bridge-only";

    // Without a top-level frame, wxApp would normally exit the moment
    // OnInit returns. Suppress that — our SignalHandler controls the
    // exit.
    app->SetExitOnFrameDelete(false);

    // The slicer normally seeds Slic3r::Http's extra headers from
    // get_extra_header() inside on_init_inner before the network agent
    // is created. Mirror that so the agent's own curl session, and any
    // other Slic3r::Http callers, see the same X-BBL-* fingerprint the
    // GUI uses.
    {
        std::map<std::string, std::string> extra_headers = app->get_extra_header();
        Slic3r::Http::set_extra_headers(extra_headers);
    }

    // Bring up NetworkAgent + DeviceManager exactly like the GUI does.
    // Skips dialogs because mainframe is null and the dialog hooks are
    // only installed via init_networking_callbacks (not called here).
    //
    // Precondition: the slicer's cloud session must already be cached
    // in `~/.config/BambuStudio/` (BambuStudio.conf +
    // BambuNetworkEngine.conf). Without a cached session the plugin
    // loads but `is_user_login()` returns false and the cloud inventory
    // walk yields zero printers — the bridge stays alive but no
    // per-printer MQTT mirror gets bound. To prime the session: run
    // `bambu-studio` (no `--bridge-only`) once interactively and log
    // in via the User panel, then close. The plugin persists the
    // session token in BambuNetworkEngine.conf for subsequent headless
    // launches.
    app->copy_network_if_available();
    if (!app->on_init_network()) {
        BOOST_LOG_TRIVIAL(error)
            << "BridgeBootstrap::run_headless: on_init_network() failed";
        return false;
    }

    // The GUI only calls connect_server in the EVT_USER_LOGIN_HANDLE
    // handler (GUI_App::on_user_login_handle:5249), which is queued by
    // request_user_login_handle — and nothing in the bridge-only path
    // ever calls that. Without connect_server the plugin loads the
    // cached login (is_user_login()==true) but the cloud MQTT socket
    // never gets opened, so is_server_connected()==false. The
    // proprietary plugin then rejects any "control" send_message
    // (ams_filament_setting, ams_control, extrusion_cali_set, …) with
    // BAMBU_NETWORK_ERR_SEND_MSG_FAILED (-4) because those need an
    // active cloud session. Mirror the GUI's post-login sequence:
    // connect_server → start_subscribe("app") → get_user_print_info,
    // all in a background thread so the wx event loop can keep
    // pumping while the plugin does its HTTPS+MQTT dance.
    if (app->m_agent) {
        std::thread([app]{
            using namespace std::chrono_literals;
            const auto t0 = std::chrono::steady_clock::now();
            auto* ag = app->m_agent;

            // -- Ship-2 first attempt: native session ----------------
            // Try the bridge's own plaintext session file. If valid
            // (or refreshable), use the bearer token to fetch the
            // device list natively — no need to wait on the plugin's
            // cached-login round-trip for the SESSION + DEVICE-LIST
            // half. The plugin is still needed for the MQTT side
            // (connect_server below) + enc_msg cert install (further
            // down).
            bool native_session_used = false;
            std::string native_devlist_body;
            {
                Slic3r::bridge::CloudSession sess;
                auto lr = sess.load_and_refresh_if_needed(/*slack_seconds=*/300);
                if (lr.ok) {
                    Slic3r::bridge::CloudDeviceList lister;
                    auto dl = lister.fetch(lr.data);
                    if (dl.ok && !dl.body.empty()) {
                        std::fprintf(stderr,
                            "[bridge] cache_session_used=true refreshed=%d "
                            "uid=%lld region=%s device_list_http=%ld "
                            "body_len=%zu\n",
                            int(lr.refreshed),
                            (long long) lr.data.uid,
                            lr.data.region.c_str(),
                            dl.http_status, dl.body.size());
                        std::fflush(stderr);
                        native_session_used = true;
                        native_devlist_body = std::move(dl.body);
                    } else {
                        std::fprintf(stderr,
                            "[bridge] native session loaded but device-list "
                            "fetch failed: status=%ld err=%s; falling back "
                            "to plugin path\n",
                            dl.http_status, dl.error.c_str());
                        std::fflush(stderr);
                    }
                } else {
                    std::fprintf(stderr,
                        "[bridge] cache_session_used=false reason=%s; "
                        "falling back to plugin path\n",
                        lr.error.c_str());
                    std::fflush(stderr);
                }
            }

            std::fprintf(stderr,
                "[bridge] cloud-bringup: start login=%d server=%d "
                "native_session=%d\n",
                int(ag->is_user_login()), int(ag->is_server_connected()),
                int(native_session_used));
            std::fflush(stderr);
            ag->enable_multi_machine(true);
            std::fprintf(stderr,
                "[bridge] enable_multi_machine(true)\n");
            std::fflush(stderr);
            int rc = ag->connect_server();
            std::fprintf(stderr,
                "[bridge] connect_server rc=%d (post login=%d server=%d)\n",
                rc, int(ag->is_user_login()), int(ag->is_server_connected()));
            std::fflush(stderr);
            // Subscribe to the "app" topic — the GUI does this after
            // login. Without it the plugin's cloud-side reactor has
            // no work to do and may keep the MQTT socket idle.
            int sub_rc = ag->start_subscribe("app");
            std::fprintf(stderr,
                "[bridge] start_subscribe(\"app\") rc=%d\n", sub_rc);
            std::fflush(stderr);
            // (reverted: add_subscribe per dev_id broke cloud-camera)
            // HTTP REST validates auth + wakes the plugin's cloud
            // state machine. The GUI does this in a background
            // thread inside on_user_login_handle.
            //
            // Ship-2: if we already fetched the device list via the
            // native CloudDeviceList path, skip the plugin's REST
            // round-trip — the plugin's get_user_print_info goes
            // through the same `/iot-service/api/user/print` endpoint
            // we just hit, and the response body is identical (we
            // matched parse_user_print_info's expected shape). The
            // plugin's call also serves as a "wake the cloud state
            // machine" pulse, so we still do it for the fallback
            // path.
            unsigned int http_code = 0;
            std::string  body;
            int rc2 = 0;
            if (!native_session_used) {
                rc2 = ag->get_user_print_info(&http_code, &body);
                std::fprintf(stderr,
                    "[bridge] get_user_print_info rc=%d http=%u body_len=%zu\n",
                    rc2, http_code, body.size());
                std::fflush(stderr);
            } else {
                body = native_devlist_body;
                http_code = 200;
                std::fprintf(stderr,
                    "[bridge] device-list from native cache (skipping plugin "
                    "REST) body_len=%zu\n", body.size());
                std::fflush(stderr);
            }
            // NOTE: parse_user_print_info crashes the bridge child with
            // SIGSEGV — it dives into GUI code that expects parts of
            // GUI_App we haven't initialised. Skipping.
            // Poll is_server_connected for up to 20s.
            bool server_ok = false;
            for (int i = 0; i < 40; ++i) {
                if (ag->is_server_connected()) {
                    std::fprintf(stderr,
                        "[bridge] is_server_connected -> true after %d ticks\n",
                        i);
                    std::fflush(stderr);
                    server_ok = true;
                    break;
                }
                std::this_thread::sleep_for(500ms);
            }
            if (!server_ok) {
                std::fprintf(stderr,
                    "[bridge] is_server_connected stayed false after 20s\n");
                std::fflush(stderr);
                return;
            }
            // Now that the cloud MQTT is up, subscribe to our owned
            // dev_ids. Plugin's `bambu_network_send_message` (cloud)
            // rejects with -2 INVALID_HANDLE when the dev_id isn't
            // in the cloud-subscribed set; that's the failure we see
            // for ams_filament_setting / ams_control / etc.
            std::vector<std::string> dev_ids = g_bridge_only_cfg.only_dev_ids;
            if (!dev_ids.empty()) {
                int add_rc = ag->add_subscribe(dev_ids);
                std::fprintf(stderr,
                    "[bridge] add_subscribe(%zu dev_ids) rc=%d ids=%s\n",
                    dev_ids.size(), add_rc, dev_ids[0].c_str());
                std::fflush(stderr);
                // The plugin's cloud send_message rejects with -2 for
                // any dev_id that isn't the "user-selected" one. The
                // GUI calls this when the user clicks a cloud-bound
                // printer (DevManager.cpp:504). Each bridge child
                // owns exactly one printer, so just select it.
                int sel_rc = ag->set_user_selected_machine(dev_ids[0]);
                std::fprintf(stderr,
                    "[bridge] set_user_selected_machine(%s) rc=%d\n",
                    dev_ids[0].c_str(), sel_rc);
                std::fflush(stderr);
                // install_device_cert is what the slicer does for every
                // owned printer after MachineObject::connect (GUI_App.cpp:2121,
                // 5455 and DevManager.cpp:913). For cloud-bound printers
                // (lan_only=false) this likely installs the device's cert
                // into the plugin's trust store so the plugin can sign
                // outgoing control-command publishes. Without this, the
                // plugin's send_message rejects with -2 SEND_MSG_FAILED.
                for (const auto& d : dev_ids) {
                    ag->install_device_cert(d, /*lan_only=*/false);
                    std::fprintf(stderr,
                        "[bridge] install_device_cert(%s, lan_only=false)\n",
                        d.c_str());
                    std::fflush(stderr);
                }

                // -- Post-LAN enc_msg gate-open cascade --
                //
                // Mirror what the GUI bridge bringup (install_gui_worker
                // → finish_cascade in this same file) does. Without this,
                // device_pub_key_map[dev_id] stays empty in the plugin and
                // every print.* publish (start_print, start_local_print_
                // with_record cloud-relay, send_message_to_printer for
                // control commands) is rejected — the plugin returns
                // SEND_MSG_FAILED (-4) or PRINT_SP_ENC_FLAG_NOT_READY
                // (-3140), and the printer itself returns
                // err_code=84033543 / reason="mqtt message verify failed"
                // for any direct-MQTT bypass that lacks the per-message
                // signature.
                //
                // The cascade is what populates device_pub_key_map:
                //   set_user_selected_machine(d) → install_device_cert(d,
                //   false) → wait for printer's cert_report reply → plugin
                //   parses, stores the per-device pub key.
                //
                // A single install_device_cert is timing-flaky (~1/3 LAN
                // races on H2S per EXP-{A..E}). The GUI loops 3 rounds ×
                // 5s. Headless was previously doing ONLY the initial
                // single install_device_cert above; on cold-start
                // --bridge-only / --bridge-multi children the gate stayed
                // shut, which is what the user-visible "the bridge can't
                // start a print" symptom traces back to. Headless now
                // matches GUI exactly.
                //
                // See project_plugin_enc_gate memory + EXP-{A..E}-RESULTS
                // for the empirical / RE paths.
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(15s);
                std::fprintf(stderr,
                    "[bridge] post-LAN enc_msg gate-open cycle "
                    "(%zu dev_ids; 3 rounds × 5s each)\n",
                    dev_ids.size());
                std::fflush(stderr);
                for (int round = 0; round < 3; ++round) {
                    for (const auto& d : dev_ids) {
                        int sel_rc2 = ag->set_user_selected_machine(d);
                        ag->install_device_cert(d, /*lan_only=*/false);
                        std::fprintf(stderr,
                            "[bridge] (gate-cycle r%d) dev=%s "
                            "set_user_selected_machine rc=%d + "
                            "install_device_cert; waiting 5s for cert_report\n",
                            round, d.c_str(), sel_rc2);
                        std::fflush(stderr);
                        std::this_thread::sleep_for(5s);
                    }
                }
                std::fprintf(stderr,
                    "[bridge] enc_msg gate cycle complete for %zu dev_ids; "
                    "print.* publishes should now route through plugin\n",
                    dev_ids.size());
                std::fflush(stderr);
            }

            const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr,
                "[bridge] cloud-bringup total elapsed=%lld ms path=%s\n",
                (long long) dt_ms,
                native_session_used ? "native" : "plugin-fallback");
            std::fflush(stderr);

            // -- Ship-2: persist native session after plugin fallback --
            // After the plugin path successfully populated tokens (and
            // brought up MQTT) we want next boot to use the native
            // path. Try `bambu_network_get_my_token` (empty ticket)
            // which per RE-CLOUD-LOGIN.md §5 returns the in-memory
            // accessToken/refreshToken triple. If extraction yields
            // usable tokens, write them to ~/.config/BambuBridge/
            // session.json with mode 0600.
            if (!native_session_used && ag->is_user_login()) {
                unsigned int gt_http = 0;
                std::string  gt_body;
                int gt_rc = ag->get_my_token(/*ticket=*/std::string(),
                                              &gt_http, &gt_body);
                std::fprintf(stderr,
                    "[bridge] post-fallback get_my_token rc=%d http=%u "
                    "body_len=%zu\n", gt_rc, gt_http, gt_body.size());
                std::fflush(stderr);

                Slic3r::bridge::CloudSessionData d;
                d.region     = Slic3r::bridge::region_for_bridge();
                d.user_email = ag->get_user_name();
                try {
                    auto uid_str = ag->get_user_id();
                    if (!uid_str.empty()) d.uid = std::stoll(uid_str);
                } catch (...) {}

                if (gt_rc == 0 && !gt_body.empty()) {
                    try {
                        auto j = nlohmann::json::parse(gt_body);
                        auto find_str = [&](const char* k) -> std::string {
                            auto it = j.find(k);
                            if (it == j.end() || it->is_null()) return {};
                            if (it->is_string()) return it->get<std::string>();
                            return {};
                        };
                        auto find_int = [&](const char* k) -> int64_t {
                            auto it = j.find(k);
                            if (it == j.end() || it->is_null()) return 0;
                            if (it->is_number()) return (int64_t) it->get<double>();
                            if (it->is_string()) {
                                try { return std::stoll(it->get<std::string>()); }
                                catch (...) { return 0; }
                            }
                            return 0;
                        };
                        d.access_token  = find_str("accessToken");
                        if (d.access_token.empty()) d.access_token = find_str("access_token");
                        d.refresh_token = find_str("refreshToken");
                        if (d.refresh_token.empty()) d.refresh_token = find_str("refresh_token");
                        int64_t ei = find_int("expiresIn");
                        int64_t re = find_int("refreshExpiresIn");
                        auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        if (ei > 0) d.access_expires_at  = now_s + ei;
                        if (re > 0) d.refresh_expires_at = now_s + re;
                    } catch (const std::exception& ex) {
                        std::fprintf(stderr,
                            "[bridge] get_my_token body not JSON: %s\n",
                            ex.what());
                        std::fflush(stderr);
                    }
                }
                if (!d.access_token.empty() && !d.refresh_token.empty()) {
                    Slic3r::bridge::CloudSession().save(d);
                    std::fprintf(stderr,
                        "[bridge] persisted native session after plugin "
                        "fallback; next boot will use native path\n");
                    std::fflush(stderr);
                } else {
                    std::fprintf(stderr,
                        "[bridge] plugin did not surface usable tokens via "
                        "get_my_token; native session NOT persisted "
                        "(access=%s refresh=%s) — next boot will fallback "
                        "again\n",
                        d.access_token.empty() ? "empty" : "present",
                        d.refresh_token.empty() ? "empty" : "present");
                    std::fflush(stderr);
                }
            }
        }).detach();
    }

    // Bridge bootstrap — identical to install_gui_worker except:
    //   * cfg is seeded from the parsed CLI in g_bridge_only_cfg (not
    //     a default-constructed BridgeAppConfig).
    //   * No DeviceManager-side dialog hooks; headless trusts the agent
    //     to come up with cached tokens (or run logged-out and just
    //     advertise nothing until a follow-up adds a login flow).
    try {
        Slic3r::bridge::headless::BridgeAppConfig cfg = g_bridge_only_cfg;

        // Slicer-identity fields baked into the storage tunnel URL —
        // libBambuSource refuses to advance Bambu_StartStreamEx for
        // storage without them. Always source from the live agent /
        // app_config so they match the GUI even if the CLI didn't
        // pass overrides.
        cfg.slicer_net_ver = Slic3r::NetworkAgent::get_version();
        if (app->app_config)
            cfg.slicer_cli_id = app->app_config->get("slicer_uuid");
        cfg.slicer_cli_ver = std::string(SLIC3R_VERSION);

        app->m_bridge_storage = std::make_unique<Slic3r::bridge::BridgeStorageBackend>();
        app->m_bridge_app     = std::make_unique<Slic3r::bridge::headless::BridgeApp>(cfg);

        // The slicer's VirtualMqttClient needs to dial the per-printer
        // MQTT port the bridge bound (mqtt_port_base + index). Without
        // this resolver the client falls back to 8883 — which only the
        // first virtual printer is listening on — and every other
        // printer's CONNECT lands on the wrong device and fails auth.
        {
            auto* bridge_raw = app->m_bridge_app.get();
            Slic3r::VirtualMqttClient::instance().set_port_resolver(
                [bridge_raw](const std::string& dev_id) -> uint16_t {
                    return bridge_raw ? bridge_raw->mqtt_port_for_dev_id(dev_id) : 0;
                });
        }

        auto* storage_raw = app->m_bridge_storage.get();
        app->m_bridge_app->attach_storage_delegate(
            [storage_raw](const std::string& real_dev_id,
                          const std::string& real_lan_ip,
                          const std::string& access_code,
                          const std::string& dev_ver,
                          const std::string& net_ver,
                          const std::string& cli_id,
                          const std::string& cli_ver,
                          int                cmdtype,
                          std::string        request_body_json,
                          std::function<void(int, std::string)> reply_cb) {
                if (!storage_raw) {
                    if (reply_cb) reply_cb(/*ERROR_PIPE*/3, "{}");
                    return;
                }
                nlohmann::json req_body;
                try {
                    req_body = request_body_json.empty()
                        ? nlohmann::json::object()
                        : nlohmann::json::parse(request_body_json);
                } catch (const std::exception& ex) {
                    BOOST_LOG_TRIVIAL(error)
                        << "bridge-storage: bad request JSON: " << ex.what();
                    if (reply_cb) reply_cb(/*ERROR_JSON*/2, "{}");
                    return;
                }
                auto wrapped_cb = [reply_cb = std::move(reply_cb)]
                                  (int rc, nlohmann::json reply) {
                    if (!reply_cb) return;
                    reply_cb(rc, reply.dump());
                };
                storage_raw->send_request(
                    real_dev_id, real_lan_ip, access_code,
                    dev_ver, net_ver, cli_id, cli_ver,
                    cmdtype, std::move(req_body),
                    std::move(wrapped_cb));
            },
            // PFS' wxEvtHandler unwind still needs to happen on the wx
            // main thread even in headless mode (there is one — wx is
            // running OnInit on it).
            [app](const std::string& dev_id) {
                if (!app->m_bridge_storage) return;
                auto* backend = app->m_bridge_storage.get();
                app->CallAfter([backend, dev_id] { backend->release(dev_id); });
            });

        if (app->m_agent) {
            auto adapter =
                std::make_shared<Slic3r::NetworkAgentPluginAdapter>(app->m_agent);
            install_print_dispatcher_resolver(*adapter, app);
            install_mtls_resolver(*adapter, app->m_bridge_app.get());
            app->m_bridge_app->attach_plugin_handle(std::move(adapter));
        } else {
            BOOST_LOG_TRIVIAL(warning)
                << "run_headless: NetworkAgent missing — "
                   "bridge will advertise nothing until login is wired";
        }

        app->m_bridge_thread = std::make_unique<std::thread>([app]() {
            try {
                const int rc = app->m_bridge_app->run();
                BOOST_LOG_TRIVIAL(info)
                    << "Bambu Bridge worker exited rc=" << rc;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error)
                    << "Bambu Bridge worker crashed: " << e.what();
            }
        });

        // Push pump: feed DeviceManager snapshots into BridgeApp every
        // 5s, on the wx main thread (the same thread that mutates
        // DeviceManager — race-free by construction).
        app->m_bridge_push_timer = std::make_unique<wxTimer>(app);
        app->Bind(wxEVT_TIMER, [app](wxTimerEvent&) {
            if (!app->m_bridge_app)     return;
            if (!app->m_device_manager) return;
            if (!app->m_agent || !app->m_agent->is_user_login()) return;
            std::vector<Slic3r::bridge::headless::VirtualPrinter> snap;
            for (const auto& kv : app->m_device_manager->get_user_machinelist()) {
                MachineObject* mo = kv.second;
                if (!mo) continue;
                std::string id = mo->get_dev_id();
                if (id.empty()) continue;
                Slic3r::bridge::headless::VirtualPrinter p;
                p.dev_id      = std::move(id);
                p.dev_name    = mo->get_dev_name();
                p.lan_ip      = mo->get_dev_ip();
                p.access_code = mo->get_access_code();
                p.model       = mo->printer_type;
                p.firmware    = mo->get_ota_version();
                // Resolve the live-view URL through build_media_live_url — the
                // single native helper that encodes the liveview protocol
                // decision (the same one MediaPlayCtrl uses). The bridge picks
                // its camera source from this URL's scheme, so this IS the
                // decision; we don't hand it a raw liveview_local to re-decide.
                // We only capture the LAN-direct branch synchronously; the
                // TUTK branch's HTTP fetch would arrive too late for this tick
                // (and the bridge's CloudCameraSource has its own
                // get_camera_url path). Empty p.camera_url => no local
                // protocol resolved -> the bridge prefers cloud.
                // Capture via shared_ptr, NOT [&p]: build_media_live_url's
                // TUTK/remote branch calls agent->get_camera_url() which fires
                // its callback ASYNCHRONOUSLY (HTTP fetch) — long after `p` is
                // moved into `snap` and destroyed. A [&p] capture there is a
                // use-after-free (segfault / heap corruption). The shared_ptr
                // outlives both; we read the SYNC (LAN-direct) result right
                // after the call, and any late async write lands harmlessly.
                auto cam_url = std::make_shared<std::string>();
                Slic3r::GUI::build_media_live_url(mo,
                    [cam_url](std::string url, Slic3r::GUI::MediaUrlError err) {
                        if (err == Slic3r::GUI::MediaUrlError::Ok)
                            *cam_url = std::move(url);
                    });
                p.camera_url = *cam_url;
                snap.push_back(std::move(p));
            }
            if (snap.empty()) return;
            app->m_bridge_app->set_virtual_printers(std::move(snap));
        }, app->m_bridge_push_timer->GetId());
        app->m_bridge_push_timer->Start(5000);

        BOOST_LOG_TRIVIAL(info)
            << "Bambu Bridge --bridge-only started; DeviceManager push "
               "every 5s, NetworkAgent " << (app->m_agent ? "attached" : "missing");
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error)
            << "Failed to start --bridge-only bridge: " << e.what();
        return false;
    }

    // SIGINT/SIGTERM trampoline. The signal arrives on some thread the
    // OS picks; we just hand the wx main loop a "please quit" via
    // CallAfter so wx's ExitMainLoop runs on its own thread.
    //
    // RAII-owned by GUI_App for the lifetime of the wxApp; OnExit's
    // bridge teardown will run after ExitMainLoop unwinds.
    static std::unique_ptr<Slic3r::bridge::headless::SignalHandler> s_signals;
    s_signals = std::make_unique<Slic3r::bridge::headless::SignalHandler>(
        [app] {
            app->CallAfter([app] {
                BOOST_LOG_TRIVIAL(info)
                    << "--bridge-only: signal received, exiting wx loop";
                app->ExitMainLoop();
            });
        });

    app->m_initialized = true;
    return true;
}

// ============================================================================
// install_gui_worker — in-GUI bridge bootstrap.
//
// Runs in the same process as the slicer so other slicers on the LAN
// see this BambuStudio's cloud-bound printers as virtual LAN devices
// for the lifetime of the session. Opt out via
// BAMBU_BRIDGE_GUI_DISABLED=1.
//
// In --bridge-only mode this is a no-op (the worker already started
// from run_headless).
// ============================================================================

void install_gui_worker(GUI_App* app)
{
    if (g_bridge_only) {
        // The headless branch already wired the bridge up.
        return;
    }

    // Pin user_last_selected_machine = BAMBU_BRIDGE_TARGET_DEV so the
    // stock GUI's natural startup path
    // (m_load_last_machine.InnerLoad → DeviceManager::set_selected_machine
    //  → m_agent->set_user_selected_machine → plugin cloud subscribe →
    //  set_on_printer_connected_fn → bootstrap triplet)
    // selects THIS child's printer without needing an interactive
    // Device-tab click. Replaces the bridge-only "device-tab-sim"
    // cascade that used to live downstream of here.
    //
    // app_config is wxWidgets-owned and persists keys we set via ->set
    // here through subsequent slicer saves; pre-edits to BambuStudio
    // .conf get clobbered on first save because the slicer's known-
    // keys map drops anything it didn't explicitly set in-process.
    if (const char* td = std::getenv("BAMBU_BRIDGE_TARGET_DEV");
        td && *td && app && app->app_config) {
        // BAMBU_BRIDGE_TARGET_DEV may carry a comma-separated list
        // (multi-process launcher passes one dev_id per child but the
        // legacy single-process mode allowed several); the GUI's
        // user_last_selected_machine slot only holds one. Pick the
        // first — that's the canonical printer for this child.
        std::string val = td;
        const auto comma = val.find(',');
        if (comma != std::string::npos) val.resize(comma);
        if (!val.empty()) {
            app->app_config->set("user_last_selected_machine", val);
            std::fprintf(stderr,
                "[bridge-gui] pinned user_last_selected_machine=%s "
                "(from BAMBU_BRIDGE_TARGET_DEV) so InnerLoad selects "
                "this printer without a Device-tab click\n",
                val.c_str());
            std::fflush(stderr);
        }
    }

    // Bambu Bridge — GUI worker thread. Runs in the same process so other
    // slicers on the LAN see this BambuStudio's cloud-bound printers as
    // virtual LAN devices for the lifetime of the session.
    //
    // The bridge is opt-out: set BAMBU_BRIDGE_GUI_DISABLED=1 in the
    // environment to skip startup (useful for users who only want the
    // slicer side without the network exposure). The same
    // $BAMBU_BRIDGE_PLUGIN_PATH the headless daemon reads applies here.
    if (const char* disabled = std::getenv("BAMBU_BRIDGE_GUI_DISABLED");
        disabled && *disabled && std::strcmp(disabled, "0") != 0) {
        BOOST_LOG_TRIVIAL(info)
            << "Bambu Bridge skipped (BAMBU_BRIDGE_GUI_DISABLED set).";
        return;
    }

    try {
        // host_drives_inventory defaults to TRUE — the bridge does
        // NOT construct its own plugin handle. The slicer's
        // NetworkAgent (owned by this GUI_App) is the (one and only)
        // plugin consumer in this process. A wxTimer further down
        // pushes DeviceManager snapshots into the bridge from the
        // main thread (the same thread that mutates DeviceManager),
        // so we never need a thread-safe accessor on the
        // DeviceManager side.
        Slic3r::bridge::headless::BridgeAppConfig cfg;
        // inventory_poll is irrelevant in push mode (reconcile_once
        // early-returns when neither printer_source nor m_inventory
        // are wired). Leaving the default 60s so the poll thread
        // stays alive for shutdown-via-stop-flag plumbing.

        // Multi-process launcher support: BAMBU_BRIDGE_PORT_OFFSET shifts
        // every server's port base by N so one-bridge-per-printer children
        // don't collide. Each child owns its own plugin LAN slot (no
        // SWAP-thrash), so the enc_msg cert handshake stays stable and
        // print.* signing is reliable for that printer. SSDP (1900/2021)
        // is multicast with SO_REUSEPORT and stays shared — each child
        // answers M-SEARCH for its own printer with its own (offset) port.
        if (const char* off = std::getenv("BAMBU_BRIDGE_PORT_OFFSET");
            off && *off) {
            const int n = std::atoi(off);
            if (n > 0 && n < 1000) {
                cfg.mqtt_port_base = static_cast<uint16_t>(cfg.mqtt_port_base + n);
                cfg.ftps_port_base = static_cast<uint16_t>(cfg.ftps_port_base + n);
                cfg.rtsp_port_base = static_cast<uint16_t>(cfg.rtsp_port_base + n);
                cfg.vtun_port_base = static_cast<uint16_t>(cfg.vtun_port_base + n);
                std::fprintf(stderr,
                    "[bridge-gui] PORT_OFFSET=%d -> mqtt=%u ftps=%u rtsp=%u vtun=%u\n",
                    n, cfg.mqtt_port_base, cfg.ftps_port_base,
                    cfg.rtsp_port_base, cfg.vtun_port_base);
                std::fflush(stderr);
            }
        }

        // Slicer-identity fields baked into the storage tunnel URL.
        // Without these, libBambuSource refuses to advance
        // Bambu_StartStreamEx past would_block — observed during
        // virtual-storage testing. Use the same values the
        // slicer's own non-virtual MediaFilePanel path embeds.
        cfg.slicer_net_ver =
            Slic3r::NetworkAgent::get_version();
        cfg.slicer_cli_id  =
            app->app_config->get("slicer_uuid");
        cfg.slicer_cli_ver = std::string(SLIC3R_VERSION);

        // Storage delegator: bridge JSON-RPC -> PrinterFileSystem ->
        // libBambuSource. Constructed BEFORE the BridgeApp so we can
        // wire it through `attach_storage_delegate` right after.
        app->m_bridge_storage = std::make_unique<Slic3r::bridge::BridgeStorageBackend>();

        app->m_bridge_app = std::make_unique<Slic3r::bridge::headless::BridgeApp>(cfg);

        // Resolver for the slicer's VirtualMqttClient — see the
        // matching block in run_headless for the
        // rationale. Without this every virtual printer except the
        // one on port 8883 hits "auth fail" because the slicer
        // would otherwise dial 8883 for all of them.
        {
            auto* bridge_raw = app->m_bridge_app.get();
            Slic3r::VirtualMqttClient::instance().set_port_resolver(
                [bridge_raw](const std::string& dev_id) -> uint16_t {
                    return bridge_raw ? bridge_raw->mqtt_port_for_dev_id(dev_id) : 0;
                });
        }

        // Bind the backend to a std::function so the bridge module
        // doesn't need to link the GUI library (tests only pull in
        // bambu_bridge). The lambda captures the raw backend
        // pointer; lifetime is brokered by GUI_App owning both.
        // JSON is passed as string across the boundary because the
        // bridge and the GUI vendor different nlohmann/json
        // versions (different inline namespaces => different
        // mangled types in function signatures).
        auto* storage_raw = app->m_bridge_storage.get();
        app->m_bridge_app->attach_storage_delegate(
            [storage_raw](const std::string& real_dev_id,
                          const std::string& real_lan_ip,
                          const std::string& access_code,
                          const std::string& dev_ver,
                          const std::string& net_ver,
                          const std::string& cli_id,
                          const std::string& cli_ver,
                          int                cmdtype,
                          std::string        request_body_json,
                          std::function<void(int, std::string)> reply_cb) {
                if (!storage_raw) {
                    if (reply_cb) reply_cb(/*ERROR_PIPE*/3, "{}");
                    return;
                }
                nlohmann::json req_body;
                try {
                    req_body = request_body_json.empty()
                        ? nlohmann::json::object()
                        : nlohmann::json::parse(request_body_json);
                } catch (const std::exception& ex) {
                    BOOST_LOG_TRIVIAL(error)
                        << "bridge-storage: bad request JSON: "
                        << ex.what();
                    if (reply_cb) reply_cb(/*ERROR_JSON*/2, "{}");
                    return;
                }
                // Re-wrap the reply callback so it serializes back
                // to JSON for the wire.
                auto wrapped_cb = [reply_cb = std::move(reply_cb)]
                                  (int rc, nlohmann::json reply) {
                    if (!reply_cb) return;
                    reply_cb(rc, reply.dump());
                };
                storage_raw->send_request(
                    real_dev_id, real_lan_ip, access_code,
                    dev_ver, net_ver, cli_id, cli_ver,
                    cmdtype, std::move(req_body),
                    std::move(wrapped_cb));
            },
            // Release callback: hop to the wx main thread (PFS'
            // wxEvtHandler unwind needs to happen there).
            [app](const std::string& dev_id) {
                if (!app->m_bridge_storage) return;
                auto* backend = app->m_bridge_storage.get();
                app->CallAfter([backend, dev_id] { backend->release(dev_id); });
            });

        // Hand the bridge an adapter wrapping the slicer's
        // NetworkAgent. This is what flips the in-GUI bridge from
        // SSDP-advertise-only to a real MQTT/cloud proxy — the
        // CloudUplink / LanUplink see a non-null plugin handle
        // (whose is_user_login() / is_server_connected() track
        // the slicer's session) and stop dropping traffic with
        // "no healthy uplink".
        //
        // Must happen BEFORE run() is invoked on the worker
        // thread because BridgeApp::initialise() reads the
        // injected handle exactly once at startup.
        if (app->m_agent) {
            auto adapter =
                std::make_shared<Slic3r::NetworkAgentPluginAdapter>(app->m_agent);
            install_print_dispatcher_resolver(*adapter, app);
            install_mtls_resolver(*adapter, app->m_bridge_app.get());
            app->m_bridge_app->attach_plugin_handle(std::move(adapter));
        }

        // Camera-URL resolver: route CloudCameraSource::open()'s
        // get_camera_url through the slicer's live NetworkAgent so the
        // bridge picks up the host's existing cloud session + TUTK
        // token. Without this, BridgeApp's plugin-adapter path works
        // for MQTT/cloud control but the camera ask hit a separate
        // (token-less) plugin instance and returned an empty URL,
        // leaving the transcoded RTSP stream black on H2S/H2D.
        // NetworkAgent::get_camera_url takes `dev_id`; CloudCameraSource
        // builds an `<id>|<dev_ver>|<protocols>` triple — the agent
        // hands the entire `ask` straight to the plugin, so the triple
        // is preserved end-to-end.
        if (app->m_agent) {
            auto* agent = app->m_agent;  // raw pointer, owned by GUI_App
            app->m_bridge_app->set_camera_url_resolver(
                [agent](const std::string& ask,
                        std::function<void(std::string)> cb) -> int {
                    return agent->get_camera_url(ask, std::move(cb));
                });
        }

        app->m_bridge_thread = std::make_unique<std::thread>([app]() {
            try {
                const int rc = app->m_bridge_app->run();
                BOOST_LOG_TRIVIAL(info)
                    << "Bambu Bridge worker exited rc=" << rc;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error)
                    << "Bambu Bridge worker crashed: " << e.what();
            }
        });

        // Push pump: every 5 s, snapshot the user's cloud-bound
        // machines from DeviceManager and feed them to the bridge.
        // Runs on the wx main thread, which is the same thread that
        // mutates m_device_manager — so iterating its map and
        // touching MachineObject fields here is race-free.
        app->m_bridge_push_timer = std::make_unique<wxTimer>(app);
        app->Bind(wxEVT_TIMER, [app](wxTimerEvent&) {
            if (!app->m_bridge_app)     return;
            if (!app->m_device_manager) return;
            // Skip pushes while the slicer isn't logged in. DeviceManager
            // can briefly return an empty user_machinelist around login
            // boundaries (cloud refresh races, logout, etc.) and
            // `set_virtual_printers([])` interprets that as "remove every
            // device" — which kills the bridge's MQTT/FTPS/vtun listeners
            // and leaves the slicer's own VirtualMqttClient hitting a
            // closed port a moment later.
            if (!app->m_agent || !app->m_agent->is_user_login()) return;
            std::vector<Slic3r::bridge::headless::VirtualPrinter> snap;
            for (const auto& kv : app->m_device_manager->get_user_machinelist()) {
                MachineObject* mo = kv.second;
                if (!mo) continue;
                std::string id = mo->get_dev_id();
                if (id.empty()) continue;
                Slic3r::bridge::headless::VirtualPrinter p;
                p.dev_id      = std::move(id);
                p.dev_name    = mo->get_dev_name();
                p.lan_ip      = mo->get_dev_ip();      // empty for cloud-only
                p.access_code = mo->get_access_code();
                p.model       = mo->printer_type;
                // BambuTunnel storage URL requires `dev_ver` (the
                // printer's OTA firmware version) to be non-empty
                // — without it the printer-side never sends its
                // first frame and bambu_start_stream_ex spins on
                // would_block forever. Push the live value.
                p.firmware    = mo->get_ota_version();
                // Resolve the live-view URL via build_media_live_url here too
                // (add_device_locked wires the camera ONCE, from whichever
                // snapshot adds the device first — so camera_url must be set
                // on both paths or the cascade-added device picks cloud). Same
                // synchronous LAN-direct capture as the push-timer snapshot.
                // Capture via shared_ptr, NOT [&p]: build_media_live_url's
                // TUTK/remote branch calls agent->get_camera_url() which fires
                // its callback ASYNCHRONOUSLY (HTTP fetch) — long after `p` is
                // moved into `snap` and destroyed. A [&p] capture there is a
                // use-after-free (segfault / heap corruption). The shared_ptr
                // outlives both; we read the SYNC (LAN-direct) result right
                // after the call, and any late async write lands harmlessly.
                auto cam_url = std::make_shared<std::string>();
                Slic3r::GUI::build_media_live_url(mo,
                    [cam_url](std::string url, Slic3r::GUI::MediaUrlError err) {
                        if (err == Slic3r::GUI::MediaUrlError::Ok)
                            *cam_url = std::move(url);
                    });
                p.camera_url = *cam_url;
                snap.push_back(std::move(p));
            }
            // Same logic at the snapshot level: an empty list from a
            // logged-in DeviceManager is almost always a transient
            // refresh, not a real "all printers gone" event.
            if (snap.empty()) return;
            app->m_bridge_app->set_virtual_printers(std::move(snap));
        }, app->m_bridge_push_timer->GetId());
        app->m_bridge_push_timer->Start(5000);

        // Periodic cloud-session refresh — bridge children only.
        //
        // Long-running observation: after 4–6 hours the H2D/H2S bridge
        // child wedges. The MQTT broker thread stops reading from the
        // slicer's TCP socket (388-byte SUBSCRIBE packet left unread),
        // the per-child log goes silent except for camera-retry noise,
        // thread count climbs to ~175 from cumulative leaks, and the
        // child no longer registers new slicer subscriptions even
        // though it still accepts the TCP connect. Root cause is the
        // proprietary plugin's cloud cert / subscription going stale —
        // the stock GUI refreshes that implicitly every time the user
        // clicks Home → Prepare → Device, but the headless bridge child
        // never re-clicks. After a stale window the plugin's reactor
        // and the bridge's broker thread end up deadlocked on some
        // shared lock and the child never recovers.
        //
        // The fix: re-run the same PROBE sequence the one-shot startup
        // cascade ran, every 60 s. get_user_print_info refreshes the
        // HTTP session cookie; parse_user_print_info refreshes the
        // DeviceManager map; load_last_machine re-asserts the bridge's
        // pinned dev_id; add_subscribe re-tells the cloud broker we
        // care about events for this dev_id. Each is a thin plugin
        // call that the stock GUI fires whenever the user navigates,
        // so re-issuing them periodically is exactly the "click Home
        // then Device" the user asked for, just dispatched from a
        // timer instead of a wxEvent.
        //
        // Gated on BAMBU_BRIDGE_TARGET_DEV — a regular slicer doesn't
        // need this; the user clicking around the UI naturally
        // refreshes the session.
        if (const char* td = std::getenv("BAMBU_BRIDGE_TARGET_DEV");
            td && *td) {
            // Capture the target dev_id (per child it's a single
            // printer; the launcher passes one comma-separated entry).
            std::string target_dev = td;
            if (auto c = target_dev.find(','); c != std::string::npos)
                target_dev.resize(c);

            // ---- Fast-path graceful shutdown -----------------------------
            //
            // Why this exists. The launcher's `stop` issues a plain
            // SIGTERM then waits 3 s before SIGKILL. The natural
            // BambuStudio shutdown path (SIGTERM → wxApp event loop →
            // OnExit → BridgeBootstrap::shutdown_hooks → BridgeApp::
            // shutdown → ~LanUplink → disconnect_printer) takes longer
            // than that window because wxApp tears down GUI widgets,
            // joins many threads, and the bridge has hot retry loops
            // (camera-fanout, cloud reconnect) that don't yield
            // promptly. So we routinely got SIGKILL'd before the LAN
            // MQTT slot was released, leaving the printer holding the
            // zombie session for 2–5 minutes — exactly the "bridge
            // doesn't come back after a restart" symptom.
            //
            // Solution: signal handler that just sets an atomic flag.
            // A dedicated polling thread observes the flag and runs
            // the critical disconnect inline (NOT through wxApp's
            // event loop), then `_Exit(0)`s without touching wxApp.
            //
            // disconnect_printer takes ~50 ms in normal cases; we add
            // 500 ms TCP-FIN flush time, so total is < 1 s — well
            // within the launcher's 3 s window. If the plugin's
            // disconnect_printer ever hangs, the launcher's SIGKILL
            // still fires; the previous-generation behaviour. No
            // worse than before.
            static std::atomic<bool> g_bridge_shutdown_requested{false};
            // Register signal handler ONCE per process. Bridge child
            // is a single process; idempotent if we ever get re-init'd.
            static std::atomic<bool> g_handler_installed{false};
            if (!g_handler_installed.exchange(true)) {
                auto handler = +[](int) {
                    // Async-signal-safe: just flip the atomic. The
                    // polling thread does the heavy lifting.
                    g_bridge_shutdown_requested.store(true);
                };
                ::signal(SIGTERM, handler);
                ::signal(SIGINT,  handler);
                ::signal(SIGHUP,  handler);
                std::fprintf(stderr,
                    "[bridge-shutdown] handler installed for SIGTERM/SIGINT/SIGHUP\n");
                std::fflush(stderr);
            }
            std::thread([app, target_dev]{
                while (!g_bridge_shutdown_requested.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                std::fprintf(stderr,
                    "[bridge-shutdown] dev=%s signal caught, "
                    "disconnecting before exit\n",
                    target_dev.c_str());
                std::fflush(stderr);

                // 1. Release the printer's LAN MQTT slot. THIS is the
                //    critical step the launcher's 3 s window often
                //    fails to fit through under the wxApp path.
                if (app && app->m_agent) {
                    int rc = app->m_agent->disconnect_printer();
                    std::fprintf(stderr,
                        "[bridge-shutdown] disconnect_printer rc=%d\n", rc);
                    std::fflush(stderr);
                }

                // 2. Let the TCP FIN packets flush to the printer's
                //    broker AND to the bridge's connected slicers.
                //    500 ms is enough — empirically the printer
                //    releases the slot ~200 ms after the FIN.
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

                std::fprintf(stderr,
                    "[bridge-shutdown] dev=%s exiting (_Exit 0)\n",
                    target_dev.c_str());
                std::fflush(stderr);
                // _Exit bypasses static dtors + wxApp's slow shutdown.
                // Anything not flushed by now wasn't going to be.
                std::_Exit(0);
            }).detach();

            // Start-of-life timestamp for the "stale > 5 min" self-
            // restart gate.
            static const auto s_bridge_start =
                std::chrono::steady_clock::now();
            // Last-seen-pushall timestamp, updated by a snoop on
            // DeviceManager's per-machine m_push_count. Atomic so the
            // worker thread + timer thread can read/write without a
            // lock.
            static std::atomic<long long> s_last_push_count{0};
            static std::atomic<std::chrono::steady_clock::time_point>
                s_last_push_at{std::chrono::steady_clock::now()};

            // *** Originally a wxTimer; empirically the bridge child's
            // invisible-GUI wx event loop doesn't pump wxEVT_TIMER
            // reliably (a 5 s push_timer fired ~once/min, our 60 s
            // refresh never fired). Replaced with a plain std::thread
            // so the watchdog and the self-restart gate actually run.
            std::thread([app, target_dev]() {
                using clock = std::chrono::steady_clock;
                std::fprintf(stderr,
                    "[bridge-refresh] worker thread started dev=%s\n",
                    target_dev.c_str());
                std::fflush(stderr);
                while (true) {
                    std::this_thread::sleep_for(std::chrono::seconds(60));
                    // Run the tick body. Mirrors the wxTimer lambda
                    // we replaced.
                    [&] {
                using clock = std::chrono::steady_clock;
                const auto now    = clock::now();
                const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                                       now - s_bridge_start).count();

                // ----- A. Health probes --------------------------------------
                const int thread_count = [] {
                    DIR* d = ::opendir("/proc/self/task");
                    if (!d) return -1;
                    int n = 0;
                    while (auto* e = ::readdir(d)) {
                        if (e->d_name[0] != '.') ++n;
                    }
                    ::closedir(d);
                    return n;
                }();

                // Sample push_count for the bridge's target printer
                // (updated by DeviceManager when /report msg=0 arrives).
                long long current_push = 0;
                if (app->m_device_manager) {
                    if (auto* mo = app->m_device_manager->get_my_machine(target_dev)) {
                        current_push = mo->m_push_count;
                    }
                }
                if (current_push > s_last_push_count.load()) {
                    s_last_push_count.store(current_push);
                    s_last_push_at.store(now);
                }
                const auto stale_secs =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        now - s_last_push_at.load()).count();

                const bool user_login    = app->m_agent
                                           && app->m_agent->is_user_login();
                const bool srv_connected = app->m_agent
                                           && app->m_agent->is_server_connected();

                std::fprintf(stderr,
                    "[bridge-refresh] tick dev=%s uptime=%llds threads=%d "
                    "push_count=%lld stale=%llds user_login=%d server_connected=%d\n",
                    target_dev.c_str(), (long long) uptime,
                    thread_count, current_push,
                    (long long) stale_secs,
                    int(user_login), int(srv_connected));
                std::fflush(stderr);

                // ----- B. Self-restart on hard-stuck conditions -------------
                // Threshold-based: thread leak past 250 OR no pushall for
                // 180 s AND we've been up at least 90 s. Earlier ship
                // had 300 s, but empirically the printer's LAN MQTT slot
                // gets grabbed by Handy mid-print and the plugin's
                // recovery is best-effort at best — we'd rather restart
                // a notch sooner than ride out an indefinite stall in a
                // camera-retry hot loop.
                if (uptime > 90 && (thread_count > 250 || stale_secs > 180)) {
                    std::fprintf(stderr,
                        "[bridge-refresh] SELF-RESTART dev=%s reason=%s "
                        "uptime=%llds threads=%d stale=%llds — "
                        "bridge-multiproc launcher will respawn\n",
                        target_dev.c_str(),
                        (thread_count > 250 ? "thread-leak" : "no-pushall"),
                        (long long) uptime, thread_count, (long long) stale_secs);
                    std::fflush(stderr);
                    // exit(0) so the supervisor respawns us cleanly.
                    std::_Exit(0);
                }

                // ----- C. Active reconnect when MQTT looks dead -------------
                // If we're past 60 s without a pushall AND we've been up
                // longer than that (so we don't fire during the initial
                // login lull), kick the plugin: re-issue add_subscribe for
                // our dev_id. That's the canonical primitive the GUI's
                // device-tab click triggers; nothing else in the plugin
                // re-asserts subscription state once it's been lost (e.g.
                // by a Handy app grabbing the printer's single LAN MQTT
                // slot or a TUTK-camera teardown). Cheap to over-call —
                // add_subscribe is idempotent on the plugin side.
                //
                // CRITICAL: run on a detached worker thread, not on the
                // watchdog thread. add_subscribe goes through the
                // proprietary plugin's MQTT reactor and EMPIRICALLY
                // blocks when the cloud session is wedged — last ship
                // ran KICK inline and the watchdog thread hung at
                // uptime=360s, never reaching the self-restart gate.
                // The worker thread can hang forever; the watchdog
                // keeps ticking and eventually self-restarts.
                if (uptime > 90 && stale_secs > 60
                    && app->m_agent && user_login) {
                    NetworkAgent* agent = app->m_agent;
                    std::string dev_id = target_dev;
                    long long stale_at_dispatch = (long long) stale_secs;
                    std::thread([agent, dev_id, stale_at_dispatch]{
                        std::vector<std::string> subs{ dev_id };
                        std::fprintf(stderr,
                            "[bridge-refresh] KICK add_subscribe(%s) "
                            "DISPATCHING (no pushall for %llds)\n",
                            dev_id.c_str(), stale_at_dispatch);
                        std::fflush(stderr);
                        int sub_rc = agent->add_subscribe(subs);
                        std::fprintf(stderr,
                            "[bridge-refresh] KICK add_subscribe(%s) rc=%d\n",
                            dev_id.c_str(), sub_rc);
                        std::fflush(stderr);
                    }).detach();
                }

                // ----- D. Normal session refresh ----------------------------
                if (!user_login || !srv_connected) return;
                std::thread([app]{
                    unsigned int http = 0;
                    std::string  body;
                    int rc = app->m_agent->get_user_print_info(&http, &body);
                    std::fprintf(stderr,
                        "[bridge-refresh] get_user_print_info rc=%d http=%u "
                        "body_len=%zu\n", rc, http, body.size());
                    std::fflush(stderr);
                    if (rc != 0 || body.empty()) return;

                    std::string body_copy = body;
                    app->CallAfter([app, body_copy = std::move(body_copy)]() mutable {
                        if (!app->m_device_manager) return;
                        try {
                            app->m_device_manager->parse_user_print_info(body_copy);
                            app->m_device_manager->load_last_machine();
                            auto* sel = app->m_device_manager->get_selected_machine();
                            if (sel && app->m_agent) {
                                std::vector<std::string> subs{ sel->get_dev_id() };
                                int sub_rc = app->m_agent->add_subscribe(subs);
                                std::fprintf(stderr,
                                    "[bridge-refresh] add_subscribe(%s) rc=%d\n",
                                    sel->get_dev_id().c_str(), sub_rc);
                                std::fflush(stderr);
                            }
                        } catch (const std::exception& ex) {
                            std::fprintf(stderr,
                                "[bridge-refresh] parse_user_print_info threw: %s\n",
                                ex.what());
                            std::fflush(stderr);
                        }
                    });
                }).detach();
                    }();
                }
            }).detach();
            std::fprintf(stderr,
                "[bridge-refresh] periodic cloud-session refresh + "
                "watchdog + self-restart armed (every 60s, std::thread) "
                "[targets: thread>250 || pushall-stale>300s self-restart, "
                "pushall-stale>60s kick add_subscribe]\n");
            std::fflush(stderr);
        }

        BOOST_LOG_TRIVIAL(info)
            << "Bambu Bridge started in GUI worker thread "
               "(DeviceManager push every 5s); set "
               "BAMBU_BRIDGE_GUI_DISABLED=1 to skip.";

        // The GUI normally only triggers the post-login cascade
        // (connect_server → update_user_machine_list_info →
        // parse_user_print_info → populate DeviceManager) when the
        // OAuth flow completes interactively. When BambuStudio comes
        // up with cached tokens (the usual case for headless / xvfb
        // bridge launches), nobody fires that event, so the bridge
        // sits with an empty DeviceManager and the slicer's own UI
        // shows only the previously-cached "last selected" printer.
        // Force the post-login cascade once we have a session — either
        // loaded from the native bridge session file (ship-2 native
        // path) or via the plugin's cached-login (legacy fallback for
        // users without a bridge session file yet).
        //
        // The per-device plugin work (add_subscribe + set_user_selected
        // _machine + install_device_cert + the post-LAN cycle) still
        // needs the plugin's agent — ship-2 only replaces the SESSION +
        // DEVICE-LIST steps, not the enc_msg gate cycle. So both paths
        // converge to feed the plugin the user_machinelist; the bridge
        // then trusts the GUI's natural set_on_server_connected_fn /
        // set_on_printer_connected_fn callbacks (installed by
        // GUI_App::init_networking_callbacks, identical to the stock
        // GUI) to drive set_user_selected_machine + command_request_push_all
        // / get_version / get_access_code / install_device_cert exactly
        // as the GUI does on an interactive Device-tab click.
        //
        // Earlier revisions ran an explicit `finish_cascade` here that
        // synthesised a per-printer cert_report retry cycle + a
        // device-tab-sim + a proof-of-life probe. None of that exists
        // in the stock GUI; empirically the proprietary plugin's
        // send_message refuses publishes that originate from a non-GUI-
        // driven code path with rc_cloud=-2 regardless of how aggressive
        // the synthesised retries are. Removing the simulation lets the
        // plugin's natural state machine run unmolested. See
        // experiment/identical-to-gui branch rationale.
        if (app->m_agent) {
            auto finish_cascade = [] {
            };

            std::thread([app, finish_cascade]{
                using namespace std::chrono_literals;
                const auto t0 = std::chrono::steady_clock::now();

                // -- Ship-2: native session path --------------------------
                // Try loading + refreshing the bridge's own plaintext
                // session file at ~/.config/BambuBridge/session.json.
                // If that succeeds AND we can fetch the device list via
                // the cloud REST API, hand the body to
                // DeviceManager::parse_user_print_info() ourselves and
                // skip the plugin's request_user_handle round-trip.
                bool native_used = false;
                {
                    Slic3r::bridge::CloudSession sess;
                    auto load_rc = sess.load_and_refresh_if_needed(/*slack_seconds=*/300);
                    if (load_rc.ok) {
                        Slic3r::bridge::CloudDeviceList lister;
                        auto dl = lister.fetch(load_rc.data);
                        if (dl.ok && !dl.body.empty()) {
                            std::fprintf(stderr,
                                "[bridge-gui] cache_session_used=true "
                                "refreshed=%d uid=%lld region=%s "
                                "device_list_http=%ld body_len=%zu\n",
                                int(load_rc.refreshed),
                                (long long) load_rc.data.uid,
                                load_rc.data.region.c_str(),
                                dl.http_status, dl.body.size());
                            std::fflush(stderr);

                            // Plugin's agent still owns MQTT bringup +
                            // enc_msg cert flow. Bring its cached login
                            // online so install_device_cert works, but
                            // we DON'T wait on update_user_machine_list
                            // _info — we feed the list ourselves.
                            if (app->m_agent && app->m_agent->is_user_login()) {
                                // (legacy plugin already loaded cached
                                // tokens — no extra wakeup needed)
                            }
                            // Feed the body into DeviceManager on the
                            // wx main thread (touches DeviceManager
                            // mutable state that the timer also pokes).
                            std::string body = std::move(dl.body);
                            app->CallAfter([app, body = std::move(body)]() mutable {
                                if (!app->m_device_manager) return;
                                try {
                                    app->m_device_manager->parse_user_print_info(body);
                                    std::fprintf(stderr,
                                        "[bridge-gui] native parse_user_print_info"
                                        " ok, list size=%zu\n",
                                        app->m_device_manager
                                            ->get_user_machinelist().size());
                                    std::fflush(stderr);
                                } catch (const std::exception& ex) {
                                    std::fprintf(stderr,
                                        "[bridge-gui] native parse_user_print_info"
                                        " threw: %s\n", ex.what());
                                    std::fflush(stderr);
                                }
                            });
                            native_used = true;
                        } else {
                            std::fprintf(stderr,
                                "[bridge-gui] native session loaded but "
                                "device-list fetch failed: status=%ld err=%s; "
                                "falling back to plugin path\n",
                                dl.http_status, dl.error.c_str());
                            std::fflush(stderr);
                        }
                    } else {
                        std::fprintf(stderr,
                            "[bridge-gui] cache_session_used=false "
                            "reason=%s; falling back to plugin path\n",
                            load_rc.error.c_str());
                        std::fflush(stderr);
                    }
                }

                // -- Plugin fallback path (legacy) ------------------------
                if (!native_used) {
                    bool login_fired = false;
                    for (int i = 0; i < 60; ++i) {
                        if (app->m_agent && app->m_agent->is_user_login()) {
                            app->CallAfter([app]{
                                std::fprintf(stderr,
                                    "[bridge-gui] firing request_user_login_handle"
                                    " for cached login (plugin fallback)\n");
                                std::fflush(stderr);
                                app->request_user_handle(1);
                            });
                            login_fired = true;
                            break;
                        }
                        std::this_thread::sleep_for(500ms);
                    }
                    if (!login_fired) {
                        std::fprintf(stderr,
                            "[bridge-gui] timed out waiting for is_user_login\n");
                        std::fflush(stderr);
                        return;
                    }
                }

                // Common: run the per-device cascade once the list lands.
                finish_cascade();
                const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - t0).count();
                std::fprintf(stderr,
                    "[bridge-gui] cascade total elapsed=%lld ms path=%s\n",
                    (long long) dt_ms, native_used ? "native" : "plugin-fallback");
                std::fflush(stderr);

                // PROBE: explicitly call get_user_print_info (the HTTP
                // endpoint backing update_user_machine_list_info) so we
                // can see whether the plugin's cookie/session auth
                // works for HTTP even when get_my_token can't surface
                // bearer tokens. If this returns rc=0 with non-empty
                // body, the auth state IS workable — we just need to
                // drive load_last_machine ourselves rather than relying
                // on TryLoadFromHttpCB firing through the natural flow.
                if (app->m_agent) {
                    unsigned int probe_http = 0;
                    std::string  probe_body;
                    int probe_rc = app->m_agent->get_user_print_info(
                        &probe_http, &probe_body);
                    std::fprintf(stderr,
                        "[bridge-gui] PROBE get_user_print_info "
                        "rc=%d http=%u body_len=%zu body_head=%.120s\n",
                        probe_rc, probe_http, probe_body.size(),
                        probe_body.c_str());
                    std::fflush(stderr);
                    // If the probe gave us the device list, manually
                    // drive what TryLoadFromHttpCB would have done: feed
                    // DeviceManager + call load_last_machine which uses
                    // our user_last_selected_machine pin to pick the
                    // target dev_id and call set_selected_machine →
                    // m_agent->set_user_selected_machine → plugin
                    // subscribes → set_on_printer_connected_fn fires.
                    if (probe_rc == 0 && !probe_body.empty()) {
                        // Wait for is_server_connected before doing
                        // load_last_machine + add_subscribe — set_on_server
                        // _connected_fn fires from the plugin's cloud-MQTT
                        // broker SUBACK and is what makes send_message
                        // actually work. Without the wait we end up calling
                        // add_subscribe before the cloud session is alive
                        // and every subsequent publish gets rc_cloud=-2
                        // server=0 even though the slicer-side bootstrap
                        // (pushall/get_version) has already fired.
                        for (int i = 0; i < 60; ++i) {
                            if (app->m_agent && app->m_agent->is_server_connected())
                                break;
                            std::this_thread::sleep_for(500ms);
                        }
                        std::fprintf(stderr,
                            "[bridge-gui] PROBE is_server_connected=%d "
                            "(after wait)\n",
                            app->m_agent ?
                                int(app->m_agent->is_server_connected()) : -1);
                        std::fflush(stderr);
                        std::string body_copy = probe_body;
                        app->CallAfter([app, body_copy = std::move(body_copy)]() mutable {
                            if (!app->m_device_manager) return;
                            try {
                                app->m_device_manager->parse_user_print_info(body_copy);
                                std::fprintf(stderr,
                                    "[bridge-gui] PROBE parse_user_print_info OK "
                                    "userMachineList.size=%zu\n",
                                    app->m_device_manager->get_user_machinelist().size());
                                app->m_device_manager->load_last_machine();
                                auto* sel = app->m_device_manager->get_selected_machine();
                                std::fprintf(stderr,
                                    "[bridge-gui] PROBE load_last_machine fired; "
                                    "selected=%s\n",
                                    sel ? sel->get_dev_id().c_str() : "<none>");
                                // GUI's set_on_server_connected_fn handler
                                // explicitly calls subscribe_device_list /
                                // add_subscribe in multi-machine mode; the
                                // cascade we removed was doing the same thing
                                // for single-machine bridge mode. Restore JUST
                                // that one plugin call: add_subscribe for the
                                // selected dev_id. set_user_selected_machine
                                // alone isn't sufficient — the plugin needs
                                // an explicit subscribe request for the cloud
                                // broker to deliver SUBACK and fire
                                // set_on_printer_connected_fn.
                                if (sel && app->m_agent) {
                                    std::vector<std::string> subs{ sel->get_dev_id() };
                                    int sub_rc = app->m_agent->add_subscribe(subs);
                                    std::fprintf(stderr,
                                        "[bridge-gui] PROBE add_subscribe(%s) rc=%d\n",
                                        sel->get_dev_id().c_str(), sub_rc);
                                }
                                std::fflush(stderr);
                            } catch (const std::exception& ex) {
                                std::fprintf(stderr,
                                    "[bridge-gui] PROBE parse threw: %s\n", ex.what());
                                std::fflush(stderr);
                            }
                        });
                    }
                }

                // -- Ship-2: persist the resulting session for next boot --
                // After the plugin-fallback path succeeds we want next
                // boot to use the native session. Try
                // bambu_network_get_my_token (empty ticket) — per RE
                // doc §5 it returns the in-memory access_token. If we
                // can extract a non-empty token, save it to the bridge
                // session file (refresh_token + expiry are not exposed
                // by the plugin export surface, so we save what we have
                // and let the refresh round-trip top up expiry on next
                // boot).
                //
                // POLL: the plugin's login HTTP roundtrip (initiated by
                // request_user_handle) takes seconds to complete.
                // Calling get_my_token immediately gets rc=-1 because
                // the in-memory token slot isn't populated yet. The
                // pre-cascade-removal code accidentally papered over
                // this with its 30 s of synthetic GUI-event sleeps;
                // now that those are gone we have to wait deliberately.
                // Poll every 1 s for up to 30 s — same total budget
                // the removed cascade consumed, just spent on waiting
                // rather than spamming set_user_selected_machine.
                if (!native_used && app->m_agent && app->m_agent->is_user_login()) {
                    unsigned int http = 0;
                    std::string  body;
                    int rc = -1;
                    for (int i = 0; i < 30; ++i) {
                        body.clear(); http = 0;
                        rc = app->m_agent->get_my_token(
                            /*ticket=*/std::string(), &http, &body);
                        if (rc == 0 && !body.empty()) break;
                        std::this_thread::sleep_for(1000ms);
                    }
                    std::fprintf(stderr,
                        "[bridge-gui] post-fallback get_my_token rc=%d http=%u "
                        "body_len=%zu\n", rc, http, body.size());
                    std::fflush(stderr);

                    Slic3r::bridge::CloudSessionData d;
                    d.region     = Slic3r::bridge::region_for_bridge();
                    d.user_email = app->m_agent->get_user_name();
                    try {
                        auto uid_str = app->m_agent->get_user_id();
                        if (!uid_str.empty()) d.uid = std::stoll(uid_str);
                    } catch (...) {}

                    if (rc == 0 && !body.empty()) {
                        try {
                            auto j = nlohmann::json::parse(body);
                            auto find_str = [&](const char* k) -> std::string {
                                auto it = j.find(k);
                                if (it == j.end() || it->is_null()) return {};
                                if (it->is_string()) return it->get<std::string>();
                                return {};
                            };
                            auto find_int = [&](const char* k) -> int64_t {
                                auto it = j.find(k);
                                if (it == j.end() || it->is_null()) return 0;
                                if (it->is_number()) return (int64_t) it->get<double>();
                                if (it->is_string()) {
                                    try { return std::stoll(it->get<std::string>()); }
                                    catch (...) { return 0; }
                                }
                                return 0;
                            };
                            d.access_token  = find_str("accessToken");
                            if (d.access_token.empty()) d.access_token = find_str("access_token");
                            d.refresh_token = find_str("refreshToken");
                            if (d.refresh_token.empty()) d.refresh_token = find_str("refresh_token");
                            int64_t ei = find_int("expiresIn");
                            int64_t re = find_int("refreshExpiresIn");
                            if (ei > 0) d.access_expires_at =
                                std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count() + ei;
                            if (re > 0) d.refresh_expires_at =
                                std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count() + re;
                        } catch (const std::exception& ex) {
                            std::fprintf(stderr,
                                "[bridge-gui] get_my_token body not JSON: %s\n",
                                ex.what());
                            std::fflush(stderr);
                        }
                    }
                    if (!d.access_token.empty() && !d.refresh_token.empty()) {
                        Slic3r::bridge::CloudSession().save(d);
                        std::fprintf(stderr,
                            "[bridge-gui] persisted native session after "
                            "plugin fallback; next boot will use native path\n");
                        std::fflush(stderr);
                    } else {
                        // LOUD: this is the silent killer. The plugin
                        // logs in OK and reports is_user_login=1 /
                        // is_server_connected=1, but bambu_network_send_message
                        // ALSO needs a native access_token to be co-present
                        // in BambuStudio.conf — without it every cloud
                        // publish returns rc_cloud=-2 and slicers see
                        // "Failed to connect" with no visible bridge error.
                        // See project_bridge_cloud_tunnel memory.
                        std::fprintf(stderr,
                            "\n"
                            "============================================================\n"
                            "  ⚠ BRIDGE WILL NOT RELAY CLOUD PUBLISHES UNTIL LOGIN\n"
                            "------------------------------------------------------------\n"
                            "  [bridge-gui] plugin did not surface usable tokens via\n"
                            "  get_my_token; native session NOT persisted\n"
                            "  (access=%s refresh=%s).\n"
                            "\n"
                            "  Symptom you will see in slicers:\n"
                            "    'Failed to connect' / spinner stuck on Connecting.\n"
                            "  Symptom you will see in this log:\n"
                            "    [adapter] send_message ... rc_cloud=-2 rc_lan=-4\n"
                            "    even though login=1 server=1.\n"
                            "\n"
                            "  How to fix:\n"
                            "    1. Open the slicer GUI on this machine (Linux\n"
                            "       BambuStudio), Account → Log in. The slicer\n"
                            "       writes access_token+refresh_token into:\n"
                            "         ~/.config/BambuStudio/BambuStudio.conf\n"
                            "    2. Restart the bridge.\n"
                            "    3. This warning should disappear.\n"
                            "============================================================\n",
                            d.access_token.empty() ? "empty" : "present",
                            d.refresh_token.empty() ? "empty" : "present");
                        std::fflush(stderr);
                    }
                }
            }).detach();
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error)
            << "Failed to start Bambu Bridge: " << e.what();
    }
}

// ============================================================================
// rehydrate_virtual_lan_printers — replay persisted FFFF printers.
//
// Re-hydrate any virtual LAN printers the user has previously added so
// they don't have to re-enter the access code every session. The store
// only contains FFFF-prefix dev_ids; real LAN printers keep their
// existing access_code-only persistence in app_config. See
// VirtualLanPrinterStore.hpp for the format.
// ============================================================================

void rehydrate_virtual_lan_printers(GUI_App* app)
{
    if (!app || !app->m_device_manager)
        return;
    Slic3r::VirtualLanPrinterStore store;
    for (const auto& e : store.load()) {
        if (!Slic3r::NetworkAgent::is_virtual_dev_id(e.dev_id))
            continue;
        auto* obj = app->m_device_manager->insert_local_device(
            e.dev_name, e.dev_id, e.lan_ip,
            /*connection_type=*/"lan",
            /*bind_state=*/"free",
            /*version=*/std::string(),
            e.access_code, e.printer_type);
        if (obj) obj->set_user_access_code(e.access_code);
    }
}

// ============================================================================
// install_networking_callbacks — bridge-only vs. full dispatch.
//
// Bridge-only mode uses a minimal callback set (just enough to keep
// DeviceManager fed via parse_json on every push_status, plus the
// CallAfter marshaller for queue_on_main_fn). Normal mode uses the
// full GUI_App::init_networking_callbacks with all dialog hooks.
// ============================================================================

void install_networking_callbacks(GUI_App* app)
{
    if (!g_bridge_only) {
        app->init_networking_callbacks();
        return;
    }

    // ---- bridge-only minimal callbacks (inlined here so this function's
    // friend grant covers the private-member access below). ----
    BOOST_LOG_TRIVIAL(info)
        << "install_networking_callbacks: enter (bridge-only)";
    if (!app || !app->m_agent) return;

    // Cloud-side push_status fanout. NetworkAgent::set_on_message_fn
    // wraps our fn with the BridgeMessageTap forwarder (set by
    // NetworkAgentPluginAdapter on the plugin handle), so as long as
    // SOME message_fn is installed in the plugin, the bridge will
    // see inbound printer reports. Inside the wrapper our fn runs
    // first; we use it to keep DeviceManager fresh (so the 5s push
    // pump has accurate VirtualPrinter entries). No plater / sidebar
    // / dialog work — those belong to the full GUI path.
    app->m_agent->set_on_message_fn([app](std::string dev_id, std::string msg) {
        // Proof-of-life observation — record any inbound for this dev_id
        // so the cascade's tail probe can confirm cloud tunnel liveness.
        // Must run unconditionally (i.e. even during shutdown) so we
        // don't accidentally drop a probe-reply.
        note_inbound(dev_id);
        if (app->is_closing()) return;
        app->CallAfter([app, dev_id, msg] {
            if (app->is_closing()) return;
            if (!app->m_device_manager) return;
            if (MachineObject* obj = app->m_device_manager->get_my_machine(dev_id)) {
                obj->parse_json("cloud", msg);
            }
        });
    });

    // Same idea for LAN push_status (real LAN printers, plus cloud
    // local-tunnelled). Bridge tap wraps this too.
    app->m_agent->set_on_local_message_fn([app](std::string dev_id, std::string msg) {
        note_inbound(dev_id); // see cloud branch above
        if (app->is_closing()) return;
        app->CallAfter([app, dev_id, msg] {
            if (app->is_closing()) return;
            if (!app->m_device_manager) return;
            if (MachineObject* obj = app->m_device_manager->get_my_machine(dev_id)) {
                obj->parse_json("lan", msg);
            }
        });
    });

    // The agent uses this to hop work back to the wx main thread
    // (e.g. tutk callbacks). CallAfter is the standard route.
    app->m_agent->set_queue_on_main_fn([app](std::function<void()> callback) {
        app->CallAfter(callback);
    });

    BOOST_LOG_TRIVIAL(info)
        << "install_networking_callbacks: exit (bridge-only)";
}

// ============================================================================
// shutdown_hooks — OnExit teardown.
//
// Drops the push timer, storage backend, BridgeApp, worker thread, and
// the VirtualMqttClient port resolver. Order matters: shut the storage
// backend down BEFORE BridgeApp tears its VirtualTunnelServer down,
// because the vtun holds a borrowed pointer to the backend and any
// in-flight reply callbacks need PFS' recv threads still around to
// finish unwinding.
//
// In --bridge-only mode also short-circuits with _Exit(0) after
// flushing stdio (see implementation for why; static-dtor double-free
// in the proprietary plugin).
// ============================================================================

void shutdown_hooks(GUI_App* app)
{
    if (!app) return;

    if (app->m_bridge_push_timer) {
        app->m_bridge_push_timer->Stop();
        app->m_bridge_push_timer.reset();
    }
    // Order matters: shut the storage backend down BEFORE BridgeApp
    // tears its VirtualTunnelServer down. The vtun holds a borrowed
    // pointer to the backend and any in-flight reply callbacks need
    // PFS' recv threads still around to finish unwinding.
    if (app->m_bridge_storage) {
        app->m_bridge_storage->shutdown();
    }
    if (app->m_bridge_app) {
        // Drop the resolver before BridgeApp dies; it captured a raw
        // pointer to m_bridge_app.
        Slic3r::VirtualMqttClient::instance().set_port_resolver(nullptr);
        app->m_bridge_app->shutdown();
    }
    if (app->m_bridge_thread && app->m_bridge_thread->joinable()) {
        app->m_bridge_thread->join();
    }
    // m_bridge_storage destructs with GUI_App; explicit reset would
    // run on the wx main thread anyway, so we let unique_ptr handle it.

    // In bridge-only mode the worker has reported rc=0 by this point;
    // the bridge servers are torn down, MQTT/FTPS/RTSP/vtun sockets
    // closed, LAN uplink disconnected, push pump stopped. Everything
    // the supervisor cares about is durably persisted (app_config
    // already saved above). The remaining work is just C++ destructor
    // unwind for ~50+ static objects across libslic3r/libBambuSource/
    // the network plugin — and at least one of those static dtors
    // double-frees somewhere, fatally aborting glibc malloc with
    // `mismatching next->prev_size`. Diagnosing the root cause needs
    // a debug build of the proprietary plugin we don't have source
    // for; in the meantime _Exit(0) gives systemd/runit/etc. the
    // clean exit code they want without exposing the crash.
    if (g_bridge_only) {
        std::fflush(stderr);
        std::fflush(stdout);
        std::_Exit(0);
    }
}

// ============================================================================
// on_filter_event — wxApp::FilterEvent delegate.
//
// Bridge-debug: log every left-mouse click + its target widget so a
// human-in-the-loop session can correlate the slicer's UI state with
// the bridge's MQTT relay activity. Returns -1 (let event continue).
// The body currently captures the widget context into locals but
// doesn't log them; kept as scaffolding for ad-hoc print-debug.
// ============================================================================

// Gated on BAMBU_BRIDGE_GUI_EVENT_TRACE=1 (separate from PLUGIN_TRACE
// because GUI event volume is high — wxEVT_PAINT, MOTION, IDLE fire
// constantly). Emits `[gui_event] HH:MM:SS.mmm tid=<n> <type> widget=…`
// so the operator's UI actions can be correlated with the [plugincall]
// stream by timestamp. Mirrors the [plugincall] line shape exactly so
// `sort` keeps them interleaved chronologically.
static bool gui_event_trace_enabled() {
    static const bool on = []{
        const char* e = std::getenv("BAMBU_BRIDGE_GUI_EVENT_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
}

// Whitelist: only event types that map cleanly to user actions in the
// PROTOCOL.md test sequence. Everything else (PAINT, MOTION, IDLE, etc.)
// is suppressed to keep the trace readable.
static const char* gui_event_name(wxEventType t) {
    if (t == wxEVT_LEFT_DOWN)               return "LEFT_DOWN";
    if (t == wxEVT_LEFT_DCLICK)             return "LEFT_DCLICK";
    if (t == wxEVT_BUTTON)                  return "BUTTON";
    if (t == wxEVT_TOGGLEBUTTON)            return "TOGGLEBUTTON";
    if (t == wxEVT_COMBOBOX)                return "COMBOBOX";
    if (t == wxEVT_CHOICE)                  return "CHOICE";
    if (t == wxEVT_NOTEBOOK_PAGE_CHANGED)   return "NOTEBOOK_PAGE_CHANGED";
    if (t == wxEVT_MENU)                    return "MENU";
    if (t == wxEVT_TOOL)                    return "TOOL";
    if (t == wxEVT_CHECKBOX)                return "CHECKBOX";
    if (t == wxEVT_RADIOBUTTON)             return "RADIOBUTTON";
    if (t == wxEVT_TEXT_ENTER)              return "TEXT_ENTER";
    return nullptr;
}

int on_filter_event(wxEvent& event)
{
    if (!gui_event_trace_enabled()) return -1;
    const wxEventType t = event.GetEventType();
    const char* tname = gui_event_name(t);
    if (!tname) return -1;

    wxObject* obj = event.GetEventObject();
    const auto* w = wxDynamicCast(obj, wxWindow);
    wxString label = w ? w->GetLabel() : wxString();
    wxString name  = w ? w->GetName()  : wxString();
    wxClassInfo* ci = w ? w->GetClassInfo() : nullptr;
    wxString klass = ci ? wxString(ci->GetClassName()) : wxString("?");
    wxPoint pos    = w ? w->GetScreenPosition() : wxPoint(-1, -1);
    wxSize  sz     = w ? w->GetSize()           : wxSize(0, 0);
    wxPoint mp(-1, -1);
    if (auto* me = wxDynamicCast(&event, wxMouseEvent))
        mp = me->GetPosition();

    // Match the [plugincall] line prefix so the two streams interleave
    // by timestamp. Distinguished from plugin calls by `[gui_event]`.
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count() % 1000;
    struct tm lt;
    localtime_r(&tt, &lt);
    char ts[16];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03lld",
        lt.tm_hour, lt.tm_min, lt.tm_sec, (long long) ms);
    unsigned long tid = (unsigned long) pthread_self() & 0xFFFFF;

    std::fprintf(stderr,
        "[gui_event] %s tid=%lu %s class=%s name=%s label=%s "
        "scr_pos=(%d,%d) widget_size=(%d,%d) click_xy=(%d,%d)\n",
        ts, tid, tname,
        std::string(klass.mb_str()).c_str(),
        std::string(name.mb_str()).c_str(),
        std::string(label.mb_str()).c_str(),
        pos.x, pos.y, sz.GetWidth(), sz.GetHeight(),
        mp.x, mp.y);
    std::fflush(stderr);
    return -1;
}

} // namespace BridgeBootstrap
}} // namespace Slic3r::GUI

#endif // BAMBU_BRIDGE
