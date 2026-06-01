// Bambu Bridge — single-reader-many-watcher fan-out for camera sources.
//
// Problem this solves
// -------------------
//   The proprietary BambuSource SDK (used by Cloud/Lan/JpegCameraSource)
//   exposes `bambu_read_sample` with non-blocking-poll semantics: a single
//   reader must drain it aggressively or the SDK's internal frame buffer
//   overflows and drops samples. Empirically (H2S TUTK, 2026-05-31): the
//   SDK's `frame_count` advanced ~91 frames during a 30 s session while
//   the RTSP pump only extracted 2 — because `CloudCameraSource::next_frame`
//   returns immediately on WOULD_BLOCK and the pump's polling cadence
//   doesn't match the SDK's expected drain rate.
//
//   Two structural needs follow:
//     1. A dedicated reader thread that drains the upstream source at a
//        consistent cadence, decoupled from how fast (or slowly) any
//        particular RTSP session pulls frames.
//     2. A fan-out so one upstream stream can feed N concurrent watchers
//        (slicer + monitoring tools + recorders) without paying for one
//        SDK session per watcher.
//
// Architecture
// ------------
//   CameraFrameFanout owns ONE upstream `ICameraSource` (LAN / Cloud /
//   Jpeg). On `open()` it starts a reader thread that loops
//   `upstream->next_frame(poll_timeout_ms)` and pushes every received
//   `VideoFrame` into a fixed-capacity ring. Each consumer obtains a
//   `Cursor` via `create_cursor()`; the cursor maintains its own monotonic
//   read position in the ring and pulls via `Cursor::next_frame(timeout_ms)`
//   (which blocks on a condition variable until either a fresh frame is
//   visible at the cursor's position or the timeout expires).
//
//   Slow-consumer policy: if a cursor falls more than `drop_threshold`
//   frames behind the writer, its next read fast-forwards the cursor to
//   the most recent keyframe in the ring (or, failing that, the newest
//   frame). The upstream is never blocked by a slow consumer.
//
//   Lifecycle: `open()` is idempotent; subsequent calls are no-ops.
//   `close()` stops the reader and clears the ring; the next `open()`
//   restarts everything. Cursors created against a closed fanout are
//   still valid — they just return `nullopt` until `open()` is called.
//
// Thread model
// ------------
//   - One internal reader thread per fanout (started on `open()`).
//   - Many concurrent `Cursor::next_frame` callers; each is independent.
//   - All ring mutations happen under one mutex; readers wait on a single
//     condition_variable.
//   - Cursor destruction is safe at any time (no orphan threads).

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_CAMERA_FRAME_FANOUT_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_CAMERA_FRAME_FANOUT_HPP

#include "../server/ICameraSource.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace router {

