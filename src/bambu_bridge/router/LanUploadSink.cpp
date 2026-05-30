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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

// Cache lifetime for the port-6000 BambuTunnel probe — re-probe at most
// every 5 minutes per dev_id. Prints take long enough that the printer's
// firmware / network state can shift between uploads; refreshing
// occasionally catches a firmware update that opened the tunnel without
// reverting the FTPS path for every upload in between.
static constexpr std::chrono::minutes kTunnelProbeTtl{5};

namespace {

const char* err_for_rc(int rc) {
    switch (rc) {
        case  0: return "ok";
        case -1: return "no plugin agent (proprietary bambu_networking missing or not initialised)";
        case -2: return "plugin missing bambu_network_start_local_print_with_record export (older plugin?)";
        default: return "plugin reported LAN upload error";
    }
}

// X1C/P1S are the two model lines whose firmware exposes the
// "eMMC vs SD card" choice at print-start. The plugin honours
// `PluginPrintParams::try_emmc_print` only for printers whose
// model report matches one of these families — for H2/H2S/H2D the
// transport is BambuTunnel on port 6000 unconditionally (no eMMC
// concept), for A1 there is no FTPS server and no eMMC. Returns true
// iff the model string (vendor marketing name or internal C-code)
// belongs to an X1/P1-family printer.
bool model_supports_emmc(const std::string& model) {
    if (model.empty()) return false;
    std::string up;
    up.reserve(model.size());
    for (char c : model) up.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    auto has = [&](const char* needle) {
        return up.find(needle) != std::string::npos;
    };
    if (has("X1")) return true;            // X1, X1C, X1E
    if (has("C11") || has("C12")) return true;   // X1 internal codes (3DPrinter-C11/C12)
    if (has("P1")) return true;            // P1P, P1S
    if (has("C13") || has("C14")) return true;   // P1P/P1S internal codes
    return false;
}

// Synchronous best-effort TCP-connect probe to <ip>:6000 with a
// `timeout` budget. Returns true iff the kernel reported the connection
// succeeded within the budget. We DO NOT speak the BambuTunnel auth
// handshake here — a plain TCP accept is sufficient evidence that the
// printer's BambuTunnel server is up; if auth fails later the plugin
// will surface that to the caller as a regular upload-rc.
bool probe_bambu_tunnel_port_6000(const std::string& ip,
                                   std::chrono::milliseconds timeout) {
    if (ip.empty()) return false;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    // Non-blocking connect so we can apply the timeout via poll().
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return false;
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(6000);
    if (::inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1) {
        ::close(fd);
        return false;
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    bool ok = false;
    if (rc == 0) {
        ok = true;                          // immediate success (loopback)
    } else if (errno == EINPROGRESS) {
        pollfd p{fd, POLLOUT, 0};
        int pr = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (pr > 0 && (p.revents & POLLOUT)) {
            int       so_err = 0;
            socklen_t slen   = sizeof(so_err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 &&
                so_err == 0) {
                ok = true;
            }
        }
    }
    ::close(fd);
    return ok;
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
        return res;
    }

    std::string tmp_path = spool_upload_to_tempfile(job);
    if (tmp_path.empty()) {
        res.ok            = false;
        res.error_message =
            std::string("LanUploadSink: spool failed: ") + std::strerror(errno);
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

    // GUI parity for X1C/P1S: prefer the BambuTunnel-on-port-6000
    // route (eMMC target) over legacy FTPS-on-990 (SD card) when the
    // printer's BambuTunnel server is reachable. Plugin honours the
    // hint only on these models; for H2/H2S/H2D it routes via port
    // 6000 unconditionally and for A1 there's no FTPS at all (the
    // adapter's cloud-relay fallback covers that case). Cache the
    // probe so we don't open a fresh TCP connection on every upload.
    if (model_supports_emmc(dev.printer_model)) {
        bool reachable = false;
        bool need_probe = false;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            auto it = m_tunnel_probes.find(job.dev_id);
            const auto now = std::chrono::steady_clock::now();
            if (it == m_tunnel_probes.end() ||
                (now - it->second.probed_at) > kTunnelProbeTtl) {
                need_probe = true;
            } else {
                reachable = it->second.reachable;
            }
        }
        if (need_probe) {
            reachable = probe_bambu_tunnel_port_6000(
                dev.printer_ip, std::chrono::milliseconds(750));
            {
                std::lock_guard<std::mutex> lk(m_mu);
                m_tunnel_probes[job.dev_id] = TunnelProbeResult{
                    reachable, std::chrono::steady_clock::now()};
            }
            std::fprintf(stderr,
                "[lan-upload] dev=%s model=%s port-6000 preflight: %s\n",
                job.dev_id.c_str(),
                dev.printer_model.c_str(),
                reachable ? "reachable (try_emmc_print=1)"
                          : "unreachable (FTPS-990 fallback)");
            std::fflush(stderr);
        }
        lp.try_emmc_print = reachable;
    }

    int rc = handle->start_local_print_with_record(lp);

    // `start_local_print_with_record` is documented synchronous in
    // upstream — the slicer's FTPS 226 reply waits on this completion,
    // so it's safe to unlink the spool tempfile now.
    ::unlink(tmp_path.c_str());

    res.ok = (rc == 0);
    if (res.ok) {
        res.remote_url = "bambu-lan:///model/" + job.filename;
    } else {
        res.error_message =
            std::string("LanUploadSink: plugin upload rc=") +
            std::to_string(rc) + " — " + err_for_rc(rc);
    }
    return res;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
