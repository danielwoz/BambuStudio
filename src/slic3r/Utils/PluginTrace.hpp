// Bambu Bridge — shared plugin-call trace.
//
// Single header included by every layer that wraps proprietary plugin
// entrypoints (NetworkAgent.cpp, BambuSourceHandle.cpp,
// PrinterFileSystem.cpp). Gated on `BAMBU_BRIDGE_PLUGIN_TRACE=1` at
// runtime — zero-cost when off.
//
// Every emitted line:
//   [plugincall] HH:MM:SS.mmm tid=<n> <message>
//
// so timing/frequency of unsolicited calls (install_device_cert on a
// timer, etc.) is recoverable from the log via awk.
//
// To snapshot .3mf files passed by reference (PrintParams.filename
// and config_filename), call snapshot_path() — it hard-links into
// /tmp/plugin-trace-3mf-snapshots/ if BAMBU_BRIDGE_PLUGIN_SNAPSHOT=1
// is set.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define BAMBU_BRIDGE_HAVE_EXECINFO 1
#endif
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#define BAMBU_BRIDGE_HAVE_CXXABI 1
#endif
#include <dlfcn.h>

namespace Slic3r {
namespace plugin_trace {

inline bool enabled() {
    static const bool on = []{
        const char* e = std::getenv("BAMBU_BRIDGE_PLUGIN_TRACE");
        return e && *e && *e != '0';
    }();
    return on;
}

inline bool snapshot_enabled() {
    static const bool on = []{
        const char* e = std::getenv("BAMBU_BRIDGE_PLUGIN_SNAPSHOT");
        return e && *e && *e != '0';
    }();
    return on;
}

inline std::string truncate(const std::string& s, std::size_t n = 200) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "...<" + std::to_string(s.size()) + "B>";
}

// Per-line prefix: wall-clock HH:MM:SS.mmm + thread id. Captured into a
// fixed buffer to keep allocator pressure off the trace path.
inline void write_prefix(FILE* f) {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count() % 1000;
    struct tm lt;
    localtime_r(&tt, &lt);
    char ts[16];
    std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03lld",
        lt.tm_hour, lt.tm_min, lt.tm_sec, (long long) ms);
    // pthread_self() is opaque; cast and mask to a usable short id.
    unsigned long tid = (unsigned long) pthread_self();
    std::fprintf(f, "[plugincall] %s tid=%lu ", ts, tid & 0xFFFFF);
}

inline void log_event(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void log_event(const char* fmt, ...) {
    if (!enabled()) return;
    write_prefix(stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// Snapshot a file the plugin is about to read (PrintParams.filename /
// config_filename) into /tmp/plugin-trace-3mf-snapshots/ for offline
// analysis. Naming:
//   <epoch_ms>_<call>_<role>_<basename>
// where role is "filename" or "config_filename" (or anything caller
// passes). Same source path snapshotted twice in the same ms gets the
// same name — that's a feature (lets us see if filename == config_
// filename, which is one of the open questions for the bridge).
//
// Uses hard-link first (free, same-filesystem), falls back to copy if
// hard-link fails (e.g. cross-fs / EXDEV).
inline void snapshot_path(const char* call,
                          const char* role,
                          const std::string& src_path) {
    if (!snapshot_enabled() || src_path.empty()) return;
    struct stat st{};
    if (::stat(src_path.c_str(), &st) != 0) {
        log_event("SNAPSHOT %s %s missing: %s",
            call, role, src_path.c_str());
        return;
    }
    // Ensure the snapshot dir exists. mkdir is idempotent enough for
    // our purposes; ignore EEXIST.
    static constexpr const char* dir = "/tmp/plugin-trace-3mf-snapshots";
    ::mkdir(dir, 0700);

    auto pos = src_path.find_last_of('/');
    std::string base = (pos == std::string::npos)
                       ? src_path : src_path.substr(pos + 1);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    char out[1024];
    std::snprintf(out, sizeof(out), "%s/%lld_%s_%s_%s",
        dir, (long long) ms, call, role, base.c_str());

    ::unlink(out); // overwrite any same-ms collision
    if (::link(src_path.c_str(), out) != 0) {
        // Cross-fs or hard-link denied; fall back to a stream copy.
        FILE* in  = std::fopen(src_path.c_str(), "rb");
        FILE* of  = std::fopen(out, "wb");
        if (in && of) {
            char buf[64 * 1024];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
                std::fwrite(buf, 1, n, of);
        }
        if (in) std::fclose(in);
        if (of) std::fclose(of);
    }
    log_event("SNAPSHOT %s %s src=%s size=%lld -> %s",
        call, role, src_path.c_str(),
        (long long) st.st_size, out);
}

// Backtrace dump for one log line. Enabled by BAMBU_BRIDGE_PLUGIN_STACK=1
// (separate from BAMBU_BRIDGE_PLUGIN_TRACE so the regular trace stays
// cheap — backtrace_symbols allocates and walks /proc/self/maps).
//
// Emitted as one line per frame, prefixed with `[plugincall] callstack[<n>]…`
// so post-processing can group via the matching `[plugincall] <call>`
// line just before it. Skips the topmost frame (dump_stack itself).
//
// `tag` is the caller's identifier (e.g. "start_local_print_with_record")
// so the stack can be associated with the plugin call when interleaved
// with other threads' output.
inline bool stack_enabled() {
    static const bool on = []{
        const char* e = std::getenv("BAMBU_BRIDGE_PLUGIN_STACK");
        return e && *e && *e != '0';
    }();
    return on;
}

inline void dump_stack(const char* tag) {
    if (!enabled() || !stack_enabled()) return;
#if BAMBU_BRIDGE_HAVE_EXECINFO
    constexpr int kMax = 16;
    void* frames[kMax];
    int   n = ::backtrace(frames, kMax);
    if (n <= 1) return;
    // Skip frame 0 (this fn) and frame 1 (caller's log_event wrapper if
    // any). For each remaining frame, prefer dladdr+demangle to give us
    // a readable name; fall back to backtrace_symbols.
    char**  symbols = ::backtrace_symbols(frames, n);
    for (int i = 1; i < n && i < kMax; ++i) {
        Dl_info info{};
        std::string name;
        if (::dladdr(frames[i], &info) && info.dli_sname) {
#if BAMBU_BRIDGE_HAVE_CXXABI
            int    status = 0;
            char*  d = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
            name = (status == 0 && d) ? std::string(d) : std::string(info.dli_sname);
            std::free(d);
#else
            name = info.dli_sname;
#endif
        } else if (symbols) {
            name = symbols[i];
        } else {
            name = "<?>";
        }
        write_prefix(stderr);
        std::fprintf(stderr, "callstack[%s] frame=%d %p %s\n",
            tag, i - 1, frames[i], name.c_str());
    }
    if (symbols) std::free(symbols);
    std::fflush(stderr);
#else
    (void) tag;
#endif
}

} // namespace plugin_trace
} // namespace Slic3r