class CameraFrameFanout
    : public std::enable_shared_from_this<CameraFrameFanout> {
public:
    struct Config {
        // Ring capacity (frames). 60 ≈ 2 s at 30 fps for H.264; ample
        // for MotionJpeg too because the printer's JPEG rate is ~6 fps.
        std::size_t ring_capacity            = 60;

        // Per-iteration block on the upstream. 33 ms ≈ one frame at 30 fps.
        // The reader's effective rate is whatever the upstream delivers;
        // this is just the bound on how long we wait between checks.
        // NOTE: CloudCameraSource / LanCameraSource currently ignore this
        // parameter and call `bambu_read_sample` unconditionally. The
        // pacing happens via the idle-backoff knobs below.
        int         upstream_poll_timeout_ms = 33;

        // Backoff between consecutive WOULD_BLOCK polls. The proprietary
        // BambuSource SDK only emits ~30 frames/sec; polling much faster
        // wastes CPU AND (empirically) triggers the SDK's own WOULD_BLOCK
        // rate-limit which appears to drop frames it would otherwise hand
        // off. After this many consecutive WOULD_BLOCK results we sleep
        // `idle_backoff_us` between probes — sized so we natually align
        // to the SDK's frame cadence. Reset on every successful frame.
        int         idle_backoff_threshold   = 1;
        int         idle_backoff_us          = 8000;  // ~8 ms → 125 Hz

        // If a consumer cursor is more than `drop_threshold` frames behind
        // the writer, fast-forward it to the most recent keyframe (or the
        // newest frame if no keyframe is visible in the ring).
        std::size_t drop_threshold           = 30;
    };

    // Per-watcher handle. Holding one keeps the cursor registered with
    // the fanout; destruction is fully thread-safe. Cursors are created
    // via `CameraFrameFanout::create_cursor()`.
    class Cursor {
    public:
        Cursor(std::shared_ptr<CameraFrameFanout> fanout, std::size_t id);
        ~Cursor();

        // Block up to `timeout_ms` ms for a fresh frame at this cursor's
        // position. Returns the frame and advances the cursor on success.
        // Returns `nullopt` on timeout, on fanout-closed, or if the
        // upstream signalled EOS.
        std::optional<server::VideoFrame> next_frame(int timeout_ms);

        Cursor(const Cursor&)            = delete;
        Cursor& operator=(const Cursor&) = delete;

    private:
        std::shared_ptr<CameraFrameFanout> m_fanout;
        std::size_t                         m_id;
    };

    // Construct around an upstream source. The fanout takes a shared
    // reference; closing the fanout does NOT close the upstream — the
    // caller (typically `RtspServer::Device`) keeps owning that lifecycle.
    //
    // Always allocate via `std::make_shared` (or use `create()`) — cursors
    // hold a `shared_ptr` to the fanout, so the fanout MUST be owned by a
    // shared_ptr from the start (`enable_shared_from_this` contract).
    //
    // Default-arg trick avoided so Config's nested default-initialisers
    // don't have to be visible in the function declaration (gcc/clang
    // refuse `Config cfg = {}` in-class when Config is itself defined
    // in-class with default member-initialisers).
    CameraFrameFanout(std::shared_ptr<server::ICameraSource> upstream,
                      Config                                  cfg);
    ~CameraFrameFanout();

    // Convenience: equivalent to std::make_shared<CameraFrameFanout>(...).
    // Defaults `cfg` to Config{} (filled by member-initialisers).
    static std::shared_ptr<CameraFrameFanout>
    create(std::shared_ptr<server::ICameraSource> upstream);
    static std::shared_ptr<CameraFrameFanout>
    create(std::shared_ptr<server::ICameraSource> upstream, Config cfg);

    // Idempotent. `open()` first opens the upstream (if not already open)
    // then spawns the reader thread. `close()` stops the reader and
    // clears the ring; the upstream is NOT closed (caller's responsibility).
    bool open();
    void close();
    bool is_open() const;

    // Delegated to the upstream — codec/SPS/PPS don't change once open.
    server::ICameraSource::StreamInfo info() const;

    // Register a new cursor. The cursor's initial position is the current
    // writer position (i.e. it will see only frames produced AFTER it was
    // created). Safe to call from any thread.
    std::shared_ptr<Cursor> create_cursor();

    // Test/diagnostic hook: how many frames have we pushed into the ring
    // since `open()`? Monotonic; resets on `close()`.
    std::size_t frames_produced() const;
    // Test/diagnostic: how many cursors are currently registered?
    std::size_t active_cursors() const;

private:
    void reader_loop();

    // Cursor-API helpers. Called only via the Cursor class.
    friend class Cursor;
    std::size_t register_cursor_locked();
    void unregister_cursor(std::size_t id);
    std::optional<server::VideoFrame>
        wait_and_pop(std::size_t id, int timeout_ms);

    struct Slot {
        server::VideoFrame frame;
        bool               valid = false;
    };

    struct CursorState {
        std::size_t position = 0; // next frame index to read
    };

    std::shared_ptr<server::ICameraSource> m_upstream;
    Config                                 m_cfg;

    mutable std::mutex      m_mu;
    std::condition_variable m_cv;
    std::vector<Slot>       m_ring;
    std::size_t             m_writer_pos     = 0; // monotonic count of frames pushed
    std::atomic<bool>       m_running        { false };
    std::atomic<bool>       m_eos            { false };
    std::thread             m_reader;

    std::size_t                                      m_next_cursor_id = 1;
    std::unordered_map<std::size_t, CursorState>     m_cursors;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_CAMERA_FRAME_FANOUT_HPP
