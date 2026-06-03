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

#include "../../miniz/miniz.h"

#include <nlohmann/json.hpp>

#include "../platform/WinsockShim.hpp"   // sockets (winsock2 before windows.h)
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#  include <poll.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>     // getenv
#include <cstring>
#include <ctime>       // timestamp formatting
#include <set>
#include <string>
#ifndef _WIN32
#  include <unistd.h>
#endif
#include <utility>
#include <vector>

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
    // H2 family. The plugin's legacy `CWD model + STOR /model/<name>.gcode`
    // path is broken on current H2D / H2S firmware — the FTPS root is now
    // flat (no `/model/` directory) and existing print files use the
    // `.gcode.3mf` extension at the root. Setting try_emmc_print=true here
    // pushes the plugin onto its port-6000 BambuTunnel route, which mirrors
    // what the GUI's PrintJob does when `obj->is_support_print_with_emmc`.
    if (has("H2"))  return true;           // H2D, H2S
    if (has("O1"))  return true;           // H2D internal "O1D", H2S "O1S"
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
    bambu_set_nonblocking(fd, true);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(6000);
    if (::inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) != 1) {
        bambu_close_socket(fd);
        return false;
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    bool ok = false;
    const int conn_err = bambu_last_socket_error();
    if (rc == 0) {
        ok = true;                          // immediate success (loopback)
    } else if (conn_err == EINPROGRESS || conn_err == EWOULDBLOCK) {
        pollfd p{};
        p.fd = fd; p.events = POLLOUT;
        int pr;
#ifdef _WIN32
        pr = ::WSAPoll(&p, 1, static_cast<int>(timeout.count()));
#else
        pr = ::poll(&p, 1, static_cast<int>(timeout.count()));
#endif
        if (pr > 0 && (p.revents & POLLOUT)) {
            int so_err = 0;
            int slen   = sizeof(so_err);
            if (bambu_getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 &&
                so_err == 0) {
                ok = true;
            }
        }
    }
    bambu_close_socket(fd);
    return ok;
}

// Extract the plate G-code (Metadata/plate_*.gcode) from a Bambu .3mf
// to a sibling file in the same per-job tempdir. Returns the path of
// the extracted .gcode on success, empty string on failure.
//
// Why we have to split: the GUI's PrintJob fills
//   params.filename        = <plate>.gcode  (raw G-code; what the plugin
//                                            FTPS-uploads to printer SD)
//   params.config_filename = <plate>_config.3mf  (small .3mf with the
//                                                 project settings)
// — two separate files. Orca's FFFF flow STORs ONE combined .3mf that
// contains both the G-code and the settings. Without the split the
// plugin's start_local_print_with_record tries to FTPS-upload a `.3mf`
// to the printer's SD card; the printer's FTPS server only accepts
// `.gcode` writes there and rejects with CURLE_REMOTE_ACCESS_DENIED
// (libcurl error 9 — surfaces as info=`[ftp code]: 9` on stage 7 here).
// Not currently called — the plugin handles gcode extraction itself
// when connection_type="cloud" and filename points at a combined .3mf.
// Kept for the legacy LAN-FTPS path where the plugin expected the
// caller to pre-split the gcode out.
[[maybe_unused]]
static std::string extract_plate_gcode(const std::string& threemf_path,
                                       const std::string& dev_id) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, threemf_path.c_str(), 0)) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s zip-open failed on %s\n",
            dev_id.c_str(), threemf_path.c_str());
        std::fflush(stderr);
        return {};
    }

    // Walk the central directory for "Metadata/plate_*.gcode". Pick the
    // first match — Orca's slice job is per-plate so there's usually
    // only one. Multi-plate combined .3mfs would need the slicer-side
    // plate_index, which the bridge doesn't see at the FTPS layer.
    int          gcode_idx  = -1;
    std::string  gcode_name;
    mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < num_files; ++i) {
        char name[512];
        mz_uint n = mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        if (n == 0) continue;
        std::string nm(name);
        // case-sensitive: Bambu's spec is "Metadata/plate_<N>.gcode".
        if (nm.size() > 6 &&
            nm.compare(0, 9, "Metadata/") == 0 &&
            nm.size() > 6 + 9 &&
            nm.compare(9, 6, "plate_") == 0 &&
            nm.size() > 6 &&
            nm.compare(nm.size() - 6, 6, ".gcode") == 0) {
            gcode_idx  = static_cast<int>(i);
            gcode_name = std::move(nm);
            break;
        }
    }
    if (gcode_idx < 0) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s no Metadata/plate_*.gcode entry in %s\n",
            dev_id.c_str(), threemf_path.c_str());
        std::fflush(stderr);
        mz_zip_reader_end(&zip);
        return {};
    }

    // Output path: same per-job tempdir, basename derived from the
    // .3mf basename so the printer-side filename matches what the user
    // would see in the GUI ("MyProject_plate_1.gcode").
    auto pos = threemf_path.find_last_of('/');
    std::string dir  = (pos == std::string::npos) ? "/tmp" : threemf_path.substr(0, pos);
    std::string base = (pos == std::string::npos) ? threemf_path : threemf_path.substr(pos + 1);
    auto dot = base.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
    std::string out_path = dir + "/" + stem + ".gcode";

    if (!mz_zip_reader_extract_to_file(&zip, gcode_idx, out_path.c_str(), 0)) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s zip-extract %s -> %s failed\n",
            dev_id.c_str(), gcode_name.c_str(), out_path.c_str());
        std::fflush(stderr);
        mz_zip_reader_end(&zip);
        return {};
    }
    mz_zip_reader_end(&zip);

    std::fprintf(stderr,
        "[lan-upload] dev=%s extracted %s -> %s\n",
        dev_id.c_str(), gcode_name.c_str(), out_path.c_str());
    std::fflush(stderr);
    return out_path;
}

