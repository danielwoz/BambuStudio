// Bambu Bridge — per-device RTSPS server implementation (phase 8).
//
// Raw POSIX sockets + OpenSSL. TLS 1.2 server pinned, no client cert
// verification (real-printer parity). The protocol surface is the RTSP/1.0
// method subset slicers actually drive plus the bare-minimum control
// shape ffplay/VLC expect:
//
//   OPTIONS *                  -> 200, Public list.
//   DESCRIBE rtsp://h/foo      -> 200, SDP body.
//   SETUP rtsp://h/foo/track   -> 200, Session, Transport echo.
//   PLAY  rtsp://h/foo         -> 200, kicks off the streaming thread.
//   GET_PARAMETER / KEEP-ALIVE -> 200, empty body.
//   TEARDOWN                   -> 200, stop streaming thread + return.
//
// SETUP only honours `RTP/AVP/TCP;interleaved=0-1`. The data path is
// "interleaved RTP over RTSP": every RTP packet is framed as
//   '$' (0x24), 1-byte channel id, 2-byte big-endian length, payload
// and written on the same TLS socket the RTSP commands arrive on. This
// is the simplest possible camera transport that survives slicer-side
// firewalls (one socket, one TLS handshake) and avoids the second-port
// dance of UDP RTP.
//
// RFC-6184 packetisation:
//   - NAL unit splitter walks the Annex-B `nal_data` from the source.
//   - For NAL <= rtp_max_payload-1 -> single-NAL packet (RFC 6184 §5.6).
//   - For NAL >  rtp_max_payload   -> FU-A (RFC 6184 §5.8) fragmentation.
//     Start fragment sets S=1, end fragment sets E=1, middle fragments
//     have S=0,E=0. The FU-A indicator copies the NAL header's NRI; the
//     FU header carries S/E/R + the original NAL type.
//
// Threading:
//   - One accept thread per device listener.
//   - One I/O thread per accepted RTSP session: it runs the RTSP control
//     loop AND, after PLAY, doubles as the streaming thread (writes to
//     the same TLS socket — single-writer invariant).
//
// What's deliberately not here:
//   - UDP RTP transport. Adds firewall surface + a second socket per
//     session, no operational upside for our LAN use case.
//   - RTCP. Real cameras emit sender reports but slicers tolerate their
//     absence; we'll add this in phase 11 if wire-diff demands it.
//   - RTSP authentication. Stubbed behind RtspServerConfig::require_auth.

#include "RtspServer.hpp"

#include "../router/CameraFrameFanout.hpp"

#include "ICameraSource.hpp"
#include "RtspJpegPacketiser.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../platform/WinsockShim.hpp"
#ifndef _WIN32
#  include <signal.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <chrono>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace Slic3r {
namespace bridge {
namespace server {

namespace {

// ---------------------------------------------------------------------------
// OpenSSL bootstrap (idempotent).
// ---------------------------------------------------------------------------
struct OpenSSLInit {
    OpenSSLInit() {
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
#ifndef _WIN32
        struct sigaction sa{};
        sa.sa_handler = SIG_IGN;
        ::sigaction(SIGPIPE, &sa, nullptr);
#endif
    }
};
void ensure_openssl_init() {
    static OpenSSLInit s_init;
    (void)s_init;
}

// File-flushed diagnostic sink. The genuine GUI exe has dead stderr, so the
// RtspServer's existing [rtsp-server] handshake instrumentation is invisible
// there. Mirror it to $BAMBU_BRIDGE_GUI_LOG (fopen/fwrite/fclose per call so a
// hang leaves the last step on disk). Used to localise the RTSPS SSL_accept
// silent-hang.
void rtsp_flog(const char* fmt, ...) {
    const char* path = std::getenv("BAMBU_BRIDGE_GUI_LOG");
    if (!path || !*path) return;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    FILE* f = std::fopen(path, "a");
    if (!f) return;
    std::fprintf(f, "%lld [rtsp] %s\n", ms, buf);
    std::fclose(f);
}

void log_ssl_err(const char* where) {
    unsigned long e = ERR_peek_last_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    // 2026-06-02: this used to silently swallow the error string —
    // SSL_accept failures during RTSPS handshake from Orca's GStreamer
    // rtspsrc produced no log output, leaving the camera at a black
    // screen with no diagnostic trail. Emit the where + OpenSSL string
    // to stderr so the bridge's per-device log captures it.
    std::fprintf(stderr,
        "[rtsp-server] ssl error at %s: %s\n",
        where ? where : "?", e ? buf : "(no openssl error queued)");
    std::fflush(stderr);
    ERR_clear_error();
}

// Per-device SSL_CTX (server). Same recipe as FtpsServer.
SSL_CTX* make_device_ctx(const tls::CertMaterial& cert) {
    ensure_openssl_init();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { log_ssl_err("SSL_CTX_new"); return nullptr; }
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) != 1) {
        log_ssl_err("SSL_CTX_set_*_proto_version");
        SSL_CTX_free(ctx);
        return nullptr;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    {
        BIO* bio = BIO_new_mem_buf(cert.cert_pem.data(),
                                   static_cast<int>(cert.cert_pem.size()));
        if (!bio) { log_ssl_err("BIO cert"); SSL_CTX_free(ctx); return nullptr; }
        X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!x) { log_ssl_err("PEM_read_bio_X509"); SSL_CTX_free(ctx); return nullptr; }
        if (SSL_CTX_use_certificate(ctx, x) != 1) {
            log_ssl_err("SSL_CTX_use_certificate");
            X509_free(x); SSL_CTX_free(ctx); return nullptr;
        }
        X509_free(x);
    }
    {
        BIO* bio = BIO_new_mem_buf(cert.privkey_pem.data(),
                                   static_cast<int>(cert.privkey_pem.size()));
        if (!bio) { log_ssl_err("BIO key"); SSL_CTX_free(ctx); return nullptr; }
        EVP_PKEY* k = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!k) { log_ssl_err("PEM_read_bio_PrivateKey"); SSL_CTX_free(ctx); return nullptr; }
        if (SSL_CTX_use_PrivateKey(ctx, k) != 1) {
            log_ssl_err("SSL_CTX_use_PrivateKey");
            EVP_PKEY_free(k); SSL_CTX_free(ctx); return nullptr;
        }
        EVP_PKEY_free(k);
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        log_ssl_err("SSL_CTX_check_private_key");
        SSL_CTX_free(ctx); return nullptr;
    }
    return ctx;
}

int open_listener(const std::string& ip, uint16_t port, int backlog,
                  uint16_t& bound_port_out) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    bambu_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        bambu_close_socket(fd);
        errno = EINVAL;
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const int saved = errno; bambu_close_socket(fd); errno = saved; return -1;
    }
    if (::listen(fd, backlog) < 0) {
        const int saved = errno; bambu_close_socket(fd); errno = saved; return -1;
    }
    sockaddr_in actual{};
    bridge_socklen_t len = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
        bound_port_out = ntohs(actual.sin_port);
    } else {
        bound_port_out = port;
    }
    return fd;
}

