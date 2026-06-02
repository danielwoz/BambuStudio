// BridgeStorageBackend implementation. See header for design.

#include "BridgeStorageBackend.hpp"

#include "PrinterFileSystem.h"
#include "MediaUrlBuilder.hpp"
#include "../GUI_App.hpp"
#include "../DeviceCore/DevManager.h"

#include <wx/app.h>
#include <wx/event.h>

#include "nlohmann/json.hpp"

#include <boost/log/trivial.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/smart_ptr/weak_ptr.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace Slic3r {
namespace bridge {

namespace {

// Base64 encode. Bytes → ASCII so the binary `data` payload from an
// upstream PFS SUB_FILE response (thumbnail bytes, zipped 3mf metadata)
// can ride inside the JSON reply across the VirtualTunnelServer wire to
// the slicer-side PFS, which decodes back to bytes before splicing them
// into the canonical JSON\n\n+bytes sample buffer it expects. Mirror
// helper lives in `bambu_bridge/server/VirtualTunnelServer.cpp`.
std::string bridge_b64_encode(const unsigned char* data, std::size_t len) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t triple =
            (std::uint32_t(data[i]) << 16)
          | (i + 1 < len ? std::uint32_t(data[i + 1]) << 8 : 0u)
          | (i + 2 < len ? std::uint32_t(data[i + 2])      : 0u);
        out += alphabet[(triple >> 18) & 0x3f];
        out += alphabet[(triple >> 12) & 0x3f];
        out += i + 1 < len ? alphabet[(triple >> 6) & 0x3f] : '=';
        out += i + 2 < len ? alphabet[triple        & 0x3f] : '=';
    }
    return out;
}

// Helper: run `fn` on the wx main thread. If the wx app is already in
// the wx main thread we run it synchronously; otherwise queue via
// CallAfter. Most call sites are off-thread (bridge session_loop), so
// the async path is the hot one.
//
// We use `wxTheApp` (wxAppConsole*) rather than `wxGetApp()` because
// in `--bridge-only` mode the running app instance is
// `Slic3r::GUI::BridgeOnlyConsoleApp` (a wxAppConsole subclass),
// not `GUI_App`. `wxAppConsole::CallAfter` is the same wxEvtHandler
// member in either case.
template <typename Fn>
void on_wx_main(Fn&& fn) {
    if (wxThread::IsMain()) {
        fn();
    } else if (wxTheApp) {
        wxTheApp->CallAfter(std::forward<Fn>(fn));
    } else {
        // No app instance at all — run inline. Shouldn't happen in
        // practice; PFS recv threads stop before OnExit returns.
        fn();
    }
}

} // namespace

BridgeStorageBackend::BridgeStorageBackend() = default;

BridgeStorageBackend::~BridgeStorageBackend() {
    // Best-effort shutdown so the destructor doesn't leak PFS recv
    // threads. shutdown() is idempotent and safe to call again from
    // the explicit GUI_App OnExit hook.
    shutdown();
}

