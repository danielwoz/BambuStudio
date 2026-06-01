// Bambu Bridge — shared upload spool helper.

#include "UploadSpool.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

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
    const char* tmpdir_env = std::getenv("TMPDIR");
    std::string parent = (tmpdir_env && *tmpdir_env ? tmpdir_env : "/tmp");

    // Per-job tempdir so we can preserve the slicer's original filename
    // verbatim — that's what the GUI's SendJob would pass to the plugin
    // (e.g. "MyProject_plate_3.3mf"). Plugin uses the filename in
    // task-record / printer-side UI, so propagating it matters for
    // parity. mkdtemp gives us "bridge-upload-XXXXXX/".
    std::string dir_tmpl = parent + "/bridge-upload-XXXXXX";
    std::vector<char> dbuf(dir_tmpl.begin(), dir_tmpl.end());
    dbuf.push_back('\0');
    if (::mkdtemp(dbuf.data()) == nullptr) return {};
    std::string dir(dbuf.data());

    std::string final_path = dir + "/" + safe_basename(job.filename);
    int fd = ::open(final_path.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        ::rmdir(dir.c_str());
        return {};
    }

    const auto* src = job.content.data();
    std::size_t remaining = job.content.size();
    while (remaining > 0) {
        ssize_t w = ::write(fd, src, remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(final_path.c_str());
            ::rmdir(dir.c_str());
            return {};
        }
        src       += w;
        remaining -= static_cast<std::size_t>(w);
    }
    ::close(fd);
    return final_path;
}

void cleanup_upload_tempfile(const std::string& path) {
    if (path.empty()) return;
    ::unlink(path.c_str());
    // Spool layout is `<parent>/bridge-upload-XXXXXX/<basename>` — rmdir
    // the directory we created. Defensive: only rmdir if the parent's
    // basename starts with the spool prefix, so we never blow away a
    // dir we didn't own.
    auto pos = path.find_last_of('/');
    if (pos == std::string::npos) return;
    std::string dir = path.substr(0, pos);
    auto dir_pos = dir.find_last_of('/');
    std::string dir_base = (dir_pos == std::string::npos)
                           ? dir : dir.substr(dir_pos + 1);
    if (dir_base.rfind("bridge-upload-", 0) == 0) {
        ::rmdir(dir.c_str());
    }
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