// Write helper. When ssl is non-null this is TLS (RTSPS, real-printer
// mimicry); when ssl is null we write the raw fd (plain RTSP — standard
// clients like GStreamer/VLC that reject our self-signed cert).
bool ssl_write_all(SSL* ssl, int fd, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t off = 0;
    while (off < n) {
        int w;
        if (ssl) {
            w = SSL_write(ssl, p + off, static_cast<int>(n - off));
        } else {
            ssize_t s = ::send(fd, reinterpret_cast<const char*>(p + off),
                               static_cast<int>(n - off), MSG_NOSIGNAL);
            w = static_cast<int>(s);
        }
        if (w <= 0) return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

// Base64 (RFC 4648) — small, no deps. We need it for SDP
// sprop-parameter-sets and for Basic auth checks.
std::string b64_encode(const uint8_t* data, size_t n) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i+1] << 8 | data[i+2];
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6 ) & 0x3F]);
        out.push_back(tbl[ v        & 0x3F]);
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < n) v |= (uint32_t)data[i+1] << 8;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string to_upper(std::string s) {
    for (auto& c : s) if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    return s;
}

// Read one RTSP message off the TLS socket.
// RTSP/1.0 messages are HTTP-shaped: REQUEST-LINE \r\n HEADERS \r\n \r\n
// [BODY]. We read the head, parse Content-Length, then read the body.
//
// Returns true on success and fills (verb, target, headers, body). false
// on EOF / TLS error.
struct RtspRequest {
    std::string verb;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers; // lower-cased keys
    std::string body;
};

bool read_some(SSL* ssl, int fd, std::vector<uint8_t>& buf,
               std::chrono::seconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!ssl || SSL_pending(ssl) == 0) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
            int s = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
            if (s < 0) return false;
            if (s == 0) continue;
        }
        uint8_t tmp[2048];
        int n;
        if (ssl) {
            n = SSL_read(ssl, tmp, sizeof(tmp));
        } else {
            ssize_t s = ::recv(fd, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
            n = static_cast<int>(s);
        }
        if (n > 0) {
            buf.insert(buf.end(), tmp, tmp + n);
            return true;
        }
        if (ssl) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ) continue;
        } else if (n < 0) {
            const int e = bambu_last_socket_error();
            if (e == EAGAIN || e == EWOULDBLOCK) continue;
        }
        return false;
    }
    return false;
}

bool read_rtsp_request(SSL* ssl, int fd, std::vector<uint8_t>& buf,
                       RtspRequest& out, std::chrono::seconds timeout) {
    // Scan for "\r\n\r\n" header terminator; pull more bytes until found.
    auto find_hdr_end = [&]() -> ssize_t {
        for (size_t i = 0; i + 3 < buf.size(); ++i) {
            if (buf[i] == '\r' && buf[i+1] == '\n' &&
                buf[i+2] == '\r' && buf[i+3] == '\n') {
                return static_cast<ssize_t>(i);
            }
        }
        return -1;
    };

    ssize_t hend = find_hdr_end();
    while (hend < 0) {
        if (!read_some(ssl, fd, buf, timeout)) return false;
        hend = find_hdr_end();
    }
    std::string head(reinterpret_cast<const char*>(buf.data()),
                     static_cast<size_t>(hend));
    buf.erase(buf.begin(), buf.begin() + hend + 4);

    // Parse request line.
    auto first_eol = head.find("\r\n");
    if (first_eol == std::string::npos) first_eol = head.size();
    std::string rl = head.substr(0, first_eol);
    auto sp1 = rl.find(' ');
    auto sp2 = (sp1 == std::string::npos) ? std::string::npos
                                          : rl.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    out.verb    = to_upper(rl.substr(0, sp1));
    out.target  = rl.substr(sp1 + 1, sp2 - sp1 - 1);
    out.version = rl.substr(sp2 + 1);

    // Parse headers.
    out.headers.clear();
    size_t p = first_eol + 2;
    while (p < head.size()) {
        auto eol = head.find("\r\n", p);
        if (eol == std::string::npos) eol = head.size();
        std::string line = head.substr(p, eol - p);
        p = eol + 2;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = trim(line.substr(0, colon));
        std::string v = trim(line.substr(colon + 1));
        for (auto& c : k) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        out.headers[k] = v;
    }

    // Body, if Content-Length says so.
    size_t clen = 0;
    auto it = out.headers.find("content-length");
    if (it != out.headers.end()) {
        clen = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    }
    while (buf.size() < clen) {
        if (!read_some(ssl, fd, buf, timeout)) return false;
    }
    out.body.assign(reinterpret_cast<const char*>(buf.data()), clen);
    buf.erase(buf.begin(), buf.begin() + clen);
    return true;
}

bool write_rtsp_response(SSL* ssl, int fd, int code, const char* status,
                         const std::string& cseq,
                         const std::vector<std::pair<std::string,std::string>>& hdrs,
                         const std::string& body) {
    std::ostringstream os;
    os << "RTSP/1.0 " << code << " " << status << "\r\n";
    os << "CSeq: " << cseq << "\r\n";
    bool has_clen = false;
    for (const auto& kv : hdrs) {
        os << kv.first << ": " << kv.second << "\r\n";
        std::string k = kv.first;
        for (auto& c : k) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if (k == "content-length") has_clen = true;
    }
    if (!body.empty() && !has_clen) {
        os << "Content-Length: " << body.size() << "\r\n";
    }
    os << "\r\n";
    os << body;
    std::string s = os.str();
    return ssl_write_all(ssl, fd, s.data(), s.size());
}

// Forward declaration: RFC 6184 / RFC 2435 packetisers share the
// interleaved RTP-over-TLS sender defined a few hundred lines below.
bool send_rtp_interleaved(SSL* ssl, int fd, uint8_t channel,
                          uint16_t& seq, uint32_t ts, uint32_t ssrc,
                          uint8_t pt, bool marker,
                          const uint8_t* payload, size_t plen);

// ---------------------------------------------------------------------------
// RFC 2435 RTP/JPEG packetisation.
// ---------------------------------------------------------------------------
//
// What RFC 2435 wants on the wire (per RTP packet):
//
//   Main JPEG header (8 bytes, every packet of a frame):
//     Type-specific (1)  | Fragment offset (3) |
//     Type (1)           | Q (1)               |
//     Width (1, /8 px)   | Height (1, /8 px)
//
//   Optional Quantization Table header (only on FIRST packet, when Q in
//   128..255 — we always set Q=255 so receivers don't need a static table):
//     MBZ (1) | Precision (1) | Length (2, BE) | <quant table bytes>
//
//   Payload bytes: the JPEG entropy-coded scan data only (no JFIF / DQT /
//   SOF / SOS headers). The decoder reconstructs those from the RTP-JPEG
//   header fields + quant tables.
//
// What we implement:
//   - Type=1 (4:2:0) or Type=0 (4:2:2), picked from the SOF horizontal
//     sampling factor of component 1 (Y). 4:2:2 cameras like the A1
//     report (Hi=2,Vi=1); 4:2:0 cameras report (Hi=2,Vi=2).
//   - Q=255 + Quantization Table header so we don't depend on receiver-side
//     static tables. Precision=0 (8-bit) — every IP camera ships 8-bit DQTs.
//   - Width/Height in units of 8 px; capped at 2040 px (RFC 2435 §3.1.5).
//   - Fragment offset advances per-fragment; M (marker) bit set on the
//     final fragment of each frame.
//   - Restart Marker Header: NOT emitted. Most IP cameras (incl A1) don't
//     use restart markers, and many decoders mishandle this header anyway.
//     If we ever see DRI > 0 in the source frame we'll need to extend.
//
// Sources whose codec is MotionJpeg deliver one whole JPEG per VideoFrame in
// `nal_data`. The packetiser parses the JFIF in-place, extracts dimensions
// + quant tables + scan-data offset/length, and emits one or more RTP
// packets all sharing the same timestamp.