// Build a settings-only `.3mf` by copying every entry of the combined
// .3mf EXCEPT the plate G-code(s) (Metadata/plate_*.gcode and the
// matching .gcode.md5). Result lives in the same per-job tempdir as
// `threemf_path`. Returns its path on success, empty on failure.
//
// The plugin's `start_local_print_with_record` / `start_print` expect
// the two-file shape the GUI's PrintJob feeds it:
//   filename        = <plate>.gcode             (raw print payload)
//   config_filename = <project>_config.3mf      (project settings;
//                                                MUST NOT contain the
//                                                plate gcode again)
// When config_filename has duplicate plate_*.gcode entries inside, the
// plugin's bundle assembly produces a malformed .gcode.3mf and the
// printer rejects with "content of print file is unreadable" (seen in
// the H2D's printer-side error after the upload completed at rc=0 in
// an earlier session).
static std::string make_settings_only_zip(const std::string& threemf_path,
                                          const std::string& dev_id) {
    mz_zip_archive in{}, out{};
    if (!mz_zip_reader_init_file(&in, threemf_path.c_str(), 0)) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s settings-only: open input %s failed\n",
            dev_id.c_str(), threemf_path.c_str());
        std::fflush(stderr);
        return {};
    }

    auto pos = threemf_path.find_last_of('/');
    std::string dir  = (pos == std::string::npos) ? "/tmp" : threemf_path.substr(0, pos);
    std::string base = (pos == std::string::npos) ? threemf_path : threemf_path.substr(pos + 1);
    auto dot = base.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? base : base.substr(0, dot);
    std::string out_path = dir + "/" + stem + "_config.3mf";
    ::unlink(out_path.c_str());

    if (!mz_zip_writer_init_file(&out, out_path.c_str(), 0)) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s settings-only: open output %s failed\n",
            dev_id.c_str(), out_path.c_str());
        std::fflush(stderr);
        mz_zip_reader_end(&in);
        return {};
    }

    auto is_plate_gcode = [](const std::string& nm) {
        if (nm.compare(0, 9, "Metadata/") != 0) return false;
        if (nm.compare(9, 6, "plate_")    != 0) return false;
        return (nm.size() >= 6 &&
                nm.compare(nm.size() - 6, 6, ".gcode") == 0) ||
               (nm.size() >= 10 &&
                nm.compare(nm.size() - 10, 10, ".gcode.md5") == 0);
    };

    mz_uint n_in = mz_zip_reader_get_num_files(&in);
    int n_copied = 0, n_skipped = 0;
    for (mz_uint i = 0; i < n_in; ++i) {
        char name[512];
        if (mz_zip_reader_get_filename(&in, i, name, sizeof(name)) == 0) continue;
        std::string nm(name);
        if (is_plate_gcode(nm)) { ++n_skipped; continue; }
        if (!mz_zip_writer_add_from_zip_reader(&out, &in, i)) {
            std::fprintf(stderr,
                "[lan-upload] dev=%s settings-only: copy of '%s' failed\n",
                dev_id.c_str(), nm.c_str());
            std::fflush(stderr);
            mz_zip_writer_end(&out);
            mz_zip_reader_end(&in);
            ::unlink(out_path.c_str());
            return {};
        }
        ++n_copied;
    }

    if (!mz_zip_writer_finalize_archive(&out) || !mz_zip_writer_end(&out)) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s settings-only: finalize failed\n",
            dev_id.c_str());
        std::fflush(stderr);
        mz_zip_reader_end(&in);
        ::unlink(out_path.c_str());
        return {};
    }
    mz_zip_reader_end(&in);

    std::fprintf(stderr,
        "[lan-upload] dev=%s built settings-only %s (copied=%d skipped=%d)\n",
        dev_id.c_str(), out_path.c_str(), n_copied, n_skipped);
    std::fflush(stderr);
    return out_path;
}

