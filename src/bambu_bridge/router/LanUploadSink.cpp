// Bambu Bridge — LAN-side upload sink.
//
// Routes a slicer's .3mf upload through the proprietary plugin's
// `start_local_print_with_record` export. The plugin owns the actual
// transport (FTPS-990 for X1/P1, BambuTunnel on port 6000 for H2/H2S/A1)
// and handles auth, retries, and SD-card placement. The bridge just
// spools the payload to a tempfile and points the plugin at it.

#include "LanUploadSink.hpp"

#include "../BambuNetworkingPluginHandle.hpp"
#include "UploadSpool.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

const char* err_for_rc(int rc) {
    switch (rc) {
        case  0: return "ok";
        case -1: return "no plugin agent (proprietary bambu_networking missing or not initialised)";
        case -2: return "plugin missing bambu_network_start_local_print_with_record export (older plugin?)";
        default: return "plugin reported LAN upload error";
    }
}

} // namespace

void LanUploadSink::attach_plugin(
        std::shared_ptr<BambuNetworkingPluginHandle> handle) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_handle = std::move(handle);
}

void LanUploadSink::add_device(LanUploadSinkDevice dev) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_devices[dev.dev_id] = std::move(dev);
}

void LanUploadSink::remove_device(const std::string& dev_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_devices.erase(dev_id);
}

server::UploadResult LanUploadSink::deliver(server::UploadJob job) {
    server::UploadResult res;

    LanUploadSinkDevice                          dev;
    std::shared_ptr<BambuNetworkingPluginHandle> handle;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_devices.find(job.dev_id);
        if (it == m_devices.end()) {
            res.ok            = false;
            res.error_message = "LanUploadSink: no device registered for " + job.dev_id;
            return res;
        }
        dev    = it->second;
        handle = m_handle;
    }

    if (!handle) {
        res.ok            = false;
        res.error_message =
            "LanUploadSink: no plugin handle attached "
            "(LAN uploads unavailable without bambu_networking plugin).";
        std::fprintf(stderr,
            "[lan-upload-sink] dev=%s file=%s bytes=%zu — %s\n",
            job.dev_id.c_str(), job.filename.c_str(),
            job.content.size(), res.error_message.c_str());
        return res;
    }

    std::string tmp_path = spool_upload_to_tempfile(job);
    if (tmp_path.empty()) {
        res.ok            = false;
        res.error_message =
            std::string("LanUploadSink: spool failed: ") + std::strerror(errno);
        std::fprintf(stderr,
            "[lan-upload-sink] dev=%s file=%s — spool failed: %s\n",
            job.dev_id.c_str(), job.filename.c_str(),
            std::strerror(errno));
        return res;
    }

    BambuNetworkingPluginHandle::LocalPrintParams lp;
    lp.dev_id           = job.dev_id;
    lp.dev_ip           = dev.printer_ip;
    lp.access_code      = dev.access_code;
    lp.local_file_path  = tmp_path;
    lp.project_name     = job.filename;
    lp.connection_type  = "lan";
    lp.use_ssl_for_ftp  = true;
    lp.use_ssl_for_mqtt = true;

    int rc = handle->start_local_print_with_record(lp);

    // `start_local_print_with_record` is documented synchronous in
    // upstream — the slicer's FTPS 226 reply waits on this completion,
    // so it's safe to unlink the spool tempfile now.
    ::unlink(tmp_path.c_str());

    res.ok = (rc == 0);
    if (res.ok) {
        res.remote_url = "bambu-lan:///model/" + job.filename;
        std::fprintf(stderr,
            "[lan-upload-sink] forwarded dev=%s file=%s bytes=%zu -> %s\n",
            job.dev_id.c_str(), job.filename.c_str(),
            job.content.size(), res.remote_url.c_str());
    } else {
        res.error_message =
            std::string("LanUploadSink: plugin upload rc=") +
            std::to_string(rc) + " — " + err_for_rc(rc);
        std::fprintf(stderr,
            "[lan-upload-sink] FAIL dev=%s file=%s bytes=%zu rc=%d (%s)\n",
            job.dev_id.c_str(), job.filename.c_str(),
            job.content.size(), rc, err_for_rc(rc));
    }
    return res;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