// Type alias: anonymous-namespace shorthand for the test-exposed parse struct.
using JpegParse = RtpJpegParse;

// Parse a JFIF buffer. Tolerates missing JFIF APP0 marker — many IP cameras
// emit raw "SOI ... DQT ... SOF ... SOS ... data ... EOI" without an APP0.
// Returns ok=true iff width/height/scan-data and at least one quant table
// were found.
JpegParse parse_jfif(const uint8_t* p, size_t n) {
    JpegParse out;
    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) return out;  // SOI
    size_t i = 2;
    while (i + 1 < n) {
        if (p[i] != 0xFF) { ++i; continue; }
        // Skip fill bytes.
        while (i < n && p[i] == 0xFF) ++i;
        if (i >= n) break;
        const uint8_t marker = p[i++];
        if (marker == 0xD9 || marker == 0xDA) {
            // EOI or SOS — SOS gets its own handling below.
            if (marker == 0xDA) {
                // SOS: 2-byte length, then `Ns` + 2*Ns + 3 bytes of header,
                // then scan data through EOI.
                if (i + 2 > n) return out;
                const uint16_t sos_len =
                    (static_cast<uint16_t>(p[i]) << 8) | p[i + 1];
                if (i + sos_len > n) return out;
                const size_t scan_start = i + sos_len;
                // Walk to EOI (search for FF D9, skipping stuffed FF 00 and
                // restart-marker FF D0..D7).
                size_t j = scan_start;
                size_t scan_end = n;
                while (j + 1 < n) {
                    if (p[j] != 0xFF) { ++j; continue; }
                    const uint8_t m2 = p[j + 1];
                    if (m2 == 0x00 ||
                        (m2 >= 0xD0 && m2 <= 0xD7)) { j += 2; continue; }
                    if (m2 == 0xD9) { scan_end = j; break; }
                    // Some other marker before EOI — unexpected but treat as
                    // end-of-scan to avoid corrupting downstream data.
                    scan_end = j;
                    break;
                }
                out.scan_off = scan_start;
                out.scan_len = (scan_end > scan_start) ? (scan_end - scan_start) : 0;
                break;
            }
            // EOI without prior SOS — malformed.
            return out;
        }
        // Standalone markers with no length: D0..D7 (RST), 01, 02 etc.
        if ((marker >= 0xD0 && marker <= 0xD7) ||
            marker == 0x01) {
            continue;
        }
        // All other markers carry a 2-byte length covering the length bytes.
        if (i + 2 > n) return out;
        const uint16_t seg_len =
            (static_cast<uint16_t>(p[i]) << 8) | p[i + 1];
        if (seg_len < 2 || i + seg_len > n) return out;
        const uint8_t* seg = p + i + 2;
        const size_t   slen = seg_len - 2;

        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            // SOF0 / SOF1 / SOF2: precision(1), height(2 BE), width(2 BE),
            // Nf(1), then Nf*(Ci, HiVi, Tqi).
            if (slen < 6) return out;
            out.height = (static_cast<int>(seg[1]) << 8) | seg[2];
            out.width  = (static_cast<int>(seg[3]) << 8) | seg[4];
            const uint8_t nf = seg[5];
            if (nf >= 1 && slen >= 6u + static_cast<size_t>(nf) * 3u) {
                // Component 1 sampling factors: high nibble = H, low = V.
                const uint8_t hv = seg[6 + 1];   // (Ci, HiVi, Tqi)
                const uint8_t h  = (hv >> 4) & 0x0F;
                const uint8_t v  = hv & 0x0F;
                // Type=0 (yuv422): H=2,V=1.  Type=1 (yuv420): H=2,V=2.
                // Fallback to Type=1 for anything else (most permissive).
                if (h == 2 && v == 1) out.type = 0;
                else if (h == 2 && v == 2) out.type = 1;
                else                      out.type = 1;
            }
        } else if (marker == 0xDB) {
            // DQT: one or more (Pq|Tq, q0..q63) blocks. We collect 8-bit
            // tables in slot-0/slot-1 order to keep RFC-2435 receivers happy.
            size_t k = 0;
            while (k < slen) {
                if (k + 1 > slen) return out;
                const uint8_t pq_tq = seg[k++];
                const uint8_t pq    = (pq_tq >> 4) & 0x0F;
                const size_t  qsize = (pq == 0) ? 64u : 128u;
                if (k + qsize > slen) return out;
                // For RFC 2435 Q=255 with Precision=0 we only emit 8-bit
                // tables. Skip 16-bit tables — cameras almost never use them.
                if (pq == 0) {
                    out.qtables.insert(out.qtables.end(),
                                       seg + k, seg + k + qsize);
                }
                k += qsize;
            }
        }
        // Other markers (APPn, COM, DRI, DHT, etc.) are skipped — the
        // decoder reconstructs DHT from defaults and we ignore DRI as noted.
        i += seg_len;
    }

    out.ok = (out.width > 0 && out.height > 0 &&
              out.scan_len > 0 && !out.qtables.empty());
    return out;
}

