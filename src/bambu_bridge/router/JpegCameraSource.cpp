// Bambu Bridge — A1 / P1 native JPEG-on-port-6000 camera source.
//
// Wire format: OpenBambuAPI/video.md, section "A1 and P1".
// TLS settings mirror LocalControlTunnel (TLS 1.2 only, no cert verify,
// AES256-GCM-SHA384, empty SNI — printer rejects connections with SNI set).

#include "../platform/WinsockShim.hpp"

#include "JpegCameraSource.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#ifndef _WIN32
#include <poll.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

// Auth packet constants from OpenBambuAPI/video.md.
constexpr uint32_t kAuthPayloadSize = 0x40;     // bytes 0..3
constexpr uint32_t kAuthTypeJpeg    = 0x3000;   // bytes 4..7
constexpr std::size_t kAuthPacketLen = 80;      // 16-byte header + 32 user + 32 pass
constexpr std::size_t kFrameHeaderLen = 16;
// JPEG SOI / EOI markers for sanity checking received payloads.
constexpr uint8_t kJpegSoi[2] = {0xFF, 0xD8};
constexpr uint8_t kJpegEoi[2] = {0xFF, 0xD9};
// Cap to avoid runaway alloc on a malicious / corrupted server. Typical
// 1280x720 high-quality JPEGs are ~150 kB; 8 MB is comfortably above
// anything plausible.
constexpr uint32_t kMaxPayloadBytes = 8u * 1024u * 1024u;

void write_u32_le(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v & 0xff);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

uint32_t read_u32_le(const uint8_t* in) {
    return  static_cast<uint32_t>(in[0])
         | (static_cast<uint32_t>(in[1]) <<  8)
         | (static_cast<uint32_t>(in[2]) << 16)
         | (static_cast<uint32_t>(in[3]) << 24);
}

