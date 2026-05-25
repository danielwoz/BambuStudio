// BridgeOnlyConsoleApp implementation. See header for design.
//
// This is the wxAppConsole path the slicer takes when `--bridge-only`
// is on the CLI. It deliberately AVOIDS:
//   * Constructing GUI_App (which derives from wxApp -> wxApp::Initialize
//     -> gtk_init -> crash on a monitor-less server).
//   * Including any GUI-only wx headers (wx/wx.h, wx/frame.h, wx/bitmap.h)
//     so this TU doesn't accidentally pin GTK symbols to the .o.
//   * Calling helpers off `GUI_App` (which would need a GUI_App instance
//     to be alive — there isn't one in this mode).
//
// What it does:
//   * Owns AppConfig directly (mirrors the minimal subset of
//     GUI_App::init_app_config — data_dir, log setup, AppConfig load).
//   * Brings up NetworkAgent + DeviceManager inline (a stripped-down
//     copy of GUI_App::on_init_network — no UserManager/TaskManager,
//     no virtual-store hydration, no dialog hooks). Login is best
//     effort; if cached tokens aren't there we just log and continue
//     with `user_login=no` and zero devices. The architectural intent
//     of this commit is to prove the wxAppConsole dispatch works.
//   * Runs the SAME bridge bootstrap GUI_App does
//     (BridgeStorageBackend + NetworkAgentPluginAdapter + push pump),
//     with the same teardown order in OnExit.

#include "BridgeOnlyConsoleApp.hpp"

#if defined(BAMBU_BRIDGE)

#include "BridgeOnlyFlag.hpp"
#include "Printer/BridgeStorageBackend.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentPluginAdapter.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/LogSink.hpp"
#include "libslic3r_version.h"
#include "libslic3r/Thread.hpp"

#include "DeviceCore/DevManager.h"
// MachineObject lives in DeviceManager.hpp (alongside DeviceManager
// forward decls). This pulls in CameraPopup.hpp transitively, which
// includes a few wx GUI headers (wx/panel.h, wx/bitmap.h). That's link-
// time only — the symbols come in but never execute because we never
// construct a wxApp.
#include "DeviceManager.hpp"

#include "bambu_bridge/headless/BridgeApp.hpp"
#include "bambu_bridge/headless/SignalHandler.hpp"

#include <wx/timer.h>
#include <wx/event.h>
#include <wx/string.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/nowide/convert.hpp>

#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

BridgeOnlyConsoleApp::BridgeOnlyConsoleApp()
{
    SetAppName(SLIC3R_APP_KEY);
}

BridgeOnlyConsoleApp::~BridgeOnlyConsoleApp() = default;

// Minimal AppConfig + data_dir + log setup. Mirrors the wx-platform-
// independent path through GUI_App::init_app_config but skips
// region/log-encryption probing (encryption-off is fine for a
// background daemon) and the legacy Win32 "_chdir to log folder" dance.
static bool bridge_init_app_config_and_paths(AppConfig*& out_app_config)
{
    // Pick the same per-user data dir GUI_App would.
    if (data_dir().empty()) {
#ifdef __linux__
        wxString dir;
        if (!wxGetEnv(wxS("XDG_CONFIG_HOME"), &dir) || dir.empty())
            dir = wxFileName::GetHomeDir() + wxS("/.config");
        std::string d = (dir + "/" + SLIC3R_APP_KEY).ToUTF8().data();
#else
        std::string d =
            wxStandardPaths::Get().GetUserDataDir().ToUTF8().data();
#endif
        fs::path data_dir_path(d);
        fs::path log_dir_path = data_dir_path / "log";
        if (!fs::exists(data_dir_path))
            fs::create_directories(data_dir_path);
        if (!fs::exists(log_dir_path))
            fs::create_directories(log_dir_path);
        set_data_dir(d);

        // Match the GUI: cd into the log dir so any path-relative log
        // sink ends up under data_dir/log/.
        if (chdir((d + "/log").c_str()) != 0) {
        }
    }

    // Bring up the log sink. Encryption-OFF for headless — we never run
    // BBL_RELEASE_TO_PUBLIC log encryption on a server-side daemon.
    LogEncOptions enc_opts;
    enc_opts.enc_type = LogEncOptions::LOG_ENC_NONE;
    const auto& log_filename = LogSinkUtil::get_log_filaname_format(enc_opts);
    set_log_path_and_level(log_filename, /*level=*/3, enc_opts);

    out_app_config = new AppConfig();
    if (out_app_config->exists()) {
        std::string error = out_app_config->load();
        if (!error.empty()) {
            BOOST_LOG_TRIVIAL(error)
                << "BridgeOnlyConsoleApp: AppConfig parse failed: " << error;
            // Continue anyway — bridge can run with a default config; it
            // just won't have a logged-in agent.
        }
    } else {
        // Save default config so next launch has something to load.
        try {
            out_app_config->set_defaults();
            out_app_config->save();
        } catch (const std::exception& ex) {
        }
    }
    return true;
}

