// Bambu Bridge — test-pattern camera source (phase 8).
//
// `NullCameraSource` is the "battery-included" ICameraSource for tests and
// for `bridge-cli rtsp --source null`. It emits a fixed H.264 fixture
// (SPS + PPS + IDR slice for a 16x16 all-black frame, baseline profile,
// 30 fps) in an infinite loop, gated so callers see roughly 30 fps even if
// they call `next_frame` more aggressively. Callers blocking on a 33 ms
// boundary get the same frame data with a fresh PTS each tick.
//
// Why a hand-rolled tiny fixture and not libavcodec: zero external deps.
// The bytes below were generated with x264 (baseline profile, 1 IDR,
// no B-frames, 16x16 single-MB) and verified to decode in ffplay/VLC.
//
// The "loop" only re-emits the IDR — there's no P-slice variation,
// which is fine for what the bridge is testing (the RFC-6184 packetiser
// and the RTSP control flow). Real-world camera variety is exercised by
// LanCameraSource / CloudCameraSource against actual printers/cloud.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_CAMERA_SOURCE_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_CAMERA_SOURCE_HPP

#include "../server/ICameraSource.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace router {

class NullCameraSource : public server::ICameraSource {
public:
    NullCameraSource();
    ~NullCameraSource() override = default;

    bool open() override;
    void close() override;
    bool is_open() const override { return m_open.load(); }

    std::optional<server::VideoFrame> next_frame(int timeout_ms) override;

    server::ICameraSource::StreamInfo info() const override;

private:
    std::atomic<bool>                              m_open{false};
    std::mutex                                     m_mu;
    std::chrono::steady_clock::time_point          m_t0;
    std::chrono::steady_clock::time_point          m_next_at;
    int                                            m_frame_count = 0;

    // Annex-B encoded SPS + PPS + IDR for a 16x16 baseline-profile frame.
    std::vector<uint8_t>                           m_annexb_full;
    // Raw (no-startcode) SPS / PPS for SDP sprop-parameter-sets.
    std::vector<uint8_t>                           m_sps_raw;
    std::vector<uint8_t>                           m_pps_raw;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_CAMERA_SOURCE_HPP
