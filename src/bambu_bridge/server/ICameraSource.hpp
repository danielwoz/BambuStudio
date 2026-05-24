// Bambu Bridge — pull-mode camera-source interface (phase 8).
//
// `RtspServer` re-serves a printer's live camera to slicers on the LAN. It
// doesn't care WHERE the H.264 frames come from — only that they arrive as
// length-prefixed NAL units with usable SPS/PPS for SDP advertising. That
// indirection is `ICameraSource`.
//
// Three production impls exist (see `router/Null/Lan/CloudCameraSource.*`):
//
//   - NullCameraSource:   built-in test pattern. Used by tests and the
//                          bridge-cli `rtsp` subcommand's default --source.
//   - LanCameraSource:    speaks the printer's direct RTSPS on port 322.
//                          Phase 8 stubs it — full client lands in a
//                          follow-up so phase-12 real-printer testing can
//                          exercise it.
//   - CloudCameraSource:  fetches a URL from the proprietary plugin
//                          (`bambu_network_get_camera_url`). For an
//                          `rtsps://` URL it dispatches to LanCameraSource;
//                          for `bambu:///agora/...` it stubs (those need
//                          the proprietary BambuSource library).
//
// Threading: `RtspServer` runs one streaming thread per accepted RTSP
// session and calls `next_frame()` from that thread. Implementations MUST
// be thread-safe with respect to lifecycle (`open`/`close`/`is_open`) but
// only one concurrent `next_frame()` per source is permitted (server-side
// invariant — `RtspServer` ties one source to one session).
//
// Frame encoding (documented once here so the RTSP packetiser doesn't
// have to guess):
//   - `VideoFrame::nal_data` is a contiguous buffer of one OR MORE H.264
//     NAL units in Annex-B form, i.e. each NAL is preceded by the start
//     code `00 00 00 01`. The packetiser splits on start codes and runs
//     RFC-6184 framing (single-NAL or FU-A as appropriate).
//   - `pts_us` is a presentation timestamp in microseconds, monotonic per
//     stream. The packetiser converts to the 90 kHz RTP clock.
//   - `is_keyframe` is purely advisory (RFC 6184 packetisation looks at
//     NAL types directly), but `NullCameraSource` sets it so test
//     harnesses can sanity-check that the loop emits keyframes regularly.
//
// `StreamInfo::sps` / `pps` MUST be in raw NAL form (no start code, no
// length prefix). They're base64-encoded as `sprop-parameter-sets` in
// the SDP.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_I_CAMERA_SOURCE_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_I_CAMERA_SOURCE_HPP

#include <cstdint>
#include <optional>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace server {

struct VideoFrame {
    // One or more H.264 NAL units in Annex-B form (each prefixed by
    // 00 00 00 01). See header comment for the rationale.
    std::vector<uint8_t> nal_data;
    // Presentation timestamp in microseconds. Monotonic per stream.
    int64_t              pts_us      = 0;
    // Advisory: true iff this access unit contains an IDR / SPS / PPS NAL.
    bool                 is_keyframe = false;
};

class ICameraSource {
public:
    virtual ~ICameraSource() = default;

    // Lifecycle. `open` returns true iff the source is ready to deliver
    // frames; on failure the source stays closed and the server can fall
    // back to another source. `close` is idempotent; `is_open` is cheap.
    virtual bool open()           = 0;
    virtual void close()          = 0;
    virtual bool is_open() const  = 0;

    // Pull the next frame. Blocks up to `timeout_ms` milliseconds. Returns
    // `std::nullopt` on timeout OR on end-of-stream — the caller can
    // disambiguate via `is_open()` (false means EOS / source dropped).
    virtual std::optional<VideoFrame> next_frame(int timeout_ms) = 0;

    // Codec advertised in StreamInfo. `H264_AnnexB` is the historical
    // default and matches NullCameraSource / LanCameraSource /
    // CloudCameraSource (all of which produce H.264 NAL units in Annex-B
    // form). `MotionJpeg` is for sources that emit raw JPEG frames (one
    // JPEG per VideoFrame, in `nal_data`) — e.g. `JpegCameraSource` for
    // A1 / P1-series printers, which speak the OpenBambuAPI port-6000
    // JPEG-streaming protocol. Defaults to `H264_AnnexB` so existing
    // sources/tests need no change.
    //
    // RtspServer's RTP packetiser currently only handles H264_AnnexB;
    // adding RFC-2435 RTP-JPEG packetisation is tracked separately. A
    // MotionJpeg source still surfaces `next_frame()` and is fully
    // unit-testable in isolation.
    enum class Codec {
        H264_AnnexB = 0,
        MotionJpeg  = 1,
    };

    // Per-stream advertising info. Filled in once `open()` succeeds and
    // remains stable for the lifetime of the open source. The SDP-shaped
    // fields (sps, pps) MUST be raw NAL units (no start code) and are
    // only meaningful when `codec == H264_AnnexB`.
    struct StreamInfo {
        int                   width  = 0;
        int                   height = 0;
        int                   fps    = 0;
        Codec                 codec  = Codec::H264_AnnexB;
        std::vector<uint8_t>  sps;
        std::vector<uint8_t>  pps;
    };
    virtual StreamInfo info() const = 0;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_I_CAMERA_SOURCE_HPP
