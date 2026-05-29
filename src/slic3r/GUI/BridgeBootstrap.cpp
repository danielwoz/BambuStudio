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
#include <cstdlib>
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
        // converge into the same finish_cascade() helper below.
        if (app->m_agent) {
            // Inner helper: after the userMachineList is populated by
            // either path, run the per-dev plugin setup + post-LAN
            // cycle + probe. Captures `app` from the surrounding scope.
            auto finish_cascade = [app] {
                using namespace std::chrono_literals;
                for (int i = 0; i < 30; ++i) {
                    std::this_thread::sleep_for(1000ms);
                    if (!app->m_device_manager) continue;
                    auto list = app->m_device_manager->get_user_machinelist();
                    if (list.empty()) continue;
                    std::vector<std::string> dev_ids;
                    for (const auto& kv : list)
                        if (!kv.first.empty()) dev_ids.push_back(kv.first);
                    // Multi-process: when BAMBU_BRIDGE_TARGET_DEV pins this
                    // process to specific printer(s), restrict the cert
                    // cascade to those. Cycling non-served, non-LAN-connected
                    // dev_ids (set_user_selected_machine on a printer this
                    // process never connect_printer'd) churns the plugin's
                    // active-machine state and races the served printer's
                    // cert_report — leaving its device_pub_key_map empty.
                    if (const char* td = std::getenv("BAMBU_BRIDGE_TARGET_DEV");
                        td && *td) {
                        std::vector<std::string> keep;
                        const std::string s = td; size_t pos = 0;
                        while (pos <= s.size()) {
                            const size_t c = s.find(',', pos);
                            const size_t e = (c == std::string::npos) ? s.size() : c;
                            if (e > pos) keep.push_back(s.substr(pos, e - pos));
                            if (c == std::string::npos) break;
                            pos = c + 1;
                        }
                        dev_ids.erase(std::remove_if(dev_ids.begin(), dev_ids.end(),
                            [&keep](const std::string& d){
                                return std::find(keep.begin(), keep.end(), d) == keep.end();
                            }), dev_ids.end());
                    }
                    std::fprintf(stderr,
                        "[bridge-gui] cascade ready, %zu owned dev_ids; "
                        "wiring per-printer plugin setup\n",
                        dev_ids.size());
                    std::fflush(stderr);
                    auto* ag = app->m_agent;
                    if (!dev_ids.empty()) {
                        int add_rc = ag->add_subscribe(dev_ids);
                        std::fprintf(stderr,
                            "[bridge-gui] add_subscribe(%zu) rc=%d\n",
                            dev_ids.size(), add_rc);
                        std::fflush(stderr);
                    }
                    for (const auto& d : dev_ids) {
                        int sel_rc = ag->set_user_selected_machine(d);
                        ag->install_device_cert(d, /*lan_only=*/false);
                        std::fprintf(stderr,
                            "[bridge-gui] set_user_selected_machine(%s) "
                            "rc=%d + install_device_cert(lan_only=false)\n",
                            d.c_str(), sel_rc);
                        std::fflush(stderr);
                    }

                    // ====================================================
                    // Post-LAN enc_msg gate-open cycle.
                    //
                    // The proprietary plugin's `bambu_network_send_message`
                    // (LAN path) gates print.* publishes through an
                    // `apply_enc_msg_gate` check (RVA 0x1fa330 in the
                    // unpacked .so). The gate reads the per-device public
                    // key from `device_pub_key_map[dev_id]`, which is
                    // populated by the `cert_report` MQTT round-trip
                    // triggered by `install_device_cert` (RVA 0x247d10).
                    // Until that map entry exists, send_message_to_printer
                    // returns -4 (SEND_MSG_FAILED) and the publish is
                    // silently dropped before it reaches the wire.
                    //
                    // After LAN sessions come up, a bulk refire of
                    // install_device_cert opens the gate ONLY for the
                    // last printer the plugin connected to (the plugin
                    // keeps multiple TCP/TLS sockets open but only routes
                    // the cert_report reply for the currently-active dev
                    // selected via set_user_selected_machine). So we
                    // cycle per dev_id:
                    //
                    //   1. set_user_selected_machine(d) — makes `d` the
                    //      active LAN session in the plugin (the plugin's
                    //      cloud send_message gate is also keyed off this).
                    //   2. install_device_cert(d, false) — triggers a
                    //      cert_report request on that session; the
                    //      cert_report reply (printer → plugin) is what
                    //      populates device_pub_key_map[d].
                    //   3. Wait for the cert_report reply to land. 5s
                    //      nominal — Exp A saw the prior 3s wait was
                    //      flaky (-4 in ~1/3 launches) on H2S; 5s is
                    //      well within typical LAN round-trip headroom
                    //      and the cascade only runs once per bridge
                    //      bring-up.
                    //
                    // No silent probe at the end of the wait: the only
                    // way to probe via the plugin is to actually publish
                    // a `print.*` payload, which firmware then schema-
                    // checks and echoes back via `/report` with
                    // result:"failed". Slicer observers subscribed to
                    // `/report` would see those echoes. We trust the
                    // wait window (Exp C/D both validated 3s was
                    // sufficient when it didn't lose the race; 5s gives
                    // margin) and only the rc of the install_device_cert
                    // call is logged. If a downstream publish does
                    // return -4 from send_message_to_printer, the
                    // [lan-uplink] log line will surface it.
                    //
                    // See /mnt/cephfs/ssd/BambuBridge/EXP-{A..E}-RESULTS.md
                    // and project_plugin_enc_gate memory for the
                    // empirical / reverse-engineering paths that led here.
                    // ====================================================
                    std::this_thread::sleep_for(15s);
                    std::fprintf(stderr,
                        "[bridge-gui] post-LAN enc_msg gate-open cycle: "
                        "select + install_cert per dev_id\n");
                    std::fflush(stderr);
                    // The cert_report round-trip is timing-flaky (a single
                    // install_device_cert often loses the race and leaves
                    // device_pub_key_map empty). It's idempotent, so retry a
                    // few rounds per dev to make the gate-open reliable.
                    for (int round = 0; round < 3; ++round) {
                        for (const auto& d : dev_ids) {
                            int sel_rc2 = ag->set_user_selected_machine(d);
                            ag->install_device_cert(d, /*lan_only=*/false);
                            std::fprintf(stderr,
                                "[bridge-gui] (cycle r%d) dev=%s "
                                "set_user_selected_machine rc=%d + "
                                "install_device_cert; waiting 5s for cert_report\n",
                                round, d.c_str(), sel_rc2);
                            std::fflush(stderr);
                            std::this_thread::sleep_for(5s);
                        }
                    }

                    std::fprintf(stderr,
                        "[bridge-gui] cascade ready: enc_msg gate cycle "
                        "complete for %zu dev_ids; print.* writes will now "
                        "route through plugin send_message_to_printer\n",
                        dev_ids.size());
                    std::fflush(stderr);

                    // -------- Proof-of-life ---------------------------------
                    // The cascade's success log above is misleading on its
                    // own: login=1/server=1 plus connect_printer rc=0 does
                    // NOT prove the printer is reachable through the
                    // plugin's cloud or LAN route. In practice the cloud
                    // tunnel can silently fail to activate per-dev_id
                    // (see project_bridge_cloud_tunnel) — slicers then see
                    // "Failed to connect" hours later. Probe at boot: send
                    // info.get_version (qos=1) and pushing.pushall (qos=0)
                    // per dev_id and wait for ANY inbound message keyed to
                    // that dev_id within 10s. If nothing arrives, log a
                    // loud failure so the operator sees the broken state
                    // immediately rather than via a slicer timeout later.
                    // Detached so we don't extend cascade-thread lifetime.
                    std::thread([app, dev_ids] {
                        using namespace std::chrono_literals;
                        auto* ag = app->m_agent;
                        if (!ag) return;
                        static std::atomic<uint64_t> s_seq{50000};
                        for (const auto& d : dev_ids) {
                            // -- get_version round-trip --------------------
                            const std::string seq1 = std::to_string(s_seq.fetch_add(1));
                            const std::string pv =
                                std::string("{\"info\":{\"command\":\"get_version\",")
                              + "\"sequence_id\":\"" + seq1 + "\"}}";
                            const auto t1 = std::chrono::steady_clock::now();
                            const int rcv = ag->send_message(d, pv, /*qos=*/1, /*timeout_ms=*/0);
                            if (rcv != 0) {
                                std::fprintf(stderr,
                                    "[proof-of-life] dev=%s FAILED step=get_version "
                                    "send rc=%d (cloud route refused — plugin has "
                                    "no usable cloud tunnel for this printer)\n",
                                    d.c_str(), rcv);
                                std::fflush(stderr);
                                continue;
                            }
                            if (!wait_for_inbound(d, t1, 10s)) {
                                std::fprintf(stderr,
                                    "[proof-of-life] dev=%s FAILED step=get_version "
                                    "send rc=0 but no inbound reply in 10s (one-way "
                                    "tunnel? printer offline from cloud?)\n",
                                    d.c_str());
                                std::fflush(stderr);
                                continue;
                            }
                            std::fprintf(stderr,
                                "[proof-of-life] dev=%s step=get_version OK "
                                "(reply received)\n", d.c_str());
                            std::fflush(stderr);

                            // -- pushing.pushall round-trip ---------------
                            const std::string seq2 = std::to_string(s_seq.fetch_add(1));
                            const std::string pp =
                                std::string("{\"pushing\":{\"command\":\"pushall\",")
                              + "\"push_target\":1,\"sequence_id\":\"" + seq2 + "\"}}";
                            const auto t2 = std::chrono::steady_clock::now();
                            const int rcp = ag->send_message(d, pp, /*qos=*/0, /*timeout_ms=*/0);
                            if (rcp != 0) {
                                std::fprintf(stderr,
                                    "[proof-of-life] dev=%s FAILED step=pushall "
                                    "send rc=%d\n", d.c_str(), rcp);
                                std::fflush(stderr);
                                continue;
                            }
                            if (!wait_for_inbound(d, t2, 10s)) {
                                std::fprintf(stderr,
                                    "[proof-of-life] dev=%s FAILED step=pushall "
                                    "send rc=0 but no push_status in 10s\n",
                                    d.c_str());
                                std::fflush(stderr);
                                continue;
                            }
                            std::fprintf(stderr,
                                "[proof-of-life] dev=%s OK — cloud tunnel alive, "
                                "version + push_status received\n", d.c_str());
                            std::fflush(stderr);
                        }
                    }).detach();
                    // --------------------------------------------------------

                    return;
                }
                std::fprintf(stderr,
                    "[bridge-gui] cascade waited 30s but userMachineList "
                    "stayed empty\n");
                std::fflush(stderr);
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
                if (!native_used && app->m_agent && app->m_agent->is_user_login()) {
                    unsigned int http = 0;
                    std::string  body;
                    int rc = app->m_agent->get_my_token(
                        /*ticket=*/std::string(), &http, &body);
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
                        std::fprintf(stderr,
                            "[bridge-gui] plugin did not surface usable tokens "
                            "via get_my_token; native session NOT persisted "
                            "(access=%s refresh=%s) — next boot will "
                            "fallback again\n",
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

int on_filter_event(wxEvent& event)
{
    // Log left-button DOWN events with the target widget. Useful for
    // human-in-the-loop testing — pair each "[click] ..." line with the
    // bridge MQTT relay activity that follows. Skip wxEVT_LEFT_UP to
    // halve the noise; the DOWN is enough to locate the widget.
    const wxEventType t = event.GetEventType();
    if (t == wxEVT_LEFT_DOWN) {
        wxObject* obj = event.GetEventObject();
        const auto* w = wxDynamicCast(obj, wxWindow);
        wxString label = w ? w->GetLabel()    : wxString();
        wxString name  = w ? w->GetName()     : wxString();
        wxClassInfo* ci = w ? w->GetClassInfo() : nullptr;
        wxString klass = ci ? wxString(ci->GetClassName()) : wxString("?");
        wxPoint pos    = w ? w->GetScreenPosition() : wxPoint(-1, -1);
        wxSize  sz     = w ? w->GetSize()           : wxSize(0, 0);
        // wxMouseEvent inherits from wxEvent; cast for click coords.
        wxPoint mp(-1, -1);
        if (auto* me = wxDynamicCast(&event, wxMouseEvent))
            mp = me->GetPosition();
        (void)label; (void)name; (void)klass; (void)pos; (void)sz; (void)mp;
    }
    return -1; // continue normal dispatch
}

} // namespace BridgeBootstrap
}} // namespace Slic3r::GUI

#endif // BAMBU_BRIDGE
