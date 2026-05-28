// Bambu Bridge — bootstrap & teardown for GUI_App.
//
// All of the bridge-related modifications that used to live inline in
// GUI_App.cpp (the IMPLEMENT_APP replacement, the headless --bridge-only
// branch, the in-GUI bridge worker, the OnExit shutdown sequence, the
// per-printer MQTT port resolver wiring, the FFFF virtual-LAN store
// rehydrate, the bridge-only networking-callback set) have been moved
// here. GUI_App keeps only:
//   * a handful of `unique_ptr<...>` members holding bridge state
//     (since other GUI code reads them via `get_bridge_app()`), and
//   * a few one-line calls into this header at the points where the
//     bridge needs to hook in.
//
// The result: GUI_App.cpp's diff vs upstream bambulab/BambuStudio
// shrinks dramatically (≈770 LoC → ≈30 LoC of glue), making future
// upstream syncs touch BridgeBootstrap.{hpp,cpp} instead of
// reconciling 770 lines of bridge edits scattered across GUI_App.cpp.
//
// Behaviour-preserving: every chunk moved here is wired back to the
// same call-site it used to live at, in the same order.

#ifndef SLIC3R_GUI_BRIDGE_BOOTSTRAP_HPP
#define SLIC3R_GUI_BRIDGE_BOOTSTRAP_HPP

#if defined(BAMBU_BRIDGE)

class wxEvent;

namespace Slic3r {
namespace GUI {

class GUI_App;

namespace BridgeBootstrap {

// True iff the CLI prepass set Slic3r::GUI::g_bridge_only. Cheap shim so
// GUI_App.cpp's constructor (which conditionally allocates ImGuiWrapper /
// HMSQuery / RemovableDriveManager / OtherInstanceMessageHandler) doesn't
// need to #include BridgeOnlyFlag.hpp directly. Safe to call before wx
// is up.
bool is_bridge_only();

// True iff BAMBU_BRIDGE_INVISIBLE_GUI=1 in the environment. "Invisible GUI"
// means the full slicer process (NetworkAgent, DeviceManager, plugin, cloud
// session, bridge worker) runs as normal — we just never Show() the main
// window and auto-dismiss the startup modals that would otherwise block a
// headless Xvfb. This collapses the old --bridge-only headless code path and
// "real GUI" into ONE binary with one NetworkAgent/plugin instance, so the
// bridge's camera/cert paths share the GUI's live cloud session (no separate
// agent, no two-token confusion). Cached, safe before wx is up.
bool is_invisible_gui();

// File-scope hook: registered as a wxAppInitializer so wxEntry uses our
// custom factory instead of the IMPLEMENT_APP-emitted default. Forces
// the linker to keep this TU; without it the static-ctor never runs.
// Called automatically by a static initialiser inside BridgeBootstrap.cpp.
void register_app_factory();

// Called from GUI_App::on_init_inner() right after the splash-screen
// wxImage::SetDefaultLoadFlags. Caller pattern:
//
//   if (BridgeBootstrap::is_bridge_only())
//       return BridgeBootstrap::run_headless(app);
//
// Returns true if the headless --bridge-only bootstrap succeeded (wx
// will keep ProcessEvent running until SIGINT/SIGTERM); false if
// bootstrap failed (caller propagates `return false` from on_init_inner;
// OnExit / shutdown_hooks unwinds whatever was built).
//
// Effects on success:
//   * NetworkAgent + DeviceManager bootstrap (same path as GUI)
//   * BridgeStorageBackend + BridgeApp construction
//   * VirtualMqttClient port-resolver wiring
//   * storage delegate + plugin adapter attached
//   * worker thread launched
//   * 5s wxTimer push-pump bound
//   * SIGINT/SIGTERM trampoline installed
//   * wxApp::ExitOnFrameDelete(false) (no MainFrame will be created)
bool run_headless(GUI_App* app);

// Called from GUI_App::on_init_inner() at the very end (just before
// `return true`), once MainFrame + Plater are up. No-op in --bridge-only
// mode (the worker is already running from run_headless_if_requested).
// Otherwise: starts the in-GUI bridge worker so other slicers on the
// LAN see this BambuStudio's cloud-bound printers as virtual LAN
// devices for the lifetime of the session. Set
// BAMBU_BRIDGE_GUI_DISABLED=1 to opt out.
void install_gui_worker(GUI_App* app);

// Called from GUI_App::on_init_network() after the DeviceManager has
// been (re-)constructed. Re-hydrates virtual LAN printers the user
// has previously added (FFFF dev_ids only) from VirtualLanPrinterStore
// so they don't have to re-enter the access code every session.
void rehydrate_virtual_lan_printers(GUI_App* app);

// Called from GUI_App::on_init_network() in the `if (m_agent)` block
// instead of `init_networking_callbacks()`. Dispatches to either:
//   * GUI_App::init_networking_callbacks_bridge_only() in --bridge-only
//     mode (just enough callbacks to keep DeviceManager fed), or
//   * GUI_App::init_networking_callbacks() in normal mode (the full set
//     with all dialog hooks).
void install_networking_callbacks(GUI_App* app);

// Called from GUI_App::OnExit() before `return wxApp::OnExit()`. Tears
// down the bridge worker, storage backend, push timer, and the
// VirtualMqttClient port resolver — in that order. In --bridge-only
// mode also short-circuits with _Exit(0) after flushing stdio (see
// implementation for why; static-dtor double-free in the proprietary
// plugin). Returns true if the caller should NOT reach
// `wxApp::OnExit()` (e.g. because _Exit was called or expected).
// Always safe to call.
void shutdown_hooks(GUI_App* app);

// wxApp::FilterEvent delegate. GUI_App keeps a thin override that just
// calls this. Always returns -1 (continue normal dispatch).
int on_filter_event(wxEvent& event);

} // namespace BridgeBootstrap

}} // namespace Slic3r::GUI

#endif // BAMBU_BRIDGE

#endif // SLIC3R_GUI_BRIDGE_BOOTSTRAP_HPP