// ---------------------------------------------------------------------------
// Normalise plate naming in an Orca-produced .3mf so the H2D firmware can
// find the gcode the slicer's `print.gcode_file` MQTT command points at.
//
// Empirical (2026-06-02, "Nozzle temperature test" AND regular BENCHY from
// OrcaSlicer-bridge → bridge → H2D):
//   - Orca writes Metadata/plate_0.gcode, plate_0.png, plate_0.gcode.md5,
//     plate_no_light_0.png, top_0.png, pick_0.png, plate_0.json
//   - model_settings.config has plater_id="0" and gcode_file=
//     "Metadata/plate_0.gcode" (plus thumbnail_file etc. all _0)
//   - BUT slice_info.config says index="1" and the slicer's MQTT
//     print.gcode_file `param` is also plate_1-shaped
// Result: printer looks for plate_1.gcode, finds only plate_0.gcode,
// rejects with "didn't understand the file."
//
// Root cause is in Orca's bbs_3mf.cpp exporter (plate_data->plate_index is
// -1 at most write sites, 0 at the slice_info site); not yet root-caused
// upstream. This transform translates Orca's plate_0 archive to the BBS
// plate_1 shape the firmware expects. BBS-direct prints already have
// plate_1.gcode and are no-op'd.
//
// Inverse of the historical `rewrite_plate_to_zero` (which renamed
// plate_<N>.* → plate_0.* in the wrong direction; removed when the BBS
// flow proved plate_1 is what the firmware actually wants).
//
// Rewrites `in_path` in place via atomic rename of a sibling tempfile.
static bool normalise_orca_plate_to_one(const std::string& in_path,
                                        const std::string& dev_id) {
    mz_zip_archive in{};
    if (!mz_zip_reader_init_file(&in, in_path.c_str(), 0)) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s normalise-plate: open %s failed\n",
            dev_id.c_str(), in_path.c_str());
        std::fflush(stderr);
        return false;
    }

    // Detect Orca shape: plate_0.gcode present AND plate_1.gcode absent.
    // Either condition unmet = BBS-style or non-print spool; no-op.
    const mz_uint n_in = mz_zip_reader_get_num_files(&in);
    bool has_plate_0 = false, has_plate_1 = false;
    for (mz_uint i = 0; i < n_in; ++i) {
        char name[512];
        if (mz_zip_reader_get_filename(&in, i, name, sizeof(name)) == 0) continue;
        std::string nm(name);
        if (nm == "Metadata/plate_0.gcode") has_plate_0 = true;
        else if (nm == "Metadata/plate_1.gcode") has_plate_1 = true;
    }
    if (!has_plate_0 || has_plate_1) {
        mz_zip_reader_end(&in);
        std::fprintf(stderr,
            "[lan-upload] dev=%s normalise-plate: no-op "
            "(has_plate_0=%d has_plate_1=%d)\n",
            dev_id.c_str(), int(has_plate_0), int(has_plate_1));
        std::fflush(stderr);
        return true;
    }

    // Build the rewritten archive at a sibling path; rename atomically
    // on success.
    const std::string out_path = in_path + ".normalised";
    ::unlink(out_path.c_str());
    mz_zip_archive out{};
    if (!mz_zip_writer_init_file(&out, out_path.c_str(), 0)) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s normalise-plate: create %s failed\n",
            dev_id.c_str(), out_path.c_str());
        std::fflush(stderr);
        mz_zip_reader_end(&in);
        return false;
    }

    // Path remap. Only entries that match a known plate_0-shaped prefix
    // are renamed; everything else copies through unchanged. Tested
    // patterns from a 2026-06-02 H2D Orca capture:
    //   Metadata/plate_0.gcode, plate_0.gcode.md5, plate_0.png,
    //   plate_0_small.png, plate_0.json,
    //   Metadata/plate_no_light_0.png,
    //   Metadata/top_0.png, Metadata/pick_0.png
    auto remap_path = [](const std::string& nm) -> std::string {
        // Order matters: plate_no_light_0 prefix is longer than plate_0
        // — match the longer one first so we don't accidentally rewrite
        // "plate_no_light_0" to "plate_1_no_light_0".
        if (nm.rfind("Metadata/plate_no_light_0", 0) == 0)
            return "Metadata/plate_no_light_1" + nm.substr(25);
        if (nm.rfind("Metadata/plate_0", 0) == 0)
            return "Metadata/plate_1" + nm.substr(16);
        if (nm.rfind("Metadata/top_0", 0) == 0)
            return "Metadata/top_1" + nm.substr(14);
        if (nm.rfind("Metadata/pick_0", 0) == 0)
            return "Metadata/pick_1" + nm.substr(15);
        return nm;
    };

    auto str_replace_all = [](std::string s, const std::string& from,
                              const std::string& to) -> std::string {
        for (std::size_t p = 0; (p = s.find(from, p)) != std::string::npos;
             p += to.size())
            s.replace(p, from.size(), to);
        return s;
    };

    int n_renamed = 0, n_copied = 0;
    bool patched_model_settings = false;
    bool ok = true;

    for (mz_uint i = 0; i < n_in && ok; ++i) {
        char name[512];
        if (mz_zip_reader_get_filename(&in, i, name, sizeof(name)) == 0) continue;
        const std::string in_name(name);
        const std::string out_name = remap_path(in_name);

        if (in_name == "Metadata/model_settings.config") {
            // Extract → patch path references and plater_id → re-add.
            // The same plate_0/plate_no_light_0/top_0/pick_0 prefixes
            // appear inside as the value= of gcode_file / thumbnail_file
            // / thumbnail_no_light_file / top_file / pick_file /
            // pattern_bbox_file. Substring replace is safe because
            // those strings never legitimately appear elsewhere in this
            // file — XML attribute values are the only embedded paths.
            std::size_t sz = 0;
            void* data = mz_zip_reader_extract_to_heap(&in, i, &sz, 0);
            if (!data) { ok = false; break; }
            std::string body(static_cast<const char*>(data), sz);
            mz_free(data);
            body = str_replace_all(body, "Metadata/plate_no_light_0",
                                          "Metadata/plate_no_light_1");
            body = str_replace_all(body, "Metadata/plate_0",
                                          "Metadata/plate_1");
            body = str_replace_all(body, "Metadata/top_0",
                                          "Metadata/top_1");
            body = str_replace_all(body, "Metadata/pick_0",
                                          "Metadata/pick_1");
            // plater_id field is written as `plate_data->plate_index + 1`
            // in bbs_3mf.cpp:7917. Same -1 source → "0" here; bump to 1
            // so model_settings stays internally consistent with the
            // renamed plate files.
            body = str_replace_all(body, "key=\"plater_id\" value=\"0\"",
                                          "key=\"plater_id\" value=\"1\"");
            if (!mz_zip_writer_add_mem(&out, in_name.c_str(),
                                       body.data(), body.size(),
                                       MZ_DEFAULT_COMPRESSION)) {
                ok = false; break;
            }
            patched_model_settings = true;
            ++n_copied;
        } else if (out_name != in_name) {
            // Renamed entry: extract → re-add under new name.
            std::size_t sz = 0;
            void* data = mz_zip_reader_extract_to_heap(&in, i, &sz, 0);
            if (!data) { ok = false; break; }
            if (!mz_zip_writer_add_mem(&out, out_name.c_str(),
                                       data, sz, MZ_DEFAULT_COMPRESSION)) {
                mz_free(data);
                ok = false; break;
            }
            mz_free(data);
            ++n_renamed;
        } else {
            // Unchanged: bulk-copy preserving compression.
            if (!mz_zip_writer_add_from_zip_reader(&out, &in, i)) {
                ok = false; break;
            }
            ++n_copied;
        }
    }

    if (ok && !mz_zip_writer_finalize_archive(&out)) ok = false;
    if (!mz_zip_writer_end(&out))                    ok = false;
    mz_zip_reader_end(&in);

    if (!ok) {
        ::unlink(out_path.c_str());
        std::fprintf(stderr,
            "[lan-upload] dev=%s normalise-plate: rewrite failed\n",
            dev_id.c_str());
        std::fflush(stderr);
        return false;
    }
    if (::rename(out_path.c_str(), in_path.c_str()) != 0) {
        std::fprintf(stderr,
            "[lan-upload] dev=%s normalise-plate: rename %s -> %s "
            "failed: %s\n",
            dev_id.c_str(), out_path.c_str(), in_path.c_str(),
            std::strerror(errno));
        std::fflush(stderr);
        ::unlink(out_path.c_str());
        return false;
    }
    std::fprintf(stderr,
        "[lan-upload] dev=%s normalise-plate: rewrote %s "
        "(renamed=%d copied=%d patched_model_settings=%d)\n",
        dev_id.c_str(), in_path.c_str(),
        n_renamed, n_copied, int(patched_model_settings));
    std::fflush(stderr);
    return true;
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

    // GUI parity — short-circuit the access-code verification probe.
    // Orca's PrintJob::process / SendJob::process call
    // `start_send_gcode_to_sdcard(check_access_code.txt, "verify_job")`
    // BEFORE the real upload to confirm the LAN access code is correct.
    // The proprietary plugin opens an FTPS-TLS connection to the
    // printer's FTPS endpoint (here: the bridge's FTPS server) and
    // STORs the file. If TLS-handshake + STOR succeed, the access code
    // is good and the slicer proceeds with the real upload. If they
    // fail, the slicer raises `m_enter_ip_address_fun_fail()` and
    // re-prompts for IP + access code — that's the dialog the user
    // sees every time they click Send.
    //
    // Identification: the body is the verbatim contents of
    // resources/check_access_code.txt — the 16 ASCII bytes
    // "just a test file" — so content-match is the most reliable
    // discriminator. The plugin observed in libbambu_networking
    // 02.06.01.55 (May 2026) issues the STOR with an EMPTY remote
    // path, not `check_access_code.txt`, so a filename-based check
    // misses it. We don't trust the filename here, only the bytes.
    //
    // The bridge's FTPS server already validated the TLS handshake
    // and the access-code-as-password during the AUTH/USER/PASS dance
    // before this STOR landed — accept and confirm.
    static constexpr const char  kProbeBody[]    = "just a test file";
    static constexpr std::size_t kProbeBodySize  = sizeof(kProbeBody) - 1;
    if (job.content.size() == kProbeBodySize &&
        std::memcmp(job.content.data(), kProbeBody, kProbeBodySize) == 0) {
        res.ok         = true;
        res.remote_url = "bambu-lan:///model/check_access_code.txt";
        std::fprintf(stderr,
            "[lan-upload] dev=%s access-code probe "
            "(16-byte 'just a test file') accepted "
            "without forwarding to plugin\n",
            job.dev_id.c_str());
        std::fflush(stderr);
        return res;
    }

    std::string tmp_path = spool_upload_to_tempfile(job);
    if (tmp_path.empty()) {
        res.ok            = false;
        res.error_message =
            std::string("LanUploadSink: spool failed: ") + std::strerror(errno);
        return res;
    }

    // Normalise Orca-shaped plate_0.* archives to plate_1.* (no-op for
    // BBS-style spools that are already plate_1). Runs BEFORE the debug
    // snapshot, the settings-only zip, and the spool hard-link so all
    // downstream artefacts see the corrected paths. See the helper above.
    normalise_orca_plate_to_one(tmp_path, job.dev_id);

    // (Historically the bridge ran two .3mf transforms here:
    //  `rewrite_plate_to_zero` renamed Metadata/plate_<N>.* → plate_0.*,
    //  and `inject_filament_settings` synthesised 11 generic
    //  filament_settings_<N>.config files. Empirical comparison against
    //  a BBS-direct print proved both were the CAUSE of HMS
    //  0700700000020008 rejection, not the cure: the plate rename
    //  shifted file names but not Metadata/slice_info.config's
    //  index=1 so the printer couldn't find the renamed file, and the
    //  injected filament_settings content mismatched the actually-
    //  loaded filament. Removed; see memory `project_orca_3mf_filament
    //  _settings`. The slicer-side mirror is also removed — plates
    //  start at 1 end-to-end. The new `normalise_orca_plate_to_one`
    //  call above is the inverse: bring Orca-style plate_0 archives up
    //  to plate_1 so the firmware finds the gcode it's told to print.)

    // Debug snapshot — copy of every spooled .3mf retained at a stable
    // path per dev_id so we can inspect what the slicer is actually
    // sending the plugin (size, ZIP signature, metadata). Same path is
    // overwritten on each upload. Cleanup is on next upload's overwrite,
    // so the most recent attempt is always inspectable.
    {
        std::string dbg = "/tmp/bridge-last-upload-" + job.dev_id + ".3mf";
        struct stat st{};
        if (::stat(tmp_path.c_str(), &st) == 0) {
            std::fprintf(stderr,
                "[lan-upload] spool dev=%s bytes=%lld path=%s\n",
                job.dev_id.c_str(),
                static_cast<long long>(st.st_size),
                tmp_path.c_str());
            std::fflush(stderr);
            // Hard-link the snapshot so the plugin still sees the
            // original path and we still get to inspect the bytes after
            // the per-job tempdir is rmdir'd. Falls back to a plain copy
            // if link() fails (cross-filesystem etc.).
            ::unlink(dbg.c_str());
            if (::link(tmp_path.c_str(), dbg.c_str()) != 0) {
                FILE* in  = std::fopen(tmp_path.c_str(), "rb");
                FILE* out = std::fopen(dbg.c_str(),      "wb");
                if (in && out) {
                    char buf[64 * 1024];
                    std::size_t n;
                    while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
                        std::fwrite(buf, 1, n, out);
                }
                if (in)  std::fclose(in);
                if (out) std::fclose(out);
            }
        }
    }

    // Build a settings-only `.3mf` to pass as config_filename. The
    // GUI's PrintJob (verified via plugin-trace 2026-05-30) feeds the
    // plugin TWO `.3mf` files:
    //   filename        = full combined .3mf (`.<pid>.<idx>.3mf`)
    //   config_filename = settings-only .3mf (`.<pid>.<idx>_config.3mf`)
    // We don't have to extract the gcode anymore (the plugin handles
    // that internally when connection_type="cloud"); we just need to
    // produce a config-only sibling. With config_filename empty the
    // plugin returns -3070 "3mf is not exists".
    std::string settings_path = make_settings_only_zip(tmp_path, job.dev_id);

    // Per the GUI plugin-trace (captured 2026-05-30 from a working
    // H2D print): even when the printer is LAN-reachable, the GUI's
    // PrintJob calls start_local_print_with_record with
    //   filename        = combined .3mf
    //   config_filename = settings-only _config.3mf
    //   connection_type = "cloud"   ← NOT "lan"
    // That routes the plugin onto the cloud-OSS upload + signed MQTT
    // print-start path (the same one BENCHY worked on), instead of the
    // broken legacy CWD-model FTPS-990 path on current H2D firmware.
    BambuNetworkingPluginHandle::LocalPrintParams lp;
    lp.dev_id           = job.dev_id;
    lp.dev_ip           = dev.printer_ip;
    lp.access_code      = dev.access_code;
    lp.local_file_path  = tmp_path;
    lp.config_filename  = settings_path.empty() ? std::string{} : settings_path;
    lp.project_name     = job.filename;
    lp.connection_type  = "cloud";
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

    // Spool ONLY. The bridge is a pass-through: the file goes into a
    // per-dev_id staging directory keyed by the STOR remote name. When
    // the slicer's matching `print.command=gcode_file` MQTT command
    // arrives (a moment later — see virtual_lan_print_ in NetworkAgent.cpp)
    // it carries the full AMS mapping / plate / cali context. At that
    // point MqttBroker calls back into LanUploadSink::dispatch_print_command
    // which looks up the spool here and invokes the plugin's
    // start_local_print_with_record on the real dev_id (or start_print
    // for A1-class printers via the adapter's built-in fallback).
    //
    // This decoupling is what lets the bridge map the slicer's intent
    // (a slicer-side gcode_file MQTT) onto the appropriate real-printer
    // command (LAN-FTPS+MQTT for H2D/H2S/X1/P1, or cloud-relay for A1
    // and forced-cloud H2 firmware) — without the bridge having to
    // re-derive the AMS context the slicer already had.
    //
    // Stable per-dev spool location: `/tmp/bridge-spool/<dev_id>/<filename>`.
    // Same dev_id + filename on the next print just overwrites — fine,
    // there's only one job in flight per dev_id at a time. The per-job
    // tempfile from spool_upload_to_tempfile is hard-linked here and
    // then cleanup_upload_tempfile rmdir's the tempfile's containing
    // dir — the spool path survives because of the link.
    std::string spool_dir = "/tmp/bridge-spool/" + job.dev_id;
    ::mkdir("/tmp/bridge-spool", 0700);
    ::mkdir(spool_dir.c_str(), 0700);
    std::string spool_basename =
        job.filename.empty() ? std::string("lan_print.3mf") : job.filename;
    // Strip any leading '/' (job.filename is whatever the slicer sent in
    // its STOR remote name; harmless to defend against accidental paths).
    while (!spool_basename.empty() && spool_basename.front() == '/')
        spool_basename.erase(0, 1);
    std::string spool_path = spool_dir + "/" + spool_basename;
    ::unlink(spool_path.c_str());
    if (::link(tmp_path.c_str(), spool_path.c_str()) != 0) {
        // Cross-fs / EXDEV fallback: stream-copy.
        FILE* in_f  = std::fopen(tmp_path.c_str(),  "rb");
        FILE* out_f = std::fopen(spool_path.c_str(), "wb");
        if (in_f && out_f) {
            char buf[64 * 1024];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), in_f)) > 0)
                std::fwrite(buf, 1, n, out_f);
        }
        if (in_f)  std::fclose(in_f);
        if (out_f) std::fclose(out_f);
    }
    // Persist the settings-only sidecar alongside the spool. Both
    // legs of the plugin's start_local_print_with_record need it
    // (LAN-FTPS path uploads the config to OSS for record; cloud-
    // relay path uses it as the project_file config). Without it the
    // plugin returns -2030 (config-to-OSS failed) on the LAN leg
    // and -3070 on the cloud-fallback leg.
    //
    // settings_path (made earlier by make_settings_only_zip on the
    // per-job tempfile) currently sits in /tmp/bridge-upload-XXX/
    // which is about to be cleanup_upload_tempfile-rmdir'd. Hard-
    // link it next to the main spool so it survives.
    std::string spool_config_path;
    if (!settings_path.empty()) {
        auto dot = spool_basename.find_last_of('.');
        std::string stem = (dot == std::string::npos)
                           ? spool_basename : spool_basename.substr(0, dot);
        spool_config_path = spool_dir + "/" + stem + "_config.3mf";
        ::unlink(spool_config_path.c_str());
        if (::link(settings_path.c_str(), spool_config_path.c_str()) != 0) {
            // EXDEV fallback: stream-copy.
            FILE* in_f  = std::fopen(settings_path.c_str(),  "rb");
            FILE* out_f = std::fopen(spool_config_path.c_str(), "wb");
            if (in_f && out_f) {
                char buf[64 * 1024];
                std::size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), in_f)) > 0)
                    std::fwrite(buf, 1, n, out_f);
            }
            if (in_f)  std::fclose(in_f);
            if (out_f) std::fclose(out_f);
            // Verify the copy actually produced a file before
            // claiming we have a config sidecar.
            struct stat st{};
            if (::stat(spool_config_path.c_str(), &st) != 0 || st.st_size == 0) {
                spool_config_path.clear();
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_spool_paths[job.dev_id][spool_basename] =
            SpoolEntry{spool_path, spool_config_path};
    }
    std::fprintf(stderr,
        "[lan-upload] dev=%s spooled %s (%lld bytes) at %s "
        "config=%s — waiting for matching gcode_file MQTT to dispatch\n",
        job.dev_id.c_str(),
        spool_basename.c_str(),
        (long long)([&]{ struct stat st{}; ::stat(spool_path.c_str(), &st); return st.st_size; })(),
        spool_path.c_str(),
        spool_config_path.empty() ? "<none>" : spool_config_path.c_str());
    std::fflush(stderr);

    // Now clean up the per-job tempdir — the hard link above keeps the
    // bytes alive at spool_path.
    if (!settings_path.empty() && settings_path != tmp_path)
        ::unlink(settings_path.c_str());
    cleanup_upload_tempfile(tmp_path);

    res.ok = true;
    res.remote_url = "bambu-lan:///spool/" + spool_basename;
    return res;
}

