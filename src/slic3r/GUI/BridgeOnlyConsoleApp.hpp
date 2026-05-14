// BridgeOnlyConsoleApp — wxAppConsole subclass that runs the bridge
// worker on monitor-less servers (no DISPLAY, no GTK).
//
// Background: BambuStudio normally builds a wxApp-derived GUI_App which
// transitively triggers `gtk_init` in `wxApp::Initialize` before our
// OnInit ever runs — fatal on a server with no display. For the
// `--bridge-only` mode the slicer's UI is irrelevant; we only need the
// NetworkAgent + DeviceManager + bridge worker + wx event loop. This
// subclass of `wxAppConsole` (the no-GUI variant) provides exactly
// that: it owns the bridge state previously owned by GUI_App's
// `init_bridge_only_headless` and runs the same bootstrap, just
// without any of GUI_App's GUI-link-time baggage.
//
// Dispatch: `wxCreateApp()` is overridden in GUI_App.cpp; when
// `Slic3r::GUI::g_bridge_only` is set the factory returns a
// `BridgeOnlyConsoleApp`, otherwise the normal `GUI_App`. The choice
// happens at wxEntry-time, before `wxApp::Initialize` would have done
// any GTK init, so the headless launch never touches GTK at all.

#ifndef SLIC3R_GUI_BRIDGE_ONLY_CONSOLE_APP_HPP
#define SLIC3R_GUI_BRIDGE_ONLY_CONSOLE_APP_HPP

#include <wx/app.h>

#include <memory>
#include <atomic>
#include <thread>

class wxTimer;

namespace Slic3r {

class AppConfig;
class NetworkAgent;
class DeviceManager;

namespace bridge {
class BridgeStorageBackend;
namespace headless {
class BridgeApp;
class SignalHandler;
} // namespace headless
} // namespace bridge

namespace GUI {

class BridgeOnlyConsoleApp : public wxAppConsole {
public:
    BridgeOnlyConsoleApp();
    ~BridgeOnlyConsoleApp() override;

    bool OnInit() override;
    int  OnExit() override;

private:
    bool bring_up_network_agent();

    // Bridge state (mirrors GUI_App's bridge-owned members). Order
    // matters at teardown: storage backend must outlive BridgeApp's
    // VirtualTunnelServer because in-flight reply callbacks reference
    // it; the worker thread is joined after BridgeApp::shutdown.
    AppConfig*                                              m_app_config{nullptr};
    NetworkAgent*                                           m_agent{nullptr};
    std::unique_ptr<DeviceManager>                          m_device_manager;
    std::unique_ptr<Slic3r::bridge::BridgeStorageBackend>   m_bridge_storage;
    std::unique_ptr<Slic3r::bridge::headless::BridgeApp>    m_bridge_app;
    std::unique_ptr<std::thread>                            m_bridge_thread;
    std::unique_ptr<wxTimer>                                m_bridge_push_timer;
    bool                                                    m_mqtt_started{false};
    std::atomic<bool>                                       m_user_print_info_inflight{false};
    std::unique_ptr<Slic3r::bridge::headless::SignalHandler> m_signals;
};

} // namespace GUI
} // namespace Slic3r

#endif // SLIC3R_GUI_BRIDGE_ONLY_CONSOLE_APP_HPP
