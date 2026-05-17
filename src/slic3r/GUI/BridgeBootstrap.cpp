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

#include "libslic3r/AppConfig.hpp"
#include "libslic3r_version.h" // SLIC3R_VERSION

#include <wx/app.h>
#include <wx/event.h>
#include <wx/timer.h>
#include <wx/window.h>

#include <boost/log/trivial.hpp>
#include "nlohmann/json.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
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
    app->copy_network_if_available();
    if (!app->on_init_network()) {
        BOOST_LOG_TRIVIAL(error)
            << "BridgeBootstrap::run_headless: on_init_network() failed";
        return false;
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
                // Resolve the live-view URL through the same helper
                // MediaPlayCtrl uses. We only capture the LAN-direct
                // branch synchronously; the TUTK branch's HTTP fetch
                // would arrive too late for this tick (and the bridge's
                // CloudCameraSource has its own get_camera_url path that
                // handles it). Empty p.camera_url tells the bridge's
                // sources to fall back to their built-in URL builders.
                Slic3r::GUI::build_media_live_url(mo,
                    [&p](std::string url, Slic3r::GUI::MediaUrlError err) {
                        if (err == Slic3r::GUI::MediaUrlError::Ok)
                            p.camera_url = std::move(url);
                    });
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
