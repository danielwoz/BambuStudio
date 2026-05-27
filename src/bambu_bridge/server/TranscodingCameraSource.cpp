// Bambu Bridge — MJPEG -> H.264 transcoding camera-source decorator (impl).

#include "TranscodingCameraSource.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <x264.h>
}

namespace Slic3r {
namespace bridge {
namespace server {

TranscodingCameraSource::TranscodingCameraSource(std::shared_ptr<ICameraSource> inner)
    : m_inner(std::move(inner)) {}

TranscodingCameraSource::~TranscodingCameraSource() { close(); }

bool TranscodingCameraSource::open() {
    if (!m_inner || !m_inner->open()) return false;

    const StreamInfo in = m_inner->info();
    if (in.codec == Codec::H264_AnnexB) {
        // Already H.264 (LAN/cloud sources) — pure passthrough.
        m_passthrough = true;
        m_out_info    = in;
        m_open.store(true);
        std::fprintf(stderr, "[transcode] passthrough (inner already H.264)\n");
        std::fflush(stderr);
        return true;
    }

    // MotionJpeg inner -> set up the MJPEG decoder now; the H.264 encoder is
    // created lazily on the first decoded frame (we need the real dimensions).
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!dec) { m_inner->close(); return false; }
    m_dec = avcodec_alloc_context3(dec);
    if (!m_dec || avcodec_open2(m_dec, dec, nullptr) < 0) {
        teardown_codecs_(); m_inner->close(); return false;
    }
    m_jframe = av_frame_alloc();
    if (!m_jframe) { teardown_codecs_(); m_inner->close(); return false; }