bool BridgeOnlyConsoleApp::bring_up_network_agent()
{
    if (!m_app_config) return false;

    // Stripped-down on_init_network. We skip the OTA plugin swap (no
    // UI to drive the update flow) and the multi-machine code; both
    // are GUI-only concerns.
    int load_agent_dll = NetworkAgent::initialize_network_module(
        false,
        !m_app_config->get_bool("ignore_module_cert"));
    if (load_agent_dll != 0) {
        return false;
    }

    // Seed Slic3r::Http extra headers from g_bridge_only_cfg so the
    // agent's own curl shares the same X-BBL-* fingerprint the GUI
    // uses. The CLI prepass in BambuStudio.cpp::CLI::run already
    // populated http_extra_headers with the slicer defaults.
    {
        std::map<std::string, std::string> hdrs;
        for (const auto& kv : g_bridge_only_cfg.http_extra_headers)
            hdrs[kv.first] = kv.second;
        // Also seed X-BBL-Device-ID from the persisted slicer_uuid so
        // cloud REST calls correlate to the same client identity the
        // GUI would advertise on this host.
        if (m_app_config) {
            const std::string uuid = m_app_config->get("slicer_uuid");
            if (!uuid.empty())
                hdrs["X-BBL-Device-ID"] = uuid;
        }
        Slic3r::Http::set_extra_headers(hdrs);
    }

    const std::string data_directory = data_dir();
    m_agent = new NetworkAgent(data_directory);
    m_device_manager = std::make_unique<DeviceManager>(m_agent);

    m_agent->set_config_dir(data_directory);
    m_agent->init_log();

    // Hand the agent the same X-BBL-* set Http has.
    {
        std::map<std::string, std::string> hdrs;
        for (const auto& kv : g_bridge_only_cfg.http_extra_headers)
            hdrs[kv.first] = kv.second;
        if (m_app_config) {
            const std::string uuid = m_app_config->get("slicer_uuid");
            if (!uuid.empty())
                hdrs["X-BBL-Device-ID"] = uuid;
        }
        m_agent->set_extra_http_header(hdrs);
    }

    // Cert dir comes from g_bridge_only_cfg (CLI probe found one); fall
    // back to resources_dir()/cert if the slicer happens to know one
    // (it usually doesn't in --bridge-only mode because CLI::setup
    // doesn't run, but we try anyway).
    if (!g_bridge_only_cfg.cert_dir.empty() &&
        !g_bridge_only_cfg.cert_file.empty()) {
        m_agent->set_cert_file(g_bridge_only_cfg.cert_dir,
                               g_bridge_only_cfg.cert_file);
    } else if (!resources_dir().empty()) {
        m_agent->set_cert_file(resources_dir() + "/cert",
                               "slicer_base64.cer");
    }

    m_agent->set_country_code(m_app_config->get_country_code());

    // Mirror GUI_App: the plugin fires the user-login callback once it
    // notices the cached OAuth token in BambuNetworkEngine.conf. The
    // GUI's handler then calls connect_server. Without this callback
    // the bridge calls connect_server before the plugin has fully
    // recognised the login → the cloud TCP dial is a silent no-op and
    // `is_server_connected()` stays false. Any "control" command
    // (ams_filament_setting, ams_control, etc.) then fails with -4
    // because the plugin requires an active cloud session.
    auto* agent_ptr = m_agent;
    m_agent->set_on_user_login_fn(
        [agent_ptr](int /*online_login*/, bool login_succeeded) {
            std::fprintf(stderr,
                "[bridge-only] on_user_login fired login_succeeded=%d; "
                "calling connect_server\n",
                int(login_succeeded));
            std::fflush(stderr);
            if (login_succeeded && agent_ptr) {
                int rc = agent_ptr->connect_server();
                std::fprintf(stderr,
                    "[bridge-only] connect_server rc=%d post-call "
                    "is_user_login=%d is_server_connected=%d\n",
                    rc, int(agent_ptr->is_user_login()),
                    int(agent_ptr->is_server_connected()));
                std::fflush(stderr);
            }
        });

    m_agent->start();
    // Also try connect_server synchronously — if the plugin already
    // recognised the login by `start()` time the callback won't fire
    // a second time. Belt-and-braces.
    int rc = m_agent->connect_server();
    std::fprintf(stderr,
        "[bridge-only] connect_server (sync) rc=%d "
        "is_user_login=%d is_server_connected=%d\n",
        rc, int(m_agent->is_user_login()),
        int(m_agent->is_server_connected()));
    std::fflush(stderr);
    return true;
}