// Build the per-packet payload sequence (RTP-JPEG header + optional QT
// header + scan bytes) for one MJPEG frame. The 12-byte RTP header is NOT
// included — callers prepend it (either via send_rtp_interleaved or, for
// tests, by constructing it directly from `seq`/`ts`/`ssrc`).
//
// On the wire each packet looks like:
//   first  : [8 B JPEG hdr][4 B QT hdr][qtables][scan bytes...]
//   others : [8 B JPEG hdr][scan bytes...]
//
// All packets carry the same width/height/type/Q in the JPEG header; the
// fragment-offset field advances per packet. The caller sets the RTP M
// (marker) bit on the LAST packet — this builder doesn't model M directly.
std::vector<std::vector<uint8_t>>
build_rtp_jpeg_payloads(const JpegParse& jp,
                        const uint8_t* scan, size_t scan_len,
                        size_t max_payload) {
    std::vector<std::vector<uint8_t>> out;
    if (scan_len == 0) return out;

    // RFC 2435 §3.1.5: Width/Height fields are 8-px units; max 2040 px.
    // Above that the encoder must use a JPEG2000-style extension we don't
    // implement; cap at 2040 — A1 is 1280x720, well under.
    auto px_to_field = [](int px) -> uint8_t {
        int u = (px + 7) / 8;
        if (u > 255) u = 255;
        return static_cast<uint8_t>(u);
    };
    const uint8_t w_field = px_to_field(jp.width);
    const uint8_t h_field = px_to_field(jp.height);

    // Precompute QT header (goes on first packet only).
    const size_t qt_total = jp.qtables.size();
    std::vector<uint8_t> qt_hdr;
    qt_hdr.reserve(4 + qt_total);
    qt_hdr.push_back(0x00);                                            // MBZ
    qt_hdr.push_back(0x00);                                            // Precision (8-bit)
    qt_hdr.push_back(static_cast<uint8_t>((qt_total >> 8) & 0xFF));    // Len hi
    qt_hdr.push_back(static_cast<uint8_t>( qt_total       & 0xFF));    // Len lo
    qt_hdr.insert(qt_hdr.end(), jp.qtables.begin(), jp.qtables.end());

    const size_t jpeg_hdr_bytes = 8;
    uint32_t     frag_off = 0;
    size_t       remain   = scan_len;
    const uint8_t* p      = scan;
    bool         first    = true;

    while (remain > 0) {
        std::vector<uint8_t> pkt;
        pkt.reserve(max_payload + 16);

        // RTP-JPEG main header.
        pkt.push_back(0x00);                                              // type-spec
        pkt.push_back(static_cast<uint8_t>((frag_off >> 16) & 0xFF));     // frag off hi
        pkt.push_back(static_cast<uint8_t>((frag_off >>  8) & 0xFF));
        pkt.push_back(static_cast<uint8_t>( frag_off        & 0xFF));
        pkt.push_back(jp.type);
        pkt.push_back(255);                                                // Q=255 (QTs included)
        pkt.push_back(w_field);
        pkt.push_back(h_field);

        size_t budget = (max_payload > jpeg_hdr_bytes)
            ? (max_payload - jpeg_hdr_bytes) : 1;
        if (first) {
            if (budget <= qt_hdr.size()) {
                // Pathological: max_payload too small to fit even the QT
                // header on the first packet. Emit a runt packet — losing
                // one packet's MTU compliance beats dropping the frame.
                pkt.insert(pkt.end(), qt_hdr.begin(), qt_hdr.end());
                budget = 1;
            } else {
                pkt.insert(pkt.end(), qt_hdr.begin(), qt_hdr.end());
                budget -= qt_hdr.size();
            }
        }
        const size_t chunk = std::min(remain, budget);
        pkt.insert(pkt.end(), p, p + chunk);

        out.push_back(std::move(pkt));
        p        += chunk;
        remain   -= chunk;
        frag_off += static_cast<uint32_t>(chunk);
        first     = false;
    }
    return out;
}

