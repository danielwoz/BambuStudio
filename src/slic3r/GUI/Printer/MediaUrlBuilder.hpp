#ifndef SLIC3R_GUI_PRINTER_MEDIA_URL_BUILDER_HPP
#define SLIC3R_GUI_PRINTER_MEDIA_URL_BUILDER_HPP

// Shared URL-construction helpers used by both `MediaFilePanel::fetchUrl`
// (the GUI's Storage tab) and `BridgeStorageBackend::send_request` (the
// bridge's vtun storage proxy). Before this helper existed each call
// site built its own URL string; the bridge's copy hardcoded
// `bambu:///local/...` and so failed for printers that don't advertise
// LAN-direct storage (notably the A1, which has no SD card and only
// advertises `file.remote = "tutk"`).
//
// The helper exposes the same selection ladder MediaFilePanel uses
// (lan-mode + local → bambu:///local/, otherwise → agent->get_camera_url
// for TUTK/Agora) so every consumer agrees on which protocol to ask for.

#include <functional>
#include <string>

namespace Slic3r {

class MachineObject;

namespace GUI {

// Why a URL couldn't be built. Callers translate to their own status
// state (UI badges, RPC error codes, etc).
enum class MediaUrlError {
    Ok,                 // url string is valid and non-empty
    NotReady,           // MachineObject hasn't received its first push_status
    NoProtocols,        // file_local==0 && file_remote==0 — firmware-supported flag
    DeviceBusy,         // mo->is_camera_busy_off()
    LanOnlyNoRemote,    // local-only protocol but no LAN IP known
    LanModeNoRemote,    // LAN-only mode but printer needs cloud relay for storage
    AgentMissing,       // need TUTK but NetworkAgent isn't loaded
};

// Callback signature: receives the URL string and a status code. URL is
// empty when err != Ok.
using MediaUrlCallback = std::function<void(std::string url, MediaUrlError err)>;

// Build the storage URL the way `MediaFilePanel::fetchUrl` does.
//
// - Local branches (LAN-mode or printer reports `file.local=local`) fire
//   the callback synchronously before returning.
// - TUTK / Agora branches POST to the agent's camera-url endpoint and
//   fire the callback later, on whichever thread the agent uses for its
//   HTTP completion (usually the wx main thread via `CallAfter`).
//
// Caller owns `mo` and must keep it alive at least until either the
// function returns synchronously OR the async callback fires.
void build_media_storage_url(MachineObject* mo, MediaUrlCallback cb);

// Build the live-view (camera) URL the way `MediaPlayCtrl::Play` does.
// Same async-callback semantics as `build_media_storage_url`: LAN-direct
// branches fire synchronously; TUTK/Agora branches fire after the
// agent's HTTP completion.
//
// Picks between `bambu:///local/...`, `bambu:///rtsps___...`,
// `bambu:///rtsp___...`, or `bambu:///tutk?...` depending on the
// printer's advertised `liveview_local` (LVL_*) and `liveview_remote`
// (LVR_*) flags. Both MediaPlayCtrl and the bridge's
// LanCameraSource/CloudCameraSource call this so a single ladder picks
// the protocol everywhere.
void build_media_live_url(MachineObject* mo, MediaUrlCallback cb);

} // namespace GUI
} // namespace Slic3r

#endif // SLIC3R_GUI_PRINTER_MEDIA_URL_BUILDER_HPP