bool BridgeOnlyConsoleApp::OnInit()
{
    // AppConfig::save asserts is_main_thread_active(); both
    // BambuStudio.cpp::CLI::run and GUI_Run skip save_main_thread_id()
    // on the --bridge-only path, so we own it here.
    save_main_thread_id();

    BOOST_LOG_TRIVIAL(info)
        << "BridgeOnlyConsoleApp::OnInit: wxAppConsole bridge bootstrap";

    if (!bridge_init_app_config_and_paths(m_app_config)) {
        return false;
    }

    // Best-effort agent bring-up. Doesn't fail OnInit if the plugin
    // isn't loadable; the bridge still serves storage requests for any
    // virtual printers added later (none, for now) and the event loop
    // stays up so the user can investigate via logs.
    (void)bring_up_network_agent();

    // Bridge bootstrap — same construction order GUI_App uses.
    try {
        Slic3r::bridge::headless::BridgeAppConfig cfg = g_bridge_only_cfg;

        cfg.slicer_net_ver = NetworkAgent::get_version();
        if (m_app_config)
            cfg.slicer_cli_id = m_app_config->get("slicer_uuid");
        cfg.slicer_cli_ver = std::string(SLIC3R_VERSION);

        m_bridge_storage =
            std::make_unique<Slic3r::bridge::BridgeStorageBackend>();
        m_bridge_app =
            std::make_unique<Slic3r::bridge::headless::BridgeApp>(cfg);

        auto* storage_raw = m_bridge_storage.get();
        m_bridge_app->attach_storage_delegate(
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
            [this](const std::string& dev_id) {
                if (!m_bridge_storage) return;
                auto* backend = m_bridge_storage.get();
                CallAfter([backend, dev_id] { backend->release(dev_id); });
            });

        if (m_agent) {
            // NetworkAgentPluginAdapter wraps a shared_ptr<NetworkAgent>
            // but the slicer's NetworkAgent isn't owned by shared_ptr;
            // it's a raw owned by us. Construct the adapter directly
            // from the raw pointer — the adapter borrows it.
            auto adapter =
                std::make_shared<Slic3r::NetworkAgentPluginAdapter>(m_agent);
            m_bridge_app->attach_plugin_handle(std::move(adapter));
        } else {
            BOOST_LOG_TRIVIAL(warning)
                << "BridgeOnlyConsoleApp: NetworkAgent missing — bridge "
                   "will advertise nothing until login is wired";
        }

        m_bridge_thread = std::make_unique<std::thread>([this]() {
            try {
                const int rc = m_bridge_app->run();
                BOOST_LOG_TRIVIAL(info)
                    << "Bambu Bridge worker exited rc=" << rc;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error)
                    << "Bambu Bridge worker crashed: " << e.what();
            }
        });

        // Push pump: wxTimer is in libwx_base, so wxAppConsole owns
        // the event loop needed to dispatch ticks. Owner is `this`
        // (wxAppConsole is a wxEvtHandler).
        //
        // We also drive the MQTT subscriptions from this tick:
        //   - start_subscribe("app")   — the GUI does this on the idle
        //     hook when the app is "active". Without it the agent never
        //     wakes its MQTT client and push_status never arrives.
        //   - add_user_subscribe()    — DeviceManager subscribes the
        //     cloud topics for every user-owned printer so MachineObject
        //     ota_version (and other status) populates. Idempotent.
        // Both calls are safe to repeat; the agent dedups internally.
        m_bridge_push_timer = std::make_unique<wxTimer>(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (!m_bridge_app)     return;
            if (!m_device_manager) return;
            if (!m_agent || !m_agent->is_user_login()) return;
            if (!m_mqtt_started) {
                m_agent->start_subscribe("app");
                m_mqtt_started = true;
            }
            // Seed DeviceManager::userMachineList the same way the GUI
            // does when the printer-selection dropdown opens: fetch the
            // cloud REST inventory via the agent and feed it through
            // parse_user_print_info. Without this seed, get_user_machinelist()
            // is always empty and we can't reach MachineObject::get_ota_version().
            // We refetch every tick so devices added/removed cloud-side
            // propagate. Idempotent — parse_user_print_info merges into
            // the existing map.
            if (!m_user_print_info_inflight) {
                m_user_print_info_inflight = true;
                std::thread([this]() {
                    unsigned int http_code = 0;
                    std::string body;
                    int rc = m_agent->get_user_print_info(&http_code, &body);
                    CallAfter([this, rc, http_code, body = std::move(body)]() mutable {
                        m_user_print_info_inflight = false;
                        if (rc == 0 && !body.empty() && m_device_manager) {
                            try {
                                m_device_manager->parse_user_print_info(body);
                            } catch (const std::exception& ex) {
                            } catch (...) {
                            }
                        }
                    });
                }).detach();
            }
            m_device_manager->add_user_subscribe();
            std::vector<Slic3r::bridge::headless::VirtualPrinter> snap;
            for (const auto& kv : m_device_manager->get_user_machinelist()) {
                MachineObject* mo = kv.second;
                if (!mo) continue;
                std::string id = mo->get_dev_id();
                if (id.empty()) continue;
                const std::string fw = mo->get_ota_version();
                Slic3r::bridge::headless::VirtualPrinter p;
                p.dev_id      = std::move(id);
                p.dev_name    = mo->get_dev_name();
                p.lan_ip      = mo->get_dev_ip();
                p.access_code = mo->get_access_code();
                p.model       = mo->printer_type;
                p.firmware    = fw;
                snap.push_back(std::move(p));
            }
            if (snap.empty()) return;
            m_bridge_app->set_virtual_printers(std::move(snap));
        }, m_bridge_push_timer->GetId());
        m_bridge_push_timer->Start(5000);

        BOOST_LOG_TRIVIAL(info)
            << "Bambu Bridge --bridge-only started (wxAppConsole); "
            << "DeviceManager push every 5s, NetworkAgent "
            << (m_agent ? "attached" : "missing");
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error)
            << "Failed to start --bridge-only bridge: " << e.what();
        return false;
    }

    // SIGINT/SIGTERM -> ExitMainLoop. Same trampoline GUI_App uses.
    m_signals = std::make_unique<Slic3r::bridge::headless::SignalHandler>(
        [this] {
            CallAfter([this] {
                BOOST_LOG_TRIVIAL(info)
                    << "BridgeOnlyConsoleApp: signal received, exiting loop";
                ExitMainLoop();
            });
        });

    return true;
}

