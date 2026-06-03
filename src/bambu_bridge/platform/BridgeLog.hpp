// Bambu Bridge — lightweight verbose logger.
//
// The bridge server/router/plugin-handle code lives in the standalone
// `bambu_bridge` static lib, which does NOT link boost::log. This gives it
// a dependency-free logger gated on BAMBU_BRIDGE_VERBOSE.
//
// Output goes to a FILE (%LOCALAPPDATA%\BambuBridge\logs\bridge-relay.log on
// Windows, $TMPDIR/bridge-relay.log otherwise), NOT stderr — because when
// the bridge runs in-process inside the GUI (install_gui_worker), the GUI is
// a WINDOWS-subsystem process and its stderr is discarded. A file is
// captured in both the headless and in-GUI cases. Falls back to stderr if
// the file can't be opened.
//
// Usage:
//   BRIDGE_LOGF("route", "dev=%s lan_ok=%d cloud_ok=%d -> %s", id, l, c, r);
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace Slic3r { namespace bridge {

inline bool bridge_verbose() {
    static const int v = []() {
        const char* e = std::getenv("BAMBU_BRIDGE_VERBOSE");
        return (e && *e && std::strcmp(e, "0") != 0) ? 1 : 0;
    }();
    return v != 0;
}

inline std::FILE* bridge_log_file() {
    static std::FILE* f = []() -> std::FILE* {
        std::string path;
        if (const char* la = std::getenv("LOCALAPPDATA"); la && *la)
            path = std::string(la) + "\\BambuBridge\\logs\\bridge-relay.log";
        else if (const char* t = std::getenv("TMPDIR"); t && *t)
            path = std::string(t) + "/bridge-relay.log";
        else
            path = "/tmp/bridge-relay.log";
        std::FILE* fp = std::fopen(path.c_str(), "a");
        return fp ? fp : stderr;
    }();
    return f;
}

inline void bridge_logf(const char* tag, const char* fmt, ...) {
    if (!bridge_verbose()) return;
    std::FILE* f = bridge_log_file();
    char ts[16] = {0};
    std::time_t now = std::time(nullptr);
    if (std::tm* lt = std::localtime(&now)) std::strftime(ts, sizeof ts, "%H:%M:%S", lt);
    std::fprintf(f, "%s [bridge:%s] ", ts, tag ? tag : "?");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fflush(f);
}

}} // namespace Slic3r::bridge

#define BRIDGE_LOGF(tag, ...) ::Slic3r::bridge::bridge_logf((tag), __VA_ARGS__)
