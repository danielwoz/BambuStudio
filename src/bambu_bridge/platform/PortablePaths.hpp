// Bambu Bridge — portable runtime paths.
//
// Centralises the handful of writable-runtime directories the bridge uses.
//   Linux: /tmp/...                       (back-compat with existing layout)
//   Win:   %LOCALAPPDATA%\BambuBridge\... (per-user, matches the PowerShell
//                                          supervisor's BridgeRoot)
#pragma once

#include <string>
#include <filesystem>
#include <cstdlib>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace Slic3r { namespace bridge { namespace platform {

// Per-user bridge runtime root.
//   Linux: /tmp
//   Win:   %LOCALAPPDATA%\BambuBridge
inline std::filesystem::path bridge_runtime_root() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH + 1] = {};
    DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::filesystem::path root = (n > 0 && buf[0])
        ? (std::filesystem::path(buf) / L"BambuBridge")
        : (std::filesystem::temp_directory_path() / L"BambuBridge");
    return root;
#else
    return std::filesystem::path("/tmp");
#endif
}

inline std::filesystem::path bridge_spool_dir() {
#ifdef _WIN32
    return bridge_runtime_root() / "spool";
#else
    return std::filesystem::path("/tmp/bridge-spool");
#endif
}
inline std::filesystem::path bridge_progress_dir() {
#ifdef _WIN32
    return bridge_runtime_root() / "progress";
#else
    return std::filesystem::path("/tmp/bridge-progress");
#endif
}
inline std::filesystem::path bridge_capture_dir() {
#ifdef _WIN32
    return bridge_runtime_root() / "capture";
#else
    return std::filesystem::path("/tmp/bridge-capture");
#endif
}

// Spool/temp upload directory. Uses the platform temp dir on both OSes,
// which honours TMPDIR (POSIX) / TMP|TEMP|USERPROFILE (Windows).
inline std::filesystem::path bridge_temp_root() {
    return std::filesystem::temp_directory_path();
}

}}} // namespace Slic3r::bridge::platform

// ---- Portable file-op shims (POSIX mkdir/link -> Windows equivalents) ------
// Diagnostic/capture code in the branch uses raw ::mkdir(path,mode) and
// ::link(old,new) (hardlink). These free functions keep those callsites
// one-line on both platforms.
#ifdef _WIN32
#  include <direct.h>   // _mkdir
#  include <io.h>       // _close
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

inline int bridge_mkdir(const char* path, int mode) {
#ifdef _WIN32
    (void) mode; return ::_mkdir(path);
#else
    return ::mkdir(path, static_cast<mode_t>(mode));
#endif
}
inline int bridge_hardlink(const char* oldp, const char* newp) {
#ifdef _WIN32
    return ::CreateHardLinkA(newp, oldp, nullptr) ? 0 : -1;
#else
    return ::link(oldp, newp);
#endif
}
inline int bridge_close_fd(int fd) {
#ifdef _WIN32
    return ::_close(fd);
#else
    return ::close(fd);
#endif
}