// Build & send all RTP packets for one MJPEG frame. Returns false on TLS
// write failure. Marker bit goes on the last packet.
bool packetise_jpeg_frame(SSL* ssl, int fd, uint8_t channel,
                          uint16_t& seq, uint32_t ts, uint32_t ssrc,
                          const JpegParse& jp,
                          const uint8_t* scan, size_t scan_len,
                          size_t max_payload) {
    auto payloads = build_rtp_jpeg_payloads(jp, scan, scan_len, max_payload);
    for (size_t i = 0; i < payloads.size(); ++i) {
        const bool marker = (i + 1 == payloads.size());
        if (!send_rtp_interleaved(ssl, fd, channel, seq, ts, ssrc,
                                  /*pt=*/26, marker,
                                  payloads[i].data(), payloads[i].size())) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// RFC 6184 H.264 packetisation.
// ---------------------------------------------------------------------------

// Split an Annex-B buffer into individual NAL bodies (without the start
// codes). Out vector is invalidated each call.
void split_annexb_nals(const std::vector<uint8_t>& in,
                       std::vector<std::pair<size_t,size_t>>& out_ranges) {
    out_ranges.clear();
    const size_t n = in.size();
    size_t i = 0;
    // Find first start code.
    auto find_start = [&](size_t from) -> ssize_t {
        for (size_t j = from; j + 2 < n; ++j) {
            if (in[j] == 0 && in[j+1] == 0) {
                if (in[j+2] == 1) return static_cast<ssize_t>(j + 3);
                if (j + 3 < n && in[j+2] == 0 && in[j+3] == 1)
                    return static_cast<ssize_t>(j + 4);
            }
        }
        return -1;
    };
    ssize_t s = find_start(0);
    while (s >= 0) {
        ssize_t ns = find_start(static_cast<size_t>(s));
        size_t end = (ns < 0) ? n : static_cast<size_t>(ns) -
                                    ((static_cast<size_t>(ns) >= 4 &&
                                      in[ns-4] == 0) ? 4 : 3);
        if (end > static_cast<size_t>(s)) {
            out_ranges.emplace_back(static_cast<size_t>(s),
                                    end - static_cast<size_t>(s));
        }
        if (ns < 0) break;
        s = ns;
    }
    (void)i;
}

// Build & send a single RTP packet (header + payload). Returns false on
// TLS write failure. `marker` sets the M bit; `pt` is the payload type.
//
// We don't need to expose the RTP header struct outside this fn.
bool send_rtp_interleaved(SSL* ssl, int fd, uint8_t channel,
                          uint16_t& seq, uint32_t ts, uint32_t ssrc,
                          uint8_t pt, bool marker,
                          const uint8_t* payload, size_t plen) {
    uint8_t hdr[12];
    hdr[0] = 0x80;                      // V=2, P=0, X=0, CC=0
    hdr[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | (pt & 0x7F));
    hdr[2] = static_cast<uint8_t>(seq >> 8);
    hdr[3] = static_cast<uint8_t>(seq & 0xFF);
    hdr[4] = static_cast<uint8_t>((ts >> 24) & 0xFF);
    hdr[5] = static_cast<uint8_t>((ts >> 16) & 0xFF);
    hdr[6] = static_cast<uint8_t>((ts >>  8) & 0xFF);
    hdr[7] = static_cast<uint8_t>( ts        & 0xFF);
    hdr[8] = static_cast<uint8_t>((ssrc >> 24) & 0xFF);
    hdr[9] = static_cast<uint8_t>((ssrc >> 16) & 0xFF);
    hdr[10]= static_cast<uint8_t>((ssrc >>  8) & 0xFF);
    hdr[11]= static_cast<uint8_t>( ssrc        & 0xFF);
    ++seq;

    // Interleaved frame: '$' chan len_hi len_lo <12-byte RTP hdr><payload>
    const uint16_t total = static_cast<uint16_t>(12 + plen);
    uint8_t frm[4];
    frm[0] = 0x24;
    frm[1] = channel;
    frm[2] = static_cast<uint8_t>(total >> 8);
    frm[3] = static_cast<uint8_t>(total & 0xFF);

    if (!ssl_write_all(ssl, fd, frm, sizeof(frm))) return false;
    if (!ssl_write_all(ssl, fd, hdr, sizeof(hdr))) return false;
    if (plen && !ssl_write_all(ssl, fd, payload, plen)) return false;
    return true;
}

// Packetise ONE NAL unit per RFC 6184. Single-NAL when small, FU-A
// fragmentation when bigger than max_payload. `marker` should be true
// on the LAST NAL of the access unit.
bool packetise_nal(SSL* ssl, int fd, uint8_t channel,
                   uint16_t& seq, uint32_t ts, uint32_t ssrc,
                   const uint8_t* nal, size_t nlen,
                   bool last_nal_of_au,
                   size_t max_payload) {
    if (nlen == 0) return true;
    if (nlen <= max_payload) {
        return send_rtp_interleaved(ssl, fd, channel, seq, ts, ssrc,
                                    /*pt=*/96,
                                    /*marker=*/last_nal_of_au,
                                    nal, nlen);
    }
    // FU-A: peel the NAL header off and fragment the rest.
    const uint8_t  nal_hdr  = nal[0];
    const uint8_t  nal_nri  = nal_hdr & 0x60;
    const uint8_t  nal_type = nal_hdr & 0x1F;
    const uint8_t  fu_ind   = static_cast<uint8_t>(nal_nri | 28); // 28 = FU-A
    const uint8_t* body     = nal + 1;
    size_t         remain   = nlen - 1;
    size_t         off      = 0;
    const size_t   frag_max = (max_payload > 2) ? max_payload - 2 : 1;
    bool           first    = true;

    while (remain > 0) {
        const size_t chunk = std::min(remain, frag_max);
        const bool   last  = (remain == chunk);
        uint8_t      fu_h  = nal_type & 0x1F;
        if (first) fu_h |= 0x80;
        if (last)  fu_h |= 0x40;

        std::vector<uint8_t> buf;
        buf.reserve(2 + chunk);
        buf.push_back(fu_ind);
        buf.push_back(fu_h);
        buf.insert(buf.end(), body + off, body + off + chunk);

        const bool m = last && last_nal_of_au;
        if (!send_rtp_interleaved(ssl, fd, channel, seq, ts, ssrc,
                                  /*pt=*/96, m,
                                  buf.data(), buf.size())) {
            return false;
        }
        first   = false;
        off    += chunk;
        remain -= chunk;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SDP building.
// ---------------------------------------------------------------------------

std::string build_sdp(const std::string& ctrl_base,
                      const ICameraSource::StreamInfo& si) {
    std::ostringstream os;
    os << "v=0\r\n"
       << "o=- 0 0 IN IP4 0.0.0.0\r\n"
       << "s=Bambu Bridge Camera\r\n"
       << "c=IN IP4 0.0.0.0\r\n"
       << "t=0 0\r\n"
       << "a=control:" << ctrl_base << "\r\n";

    if (si.codec == ICameraSource::Codec::MotionJpeg) {
        // RFC 2435 / RFC 3551 §A.5 — JPEG is RTP static payload type 26 with
        // a 90kHz clock. No fmtp parameters are required (Q-tables ship in
        // the RTP-JPEG header on the first packet of each frame).
        os << "m=video 0 RTP/AVP 26\r\n"
           << "a=rtpmap:26 JPEG/90000\r\n";
        if (si.fps > 0)    os << "a=framerate:" << si.fps << "\r\n";
        if (si.width > 0 && si.height > 0) {
            os << "a=x-dimensions:" << si.width << "," << si.height << "\r\n";
        }
        os << "a=control:streamid=0\r\n";
        return os.str();
    }

    // H.264 path (default).
    // profile-level-id: from SPS bytes [1..3] if present, else a safe default.
    char plid[8] = "42C00A"; // baseline level 1.0 — matches our null source.
    if (si.sps.size() >= 4) {
        std::snprintf(plid, sizeof(plid), "%02X%02X%02X",
                      static_cast<unsigned>(si.sps[1]),
                      static_cast<unsigned>(si.sps[2]),
                      static_cast<unsigned>(si.sps[3]));
    }
    std::string sprop;
    if (!si.sps.empty()) sprop += b64_encode(si.sps.data(), si.sps.size());
    if (!si.pps.empty()) {
        if (!sprop.empty()) sprop += ",";
        sprop += b64_encode(si.pps.data(), si.pps.size());
    }

    os << "m=video 0 RTP/AVP 96\r\n"
       << "a=rtpmap:96 H264/90000\r\n"
       << "a=fmtp:96 packetization-mode=1;profile-level-id=" << plid
       << ";sprop-parameter-sets=" << sprop << "\r\n"
       << "a=control:streamid=0\r\n";
    return os.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// RtspJpegPacketiser.hpp shims — public surface for unit tests. The bodies
// just forward to the anonymous-namespace helpers used by the streaming loop.
// ---------------------------------------------------------------------------

RtpJpegParse rtp_jpeg_parse(const uint8_t* data, std::size_t n) {
    return parse_jfif(data, n);
}

std::vector<std::vector<uint8_t>>
rtp_jpeg_build_packets(uint16_t& /*seq*/, uint32_t /*ts*/, uint32_t /*ssrc*/,
                       const RtpJpegParse& jp,
                       const uint8_t* scan, std::size_t scan_len,
                       std::size_t max_payload) {
    // Note: seq/ts/ssrc are accepted for API symmetry with the production
    // sender (and to leave room for future RTP-header-included variants),
    // but the packetiser itself doesn't need them — they live in the
    // 12-byte RTP header which test code synthesises separately.
    return build_rtp_jpeg_payloads(jp, scan, scan_len, max_payload);
}

// ---------------------------------------------------------------------------
// Per-device + per-session state.
// ---------------------------------------------------------------------------

struct RtspServer::Device {
    RtspVirtualDevice spec;
    SSL_CTX*          ssl_ctx    = nullptr;
    int               listen_fd  = -1;
    uint16_t          bound_port = 0;

    // Single-reader fan-out wrapper around `spec.source`. One reader thread
    // drains the upstream regardless of how many slicer sessions are
    // currently watching (or zero) — solves the SDK-frame-drop issue
    // observed empirically and lets `max_sessions_per_device > 1` actually
    // serve concurrent watchers from the same upstream stream.
    std::shared_ptr<router::CameraFrameFanout> fanout;

    std::atomic<bool> stopped{false};
    std::thread       accept_thread;

    struct Session {
        SSL*              ssl = nullptr;
        int               fd  = -1;
        std::thread       io_thread;
        std::atomic<bool> stopped{false};
    };
    std::vector<std::unique_ptr<Session>> sessions;
    std::mutex                            sessions_mu;
};

// ---------------------------------------------------------------------------
// ctor / dtor / device registry.
// ---------------------------------------------------------------------------

RtspServer::RtspServer(RtspServerConfig cfg) : m_cfg(std::move(cfg)) {
    ensure_openssl_init();
}

RtspServer::~RtspServer() { stop(); }

RtspServer::Device* RtspServer::find_locked(const std::string& dev_id) {
    auto it = m_devices.find(dev_id);
    return it == m_devices.end() ? nullptr : it->second.get();
}

void RtspServer::add_device(RtspVirtualDevice dev) {
    auto d     = std::make_unique<Device>();
    d->spec    = std::move(dev);
    if (d->spec.tls) {
        d->ssl_ctx = make_device_ctx(d->spec.cert);
        if (!d->ssl_ctx) {
            throw std::runtime_error("RtspServer: failed to build SSL_CTX for dev_id="
                                     + d->spec.dev_id);
        }
    }  // else: plain RTSP, no TLS context
    if (d->spec.source) {
        d->fanout = router::CameraFrameFanout::create(d->spec.source);
    }
    Device* raw = d.get();
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        m_devices[d->spec.dev_id] = std::move(d);
    }
    if (m_running.load()) start_device(*raw);
}

void RtspServer::remove_device(const std::string& dev_id) {
    std::unique_ptr<Device> evicted;
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        auto it = m_devices.find(dev_id);
        if (it == m_devices.end()) return;
        evicted = std::move(it->second);
        m_devices.erase(it);
    }
    if (evicted) {
        stop_device(*evicted);
        if (evicted->ssl_ctx) { SSL_CTX_free(evicted->ssl_ctx); evicted->ssl_ctx = nullptr; }
    }
}

uint16_t RtspServer::bound_port(const std::string& dev_id) const {
    std::lock_guard<std::mutex> lk(m_devices_mu);
    auto it = m_devices.find(dev_id);
    if (it == m_devices.end()) return 0;
    return it->second->bound_port;
}

// ---------------------------------------------------------------------------
// Per-connection / per-session I/O.
// ---------------------------------------------------------------------------

namespace {

void session_io_loop(RtspServer::Device* dev,
                     RtspServer::Device::Session* sess,
                     const RtspServerConfig& cfg) {
    struct Cleanup {
        RtspServer::Device::Session* sess;
        ~Cleanup() { sess->stopped.store(true); }
    } cleanup{sess};

    // 2026-06-02: silent hang under TLS handshake from GStreamer rtspsrc
    // — instrument every step so we can localise where SSL_accept stops
    // returning. Cleared up once cursors=1 lands; remove these prints
    // after the camera path is empirically stable.
    std::fprintf(stderr,
        "[rtsp-server] session_io_loop start fd=%d ssl=%p tls=%d\n",
        sess->fd, (void*)sess->ssl, int(sess->ssl != nullptr));
    std::fflush(stderr);
    rtsp_flog("session_io_loop start fd=%d ssl=%p tls=%d",
              sess->fd, (void*)sess->ssl, int(sess->ssl != nullptr));

    if (sess->ssl) {  // TLS (RTSPS); plain RTSP skips the handshake
        rtsp_flog("SSL_accept ENTER fd=%d", sess->fd);
        const int acc = SSL_accept(sess->ssl);
        std::fprintf(stderr,
            "[rtsp-server] SSL_accept fd=%d returned %d (err=%d)\n",
            sess->fd, acc, acc <= 0 ? SSL_get_error(sess->ssl, acc) : 0);
        std::fflush(stderr);
        rtsp_flog("SSL_accept EXIT fd=%d rc=%d err=%d", sess->fd, acc,
                  acc <= 0 ? SSL_get_error(sess->ssl, acc) : 0);
        if (acc != 1) {
            log_ssl_err("SSL_accept(rtsp)");
            return;
        }
    }
    std::fprintf(stderr,
        "[rtsp-server] handshake ok fd=%d — entering control loop\n", sess->fd);
    std::fflush(stderr);
    rtsp_flog("handshake ok fd=%d — entering control loop", sess->fd);

    // Per-session control state.
    std::vector<uint8_t> recv;
    std::string          session_id;
    bool                 setup_done   = false;
    bool                 playing      = false;
    uint8_t              rtp_channel  = 0;
    uint8_t              rtcp_channel = 1;
    uint16_t             rtp_seq      = 1;
    uint32_t             rtp_ssrc     = 0;
    {
        // Stable SSRC per session (derived from millis + ptr).
        auto t = std::chrono::steady_clock::now().time_since_epoch();
        rtp_ssrc = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t).count()) ^
            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sess));
    }

    auto src = dev->spec.source;
    auto fanout = dev->fanout;
    // Open the fanout (which opens the upstream source as a side effect)
    // so the reader thread starts. Idempotent — concurrent sessions all
    // hit the `m_running` short-circuit. We do NOT also call src->open()
    // here: that would double-trigger the upstream's open() and (for
    // CloudCameraSource) burn two 10 s plugin timeouts on failure.
    if (fanout && !fanout->is_open()) (void)fanout->open();
    if (!fanout && src && !src->is_open()) (void)src->open(); // legacy path
    // Per-session cursor — allocated lazily on first PLAY so DESCRIBE-only
    // sessions don't claim a cursor slot. Released when the session
    // unwinds (cursor's dtor runs).
    std::shared_ptr<router::CameraFrameFanout::Cursor> cursor;

    const auto rd_timeout = std::chrono::seconds(cfg.io_timeout_seconds > 0
                                                  ? cfg.io_timeout_seconds : 90);

    // Random session id.
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<uint32_t> dist(0x10000000, 0x7FFFFFFF);
        char b[16];
        std::snprintf(b, sizeof(b), "%08X", dist(rng));
        session_id = b;
    }

    // The streaming loop runs inline on this same thread once PLAY arrives.
    // We drive it as a co-operative state machine: each iteration either
    // (a) reads the next RTSP control message, OR (b) if `playing`, pulls
    // a frame from the source and packetises it. We multiplex via select()
    // so the control channel still gets serviced (TEARDOWN, keepalives).

    auto write_resp_ok = [&](const std::string& cseq,
                             const std::vector<std::pair<std::string,std::string>>& h,
                             const std::string& body) {
        return write_rtsp_response(sess->ssl, sess->fd,200, "OK", cseq, h, body);
    };

    // The codec is fixed at source-open time; cache it so we don't poll
    // info() per frame. Defaults to H264_AnnexB if the source is null
    // (DESCRIBE-then-die path; the streaming loop won't actually run).
    const ICameraSource::Codec stream_codec =
        src ? src->info().codec : ICameraSource::Codec::H264_AnnexB;

    // Per-session SPS+PPS cache (Layer 2 of the BBS-video-freeze fix,
    // 2026-06-02). BambuLib's RTSP client requires SPS+PPS inline before
    // every H.264 IDR — real Bambu cameras provide this; many camera
    // sources only emit them once at stream start. Without inline
    // SPS+PPS the decoder gives up ~2s in, the TCP read pauses, and
    // (combined with the now-fixed missing SO_SNDTIMEO) the bridge used
    // to wedge in a blocking sendmsg forever. Cache updates run inside
    // `stream_one_frame` so the cache always reflects the most recent
    // parameter set seen on the wire.
    std::vector<uint8_t> sps_cache;
    std::vector<uint8_t> pps_cache;

    // Helper: send one access-unit's worth of RTP packets for a Frame
    // pulled from the source. Returns false on TLS write failure.
    auto stream_one_frame = [&](const VideoFrame& f) -> bool {
        // 90kHz RTP clock — shared across codecs (RFC 6184 + RFC 2435).
        const uint32_t ts = static_cast<uint32_t>(
            (static_cast<int64_t>(f.pts_us) * 90LL) / 1000LL);

        if (stream_codec == ICameraSource::Codec::MotionJpeg) {
            if (f.nal_data.empty()) return true;
            JpegParse jp = parse_jfif(f.nal_data.data(), f.nal_data.size());
            if (!jp.ok) {
                // Malformed JPEG — drop this frame. A persistent stream of
                // malformed frames will starve the client; surface as TLS
                // failure only if EVERY frame is bad.
                return true;
            }
            return packetise_jpeg_frame(sess->ssl, sess->fd, rtp_channel, rtp_seq, ts,
                                        rtp_ssrc, jp,
                                        f.nal_data.data() + jp.scan_off,
                                        jp.scan_len,
                                        static_cast<size_t>(cfg.rtp_max_payload));
        }

        std::vector<std::pair<size_t,size_t>> ranges;
        split_annexb_nals(f.nal_data, ranges);
        if (ranges.empty()) return true;

        // First pass — classify NALs, refresh the SPS/PPS cache, and
        // note whether this access unit already carries them inline. NAL
        // header is the first byte of the body; nal_type is the low 5
        // bits (RFC 6184 §1.3).
        bool has_sps = false, has_pps = false, has_idr = false;
        for (auto& r : ranges) {
            if (r.second == 0) continue;
            const uint8_t* p = f.nal_data.data() + r.first;
            const uint8_t nal_type = p[0] & 0x1F;
            if (nal_type == 7) {      // SPS
                sps_cache.assign(p, p + r.second);
                has_sps = true;
            } else if (nal_type == 8) { // PPS
                pps_cache.assign(p, p + r.second);
                has_pps = true;
            } else if (nal_type == 5) { // IDR
                has_idr = true;
            }
        }

        // Second pass — build emission plan. If this access unit is an
        // IDR without inline SPS+PPS, prepend the cached parameter sets
        // so BambuLib's decoder can initialise on every keyframe. RTP
        // marker bit (set on the last NAL via the `last` flag) belongs
        // to the access unit, so the marker still lands on the actual
        // last NAL of the original frame — not on our injected ones.
        struct EmitEntry { const uint8_t* p; size_t n; };
        std::vector<EmitEntry> plan;
        plan.reserve(ranges.size() + 2);
        if (has_idr && !has_sps && !sps_cache.empty())
            plan.push_back({sps_cache.data(), sps_cache.size()});
        if (has_idr && !has_pps && !pps_cache.empty())
            plan.push_back({pps_cache.data(), pps_cache.size()});
        for (auto& r : ranges) {
            if (r.second == 0) continue;
            plan.push_back({f.nal_data.data() + r.first, r.second});
        }

        for (size_t i = 0; i < plan.size(); ++i) {
            const bool last = (i + 1 == plan.size());
            if (!packetise_nal(sess->ssl, sess->fd, rtp_channel, rtp_seq, ts,
                               rtp_ssrc,
                               plan[i].p, plan[i].n, last,
                               static_cast<size_t>(cfg.rtp_max_payload))) {
                return false;
            }
        }
        return true;
    };

    while (!sess->stopped.load() && !dev->stopped.load()) {
        // While PLAY-ing, pump frames between control reads. We use a
        // bounded sleep inside next_frame so the control channel stays
        // responsive (TEARDOWN should land within ~100ms).
        if (playing && (cursor || src)) {
            // Lazy-allocate the fanout cursor on the first PLAY iteration
            // so DESCRIBE-only sessions don't take a slot. If no fanout is
            // wired (legacy / test path), fall back to direct source pull.
            if (!cursor && fanout) cursor = fanout->create_cursor();
            std::optional<VideoFrame> frame;
            if (cursor) {
                frame = cursor->next_frame(33);
            } else if (src) {
                frame = src->next_frame(33);
            }
            if (frame) {
                if (!stream_one_frame(*frame)) {
                    return;
                }
            }
            // Non-blocking probe for a new control message between frames.
            fd_set rfds; FD_ZERO(&rfds); FD_SET(sess->fd, &rfds);
            timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 0;
            int s = ::select(sess->fd + 1, &rfds, nullptr, nullptr, &tv);
            // Plain-RTSP sessions have no SSL — SSL_pending(nullptr) segfaults
            // (this is what crashed the bridge on every camera PLAY). Only
            // consult the SSL read-buffer when there IS an SSL object; for
            // plain TCP, select() alone decides whether a control msg waits.
            if (s <= 0 && (!sess->ssl || SSL_pending(sess->ssl) == 0)) continue;
        }

        RtspRequest req;
        if (!read_rtsp_request(sess->ssl, sess->fd, recv, req, rd_timeout)) {
            // EOF / timeout / TLS error — session over.
            return;
        }
        auto cseq_it = req.headers.find("cseq");
        std::string cseq = (cseq_it == req.headers.end()) ? "0" : cseq_it->second;
        const std::string& verb = req.verb;

        if (verb == "OPTIONS") {
            write_resp_ok(cseq, {
                {"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"}
            }, "");
        }
        else if (verb == "DESCRIBE") {
            // Build SDP from the bound source. If the source isn't open
            // (e.g. LanCameraSource stub returned false), we still
            // describe; the test source (NullCameraSource) is always
            // open after add_device wires it.
            ICameraSource::StreamInfo si;
            if (src) si = src->info();
            // Optional auth check: real printers require Basic bblp:pw.
            if (cfg.require_auth) {
                auto it = req.headers.find("authorization");
                std::string expect = "Basic " + b64_encode(
                    reinterpret_cast<const uint8_t*>(
                        ("bblp:" + dev->spec.access_code).c_str()),
                    5 + dev->spec.access_code.size());
                if (it == req.headers.end() || it->second != expect) {
                    write_rtsp_response(sess->ssl, sess->fd,401, "Unauthorized", cseq,
                        {{"WWW-Authenticate", "Basic realm=\"bambu\""}}, "");
                    continue;
                }
            }
            std::string sdp = build_sdp(req.target, si);
            write_resp_ok(cseq, {
                {"Content-Type", "application/sdp"},
                {"Content-Base", req.target + "/"}
            }, sdp);
        }
        else if (verb == "SETUP") {
            // Parse Transport. We only accept "RTP/AVP/TCP;interleaved=0-1".
            auto tr_it = req.headers.find("transport");
            std::string transport = (tr_it == req.headers.end()) ? "" : tr_it->second;
            std::string lower = transport;
            for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            if (lower.find("rtp/avp/tcp") == std::string::npos) {
                write_rtsp_response(sess->ssl, sess->fd,461, "Unsupported transport", cseq, {}, "");
                continue;
            }
            // Echo the interleaved channels back. Parse "interleaved=a-b".
            auto il = lower.find("interleaved=");
            if (il != std::string::npos) {
                int a = 0, b = 1;
                std::sscanf(lower.c_str() + il + 12, "%d-%d", &a, &b);
                rtp_channel  = static_cast<uint8_t>(a & 0xFF);
                rtcp_channel = static_cast<uint8_t>(b & 0xFF);
            }
            setup_done = true;
            char tbuf[128];
            std::snprintf(tbuf, sizeof(tbuf),
                "RTP/AVP/TCP;unicast;interleaved=%u-%u",
                static_cast<unsigned>(rtp_channel),
                static_cast<unsigned>(rtcp_channel));
            write_resp_ok(cseq, {
                {"Transport", tbuf},
                {"Session",   session_id + ";timeout=60"}
            }, "");
        }
        else if (verb == "PLAY") {
            if (!setup_done) {
                write_rtsp_response(sess->ssl, sess->fd,455, "Method Not Valid In This State", cseq, {}, "");
                continue;
            }
            if (src && !src->is_open()) (void)src->open();
            playing = true;
            // RTP-Info header — real cameras emit one entry per track.
            char info[256];
            std::snprintf(info, sizeof(info),
                "url=%s/streamid=0;seq=%u;rtptime=0",
                req.target.c_str(), static_cast<unsigned>(rtp_seq));
            write_resp_ok(cseq, {
                {"Session", session_id},
                {"RTP-Info", info}
            }, "");
        }
        else if (verb == "TEARDOWN") {
            playing = false;
            write_resp_ok(cseq, {{"Session", session_id}}, "");
            return;
        }
        else if (verb == "GET_PARAMETER" || verb == "SET_PARAMETER") {
            write_resp_ok(cseq, {{"Session", session_id}}, "");
        }
        else {
            write_rtsp_response(sess->ssl, sess->fd,501, "Not Implemented", cseq, {}, "");
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

void RtspServer::start() {
    if (m_running.exchange(true)) return;
    std::lock_guard<std::mutex> lk(m_devices_mu);
    for (auto& kv : m_devices) {
        try { start_device(*kv.second); }
        catch (const std::exception& ex) {
        }
    }
}

void RtspServer::stop() {
    if (!m_running.exchange(false)) return;
    std::vector<Device*> all;
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        all.reserve(m_devices.size());
        for (auto& kv : m_devices) all.push_back(kv.second.get());
    }
    for (Device* d : all) stop_device(*d);
}

void RtspServer::start_device(Device& d) {
    if (d.listen_fd >= 0) return;
    uint16_t bound = 0;
    int fd = open_listener(d.spec.lan_ip, d.spec.port,
                           m_cfg.accept_backlog, bound);
    if (fd < 0) {
        const int saved = errno;
        throw std::runtime_error(std::string("RtspServer: listen failed for ") +
                                 d.spec.lan_ip + ":" + std::to_string(d.spec.port) +
                                 ": " + std::strerror(saved));
    }
    d.listen_fd  = fd;
    d.bound_port = bound;
    d.stopped.store(false);

    const RtspServerConfig cfg = m_cfg;

    d.accept_thread = std::thread([&d, cfg]() {
        while (!d.stopped.load()) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(d.listen_fd, &rfds);
            timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
            int rc = ::select(d.listen_fd + 1, &rfds, nullptr, nullptr, &tv);
            if (rc <= 0) continue;

            sockaddr_in peer{}; bridge_socklen_t plen = sizeof(peer);
            int cfd = ::accept(d.listen_fd,
                               reinterpret_cast<sockaddr*>(&peer), &plen);
            if (cfd < 0) continue;
            rtsp_flog("accept ok dev=%s port=%u cfd=%d tls=%d",
                      d.spec.dev_id.c_str(), (unsigned)d.bound_port, (int)cfd,
                      int(d.spec.tls));

            int one = 1;
            bambu_setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            bambu_setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            if (cfg.io_timeout_seconds > 0) {
                bambu_set_recv_timeout_ms(cfd,
                    static_cast<unsigned>(cfg.io_timeout_seconds) * 1000u);
            }
            // 2026-06-02: BBS' wxMediaCtrl3 (libBambuSource RTSP client)
            // stops draining the TCP socket ~2s in when its decoder
            // can't make progress (likely missing in-band SPS+PPS before
            // each IDR — Layer 2 fix elsewhere in this file). Without
            // SO_SNDTIMEO the bridge's `::send` in `ssl_write_all` blocks
            // forever in sk_stream_wait_memory: the io_loop never
            // returns, no TEARDOWN runs, sessions zombie up with TCP
            // Send-Q queued in megabytes. Hard cap so the write fails
            // with EAGAIN, ssl_write_all returns false, session_io_loop
            // returns, Cleanup flips stopped, and accept_thread reaps.
            {
                bambu_set_send_timeout_ms(cfd, 10u * 1000u);
            }

            // Reap finished sessions; enforce max_sessions_per_device.
            {
                std::lock_guard<std::mutex> lk(d.sessions_mu);
                for (auto it = d.sessions.begin(); it != d.sessions.end();) {
                    if ((*it)->stopped.load() && (*it)->io_thread.joinable()) {
                        (*it)->io_thread.join();
                        it = d.sessions.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (cfg.max_sessions_per_device > 0 &&
                    static_cast<int>(d.sessions.size()) >= cfg.max_sessions_per_device) {
                    bambu_close_socket(cfd);
                    continue;
                }
            }

            SSL* ssl = nullptr;
            if (d.spec.tls) {
                ssl = SSL_new(d.ssl_ctx);
                if (!ssl) { bambu_close_socket(cfd); continue; }
                SSL_set_fd(ssl, cfd);
            }  // else: plain RTSP — io loop uses the raw fd directly

            auto sess = std::make_unique<RtspServer::Device::Session>();
            sess->ssl = ssl;   // nullptr in plain mode
            sess->fd  = cfd;
            sess->stopped.store(false);

            RtspServer::Device::Session* raw = sess.get();
            {
                std::lock_guard<std::mutex> lk(d.sessions_mu);
                d.sessions.push_back(std::move(sess));
            }
            raw->io_thread = std::thread(session_io_loop, &d, raw, cfg);
        }
    });
}

void RtspServer::stop_device(Device& d) {
    d.stopped.store(true);
    if (d.accept_thread.joinable()) d.accept_thread.join();
    if (d.listen_fd >= 0) { bambu_close_socket(d.listen_fd); d.listen_fd = -1; }

    std::vector<std::unique_ptr<Device::Session>> drained;
    {
        std::lock_guard<std::mutex> lk(d.sessions_mu);
        drained = std::move(d.sessions);
        d.sessions.clear();
    }
    for (auto& s : drained) {
        s->stopped.store(true);
        if (s->fd >= 0) ::shutdown(s->fd, SHUT_RDWR);
        if (s->io_thread.joinable()) s->io_thread.join();
        if (s->ssl) { SSL_free(s->ssl); s->ssl = nullptr; }
        if (s->fd >= 0) { bambu_close_socket(s->fd); s->fd = -1; }
    }
    // Stop the fanout reader thread BEFORE closing the upstream — the
    // reader calls upstream->next_frame and we want it joined before
    // the source vanishes. The fanout's close() is idempotent.
    if (d.fanout) d.fanout->close();
    if (d.spec.source) d.spec.source->close();
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
