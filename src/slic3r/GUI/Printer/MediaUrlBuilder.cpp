// Storage URL builder shared between MediaFilePanel and BridgeStorageBackend.

#include "MediaUrlBuilder.hpp"

#include "../GUI_App.hpp"
#include "../DeviceCore/DevManager.h"
#include "../../Utils/NetworkAgent.hpp"
#include "libslic3r/Utils.hpp"  // for SLIC3R_VERSION

#include <boost/algorithm/string/predicate.hpp>
#include <boost/lexical_cast.hpp>

namespace Slic3r {
namespace GUI {

// External symbol from MediaPlayCtrl.cpp (lives in Slic3r::GUI, must
// match the definition there). The agora SDK uses it as a callback to
// refresh expired stream URLs. Embedded into the storage URL the same
// way MediaFilePanel does so the proprietary plugin can call back into
// the slicer's address space when an agora token rolls.
void refresh_agora_url(char const* device, char const* dev_ver,
                       char const* channel, void* context,
                       void (*callback)(void* context, char const* url));

namespace {

std::string slicer_uuid() {
    AppConfig* cfg = wxGetApp().app_config;
    return cfg ? cfg->get("slicer_uuid") : std::string();
}

} // namespace

void build_media_storage_url(MachineObject* mo, MediaUrlCallback cb) {
    if (!cb) return;
    if (!mo) { cb({}, MediaUrlError::NotReady); return; }

    const std::string  dev_id    = mo->get_dev_id();
    const std::string  lan_ip    = mo->get_dev_ip();
    const std::string  passwd    = mo->get_access_code();
    const std::string  dev_ver   = mo->get_ota_version();
    const bool         lan_mode  = mo->is_lan_mode_printer();
    const bool         busy      = mo->is_camera_busy_off();
    const int          local_p   = mo->file_local;
    const int          remote_p  = mo->get_file_remote();

    // Same gating as MediaFilePanel::fetchUrl. Order matters — bail on
    // the simplest "can never succeed" reasons before reaching for the
    // network agent.
    if (!local_p && !remote_p) { cb({}, MediaUrlError::NoProtocols); return; }
    if (busy) { cb({}, MediaUrlError::DeviceBusy); return; }

    NetworkAgent* agent = wxGetApp().getAgent();
    const std::string agent_ver = agent ? agent->get_version() : std::string();

    // Local branch: prefer LAN-direct when in lan-mode (no cloud creds)
    // or when the printer doesn't advertise a remote protocol at all.
    if ((lan_mode || !remote_p) && local_p && !lan_ip.empty()) {
        std::string url = "bambu:///local/" + lan_ip + ".?port=6000&user=bblp&passwd=" + passwd;
        url += "&device="  + dev_id;
        url += "&net_ver=" + agent_ver;
        url += "&dev_ver=" + dev_ver;
        url += "&cli_id="  + slicer_uuid();
        url += "&cli_ver=" + std::string(SLIC3R_VERSION);
        cb(std::move(url), MediaUrlError::Ok);
        return;
    }
    if (!remote_p && local_p) {
        // local-only but no LAN IP — caller has to decide how to prompt.
        cb({}, MediaUrlError::LanOnlyNoRemote);
        return;
    }
    if (lan_mode) {
        cb({}, MediaUrlError::LanModeNoRemote);
        return;
    }
    if (!agent) {
        cb({}, MediaUrlError::AgentMissing);
        return;
    }

    // TUTK / Agora branch: agent fetches a one-shot cloud-relay URL.
    // The same protocol-list strings the slicer uses in MediaFilePanel.
    static const std::string protocols[] = {
        "", "\"tutk\"", "\"agora\"", "\"tutk\",\"agora\""};
    const std::string idx = dev_id + "|" + dev_ver + "|" + protocols[remote_p];
    agent->get_camera_url(idx,
        [cb, dev_id, agent_ver, dev_ver](std::string url) {
            if (boost::algorithm::starts_with(url, "bambu:///")) {
                url += "&device="      + dev_id;
                url += "&net_ver="     + agent_ver;
                url += "&dev_ver="     + dev_ver;
                url += "&refresh_url=" + boost::lexical_cast<std::string>(&refresh_agora_url);
                url += "&cli_id="      + slicer_uuid();
                url += "&cli_ver="     + std::string(SLIC3R_VERSION);
                cb(std::move(url), MediaUrlError::Ok);
            } else {
                cb({}, MediaUrlError::AgentMissing);  // empty agent reply
            }
        });
}

void build_media_live_url(MachineObject* mo, MediaUrlCallback cb) {
    if (!cb) return;
    if (!mo) { cb({}, MediaUrlError::NotReady); return; }

    const std::string dev_id   = mo->get_dev_id();
    const std::string lan_ip   = mo->get_dev_ip();
    const std::string passwd   = mo->get_access_code();
    const std::string dev_ver  = mo->get_ota_version();
    const bool        lan_mode = mo->is_lan_mode_printer();
    const bool        busy     = mo->is_camera_busy_off();
    const int         lan_p    = mo->liveview_local;   // LVL_None/Disable/Local/Rtsps/Rtsp
    const int         remote_p = mo->get_liveview_remote();

    if (busy) { cb({}, MediaUrlError::DeviceBusy); return; }

    NetworkAgent* agent = wxGetApp().getAgent();
    const std::string agent_ver = agent ? agent->get_version() : std::string();

    // LAN-direct branch — mirrors MediaPlayCtrl::Play lines 349-372.
    if (lan_p > MachineObject::LVL_Disable &&
        (lan_mode || !remote_p) && !lan_ip.empty()) {
        std::string url;
        if (lan_p == MachineObject::LVL_Local)
            url = "bambu:///local/" + lan_ip + ".?port=6000&user=bblp&passwd=" + passwd;
        else if (lan_p == MachineObject::LVL_Rtsps)
            url = "bambu:///rtsps___bblp:" + passwd + "@" + lan_ip + "/streaming/live/1?proto=rtsps";
        else if (lan_p == MachineObject::LVL_Rtsp)
            url = "bambu:///rtsp___bblp:"  + passwd + "@" + lan_ip + "/streaming/live/1?proto=rtsp";
        else { cb({}, MediaUrlError::NoProtocols); return; }
        url += "&device="  + dev_id;
        url += "&net_ver=" + agent_ver;
        url += "&dev_ver=" + dev_ver;
        url += "&cli_id="  + slicer_uuid();
        url += "&cli_ver=" + std::string(SLIC3R_VERSION);
        cb(std::move(url), MediaUrlError::Ok);
        return;
    }

    if (lan_p <= MachineObject::LVL_Disable && (lan_mode || !remote_p)) {
        cb({}, lan_p == MachineObject::LVL_None
            ? MediaUrlError::NoProtocols
            : MediaUrlError::LanModeNoRemote);
        return;
    }
    if (!remote_p) {
        cb({}, MediaUrlError::LanOnlyNoRemote);
        return;
    }
    if (!agent) {
        cb({}, MediaUrlError::AgentMissing);
        return;
    }

    // TUTK / Agora branch — identical post-processing to the storage
    // builder (same agent endpoint, same dev_id|dev_ver|proto index).
    static const std::string protocols[] = {
        "", "\"tutk\"", "\"agora\"", "\"tutk\",\"agora\""};
    const std::string idx = dev_id + "|" + dev_ver + "|" + protocols[remote_p];
    agent->get_camera_url(idx,
        [cb, dev_id, agent_ver, dev_ver](std::string url) {
            if (boost::algorithm::starts_with(url, "bambu:///")) {
                url += "&device="      + dev_id;
                url += "&net_ver="     + agent_ver;
                url += "&dev_ver="     + dev_ver;
                url += "&refresh_url=" + boost::lexical_cast<std::string>(&refresh_agora_url);
                url += "&cli_id="      + slicer_uuid();
                url += "&cli_ver="     + std::string(SLIC3R_VERSION);
                cb(std::move(url), MediaUrlError::Ok);
            } else {
                cb({}, MediaUrlError::AgentMissing);
            }
        });
}

} // namespace GUI
} // namespace Slic3r