// Per-virtual-dev progress file. The slicer's virtual_lan_print_ polls
// this and calls its update_fn for each event so the user's BBS print
// dialog stays open while the bridge's plugin call uploads to the real
// printer and waits for the print to actually start (~10-20 s after
// BBS finishes its FTPS upload to the bridge).
static void write_bridge_progress(const std::string& virtual_dev_id,
                                  int stage, int code,
                                  const std::string& info,
                                  const std::string& phase) {
    if (virtual_dev_id.empty()) return;
    ::mkdir("/tmp/bridge-progress", 0700);
    std::string path = "/tmp/bridge-progress/" + virtual_dev_id + ".json";
    std::string tmp_path = path + ".tmp";
    FILE* f = std::fopen(tmp_path.c_str(), "wb");
    if (!f) return;
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // Compact JSON; slicer parses with nlohmann.
    std::fprintf(f,
        "{\"ts_ms\":%lld,\"stage\":%d,\"code\":%d,"
        "\"info\":\"%s\",\"phase\":\"%s\"}\n",
        (long long) now_ms, stage, code,
        info.c_str(),  // info is plugin-formatted, safe ASCII
        phase.c_str());
    std::fclose(f);
    // Atomic publish — rename so the slicer never reads a half-written file.
    ::rename(tmp_path.c_str(), path.c_str());
}