    // Advertise H.264 with the inner's reported dims/fps; sps/pps are filled
    // in once the first keyframe is encoded.
    m_out_info        = {};
    m_out_info.codec  = Codec::H264_AnnexB;
    m_out_info.width  = in.width;
    m_out_info.height = in.height;
    m_out_info.fps    = in.fps > 0 ? in.fps : 15;
    m_passthrough     = false;
    m_open.store(true);
    std::fprintf(stderr, "[transcode] MJPEG -> H.264 (in %dx%d@%dfps)\n",
                 in.width, in.height, m_out_info.fps);
    std::fflush(stderr);
    return true;
}

bool TranscodingCameraSource::ensure_encoder_(int width, int height) {
    if (m_x264) return (width == m_enc_w && height == m_enc_h);
    if (width <= 0 || height <= 0) return false;

    const int fps = m_out_info.fps > 0 ? m_out_info.fps : 15;
    x264_param_t param;
    if (x264_param_default_preset(&param, "ultrafast", "zerolatency") < 0) return false;
    param.i_csp            = X264_CSP_I420;
    param.i_width          = width;
    param.i_height         = height;
    param.i_fps_num        = fps;
    param.i_fps_den        = 1;
    param.i_keyint_max     = fps;   // ~1s GOP
    param.b_repeat_headers = 1;     // SPS/PPS before every IDR (RTSP-friendly)
    param.b_annexb         = 1;     // Annex-B start codes (RtspServer splits NALs)
    param.i_log_level      = X264_LOG_WARNING;
    x264_param_apply_profile(&param, "baseline");   // widest client compatibility

    m_x264 = x264_encoder_open(&param);
    if (!m_x264) { std::fprintf(stderr, "[transcode] x264_encoder_open failed\n"); return false; }

    // Lift SPS/PPS for the SDP from the encoder's header NALs (start code stripped).
    x264_nal_t* hdr = nullptr; int hn = 0;
    if (x264_encoder_headers(static_cast<x264_t*>(m_x264), &hdr, &hn) >= 0) {
        for (int i = 0; i < hn; ++i) {
            const uint8_t* p = hdr[i].p_payload; const int len = hdr[i].i_payload;
            int off = (len >= 4 && !p[0] && !p[1] && !p[2] && p[3]==1) ? 4
                    : (len >= 3 && !p[0] && !p[1] && p[2]==1)          ? 3 : 0;
            if (off >= len) continue;
            const uint8_t type = p[off] & 0x1F;
            if      (type == 7 && m_out_info.sps.empty()) m_out_info.sps.assign(p + off, p + len);
            else if (type == 8 && m_out_info.pps.empty()) m_out_info.pps.assign(p + off, p + len);
        }
    }

    // I420 staging frame (x264 reads directly from swscale's output planes).
    m_yframe = av_frame_alloc();
    if (!m_yframe) return false;
    m_yframe->format = AV_PIX_FMT_YUV420P;
    m_yframe->width  = width;
    m_yframe->height = height;
    if (av_frame_get_buffer(m_yframe, 32) < 0) return false;

    m_enc_w = width; m_enc_h = height;
    m_out_info.width = width; m_out_info.height = height;
    std::fprintf(stderr, "[transcode] x264 ready %dx%d@%dfps sps=%zuB pps=%zuB\n",
                 width, height, fps, m_out_info.sps.size(), m_out_info.pps.size());
    std::fflush(stderr);
    return true;
}

std::optional<VideoFrame> TranscodingCameraSource::next_frame(int timeout_ms) {
    if (!m_open.load() || !m_inner) return std::nullopt;
    if (m_passthrough) return m_inner->next_frame(timeout_ms);

    auto jf = m_inner->next_frame(timeout_ms);
    if (!jf) return std::nullopt;                 // timeout / EOS — propagate

    // --- decode the JPEG ---
    AVPacket* in = av_packet_alloc();
    if (!in) return std::nullopt;
    in->data = jf->nal_data.data();
    in->size = static_cast<int>(jf->nal_data.size());
    int rc = avcodec_send_packet(m_dec, in);
    av_packet_free(&in);
    if (rc < 0) return std::nullopt;              // drop this frame, keep stream open
    if (avcodec_receive_frame(m_dec, m_jframe) < 0) return std::nullopt;

    const int w = m_jframe->width, h = m_jframe->height;
    if (!ensure_encoder_(w, h)) { av_frame_unref(m_jframe); return std::nullopt; }

    // --- colour-convert decoded (YUVJ*) -> YUV420P ---
    if (!m_sws) {
        m_sws = sws_getContext(w, h, static_cast<AVPixelFormat>(m_jframe->format),
                               w, h, AV_PIX_FMT_YUV420P,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) { av_frame_unref(m_jframe); return std::nullopt; }
    }
    av_frame_make_writable(m_yframe);
    sws_scale(m_sws, m_jframe->data, m_jframe->linesize, 0, h,
              m_yframe->data, m_yframe->linesize);
    av_frame_unref(m_jframe);

    // --- encode I420 -> H.264 Annex-B via x264 ---
    x264_picture_t pin; x264_picture_init(&pin);
    pin.img.i_csp   = X264_CSP_I420;
    pin.img.i_plane = 3;
    for (int p = 0; p < 3; ++p) {
        pin.img.plane[p]    = m_yframe->data[p];
        pin.img.i_stride[p] = m_yframe->linesize[p];
    }
    pin.i_pts = m_pts++;
    x264_picture_t pout;
    x264_nal_t* nals = nullptr; int n = 0;
    const int sz = x264_encoder_encode(static_cast<x264_t*>(m_x264), &nals, &n, &pin, &pout);
    if (sz < 0 || n == 0) return std::nullopt;    // error / buffered (rare w/ zerolatency)
    VideoFrame out;
    out.nal_data.reserve(static_cast<size_t>(sz));
    for (int i = 0; i < n; ++i)
        out.nal_data.insert(out.nal_data.end(),
                            nals[i].p_payload, nals[i].p_payload + nals[i].i_payload);
    out.is_keyframe = (pout.b_keyframe != 0);
    out.pts_us      = jf->pts_us;
    return out;
}

ICameraSource::StreamInfo TranscodingCameraSource::info() const {
    return m_passthrough && m_inner ? m_inner->info() : m_out_info;
}

bool TranscodingCameraSource::is_open() const {
    return m_open.load() && m_inner && m_inner->is_open();
}

void TranscodingCameraSource::teardown_codecs_() {
    if (m_sws)    { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_dec)    avcodec_free_context(&m_dec);
    if (m_x264)   { x264_encoder_close(static_cast<x264_t*>(m_x264)); m_x264 = nullptr; }
    if (m_jframe) av_frame_free(&m_jframe);
    if (m_yframe) av_frame_free(&m_yframe);
    m_enc_w = m_enc_h = 0; m_pts = 0;
}

void TranscodingCameraSource::close() {
    if (!m_open.exchange(false) && !m_inner) return;
    teardown_codecs_();
    if (m_inner) m_inner->close();
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