int BridgeOnlyConsoleApp::OnExit()
{
    // Teardown order matches GUI_App::OnExit's bridge-only block:
    //   1. Stop timer (no more snapshot pushes).
    //   2. Storage backend shutdown BEFORE BridgeApp tears its
    //      VirtualTunnelServer down — in-flight reply callbacks
    //      reference PFS' recv threads.
    //   3. BridgeApp shutdown (signals worker thread).
    //   4. Join worker.
    //   5. Reset remaining state.
    if (m_bridge_push_timer) {
        m_bridge_push_timer->Stop();
        m_bridge_push_timer.reset();
    }
    if (m_bridge_storage) {
        m_bridge_storage->shutdown();
    }
    if (m_bridge_app) {
        m_bridge_app->shutdown();
    }
    if (m_bridge_thread && m_bridge_thread->joinable()) {
        m_bridge_thread->join();
    }
    m_bridge_thread.reset();
    m_bridge_app.reset();
    m_bridge_storage.reset();
    m_device_manager.reset();

    if (m_agent) {
        delete m_agent;
        m_agent = nullptr;
        NetworkAgent::unload_network_module();
    }

    m_signals.reset();

    if (m_app_config) {
        if (m_app_config->dirty()) {
            try { m_app_config->save(); }
            catch (...) { /* best effort */ }
        }
        delete m_app_config;
        m_app_config = nullptr;
    }

    return 0;
}

} // namespace GUI
} // namespace Slic3r

#endif // BAMBU_BRIDGE
