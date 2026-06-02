// Bambu Bridge — passthrough camera-source wrapper.
//
// Wraps any ICameraSource and forwards its frames downstream without
// touching them. The inner source already de-encapsulates the printer's
// proprietary TUTK tunnel into either H.264 Annex-B or MJPEG; the
// RtspServer's RTP packetiser handles both wire formats natively
// (RFC 6184 for H.264, RFC 2435 for MJPEG). No decode, no colour
// convert, no re-encode — strict ffmpeg `-c:v copy` semantics.
//
// History: this class used to decode MJPEG → swscale colour-convert →
// x264-encode H.264 so the downstream wire format was uniformly H.264
// regardless of the printer model. That cost a meaningful chunk of
// CPU per frame on A1-class printers, spawned a long tail of avcodec/
// swscale/x264 worker threads (a noticeable contributor to the
// bridge-child thread leak), and produced one "deprecated pixel
// format used, make sure you did set range correctly" log line per
// stream init. The slicer's video player handles MJPEG-over-RTSP
// natively (libBambuSource for BBS, GStreamer rtspsrc + jpegdec for
// Orca) so none of the transcode work was necessary. Class was
// reduced to a passthrough wrapper and renamed to match what it
// actually does.
//
// Threading: same contract as the wrapped source — next_frame() is
// single-threaded per source; open/close/is_open may race next_frame.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_PASSTHROUGH_CAMERA_SOURCE_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_PASSTHROUGH_CAMERA_SOURCE_HPP

#include "ICameraSource.hpp"

#include <atomic>
#include <memory>

namespace Slic3r {
namespace bridge {
namespace server {

class PassthroughCameraSource : public ICameraSource {
public:
    explicit PassthroughCameraSource(std::shared_ptr<ICameraSource> inner);
    ~PassthroughCameraSource() override;

    PassthroughCameraSource(const PassthroughCameraSource&)            = delete;
    PassthroughCameraSource& operator=(const PassthroughCameraSource&) = delete;

    bool open()           override;
    void close()          override;
    bool is_open() const  override;
    std::optional<VideoFrame> next_frame(int timeout_ms) override;
    StreamInfo info() const override;

private:
    std::shared_ptr<ICameraSource> m_inner;
    std::atomic<bool>              m_open{false};
    StreamInfo                     m_out_info;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_PASSTHROUGH_CAMERA_SOURCE_HPP
