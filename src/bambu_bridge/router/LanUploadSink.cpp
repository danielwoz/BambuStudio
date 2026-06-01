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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

// Normalise plate index in an Orca/Bambu .3mf so the printer firmware can
// open it. Bambu's firmware (and BambuStudio's cloud project_file path,
// per H2D-cloud.yaml) always references `Metadata/plate_0.gcode`. Orca's
// PartPlate exporter writes the active plate as `Metadata/plate_<N>.gcode`
// where N is the UI plate index (1-based) — so a single-plate slice ends
// up at `plate_1.gcode` and the printer reports "couldn't read file".
//
// The fix: rewrite the .3mf in place. Find any `Metadata/plate_<N>.<ext>`
// entries (gcode, gcode.md5, png, _small.png, _no_light.png, .json, top_,
// pick_) and rename to `plate_0.<ext>`. Patch `Metadata/model_settings.config`
// so its `gcode_file`/`thumbnail_file`/etc. references match. Idempotent —
// re-running on a .3mf that's already plate_0 is a no-op.
//
// Returns true if the file was rewritten (in place), false otherwise.
// On any failure the original file is left untouched.
static bool rewrite_plate_to_zero(const std::string& threemf_path,
                                  const std::string& dev_id) {
    mz_zip_archive in{};
    if (!mz_zip_reader_init_file(&in, threemf_path.c_str(), 0)) {
        std::fprintf(stderr,
            "[plate-rewrite] dev=%s open input %s failed\n",
            dev_id.c_str(), threemf_path.c_str());
        std::fflush(stderr);
        return false;
    }

    // Detect the source plate index by scanning for a `Metadata/plate_<N>.gcode`
    // entry. If we find plate_0 already, nothing to do. If we find plate_<N>
    // for N > 0, that's our source index.
    int        src_idx = -1;
    mz_uint    n_files = mz_zip_reader_get_num_files(&in);
    for (mz_uint i = 0; i < n_files; ++i) {
        char name[512];
        if (mz_zip_reader_get_filename(&in, i, name, sizeof(name)) == 0) continue;
        std::string nm(name);
        if (nm.size() <= 15) continue;
        if (nm.compare(0, 15, "Metadata/plate_") != 0) continue;
        // Must end in ".gcode" so we don't pick up `.gcode.md5` as the lead
        // (we accept the lead by gcode alone and rename its siblings by N).
        if (nm.size() < 6 || nm.compare(nm.size() - 6, 6, ".gcode") != 0) continue;
        std::size_t dot = nm.find('.', 15);
        if (dot == std::string::npos) continue;
        try {
            src_idx = std::stoi(nm.substr(15, dot - 15));
            break;
        } catch (...) {}
    }

    if (src_idx <= 0) {
        // Already plate_0 (idx==0) or no plate_*.gcode entry at all (16-byte
        // probe / non-3mf upload). Either way: no rewrite.
        mz_zip_reader_end(&in);
        return false;
    }

    const std::string src_tag = "plate_" + std::to_string(src_idx);
    const std::string dst_tag = "plate_0";

    auto pos = threemf_path.find_last_of('/');
    std::string dir = (pos == std::string::npos) ? "/tmp"
                                                 : threemf_path.substr(0, pos);
    std::string out_path = threemf_path + ".rewrite.tmp";
    ::unlink(out_path.c_str());

    mz_zip_archive out{};
    if (!mz_zip_writer_init_file(&out, out_path.c_str(), 0)) {
        std::fprintf(stderr,
            "[plate-rewrite] dev=%s open output %s failed\n",
            dev_id.c_str(), out_path.c_str());
        std::fflush(stderr);
        mz_zip_reader_end(&in);
        return false;
    }

    auto rename_entry = [&](const std::string& nm) {
        // Replace `plate_<N>` with `plate_0` everywhere in the entry name.
        // Two known shapes:
        //   Metadata/plate_<N>.<ext>          (gcode, gcode.md5, png, json)
        //   Metadata/plate_no_light_<N>.png   (special case)
        //   Metadata/top_<N>.png
        //   Metadata/pick_<N>.png
        //   Metadata/plate_<N>_small.png
        std::string out = nm;
        auto subst = [&](const std::string& needle, const std::string& with) {
            std::size_t p = 0;
            while ((p = out.find(needle, p)) != std::string::npos) {
                out.replace(p, needle.size(), with);
                p += with.size();
            }
        };
        subst(src_tag, dst_tag);
        // Sibling-thumbnail naming variants (must use the same src_idx):
        const std::string n = std::to_string(src_idx);
        subst("plate_no_light_" + n, "plate_no_light_0");
        subst("top_" + n,             "top_0");
        subst("pick_" + n,            "pick_0");
        return out;
    };

    int n_renamed = 0, n_copied = 0;
    bool ok = true;
    for (mz_uint i = 0; i < n_files; ++i) {
        char name[512];
        if (mz_zip_reader_get_filename(&in, i, name, sizeof(name)) == 0) continue;
        std::string nm(name);
        std::string new_nm = rename_entry(nm);

        // model_settings.config carries gcode_file / thumbnail / pick / top
        // string refs that themselves need updating. Read, rewrite, write
        // explicitly. Every other entry just gets a rename via
        // `mz_zip_writer_add_mem` (we can't use add_from_zip_reader because
        // that preserves the source name).
        std::size_t sz = 0;
        std::vector<unsigned char> buf;
        {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&in, i, &st)) { ok = false; break; }
            buf.resize(static_cast<std::size_t>(st.m_uncomp_size));
            if (st.m_uncomp_size > 0
                && !mz_zip_reader_extract_to_mem(&in, i, buf.data(),
                                                  buf.size(), 0)) {
                ok = false;
                break;
            }
        }

        if (new_nm == "Metadata/model_settings.config") {
            // Patch every `plate_<N>` occurrence (handles the metadata refs
            // to gcode_file, thumbnail_file, pick_file, top_file etc.) and
            // `plater_id" value="<N>"`. Simple string replace — the XML
            // wrapping is unchanged so this is safe without a full parser.
            std::string xml(buf.begin(), buf.end());
            auto subst_all = [&](const std::string& needle,
                                 const std::string& with) {
                std::size_t p = 0;
                while ((p = xml.find(needle, p)) != std::string::npos) {
                    xml.replace(p, needle.size(), with);
                    p += with.size();
                }
            };
            const std::string n = std::to_string(src_idx);
            subst_all(src_tag,                       dst_tag);
            subst_all("plate_no_light_" + n,         "plate_no_light_0");
            subst_all("top_" + n,                    "top_0");
            subst_all("pick_" + n,                   "pick_0");
            subst_all("plater_id\" value=\"" + n + "\"",
                      "plater_id\" value=\"0\"");
            buf.assign(xml.begin(), xml.end());
        }

        sz = buf.size();
        if (!mz_zip_writer_add_mem(&out,
                                   new_nm.c_str(),
                                   sz == 0 ? nullptr : buf.data(),
                                   sz,
                                   MZ_DEFAULT_COMPRESSION)) {
            std::fprintf(stderr,
                "[plate-rewrite] dev=%s add '%s' -> '%s' failed\n",
                dev_id.c_str(), nm.c_str(), new_nm.c_str());
            std::fflush(stderr);
            ok = false;
            break;
        }
        if (new_nm != nm) ++n_renamed;
        ++n_copied;
    }

    bool finalize_ok = ok &&
        mz_zip_writer_finalize_archive(&out) &&
        mz_zip_writer_end(&out);
    mz_zip_reader_end(&in);

    if (!finalize_ok) {
        std::fprintf(stderr,
            "[plate-rewrite] dev=%s finalize failed; leaving original\n",
            dev_id.c_str());
        std::fflush(stderr);
        ::unlink(out_path.c_str());
        return false;
    }

    if (::rename(out_path.c_str(), threemf_path.c_str()) != 0) {
        std::fprintf(stderr,
            "[plate-rewrite] dev=%s rename %s -> %s failed: %s\n",
            dev_id.c_str(), out_path.c_str(), threemf_path.c_str(),
            std::strerror(errno));
        std::fflush(stderr);
        ::unlink(out_path.c_str());
        return false;
    }

    std::fprintf(stderr,
        "[plate-rewrite] dev=%s normalised plate_%d -> plate_0 "
        "(entries copied=%d renamed=%d)\n",
        dev_id.c_str(), src_idx, n_copied, n_renamed);
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

    // Normalise OrcaSlicer's `Metadata/plate_<N>.gcode` → `plate_0.gcode`
    // BEFORE forwarding to the printer. Bambu firmware (and the cloud-relay
    // project_file path observed in docs/plugin-trace/H2D-cloud.yaml) opens
    // the .3mf and looks for `Metadata/plate_0.gcode`; Orca's plater_id is
    // 1-based so a single-plate slice ends up at plate_1.gcode and the
    // printer reports "couldn't read file". The rewriter is idempotent and
    // a no-op when the upload is already plate_0 or isn't a .3mf at all.
    (void) rewrite_plate_to_zero(tmp_path, job.dev_id);

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

    int rc = handle->start_local_print_with_record(lp);

    // `start_local_print_with_record` is documented synchronous in
    // upstream — the slicer's FTPS 226 reply waits on this completion,
    // so it's safe to unlink the spool tempfile now. cleanup_upload_
    // tempfile also rmdir's the per-job dir the spool created. We also
    // unlink the settings-only .3mf sibling (lives in the same dir) so
    // the rmdir actually succeeds.
    if (!settings_path.empty() && settings_path != tmp_path)
        ::unlink(settings_path.c_str());
    cleanup_upload_tempfile(tmp_path);

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