int LanUploadSink::dispatch_print_command(const std::string& dev_id,
                                          const std::string& virtual_dev_id,
                                          const std::string& mqtt_payload_json) {
    // Pull the spooled file path + the slicer's full LocalPrintParams
    // context out of the MQTT JSON. The slicer publishes this on
    // `device/<sn>/request` with the shape virtual_lan_print_ in
    // NetworkAgent.cpp constructs.
    nlohmann::json root;
    try { root = nlohmann::json::parse(mqtt_payload_json); }
    catch (...) {
        std::fprintf(stderr,
            "[lan-upload] dispatch dev=%s parse error\n", dev_id.c_str());
        std::fflush(stderr);
        return -1;
    }
    auto pit = root.find("print");
    if (pit == root.end() || !pit->is_object()) return -1;
    const auto& p = *pit;
    auto cmd_it = p.find("command");
    if (cmd_it == p.end() || !cmd_it->is_string()
        || cmd_it->get<std::string>() != "gcode_file")
        return -1;

    auto get_str = [&](const char* k) -> std::string {
        auto it = p.find(k);
        if (it == p.end() || it->is_null()) return {};
        if (it->is_string()) return it->get<std::string>();
        return it->dump();
    };
    auto get_int = [&](const char* k, int fallback = 0) -> int {
        auto it = p.find(k);
        if (it == p.end()) return fallback;
        if (it->is_number_integer()) return it->get<int>();
        if (it->is_string()) {
            try { return std::stoi(it->get<std::string>()); }
            catch (...) {}
        }
        return fallback;
    };
    auto get_bool = [&](const char* k, bool fallback = false) -> bool {
        auto it = p.find(k);
        if (it == p.end() || it->is_null()) return fallback;
        if (it->is_boolean()) return it->get<bool>();
        return fallback;
    };

    // `print.param` carries the file reference from the slicer (matches
    // virtual_lan_print_'s remote_path = folder + fname).
    std::string param = get_str("param");
    std::string filename = param;
    // Strip the folder prefix the slicer composed.
    auto slash = filename.find_last_of('/');
    if (slash != std::string::npos) filename = filename.substr(slash + 1);

    // Look up spool (main + config sidecar).
    std::string spool_path;
    std::string spool_config_path;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_spool_paths.find(dev_id);
        if (it != m_spool_paths.end()) {
            auto f = it->second.find(filename);
            if (f != it->second.end()) {
                spool_path        = f->second.main_path;
                spool_config_path = f->second.config_path;
            }
        }
    }
    if (spool_path.empty()) {
        std::fprintf(stderr,
            "[lan-upload] dispatch dev=%s file=%s NOT spooled — "
            "ignoring (probe / out-of-order publish)\n",
            dev_id.c_str(), filename.c_str());
        std::fflush(stderr);
        return -2;
    }

    // Resolve the per-device routing config so we can fill dev_ip etc.
    LanUploadSinkDevice dev{};
    std::shared_ptr<BambuNetworkingPluginHandle> handle;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto dit = m_devices.find(dev_id);
        if (dit != m_devices.end()) dev = dit->second;
        handle = m_handle;
    }
    if (!handle) {
        std::fprintf(stderr,
            "[lan-upload] dispatch dev=%s no plugin handle attached\n",
            dev_id.c_str());
        std::fflush(stderr);
        return -3;
    }

    // Build LocalPrintParams from the MQTT JSON. The shapes virtual_lan_print_
    // emits map one-to-one to LocalPrintParams field names — except for
    // (use_ams|task_use_ams), where the slicer sets BOTH and we accept
    // either as the source.
    BambuNetworkingPluginHandle::LocalPrintParams lp;
    lp.dev_id           = dev_id;
    lp.dev_ip           = dev.printer_ip;
    lp.access_code      = dev.access_code;
    lp.local_file_path  = spool_path;
    lp.config_filename  = spool_config_path; // settings-only sidecar;
                                             // empty if make_settings_only_zip
                                             // failed for this upload
    lp.project_name     = get_str("project_name");
    if (lp.project_name.empty()) lp.project_name = filename;
    lp.task_name        = get_str("task_name");
    lp.connection_type  = "cloud";   // mirrors successful BBL GUI trace
    lp.use_ssl_for_ftp  = true;
    lp.use_ssl_for_mqtt = true;
    lp.plate_index      = get_int("plate_idx", 0);
    lp.ams_mapping      = get_str("ams_mapping");
    lp.ams_mapping2     = get_str("ams_mapping2");
    lp.ams_mapping_info = get_str("ams_mapping_info");
    lp.nozzles_info     = get_str("nozzles_info");
    lp.nozzle_mapping   = get_str("nozzle_mapping");
    lp.task_bed_type    = get_str("task_bed_type");
    lp.task_use_ams     = get_bool("task_use_ams", get_bool("use_ams", false));
    lp.task_bed_leveling    = get_bool("bed_leveling");
    lp.task_flow_cali       = get_bool("flow_cali");
    lp.task_vibration_cali  = get_bool("vibration_cali");
    lp.task_layer_inspect   = get_bool("layer_inspect");
    lp.task_record_timelapse= get_bool("timelapse");
    lp.auto_bed_leveling    = get_int("auto_bed_leveling", 0);
    lp.auto_flow_cali       = get_int("auto_flow_cali", 0);
    lp.auto_offset_cali     = get_int("auto_offset_cali", 0);
    lp.origin_model_id      = get_str("model_id");

    std::fprintf(stderr,
        "[lan-upload] dispatch dev=%s vdev=%s file=%s project=%s "
        "ams_mapping_len=%zu task_use_ams=%d plate_idx=%d\n",
        dev_id.c_str(), virtual_dev_id.c_str(), filename.c_str(),
        lp.project_name.c_str(), lp.ams_mapping.size(),
        int(lp.task_use_ams), lp.plate_index);
    std::fflush(stderr);

    // ------------------------------------------------------------------
    // Full-trace + 3mf capture (BAMBU_BRIDGE_CAPTURE).
    //
    // The dispatch site has both the spool path (.3mf + _config.3mf
    // already persisted by handle_upload_finish) and the complete
    // mapping payloads parsed out of the slicer's gcode_file MQTT.
    // We snapshot all of them into one per-print directory so the
    // .3mf can't be overwritten by the next print and the mapping
    // JSON is preserved next to the model that produced it.
    //
    //   /tmp/bridge-capture/<dev_id>/<UTC-ts>_plate<N>/
    //     ├── <basename>.3mf
    //     ├── <basename>_config.3mf      (if present)
    //     └── meta.json                  (ams_mapping*, nozzles_info,
    //                                     project_name, plate_idx, …)
    //
    // Default-on; set BAMBU_BRIDGE_CAPTURE=0 to disable.
    // ------------------------------------------------------------------
    {
        const char* cap_env = std::getenv("BAMBU_BRIDGE_CAPTURE");
        const bool  cap_on  = !cap_env || std::strcmp(cap_env, "0") != 0;
        if (cap_on) {
            // Timestamped, plate-tagged directory name.
            std::time_t now_t = std::time(nullptr);
            struct tm tm{};
#ifdef _WIN32
            ::gmtime_s(&tm, &now_t);
#else
            ::gmtime_r(&now_t, &tm);
#endif
            char ts_buf[64];
            std::snprintf(ts_buf, sizeof(ts_buf),
                "%04d%02d%02dT%02d%02d%02d.%03ldZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                long(ts.tv_nsec / 1000000));
            std::string cap_root = "/tmp/bridge-capture";
            std::string cap_dev  = cap_root + "/" + dev_id;
            char plate_buf[32];
            std::snprintf(plate_buf, sizeof(plate_buf), "_plate%d", lp.plate_index);
            std::string cap_dir  = cap_dev + "/" + ts_buf + plate_buf;
            ::mkdir(cap_root.c_str(), 0700);
            ::mkdir(cap_dev.c_str(),  0700);
            int mkdir_rc = ::mkdir(cap_dir.c_str(),  0700);

            // Best-effort hard-link copies; on EXDEV fall back to stream-copy.
            auto link_or_copy = [](const std::string& src,
                                    const std::string& dst) -> bool {
                if (src.empty()) return false;
                ::unlink(dst.c_str());
                if (::link(src.c_str(), dst.c_str()) == 0) return true;
                FILE* in_f  = std::fopen(src.c_str(),  "rb");
                FILE* out_f = std::fopen(dst.c_str(), "wb");
                if (!in_f || !out_f) {
                    if (in_f)  std::fclose(in_f);
                    if (out_f) std::fclose(out_f);
                    return false;
                }
                char buf[64 * 1024];
                std::size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), in_f)) > 0)
                    std::fwrite(buf, 1, n, out_f);
                std::fclose(in_f);
                std::fclose(out_f);
                return true;
            };

            // Use a sane basename for the capture copy (the spool name
            // can be a pid-counter dotfile from the slicer's tempfile).
            std::string cap_basename = lp.project_name;
            if (cap_basename.empty()) cap_basename = filename;
            if (cap_basename.empty()) cap_basename = "lan_print";
            // Strip any path and trailing .3mf so we can append.
            if (auto p = cap_basename.find_last_of('/'); p != std::string::npos)
                cap_basename = cap_basename.substr(p + 1);
            if (cap_basename.size() >= 4 &&
                cap_basename.compare(cap_basename.size() - 4, 4, ".3mf") == 0)
                cap_basename.resize(cap_basename.size() - 4);

            const std::string cap_3mf    = cap_dir + "/" + cap_basename + ".3mf";
            const std::string cap_cfg    = cap_dir + "/" + cap_basename + "_config.3mf";
            const std::string cap_meta   = cap_dir + "/meta.json";
            const bool got_3mf = link_or_copy(spool_path,        cap_3mf);
            const bool got_cfg = link_or_copy(spool_config_path, cap_cfg);

            // meta.json — full mapping payloads + the surrounding scalars.
            // The mapping fields arrive as already-serialised JSON strings;
            // we emit them as raw JSON values (no nested quoting) so they're
            // trivially parseable by `jq`. Anything malformed gets wrapped
            // in a string literal as a fallback.
            auto json_value_or_string = [](const std::string& s) -> std::string {
                if (s.empty()) return "null";
                // Cheap heuristic — if it parses as JSON, emit raw; else
                // emit as a quoted string. We don't need to be strict.
                try {
                    auto j = nlohmann::json::parse(s);
                    return j.dump();
                } catch (...) {
                    return nlohmann::json(s).dump();
                }
            };
            auto esc = [](const std::string& s) -> std::string {
                return nlohmann::json(s).dump();
            };
            std::string meta;
            meta.reserve(8192);
            meta += "{\n";
            meta += "  \"timestamp_utc\": "    + esc(ts_buf) + ",\n";
            meta += "  \"dev_id\": "           + esc(dev_id) + ",\n";
            meta += "  \"virtual_dev_id\": "   + esc(virtual_dev_id) + ",\n";
            meta += "  \"project_name\": "     + esc(lp.project_name) + ",\n";
            meta += "  \"task_name\": "        + esc(lp.task_name) + ",\n";
            meta += "  \"slicer_filename\": "  + esc(filename) + ",\n";
            meta += "  \"plate_index\": "      + std::to_string(lp.plate_index) + ",\n";
            meta += "  \"task_use_ams\": "     + std::string(lp.task_use_ams ? "true" : "false") + ",\n";
            meta += "  \"task_bed_type\": "    + esc(lp.task_bed_type) + ",\n";
            meta += "  \"ams_mapping\": "      + json_value_or_string(lp.ams_mapping) + ",\n";
            meta += "  \"ams_mapping2\": "     + json_value_or_string(lp.ams_mapping2) + ",\n";
            meta += "  \"ams_mapping_info\": " + json_value_or_string(lp.ams_mapping_info) + ",\n";
            meta += "  \"nozzles_info\": "     + json_value_or_string(lp.nozzles_info) + ",\n";
            meta += "  \"nozzle_mapping\": "   + json_value_or_string(lp.nozzle_mapping) + ",\n";
            meta += "  \"captured_3mf\": "     + std::string(got_3mf ? "true" : "false") + ",\n";
            meta += "  \"captured_config\": "  + std::string(got_cfg ? "true" : "false") + "\n";
            meta += "}\n";
            if (FILE* mf = std::fopen(cap_meta.c_str(), "w")) {
                std::fwrite(meta.data(), 1, meta.size(), mf);
                std::fclose(mf);
            }

            // Echo the per-field bodies to stderr too — same info, no need
            // to crack open meta.json to see what a print sent.
            std::fprintf(stderr,
                "[lan-upload] CAPTURE dir=%s mkdir_rc=%d 3mf=%d cfg=%d\n",
                cap_dir.c_str(), mkdir_rc, int(got_3mf), int(got_cfg));
            std::fprintf(stderr,
                "[lan-upload]   ams_mapping=%s\n", lp.ams_mapping.c_str());
            std::fprintf(stderr,
                "[lan-upload]   ams_mapping2=%s\n", lp.ams_mapping2.c_str());
            std::fprintf(stderr,
                "[lan-upload]   ams_mapping_info=%s\n", lp.ams_mapping_info.c_str());
            std::fprintf(stderr,
                "[lan-upload]   nozzles_info=%s\n", lp.nozzles_info.c_str());
            std::fprintf(stderr,
                "[lan-upload]   nozzle_mapping=%s\n", lp.nozzle_mapping.c_str());
            std::fflush(stderr);
        }
    }

    // Publish the "dispatch starting" marker BEFORE the plugin call so
    // the slicer's progress poll sees us advance immediately (otherwise
    // there's a perceptible blank gap between BBS finishing the FTPS
    // upload and seeing real plugin progress).
    write_bridge_progress(virtual_dev_id, 0, 0, "", "dispatching");

    // The plugin's update_fn fires from the proprietary plugin's worker
    // thread; we forward each event to the bridge-progress file so the
    // slicer can mirror real progress in its print dialog. The adapter
    // already passes a `make_update_fn` closure for its own stderr
    // logging — but that closure is hardcoded inside the adapter. We
    // install OURS by invoking the plugin call indirectly through the
    // upload-route the dispatch chain already uses: the handle's
    // start_local_print_with_record. Unfortunately that interface
    // doesn't take an update_fn — it routes through the adapter's
    // PrintDispatcher which builds its own closure. To get progress
    // events out we'd need to crack the adapter open further. For now,
    // emit a single "dispatching" marker before the call and a final
    // marker (acked / done / failed) after; the slicer at least sees
    // "uploading" → "starting" → "done" rather than nothing. The
    // intermediate stage=4 percent stream can be added by routing the
    // plugin call through a sibling code path that exposes update_fn —
    // tracked as a separate refactor.
    int rc = handle->start_local_print_with_record(lp);
    std::fprintf(stderr,
        "[lan-upload] dispatch dev=%s rc=%d\n", dev_id.c_str(), rc);
    std::fflush(stderr);

    // Publish the terminal marker. `phase=done` (rc==0) tells the
    // slicer's poll loop to stop waiting and return success;
    // `phase=failed` returns the error to the slicer's PrintJob UI
    // instead of silently completing.
    write_bridge_progress(virtual_dev_id, 0, rc,
        rc == 0 ? std::string("ok") : std::string("rc=") + std::to_string(rc),
        rc == 0 ? "done" : "failed");
    return rc;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
