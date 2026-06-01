// Bambu Bridge — CameraFrameFanout unit test.
//
// Pins:
//   1. open() succeeds when the upstream is healthy; frames flow from a
//      single reader thread into the fanout's ring.
//   2. Two concurrent cursors created from the same fanout both observe
//      the same frame stream (proves single-reader → many-watcher).
//   3. A cursor created LATER misses the earlier frames (cursor-from-now
//      semantics).
//   4. close() joins the reader and prevents new frames from being added.
//   5. Each cursor's destructor releases its slot — repeatable create/
//      destroy cycles don't accumulate.
//
// Source: NullCameraSource (synthetic test pattern, no plugin / network).

#include "router/CameraFrameFanout.hpp"
#include "router/NullCameraSource.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using Slic3r::bridge::router::CameraFrameFanout;
using Slic3r::bridge::router::NullCameraSource;
using Slic3r::bridge::server::ICameraSource;
using Slic3r::bridge::server::VideoFrame;

namespace {

int g_fails = 0;

void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

std::size_t drain_cursor(CameraFrameFanout::Cursor& c, std::size_t want,
                         int per_frame_timeout_ms) {
    std::size_t got = 0;
    for (std::size_t i = 0; i < want; ++i) {
        auto f = c.next_frame(per_frame_timeout_ms);
        if (!f) break;
        ++got;
    }
    return got;
}

} // namespace

int main() {
    auto upstream = std::make_shared<NullCameraSource>();
    auto fanout   = CameraFrameFanout::create(upstream);

    check(fanout->open(),
          "open() returns true with a healthy NullCameraSource upstream");
    check(fanout->is_open(),
          "is_open() flips true after open()");

    // (2) Two concurrent cursors both receive frames. NullCameraSource
    // delivers ~30 fps, so 5 frames each should land within ~250 ms.
    auto cur_a = fanout->create_cursor();
    auto cur_b = fanout->create_cursor();
    check(fanout->active_cursors() == 2,
          "two active cursors after create_cursor x2");

    std::atomic<std::size_t> got_a{0}, got_b{0};
    std::thread ta([&]{ got_a.store(drain_cursor(*cur_a, 5, 1000)); });
    std::thread tb([&]{ got_b.store(drain_cursor(*cur_b, 5, 1000)); });
    ta.join();
    tb.join();
    check(got_a.load() == 5,
          "cursor A pulled 5 frames within budget");
    check(got_b.load() == 5,
          "cursor B pulled 5 frames within budget (same upstream stream)");

    // (3) Late-arrival cursor lands on the most recent keyframe in the
    // ring (so a decoder can immediately make sense of the stream). For
    // NullCameraSource, every emitted frame carries is_keyframe=true (it
    // emits SPS+PPS+IDR Annex-B on every read), so a late cursor should
    // see a frame in the ring immediately on a zero-timeout poll.
    const std::size_t produced_so_far = fanout->frames_produced();
    check(produced_so_far >= 5,
          "fanout::frames_produced advances as reader pushes");
    auto cur_late = fanout->create_cursor();
    auto immediate = cur_late->next_frame(0);
    check(immediate.has_value() && immediate->is_keyframe,
          "late-arrival cursor returns a keyframe immediately (zero-timeout)");

    // And then it continues to see freshly-produced frames.
    auto next_for_late = cur_late->next_frame(1000);
    check(next_for_late.has_value(),
          "late-arrival cursor receives the next produced frame within timeout");

    // (5) Cursor destruction releases the slot.
    cur_a.reset();
    cur_b.reset();
    cur_late.reset();
    check(fanout->active_cursors() == 0,
          "all cursors release on destruction");

    // (4) close() halts further production and resets the ring state so
    // the next open() starts from a clean slate.
    fanout->close();
    check(!fanout->is_open(),
          "is_open() false after close()");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(fanout->frames_produced() == 0,
          "frames_produced() resets to 0 on close()");

    // Repeatable open/close cycle.
    check(fanout->open(),
          "open() succeeds after close()");
    auto cur2 = fanout->create_cursor();
    auto post_reopen_frame = cur2->next_frame(1000);
    check(post_reopen_frame.has_value(),
          "frames flow again after re-open");
    fanout->close();

    if (g_fails) {
        std::fprintf(stderr, "FAILURE — %d check(s) failed\n", g_fails);
        return 1;
    }
    std::fprintf(stderr, "OK — all checks passed\n");
    return 0;
}
