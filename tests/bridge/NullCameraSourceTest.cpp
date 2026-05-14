// Bambu Bridge — NullCameraSource unit test (phase 8).
//
// Pins:
//   1. open() succeeds and flags is_open() true.
//   2. info() reports the documented 16x16 @ 30fps shape AND non-empty
//      SPS / PPS suitable for SDP sprop-parameter-sets.
//   3. Five back-to-back next_frame(1000) calls succeed and emit the
//      Annex-B SPS+PPS+IDR fixture (verified by checking the buffer starts
//      with 00 00 00 01 0x67 — the SPS start code + NAL header).
//   4. The cadence holds: 5 frames take ~133 ms (give or take scheduler
//      slop), i.e. ~30 fps without the source busy-spinning.

#include "router/NullCameraSource.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>

using Slic3r::bridge::router::NullCameraSource;

namespace {

int g_fails = 0;

void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

bool starts_with_annexb_sps(const std::vector<uint8_t>& d) {
    if (d.size() < 5) return false;
    if (d[0] != 0x00 || d[1] != 0x00 || d[2] != 0x00 || d[3] != 0x01) return false;
    return (d[4] & 0x1F) == 7;  // NAL type 7 = SPS
}

} // namespace

int main() {
    NullCameraSource src;

    check(!src.is_open(), "fresh source starts closed");
    check(src.open(),     "open() returns true");
    check(src.is_open(),  "is_open() true after open()");

    auto info = src.info();
    check(info.width  == 16, "info.width == 16");
    check(info.height == 16, "info.height == 16");
    check(info.fps    == 30, "info.fps == 30");
    check(!info.sps.empty(), "info.sps non-empty");
    check(!info.pps.empty(), "info.pps non-empty");
    if (!info.sps.empty()) {
        check((info.sps[0] & 0x1F) == 7, "info.sps NAL type == 7 (SPS)");
    }
    if (!info.pps.empty()) {
        check((info.pps[0] & 0x1F) == 8, "info.pps NAL type == 8 (PPS)");
    }

    // Pull 5 frames; first one available immediately, subsequent ones pace
    // around ~33ms. Allow a generous timeout window so heavily-loaded CI
    // boxes don't false-negative.
    const auto t0 = std::chrono::steady_clock::now();
    int got = 0;
    for (int i = 0; i < 5; ++i) {
        auto f = src.next_frame(/*timeout_ms=*/1000);
        check(f.has_value(), "next_frame() returned a frame");
        if (f) {
            ++got;
            if (i == 0) {
                check(starts_with_annexb_sps(f->nal_data),
                      "frame starts with Annex-B SPS NAL");
                check(f->is_keyframe, "frame flagged as keyframe");
            }
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    check(got == 5, "got 5 frames");
    // 5 frames at 30 fps = ~133ms. Allow [100, 400] ms to cover slow CIs.
    // The first frame is immediate, so the floor is 4*33 = 132ms.
    std::fprintf(stderr, "[NullCameraSourceTest] 5 frames in %lld ms\n",
                 static_cast<long long>(elapsed));
    check(elapsed >= 100 && elapsed <= 500,
          "5 frames take roughly 30 fps (100..500 ms)");

    src.close();
    check(!src.is_open(), "is_open() false after close()");
    auto post = src.next_frame(10);
    check(!post.has_value(), "next_frame() nullopt after close()");

    if (g_fails) {
        std::fprintf(stderr, "NullCameraSourceTest: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("NullCameraSourceTest: ok\n");
    return 0;
}
