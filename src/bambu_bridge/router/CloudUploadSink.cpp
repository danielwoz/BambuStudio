// Bambu Bridge — cloud-side upload sink.
//
// Routes a slicer's .3mf upload through the proprietary plugin's cloud
// OSS + SD-card path (the same export
// `~/BambuStudio/src/slic3r/GUI/Jobs/SendJob.cpp` uses for cloud sends).
// Routing through the plugin keeps cloud-bound bytes byte-identical to
// a real BambuStudio session — the plugin handles OSS auth, multipart
// upload, and the printer-side download trigger.
//
// Implementation: write the UploadJob's content to a tempfile, then
// call `BambuNetworkingPluginHandle::upload_gcode_to_sdcard` with the
// tempfile's path. The plugin streams from disk; we clean the tempfile
// up once the upload returns.

#include "CloudUploadSink.hpp"

#include "../BambuNetworkingPluginHandle.hpp"
#include "UploadSpool.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

const char* err_for_rc(int rc) {
    switch (rc) {
        case  0: return "ok";
        case -1: return "no plugin agent (proprietary bambu_networking missing or not initialised)";
        case -2: return "plugin missing bambu_network_start_send_gcode_to_sdcard export (older plugin?)";
        default: return "plugin reported upload error";
    }
}

// Same X1/P1-family gate as LanUploadSink — for cloud uploads the
// plugin still honours `try_emmc_print` (the printer-side download
// trigger picks eMMC over SD when the flag is set). H2/H2S/H2D ignore
// it; A1 has no storage so it falls through to the cloud-relay print
// route the adapter handles in upload_gcode_to_sdcard.
bool model_supports_emmc(const std::string& model) {
    if (model.empty()) return false;
    std::string up;
    up.reserve(model.size());
    for (char c : model) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    auto has = [&](const char* needle) {
        return up.find(needle) != std::string::npos;
    };
    if (up.find("X1") != std::string::npos) return true;
    if (has("C11") || has("C12")) return true;
    if (up.find("P1") != std::string::npos) return true;
    if (has("C13") || has("C14")) return true;
    // H2 family — see LanUploadSink.cpp's model_supports_emmc for the
    // rationale (legacy /model/ FTPS path is broken on current firmware,
    // try_emmc_print forces the BambuTunnel route).
    if (up.find("H2") != std::string::npos) return true;
    if (up.find("O1") != std::string::npos) return true;
    return false;
}

} // namespace

void CloudUploadSink::attach_plugin(
        std::shared_ptr<BambuNetworkingPluginHandle> handle) {
    m_handle = std::move(handle);
}

void CloudUploadSink::add_device(Device dev) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_devices[dev.dev_id] = std::move(dev);
}

void CloudUploadSink::remove_device(const std::string& dev_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_devices.erase(dev_id);
}

server::UploadResult CloudUploadSink::deliver(server::UploadJob job) {
    server::UploadResult res;

    // GUI parity — same access-code probe short-circuit as
    // LanUploadSink. Identified by body content (16-byte literal
    // "just a test file" — the verbatim resources/check_access_code.txt)
    // because the plugin issues the STOR with an empty filename.
    static constexpr const char  kProbeBody[]   = "just a test file";
    static constexpr std::size_t kProbeBodySize = sizeof(kProbeBody) - 1;
    if (job.content.size() == kProbeBodySize &&
        std::memcmp(job.content.data(), kProbeBody, kProbeBodySize) == 0) {
        res.ok         = true;
        res.remote_url = "bambu-cloud:///model/check_access_code.txt";
        std::fprintf(stderr,
            "[cloud-upload] dev=%s access-code probe "
            "(16-byte 'just a test file') accepted "
            "without forwarding to plugin\n",
            job.dev_id.c_str());
        std::fflush(stderr);
        return res;
    }

    if (!m_handle) {
        res.ok = false;
        res.error_message =
            "CloudUploadSink: no plugin handle attached "
            "(cloud uploads unavailable without bambu_networking plugin).";
        return res;
    }

    std::string tmp_path = spool_upload_to_tempfile(job);
    if (tmp_path.empty()) {
        res.ok = false;
        res.error_message =
            std::string("CloudUploadSink: spool failed: ") +
            std::strerror(errno);
        return res;
    }

    BambuNetworkingPluginHandle::CloudUploadParams cu;
    cu.dev_id           = job.dev_id;
    cu.local_file_path  = tmp_path;
    cu.project_name     = job.filename;            // user-facing label in the printer UI
    cu.connection_type  = "cloud";
    cu.use_ssl_for_ftp  = true;
    cu.use_ssl_for_mqtt = true;
    // dev_ip / access_code populated from per-device registry. The
    // plugin's start_send_gcode_to_sdcard returns -1 silently if these
    // are empty, even with connection_type="cloud" (observed against
    // libbambu_networking 02.06.01.55 in May 2026). If we don't have
    // them yet — e.g. SSDP hasn't fired for this dev_id — leave empty
    // and let the plugin do its best.
    std::string printer_model;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_devices.find(job.dev_id);
        if (it != m_devices.end()) {
            cu.dev_ip       = it->second.printer_ip;
            cu.access_code  = it->second.access_code;
            printer_model   = it->second.printer_model;
        }
    }
    // GUI parity for X1C/P1S: prefer eMMC over SD when the model
    // supports it. The cloud-relay path doesn't need a port-6000 probe
    // (the printer does the download itself), so we set the flag
    // purely model-conditionally here.
    cu.try_emmc_print = model_supports_emmc(printer_model);

    int rc = m_handle->upload_gcode_to_sdcard(cu);

    // The plugin's `start_send_gcode_to_sdcard` is documented synchronous
    // (caller waits for the OSS POST + printer-side download trigger to
    // complete), so it's safe to unlink the spool now. cleanup_upload_
    // tempfile also rmdir's the per-job dir the spool created.
    cleanup_upload_tempfile(tmp_path);

    res.ok = (rc == 0);
    if (res.ok) {
        // Virtual remote URL — mirrors LanUploadSink's "ftps://..." shape
        // so logs are uniform; nothing actually parses this.
        res.remote_url = "bambu-cloud:///model/" + job.filename;
    } else {
        res.error_message =
            std::string("CloudUploadSink: plugin upload rc=") +
            std::to_string(rc) + " — " + err_for_rc(rc);
    }
    return res;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
