// Bambu Bridge — MJPEG -> H.264 transcoding camera-source decorator.
//
// Wraps any ICameraSource. If the inner source advertises H264_AnnexB it is a
// pure passthrough (zero overhead). If it advertises MotionJpeg (the A1 / P1
// port-6000 JPEG cameras via JpegCameraSource) this decodes each JPEG and
// re-encodes it to H.264 Annex-B with libavcodec + libx264, so the RtspServer
// republishes EVERY virtual printer as uniform standard H.264/RFC6184 — which
// the slicer's media backend (GStreamer on Linux, Media Foundation on Windows)
// plays directly, unlike MJPEG-over-RTSP.
//
// Threading: same contract as the wrapped source — next_frame() is single-
// threaded per source; open/close/is_open may race next_frame. All libav
// state is touched only from the next_frame thread (after open()).

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_TRANSCODING_CAMERA_SOURCE_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_TRANSCODING_CAMERA_SOURCE_HPP

#include "ICameraSource.hpp"

#include <atomic>
#include <memory>
#include <string>

// Opaque libav types — kept out of the header so consumers don't need the
// ffmpeg dev headers.
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace Slic3r {
namespace bridge {
namespace server {

class TranscodingCameraSource : public ICameraSource {
public:
    explicit TranscodingCameraSource(std::shared_ptr<ICameraSource> inner);
    ~TranscodingCameraSource() override;

    TranscodingCameraSource(const TranscodingCameraSource&)            = delete;
    TranscodingCameraSource& operator=(const TranscodingCameraSource&) = delete;

    bool open()           override;
    void close()          override;
    bool is_open() const  override;
    std::optional<VideoFrame> next_frame(int timeout_ms) override;
    StreamInfo info() const override;

private:
    bool ensure_encoder_(int width, int height);   // lazy, on first decoded frame
    void teardown_codecs_();

    std::shared_ptr<ICameraSource> m_inner;
    std::atomic<bool>              m_open{false};
    bool                           m_passthrough = false;   // inner already H.264

    // MotionJpeg path only. Decode + colour-convert use ffmpeg (the bundled
    // libavcodec decodes MJPEG fine); the H.264 ENCODE uses libx264 directly
    // because the bundled ffmpeg (LGPL, no --enable-gpl) ships no H.264
    // encoder. x264_t is stored as void* to keep x264.h out of this header.
    AVCodecContext* m_dec    = nullptr;   // mjpeg decoder (ffmpeg)
    SwsContext*     m_sws    = nullptr;   // YUVJ* -> I420 (ffmpeg)
    AVFrame*        m_jframe = nullptr;   // decoded JPEG frame
    AVFrame*        m_yframe = nullptr;   // I420 frame fed to x264
    void*           m_x264   = nullptr;   // x264_t* H.264 encoder
    int             m_enc_w  = 0;
    int             m_enc_h  = 0;
    int64_t         m_pts    = 0;

    StreamInfo m_out_info;   // advertised codec/dims/sps/pps after open()
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_TRANSCODING_CAMERA_SOURCE_HPP
