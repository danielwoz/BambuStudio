// Bambu Bridge — ShimRecorder (harness).
//
// Process-wide append-only JSONL writer used by the harness to capture
// every call that crosses the boundary into the closed-source
// `libbambu_networking.so` / `libBambuSource.so`. The recorder is the
// passive half of the test seam: the trampolines inside NetworkAgent.cpp
// and PrinterFileSystem.cpp call ShimRecorder::record(...) before
// forwarding to the real function pointer.
//
// Lifecycle:
//
//   1. The harness sets BAMBU_BRIDGE_SHIM=1 (any non-empty value) and
//      optionally BAMBU_BRIDGE_SHIM_TRACE=/path/to/out.jsonl in the env
//      before the slicer code under test runs. The recorder is OFF until
//      `instance().enable_from_env()` (or `enable(path)`) is called.
//   2. The slicer's trampolines call `record(...)` for every wrapped
//      function. When OFF, record(...) is a single atomic-bool load
//      with predictable branch — zero allocation.
//   3. At shutdown the recorder flushes its file descriptor; tests can
//      also call `flush()` explicitly to bound a captured trace.
//
// Thread safety: one mutex serialises the JSONL write to the file. The
// trampolines themselves run on whichever thread the plugin's worker
// pool picks (cloud-recv, lan-recv, rtsp-thread, slicer-gui) so the
// mutex MUST guard the dump+write step. The cost is ~one syscall per
// shimmed call; this is a test harness, not production code.
//
// Out-of-scope for v1:
//   - per-thread buffering (single mutex is fine at observed call rates)
//   - shared-memory IPC for multi-process traces (none planned)
//   - binary trace format (JSONL is reviewable; perf is not a concern).

#ifndef SLIC3R_BAMBU_BRIDGE_HARNESS_SHIM_RECORDER_HPP
#define SLIC3R_BAMBU_BRIDGE_HARNESS_SHIM_RECORDER_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "TraceLine.hpp"

namespace Slic3r {
namespace bridge {
namespace harness {

class ShimRecorder {
public:
    // Process-wide singleton. The recorder is OFF (record() is a no-op)
    // until enable_from_env() or enable(path) lands a successful open.
    static ShimRecorder& instance();

    // Honour BAMBU_BRIDGE_SHIM / BAMBU_BRIDGE_SHIM_TRACE. Returns true
    // iff the recorder is now active. When BAMBU_BRIDGE_SHIM is unset or
    // empty, returns false and leaves the recorder OFF. When the env
    // names a path that cannot be opened, returns false (and warns to
    // stderr) — the harness keeps running with the shim disabled.
    bool enable_from_env();

    // Direct activation: open `path` for append, mark recorder ON.
    // Returns true on success. Overwrites any prior open file (closes
    // the previous fd first).
    bool enable(const std::string& path);

    // Mark recorder OFF and close the file. Idempotent.
    void disable();

    // Force-flush the file descriptor. The destructor flushes too.
    void flush();

    // Cheap check; safe to call from any thread. Always returns false
    // when the recorder isn't active.
    bool active() const { return m_active.load(std::memory_order_acquire); }

    // Append one already-populated TraceLine. The seq + ts_ns + delta_ms
    // + duration_us fields are filled in here so callers don't have to.
    // The `line.duration_us` field is preserved if the caller set it
    // (i.e. they timed the underlying call); otherwise it stays at 0.
    void record(TraceLine line);

    // Convenience overload — the trampolines call this. Records one
    // line with the given lib + fn + args + return value. duration_us
    // and ret_out_params default to empty.
    void record(TraceLib                lib,
                const char*             fn,
                nlohmann::json          args,
                nlohmann::json          ret             = nullptr,
                nlohmann::json          ret_out_params  = nlohmann::json::object(),
                std::int64_t            duration_us     = 0,
                std::int64_t            cb_id           = -1);

    // Per-process stable id for an arbitrary callable. Returns a new
    // monotonic int the first time a given key is seen, and the same
    // int on every subsequent call. Used by the trampolines so a
    // `set_on_message_fn(cb)` line and a later `OnMessageFn fires` line
    // can be cross-referenced via the same id.
    std::int64_t callback_id_for(const void* key);

    // Test-only: drain everything to disk and return the path of the
    // active output file. Empty string if not active.
    std::string active_path() const;

    // Test-only: bytes written so far (best-effort, not lock-held).
    std::int64_t bytes_written() const { return m_bytes_written.load(); }

    // Test-only: number of lines written so far.
    std::int64_t lines_written() const { return m_lines_written.load(); }

    ShimRecorder(const ShimRecorder&)            = delete;
    ShimRecorder& operator=(const ShimRecorder&) = delete;

private:
    ShimRecorder();
    ~ShimRecorder();

    void close_locked();
    std::int64_t now_ns_locked() const;

    // Hot-path bool. Read before any work. Loaded with acquire ordering
    // to pair with the release-store inside enable().
    std::atomic<bool> m_active{false};

    mutable std::mutex      m_mu;          // guards the FILE* + counters
    std::FILE*              m_fp = nullptr;
    std::string             m_path;

    std::int64_t            m_seq            = 0;
    std::int64_t            m_prev_ts_ns     = 0;

    std::atomic<std::int64_t> m_bytes_written{0};
    std::atomic<std::int64_t> m_lines_written{0};
    std::atomic<std::int64_t> m_cb_id_next{1};

    // Callback id table. Tiny (≤ ~16 entries per slicer-session in
    // practice). Backed by a flat vector behind the mutex to avoid
    // pulling unordered_map<void*> into a header.
    struct CallbackEntry { const void* key; std::int64_t id; };
    std::vector<CallbackEntry> m_callbacks;
};

} // namespace harness
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_HARNESS_SHIM_RECORDER_HPP