void BridgeStorageBackend::send_request(const std::string& real_dev_id,
                                        const std::string& /*real_lan_ip*/,
                                        const std::string& /*access_code*/,
                                        const std::string& /*dev_ver*/,
                                        const std::string& /*net_ver*/,
                                        const std::string& /*cli_id*/,
                                        const std::string& /*cli_ver*/,
                                        int                cmdtype,
                                        nlohmann::json     request_body,
                                        ReplyCallback      cb) {
    if (real_dev_id.empty()) {
        if (cb) cb(/*ERROR_PIPE*/3, nlohmann::json::object());
        return;
    }

    std::shared_ptr<PfsEntry> entry;
    bool created = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_by_dev.find(real_dev_id);
        if (it == m_by_dev.end()) {
            // URL is left empty here; we'll resolve it via
            // MediaUrlBuilder on the wx main thread below so the bridge
            // and the GUI share one URL-construction path. For
            // already-existing entries we keep entry->url (a retry).
            entry = std::make_shared<PfsEntry>();
            m_by_dev[real_dev_id] = entry;
            created = true;
        } else {
            entry = it->second;
        }
    }

    // The closure that actually pushes the request into PFS. Captured
    // by value because it might be deferred onto entry->pending_sends.
    auto enqueue_send = [entry, cmdtype, req = std::move(request_body),
                         cb = std::move(cb), dev_id = real_dev_id]() {
        if (!entry->fs) {
            if (cb) cb(/*ERROR_PIPE*/3, nlohmann::json::object());
            return;
        }
        // PFS' SendRequest callback signature is
        //   int(int result, json const& resp, unsigned char const* data)
        // SUB_FILE responses come back as JSON + a raw byte slice — the
        // bytes carry thumbnail image data (timelapse previews,
        // model_metadata thumbnails) or the zipped 3mf metadata the
        // slicer parses for weight / print-time. We can't drop them;
        // smuggle them in a base64 sidecar field on the JSON reply, and
        // VirtualTunnelServer's session_loop strips + re-attaches the
        // bytes to the JSON\n\n+bytes frame the slicer-side PFS expects.
        // LIST_INFO / FILE_DEL / TASK_CANCEL have no data — encoding a
        // zero-length payload yields an empty `data` slice on the other
        // side, which the slicer handles fine.
        //
        // Always-on (no env gate). An earlier 2026-06-02 default-off
        // gate existed because the first attempt crashed Orca inside
        // load_gcode_3mf_from_stream / _extract_project_config_from_archive
        // — that crash is now instrumented with BOOST_LOG_TRIVIAL markers
        // and a try/catch null-guard in PrinterFileSystem::ParseThumbnail
        // so a recurrence leaves a precise trail in
        // ~/.config/OrcaSlicer/log/ instead of a silent process death.
        auto pfs_cb = [cb](int result, nlohmann::json const& resp,
                           unsigned char const* data) -> int {
            if (!cb) return result;
            const std::uint32_t size = resp.value("size", std::uint32_t{0});
            if (data && size > 0) {
                nlohmann::json resp_with_data = resp;
                resp_with_data["_bridge_data_b64"] =
                    bridge_b64_encode(data, size);
                cb(result, std::move(resp_with_data));
            } else {
                cb(result, resp);
            }
            return result;
        };
        // friend-access into PrinterFileSystem::SendRequest(...).
        entry->fs->SendRequest(cmdtype, req, pfs_cb, /*param=*/"");
    };

    on_wx_main([this, entry, created, real_dev_id, enqueue = std::move(enqueue_send)]() mutable {
        // Lazily build the PFS on the wx main thread (PFS' wxEvtHandler
        // dispatch + Bind must run on this thread).
        if (created) {
            // Resolve the URL through the same helper MediaFilePanel
            // uses. For LAN-direct printers this fires synchronously
            // and entry->url is set before we construct the PFS. For
            // TUTK/Agora the agent's get_camera_url completes
            // asynchronously, but the URL still needs to be present
            // by the time the PFS' status listener fires Initializing
            // — we set entry->url from the callback, and PFS will
            // pick it up on the next Initializing event.
            MachineObject* mo = nullptr;
            if (DeviceManager* dev =
                    ::Slic3r::GUI::wxGetApp().getDeviceManager()) {
                mo = dev->get_my_machine(real_dev_id);
            }
            if (!mo) {
                std::vector<std::function<void()>> drain;
                drain.swap(entry->pending_sends);
                for (auto& fn : drain) (void) fn;  // no-ops; cbs already fired via ERROR_PIPE below
                return;
            }
            ::Slic3r::GUI::build_media_storage_url(mo,
                [entry, real_dev_id](std::string url,
                                     ::Slic3r::GUI::MediaUrlError err) {
                    if (err != ::Slic3r::GUI::MediaUrlError::Ok) {
                        return;
                    }
                    entry->url = url;
                    // Push directly into PFS so we don't depend on
                    // catching the next Initializing event — when
                    // resolution is async (TUTK / Agora) the PFS may
                    // already be past Initializing waiting for the URL.
                    // PFS::SetUrl is a mutex+notify; safe to call from
                    // any thread.
                    if (entry->fs) entry->fs->SetUrl(url);
                });
            entry->fs = boost::shared_ptr<PrinterFileSystem>(new PrinterFileSystem());
            entry->fs->Attached();
            // Match MediaFilePanel's PFS-setup pattern exactly. The
            // A1's storage tunnel fails in the bridge path but works
            // in the GUI path despite using the same library + URL;
            // the only setup-level difference we can find is that
            // MediaFilePanel calls SetFileType(F_TIMELAPSE, "internal")
            // immediately after Attached and binds five EVTs through
            // ImageGrid::SetFileSystem. Replicate both — even if the
            // event listeners are no-ops, mirroring the wx event chain
            // is required to keep PFS' DumpLog / wxEvtHandler dispatch
            // identical to the GUI's case.
            entry->fs->SetFileType(PrinterFileSystem::F_TIMELAPSE,
                                   /*storage=*/"internal");
            auto noop = [](wxCommandEvent& e) { e.Skip(); };
            entry->fs->Bind(EVT_MODE_CHANGED, noop);
            entry->fs->Bind(EVT_FILE_CHANGED, noop);
            entry->fs->Bind(EVT_THUMBNAIL,    noop);
            entry->fs->Bind(EVT_DOWNLOAD,     noop);
            entry->fs->Bind(EVT_SELECT_CHANGED, noop);

            // Status listener: drains pending sends on ListSyncing,
            // fails them on Failed, flips ready->false on Reconnecting.
            // Capture a weak_ptr so the lambda is a no-op once the
            // entry has been released. We snapshot the backend pointer
            // because the lambda outlives the original send_request
            // call.
            std::weak_ptr<PfsEntry> wentry = entry;
            std::string dev_id_log = real_dev_id;
            entry->fs->Bind(EVT_STATUS_CHANGED,
                [wentry, dev_id_log](wxCommandEvent& e) {
                    e.Skip();
                    auto entry = wentry.lock();
                    if (!entry) return;
                    const int status = e.GetInt();
                    if (status == PrinterFileSystem::Initializing) {
                        // PFS' Reconnect() calls m_messages.clear() at the
                        // very top of its work loop, so any URL pushed
                        // *before* Reconnect starts gets wiped. The slicer
                        // GUI (MediaFilePanel::fetchUrl) handles this by
                        // pushing the URL only *after* the Initializing
                        // event fires. Mirror that pattern here.
                        if (entry->fs && !entry->url.empty()) {
                            entry->fs->SetUrl(entry->url);
                        }
                    } else if (status == PrinterFileSystem::ListSyncing) {
                        // PFS reached the "ready to serve" state. The
                        // bridge doesn't actually care about listings —
                        // we just use ListSyncing as the signal that
                        // the underlying Bambu_Tunnel is live and PFS
                        // is ready to accept arbitrary SendRequests.
                        std::vector<std::function<void()>> drain;
                        drain.swap(entry->pending_sends);
                        entry->ready = true;
                        for (auto& fn : drain) fn();
                    } else if (status == PrinterFileSystem::Failed) {
                        // Don't drain on Failed. PFS retries every 10s
                        // (timed_wait inside Reconnect) and will move
                        // back through Initializing → Connecting → ...
                        // — possibly succeeding on the next attempt
                        // (Bambu_StartStreamEx returning would_block
                        // for several seconds before succeeding is
                        // typical for the A1 when the printer hasn't
                        // been primed by an active session yet).
                        // The CLIENT enforces the overall timeout via
                        // its own SSL_read timeout; if the user gives
                        // up, the session tears down and any in-flight
                        // PFS request callback becomes a no-op
                        // through the alive flag.
                        entry->ready = false;
                    } else if (status == PrinterFileSystem::Reconnecting) {
                        entry->ready = false;
                    }
                });
            // Don't call SetUrl here — the status listener pushes the
            // URL when PFS fires Initializing (after Reconnect's
            // internal m_messages.clear()). Start() unblocks the recv
            // thread; it will progress Reconnecting → Initializing,
            // our listener pushes URL, Reconnect picks it up, then
            // Connecting → ListSyncing.
            entry->fs->Start();
        }

        // If PFS is already ready, push immediately. Otherwise queue
        // the closure for the status listener to drain when it sees
        // ListSyncing. This matches MediaFilePanel's "send only after
        // the FS is connected" gate.
        if (entry->ready) {
            enqueue();
        } else {
            entry->pending_sends.push_back(std::move(enqueue));
        }
    });
}

void BridgeStorageBackend::release(const std::string& real_dev_id) {
    std::shared_ptr<PfsEntry> doomed;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_by_dev.find(real_dev_id);
        if (it == m_by_dev.end()) return;
        doomed = std::move(it->second);
        m_by_dev.erase(it);
    }
    // PFS shutdown must run on the wx main thread — Stop notifies the
    // recv thread, which then unwinds wx event handler state.
    on_wx_main([doomed]() mutable {
        if (doomed->fs) {
            doomed->fs->Stop(/*quit=*/true);
        }
        // doomed goes out of scope at the end of this lambda; PFS dtor
        // joins its recv thread synchronously.
    });
}

void BridgeStorageBackend::shutdown() {
    std::map<std::string, std::shared_ptr<PfsEntry>> snap;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        snap.swap(m_by_dev);
    }
    for (auto& kv : snap) {
        auto entry = kv.second;
        on_wx_main([entry]() mutable {
            if (entry->fs) entry->fs->Stop(/*quit=*/true);
        });
    }
}

}} // namespace Slic3r::bridge
