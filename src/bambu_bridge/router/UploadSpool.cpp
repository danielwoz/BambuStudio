// Bambu Bridge — shared upload spool helper.

#include "UploadSpool.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#ifndef _WIN32
#  include <sys/stat.h>   // ::chmod for 0600 hardening on POSIX
#endif

namespace Slic3r {
namespace bridge {
namespace router {

// Sanitise the slicer-supplied filename so it can't escape the per-job
// tempdir. The slicer normally hands us a clean basename like
// "Box_plate_1.3mf" — but the bridge's FTPS server accepts whatever
// STOR target the client picks, and we shouldn't blindly trust it.
// Strip directory components and reject empty / hidden results.
static std::string safe_basename(const std::string& name) {
    std::string b = name;
    auto pos = b.find_last_of("/\\");
    if (pos != std::string::npos) b.erase(0, pos + 1);
    // Drop leading dots (no hidden / parent-traversal cleverness).
    while (!b.empty() && b.front() == '.') b.erase(0, 1);
    if (b.empty()) b = "upload.3mf";
    // Force `.3mf` suffix. Per the GUI plugin-trace (BAMBU_BRIDGE_PLUGIN_
    // TRACE=1 captured 2026-05-30 from a working H2D print):
    //   filename        = `<dir>/.<pid>.<idx>.3mf`         ← plain .3mf
    //   config_filename = `<dir>/.<pid>.<idx>_config.3mf`  ← plain .3mf
    // — the GUI passes both as `.3mf`. An earlier guess at `.gcode.3mf`
    // (matching files on the printer's FTPS root) was wrong; that
    // extension is what ENDS UP on the printer's SD after the plugin's
    // upload, not what the plugin RECEIVES from the slicer.
    std::string lower = b;
    for (auto& c : lower) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    auto endswith = [&](const std::string& suf) {
        return lower.size() >= suf.size() &&
               lower.compare(lower.size() - suf.size(), suf.size(), suf) == 0;
    };
    if (!endswith(".3mf")) b += ".3mf";
    return b;
}

std::string spool_upload_to_tempfile(const server::UploadJob& job) {
    namespace fs = std::filesystem;

    // temp_directory_path() honours TMPDIR on POSIX and TMP/TEMP/USERPROFILE
    // on Windows — the cross-platform replacement for the old getenv("TMPDIR")
    // / "/tmp" fallback.
    std::error_code ec;
    fs::path parent = fs::temp_directory_path(ec);
    if (ec) return {};

    // Per-job subdir so we can preserve the slicer's original filename
    // verbatim — that's what the GUI's SendJob would pass to the plugin
    // (e.g. "MyProject_plate_3.3mf"); the plugin uses it in the task record
    // / printer-side UI, so propagating it matters for parity. This is the
    // portable replacement for mkdtemp(): a process-unique directory name.
    static std::atomic<std::uint64_t> s_counter{0};
    const auto ns  = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const std::uint64_t uniq = s_counter.fetch_add(1, std::memory_order_relaxed);

    fs::path dir = parent / ("bridge-upload-"
                       + std::to_string(static_cast<std::uint64_t>(ns))
                       + "-" + std::to_string(static_cast<std::uint64_t>(tid))
                       + "-" + std::to_string(uniq));
    fs::create_directories(dir, ec);
    if (ec) return {};

    fs::path final_path = dir / safe_basename(job.filename);
    {
        std::ofstream out(final_path, std::ios::binary | std::ios::trunc);
        if (!out) { fs::remove_all(dir, ec); return {}; }
        if (!job.content.empty())
            out.write(reinterpret_cast<const char*>(job.content.data()),
                      static_cast<std::streamsize>(job.content.size()));
        if (!out) { out.close(); fs::remove_all(dir, ec); return {}; }
    }

#ifndef _WIN32
    // Best-effort owner-only hardening on POSIX. On Windows the per-user
    // %TEMP% directory is already ACL-restricted to the current user.
    ::chmod(final_path.string().c_str(), 0600);
#endif

    return final_path.string();
}

void cleanup_upload_tempfile(const std::string& path) {
    if (path.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;

    // Spool layout is `<parent>/bridge-upload-XXXX/<basename>` — remove the
    // whole per-job directory we created. Defensive: only remove_all the
    // dir if its basename starts with the spool prefix, so we never blow
    // away a dir we didn't own.
    fs::path p(path);
    fs::path dir = p.parent_path();
    const std::string dir_base = dir.filename().string();
    if (dir_base.rfind("bridge-upload-", 0) == 0) {
        fs::remove_all(dir, ec);
    } else {
        fs::remove(p, ec);
    }
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