std::string to_upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Token-aware substring search — matches "A1" in "A1", "A1mini", "A1 mini",
// "3DPrinter-A1", but NOT in arbitrary unrelated strings. Mirrors the
// pattern used by NativeStorageDelegate. We intentionally accept ANY
// substring containing the token rather than requiring word boundaries
// because Bambu's vendor model strings are extremely inconsistent.
bool contains_substr(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Portable single-fd poll wrapper. POSIX uses ::poll/struct pollfd;
// Winsock provides the byte-compatible WSAPoll/WSAPOLLFD with the same
// POLLIN/POLLOUT semantics. Both take the same (fds, nfds, timeout_ms)
// shape, so this is a behaviour-preserving alias.
inline int bridge_poll_one(int fd, short events, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD p{};
    p.fd      = static_cast<SOCKET>(fd);
    p.events  = events;
    p.revents = 0;
    return ::WSAPoll(&p, 1, timeout_ms);
#else
    pollfd p{fd, events, 0};
    return ::poll(&p, 1, timeout_ms);
#endif
}

int wait_writable(int fd, int timeout_ms) {
    return bridge_poll_one(fd, POLLOUT, timeout_ms);
}

int wait_readable(int fd, int timeout_ms) {
    return bridge_poll_one(fd, POLLIN, timeout_ms);
}

// Connect TCP non-blocking, return fd or -1. Same shape as
// LocalControlTunnel::tcp_connect so a future refactor can share.
int tcp_connect(const std::string& host, int port, int timeout_ms) {
    addrinfo  hints{};
    addrinfo* res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);
    if (::getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
        return -1;
    }

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        // SOCK_NONBLOCK is a Linux socket()-flag extension with no Winsock
        // equivalent; create the socket normally then flip it non-blocking
        // via the shim (ioctlsocket FIONBIO on Windows, fcntl O_NONBLOCK on
        // POSIX). Same end state.
        fd = static_cast<int>(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (fd < 0) continue;
        bambu_set_nonblocking(fd, true);
        int yes = 1;
        bambu_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        int rc = ::connect(fd, ai->ai_addr, static_cast<bridge_socklen_t>(ai->ai_addrlen));
        if (rc == 0) break;
        // A non-blocking connect in progress is EINPROGRESS on POSIX and
        // WSAEWOULDBLOCK on Winsock; accept either.
        const int conn_err = bambu_last_socket_error();
        if (conn_err == EINPROGRESS || conn_err == EWOULDBLOCK) {
            int pr = wait_writable(fd, timeout_ms);
            if (pr > 0) {
                int err = 0;
                int errlen = sizeof(err);
                if (bambu_getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == 0 && err == 0) {
                    break;
                }
            }
        }
        bambu_close_socket(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

}  // namespace

// ---- model-gate helpers (free functions) ---------------------------------

bool is_jpeg_camera_model(const std::string& model) {
    if (model.empty()) return false;
    const std::string up = to_upper(model);
    // X1 / H2 series have their own protocols on 6000 — exclude first.
    if (contains_substr(up, "X1") || contains_substr(up, "H2")) return false;
    if (contains_substr(up, "C11") || contains_substr(up, "C12")) return false;  // X1 internals
    if (contains_substr(up, "C16") || contains_substr(up, "C18")) return false;  // H2D / H2S internals

    // A1 family — both user-visible and internal codes.
    if (contains_substr(up, "A1"))  return true;   // A1, A1 mini, A1MINI
    if (contains_substr(up, "N1"))  return true;   // A1 mini internal
    if (contains_substr(up, "N2S")) return true;   // A1 internal

    // P1 family.
    if (contains_substr(up, "P1"))  return true;   // P1P, P1S
    if (contains_substr(up, "C13")) return true;   // P1P internal
    if (contains_substr(up, "C14")) return true;   // P1S internal
    return false;
}

std::string jpeg_camera_model_tag(const std::string& model) {
    if (model.empty()) return "";
    const std::string up = to_upper(model);
    if (contains_substr(up, "A1MINI") || contains_substr(up, "A1 MINI") ||
        contains_substr(up, "N1")) return "A1mini";
    if (contains_substr(up, "A1") || contains_substr(up, "N2S")) return "A1";
    if (contains_substr(up, "P1S") || contains_substr(up, "C14")) return "P1S";
    if (contains_substr(up, "P1P") || contains_substr(up, "C13")) return "P1P";
    if (contains_substr(up, "P1"))  return "P1";
    return model;
}

// ---- JpegCameraSource ----------------------------------------------------

JpegCameraSource::JpegCameraSource(JpegCameraSourceConfig cfg)
    : m_cfg(std::move(cfg)) {}

JpegCameraSource::~JpegCameraSource() {
    close();
}

std::vector<uint8_t> JpegCameraSource::build_auth_packet() const {
    std::vector<uint8_t> pkt(kAuthPacketLen, 0);
    write_u32_le(&pkt[0],  kAuthPayloadSize);
    write_u32_le(&pkt[4],  kAuthTypeJpeg);
    write_u32_le(&pkt[8],  0);   // flags
    write_u32_le(&pkt[12], 0);   // reserved
    // Username at offset 16, 32 bytes NUL-padded.
    const std::size_t user_n = std::min<std::size_t>(32, m_cfg.username.size());
    std::memcpy(&pkt[16], m_cfg.username.data(), user_n);
    // Password (access code) at offset 48, 32 bytes NUL-padded.
    const std::size_t pass_n = std::min<std::size_t>(32, m_cfg.access_code.size());
    std::memcpy(&pkt[48], m_cfg.access_code.data(), pass_n);
    return pkt;
}

bool JpegCameraSource::parse_frame_header(const uint8_t* hdr16,
                                          uint32_t&      out_payload_size,
                                          uint32_t&      out_itrack,
                                          uint32_t&      out_flags) {
    if (!hdr16) return false;
    out_payload_size = read_u32_le(hdr16 + 0);
    out_itrack       = read_u32_le(hdr16 + 4);
    out_flags        = read_u32_le(hdr16 + 8);
    // bytes 12..15 are documented as 0; we don't enforce — A1 firmware
    // hasn't always honoured "reserved" fields. The payload-size sanity
    // bound below catches genuine corruption.
    return out_payload_size > 0 && out_payload_size <= kMaxPayloadBytes;
}

int JpegCameraSource::tcp_connect_(int timeout_ms) {
    return tcp_connect(m_cfg.printer_ip, m_cfg.printer_port, timeout_ms);
}

int JpegCameraSource::tls_handshake_(int timeout_ms) {
    m_ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!m_ssl_ctx) return -1;
    SSL_CTX_set_min_proto_version(m_ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(m_ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(m_ssl_ctx, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_cipher_list(m_ssl_ctx, "AES256-GCM-SHA384");

    m_ssl = SSL_new(m_ssl_ctx);
    if (!m_ssl) return -1;
    SSL_set_fd(m_ssl, m_fd);
    // Empty SNI — A1/P1 (and X1/H2) reject otherwise.
    SSL_set_tlsext_host_name(m_ssl, nullptr);

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (true) {
        int r = SSL_connect(m_ssl);
        if (r == 1) return 0;
        int e = SSL_get_error(m_ssl, r);
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return -1;
        const int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (e == SSL_ERROR_WANT_READ) {
            if (wait_readable(m_fd, remain) <= 0) return -1;
        } else if (e == SSL_ERROR_WANT_WRITE) {
            if (wait_writable(m_fd, remain) <= 0) return -1;
        } else {
            std::fprintf(stderr,
                "[jpeg-camera] dev=%s tls handshake err=%d\n",
                m_cfg.dev_id.c_str(), e);
            std::fflush(stderr);
            return -1;
        }
    }
}

int JpegCameraSource::io_write_full_(const uint8_t* src, std::size_t n, int timeout_ms) {
    if (!m_ssl) return -1;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    std::size_t off = 0;
    while (off < n) {
        int r = SSL_write(m_ssl, src + off, static_cast<int>(n - off));
        if (r > 0) { off += static_cast<std::size_t>(r); continue; }
        int e = SSL_get_error(m_ssl, r);
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return -1;
        const int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (e == SSL_ERROR_WANT_READ) {
            if (wait_readable(m_fd, remain) <= 0) return -1;
        } else if (e == SSL_ERROR_WANT_WRITE) {
            if (wait_writable(m_fd, remain) <= 0) return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

int JpegCameraSource::io_read_full_(uint8_t* dst, std::size_t n, int timeout_ms) {
    if (!m_ssl) return -1;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    std::size_t off = 0;
    while (off < n) {
        int r = SSL_read(m_ssl, dst + off, static_cast<int>(n - off));
        if (r > 0) { off += static_cast<std::size_t>(r); continue; }
        int e = SSL_get_error(m_ssl, r);
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return -1;
        const int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (e == SSL_ERROR_WANT_READ) {
            if (wait_readable(m_fd, remain) <= 0) return -1;
        } else if (e == SSL_ERROR_WANT_WRITE) {
            if (wait_writable(m_fd, remain) <= 0) return -1;
        } else if (e == SSL_ERROR_ZERO_RETURN) {
            // Clean TLS shutdown — treat as EOF.
            return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

int JpegCameraSource::send_auth_(int timeout_ms) {
    const auto pkt = build_auth_packet();
    return io_write_full_(pkt.data(), pkt.size(), timeout_ms);
}

bool JpegCameraSource::open() {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_open.load()) return true;

    if (m_cfg.printer_ip.empty()) {
        std::fprintf(stderr,
            "[jpeg-camera] dev=%s open FAIL: empty printer_ip\n",
            m_cfg.dev_id.c_str());
        std::fflush(stderr);
        return false;
    }

    const int connect_ms = static_cast<int>(m_cfg.connect_timeout.count());

    m_fd = tcp_connect_(connect_ms);
    if (m_fd < 0) {
        std::fprintf(stderr,
            "[jpeg-camera] dev=%s open FAIL: tcp_connect %s:%u errno=%d\n",
            m_cfg.dev_id.c_str(), m_cfg.printer_ip.c_str(),
            static_cast<unsigned>(m_cfg.printer_port), bambu_last_socket_error());
        std::fflush(stderr);
        return false;
    }
    if (tls_handshake_(connect_ms) != 0) {
        close_locked_();
        return false;
    }
    if (send_auth_(connect_ms) != 0) {
        std::fprintf(stderr,
            "[jpeg-camera] dev=%s open FAIL: auth write\n",
            m_cfg.dev_id.c_str());
        std::fflush(stderr);
        close_locked_();
        return false;
    }

    // The server does NOT send an auth-OK reply — per OpenBambuAPI/video.md
    // the next bytes on the wire are the first frame's 16-byte header (or
    // the TLS session closes if auth was wrong). We defer "did auth
    // succeed?" to the first next_frame() — a closed socket there will
    // surface as a parse failure / EOF.

    // Per video.md the stream is always 1280x720 JPEG. fps isn't given;
    // observed firmware is ~10fps. Advertise that for SDP-equivalent
    // consumers (RtspServer's MJPEG path is a TODO; until then this is
    // informational only).
    m_info             = server::ICameraSource::StreamInfo{};
    m_info.width       = 1280;
    m_info.height      = 720;
    m_info.fps         = 10;
    m_info.codec       = server::ICameraSource::Codec::MotionJpeg;
    // sps/pps remain empty — meaningless for MJPEG.

    m_jpeg_scratch.reserve(256 * 1024);
    m_frames_seen = 0;
    m_t0          = std::chrono::steady_clock::now();
    m_open.store(true);

    std::fprintf(stderr,
        "[jpeg-camera] dev=%s open OK %s:%u (MJPEG 1280x720)\n",
        m_cfg.dev_id.c_str(), m_cfg.printer_ip.c_str(),
        static_cast<unsigned>(m_cfg.printer_port));
    std::fflush(stderr);
    return true;
}

void JpegCameraSource::close() {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_open.load() && m_fd < 0 && !m_ssl) return;
    close_locked_();
}

void JpegCameraSource::close_locked_() {
    m_open.store(false);
    if (m_ssl) {
        SSL_shutdown(m_ssl);
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_ssl_ctx) {
        SSL_CTX_free(m_ssl_ctx);
        m_ssl_ctx = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool JpegCameraSource::is_open() const {
    return m_open.load();
}

std::optional<server::VideoFrame>
JpegCameraSource::next_frame(int timeout_ms) {
    if (!m_open.load()) return std::nullopt;

    const int effective_timeout = std::max(
        timeout_ms,
        static_cast<int>(m_cfg.io_timeout.count()));

    uint8_t hdr[kFrameHeaderLen];
    if (io_read_full_(hdr, sizeof(hdr), effective_timeout) != 0) {
        std::lock_guard<std::mutex> lk(m_mu);
        close_locked_();
        return std::nullopt;
    }

    uint32_t payload_size = 0, itrack = 0, flags = 0;
    if (!parse_frame_header(hdr, payload_size, itrack, flags)) {
        std::fprintf(stderr,
            "[jpeg-camera] dev=%s bad frame header size=%u\n",
            m_cfg.dev_id.c_str(), payload_size);
        std::fflush(stderr);
        std::lock_guard<std::mutex> lk(m_mu);
        close_locked_();
        return std::nullopt;
    }

    if (m_jpeg_scratch.size() < payload_size) {
        m_jpeg_scratch.resize(payload_size);
    }
    if (io_read_full_(m_jpeg_scratch.data(), payload_size, effective_timeout) != 0) {
        std::lock_guard<std::mutex> lk(m_mu);
        close_locked_();
        return std::nullopt;
    }

    // Sanity: JPEG SOI / EOI. We don't fail closed on a missing EOI —
    // some firmwares pad differently — but we DO check SOI as a cheap
    // protocol-sync check. A missing SOI almost always means we're
    // re-reading mid-stream after a partial frame.
    if (payload_size < 2 ||
        m_jpeg_scratch[0] != kJpegSoi[0] ||
        m_jpeg_scratch[1] != kJpegSoi[1]) {
        std::fprintf(stderr,
            "[jpeg-camera] dev=%s frame missing SOI (size=%u first=0x%02X%02X)\n",
            m_cfg.dev_id.c_str(), payload_size,
            payload_size >= 1 ? m_jpeg_scratch[0] : 0,
            payload_size >= 2 ? m_jpeg_scratch[1] : 0);
        std::fflush(stderr);
        std::lock_guard<std::mutex> lk(m_mu);
        close_locked_();
        return std::nullopt;
    }
    // EOI check is advisory only.
    (void) kJpegEoi;

    server::VideoFrame f;
    f.nal_data.assign(m_jpeg_scratch.begin(),
                      m_jpeg_scratch.begin() + payload_size);
    // PTS: frames are paced by the printer. We synthesise a monotonic
    // microsecond clock from steady_clock so downstream consumers see a
    // sensible cadence. Each MJPEG frame is independent so every frame
    // is effectively a keyframe.
    const auto now = std::chrono::steady_clock::now();
    f.pts_us = std::chrono::duration_cast<std::chrono::microseconds>(now - m_t0).count();
    f.is_keyframe = true;
    ++m_frames_seen;
    return f;
}

server::ICameraSource::StreamInfo JpegCameraSource::info() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_info;
}

}  // namespace router
}  // namespace bridge
}  // namespace Slic3r
