// Lightweight stderr tracing for NetworkAgent / DeviceManager method
// entries. Used to compare a "golden" trace (real-printer session)
// against a virtual-printer session to find what's missing.
//
// Gated on the `BAMBU_TRACE` env var. Off by default (zero overhead
// beyond the env probe, which is cached after first call).
//
// Usage at method entry:
//     BS_TRACE("NA::connect_printer", "dev_id=%s ip=%s",
//              dev_id.c_str(), dev_ip.c_str());
//
// With BAMBU_TRACE=1 a slicer launched as
//     BAMBU_BRIDGE_GUI_DISABLED=1 BAMBU_TRACE=1 ./bambu-studio
// emits one stderr line per traced call:
//     [trace] NA::connect_printer dev_id=03900D... ip=192.168.1.247

#ifndef SLIC3R_BAMBU_TRACE_HPP
#define SLIC3R_BAMBU_TRACE_HPP

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Slic3r {
namespace bambu_trace {

inline bool enabled() {
    static const bool v = []() {
        const char* e = std::getenv("BAMBU_TRACE");
        return e && *e && std::strcmp(e, "0") != 0;
    }();
    return v;
}

inline void log_line(const char* tag, const char* fmt, ...) {
    if (!enabled()) return;
    std::fprintf(stderr, "[trace] %s ", tag);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

} // namespace bambu_trace
} // namespace Slic3r

#define BS_TRACE(tag, fmt, ...) \
    ::Slic3r::bambu_trace::log_line(tag, fmt, ##__VA_ARGS__)

// Argument-less variant — most methods just need the name.
#define BS_TRACE_ENTER(tag) \
    ::Slic3r::bambu_trace::log_line(tag, "%s", "")

#endif // SLIC3R_BAMBU_TRACE_HPP
