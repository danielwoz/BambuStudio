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

#include "ICameraSource.hpp"

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

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#endif

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace Slic3r {
namespace bridge {
namespace server {

namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

namespace {

// ---------------------------------------------------------------------------
// OpenSSL bootstrap (idempotent).
// ---------------------------------------------------------------------------
struct OpenSSLInit {
    OpenSSLInit() {
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
        struct sigaction sa{};
        sa.sa_handler = SIG_IGN;
        ::sigaction(SIGPIPE, &sa, nullptr);
    }
};
void ensure_openssl_init() {
    static OpenSSLInit s_init;
    (void)s_init;
}

void log_ssl_err(const char* where) {
    unsigned long e = ERR_peek_last_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    std::fprintf(stderr, "[rtsp-server] ssl-err at %s: %s\n",
                 where, buf[0] ? buf : "no-error");
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

// Bind+listen via asio. Returns nullptr on failure, otherwise an owning
// pointer to a listening acceptor on the given io_context.
std::unique_ptr<tcp::acceptor> open_listener_asio(
        asio::io_context& io, const std::string& ip, uint16_t port,
        int backlog, uint16_t& bound_port_out)
{
    auto acc = std::make_unique<tcp::acceptor>(io);
    error_code ec;
    asio::ip::address bind_addr;
    if (ip.empty() || ip == "0.0.0.0") {
        bind_addr = asio::ip::address_v4::any();
    } else {
        bind_addr = asio::ip::make_address(ip, ec);
        if (ec) return nullptr;
    }
    tcp::endpoint ep(bind_addr, port);
    acc->open(ep.protocol(), ec);
    if (!ec) acc->set_option(asio::socket_base::reuse_address(true), ec);
    if (!ec) acc->bind(ep, ec);
    if (!ec) acc->listen(backlog, ec);
    if (ec) return nullptr;
    bound_port_out = acc->local_endpoint().port();
    return acc;
}

bool ssl_write_all(SSL* ssl, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t off = 0;
    while (off < n) {
        int w = SSL_write(ssl, p + off, static_cast<int>(n - off));
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
        if (SSL_pending(ssl) == 0) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
            int s = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
            if (s < 0) return false;
            if (s == 0) continue;
        }
        uint8_t tmp[2048];
        int n = SSL_read(ssl, tmp, sizeof(tmp));
        if (n > 0) {
            buf.insert(buf.end(), tmp, tmp + n);
            return true;
        }
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ) continue;
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

bool write_rtsp_response(SSL* ssl, int code, const char* status,
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
    return ssl_write_all(ssl, s.data(), s.size());
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
bool send_rtp_interleaved(SSL* ssl, uint8_t channel,
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

    if (!ssl_write_all(ssl, frm, sizeof(frm))) return false;
    if (!ssl_write_all(ssl, hdr, sizeof(hdr))) return false;
    if (plen && !ssl_write_all(ssl, payload, plen)) return false;
    return true;
}

// Packetise ONE NAL unit per RFC 6184. Single-NAL when small, FU-A
// fragmentation when bigger than max_payload. `marker` should be true
// on the LAST NAL of the access unit.
bool packetise_nal(SSL* ssl, uint8_t channel,
                   uint16_t& seq, uint32_t ts, uint32_t ssrc,
                   const uint8_t* nal, size_t nlen,
                   bool last_nal_of_au,
                   size_t max_payload) {
    if (nlen == 0) return true;
    if (nlen <= max_payload) {
        return send_rtp_interleaved(ssl, channel, seq, ts, ssrc,
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
        if (!send_rtp_interleaved(ssl, channel, seq, ts, ssrc,
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
                      const std::vector<uint8_t>& sps_raw,
                      const std::vector<uint8_t>& pps_raw) {
    // profile-level-id: from SPS bytes [1..3] if present, else a safe default.
    char plid[8] = "42C00A"; // baseline level 1.0 — matches our null source.
    if (sps_raw.size() >= 4) {
        std::snprintf(plid, sizeof(plid), "%02X%02X%02X",
                      static_cast<unsigned>(sps_raw[1]),
                      static_cast<unsigned>(sps_raw[2]),
                      static_cast<unsigned>(sps_raw[3]));
    }
    std::string sprop;
    if (!sps_raw.empty()) sprop += b64_encode(sps_raw.data(), sps_raw.size());
    if (!pps_raw.empty()) {
        if (!sprop.empty()) sprop += ",";
        sprop += b64_encode(pps_raw.data(), pps_raw.size());
    }

    std::ostringstream os;
    os << "v=0\r\n"
       << "o=- 0 0 IN IP4 0.0.0.0\r\n"
       << "s=Bambu Bridge Camera\r\n"
       << "c=IN IP4 0.0.0.0\r\n"
       << "t=0 0\r\n"
       << "a=control:" << ctrl_base << "\r\n"
       << "m=video 0 RTP/AVP 96\r\n"
       << "a=rtpmap:96 H264/90000\r\n"
       << "a=fmtp:96 packetization-mode=1;profile-level-id=" << plid
       << ";sprop-parameter-sets=" << sprop << "\r\n"
       << "a=control:streamid=0\r\n";
    return os.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Per-device + per-session state.
// ---------------------------------------------------------------------------

struct RtspServer::Device {
    RtspVirtualDevice spec;
    SSL_CTX*          ssl_ctx    = nullptr;

    // asio plumbing per-device.
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<tcp::acceptor>    acceptor;
    uint16_t          bound_port = 0;

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
    d->ssl_ctx = make_device_ctx(d->spec.cert);
    if (!d->ssl_ctx) {
        throw std::runtime_error("RtspServer: failed to build SSL_CTX for dev_id="
                                 + d->spec.dev_id);
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

    if (SSL_accept(sess->ssl) != 1) {
        log_ssl_err("SSL_accept(rtsp)");
        return;
    }

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
    if (src && !src->is_open()) (void)src->open();

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
        return write_rtsp_response(sess->ssl, 200, "OK", cseq, h, body);
    };

    // Helper: send one access-unit's worth of RTP packets for a Frame
    // pulled from the source. Returns false on TLS write failure.
    auto stream_one_frame = [&](const VideoFrame& f) -> bool {
        std::vector<std::pair<size_t,size_t>> ranges;
        split_annexb_nals(f.nal_data, ranges);
        if (ranges.empty()) return true;
        // 90kHz RTP clock.
        const uint32_t ts = static_cast<uint32_t>(
            (static_cast<int64_t>(f.pts_us) * 90LL) / 1000LL);
        for (size_t i = 0; i < ranges.size(); ++i) {
            const uint8_t* p = f.nal_data.data() + ranges[i].first;
            const size_t   n = ranges[i].second;
            const bool last  = (i + 1 == ranges.size());
            if (!packetise_nal(sess->ssl, rtp_channel, rtp_seq, ts, rtp_ssrc,
                               p, n, last,
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
        if (playing && src) {
            auto frame = src->next_frame(33);
            if (frame) {
                if (!stream_one_frame(*frame)) {
                    std::fprintf(stderr,
                        "[rtsp-server] session=%s stream write failed; "
                        "closing\n", session_id.c_str());
                    return;
                }
            }
            // Non-blocking probe for a new control message between frames.
            fd_set rfds; FD_ZERO(&rfds); FD_SET(sess->fd, &rfds);
            timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 0;
            int s = ::select(sess->fd + 1, &rfds, nullptr, nullptr, &tv);
            if (s <= 0 && SSL_pending(sess->ssl) == 0) continue;
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
                    write_rtsp_response(sess->ssl, 401, "Unauthorized", cseq,
                        {{"WWW-Authenticate", "Basic realm=\"bambu\""}}, "");
                    continue;
                }
            }
            std::string sdp = build_sdp(req.target, si.sps, si.pps);
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
                write_rtsp_response(sess->ssl, 461, "Unsupported transport", cseq, {}, "");
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
                write_rtsp_response(sess->ssl, 455, "Method Not Valid In This State", cseq, {}, "");
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
            write_rtsp_response(sess->ssl, 501, "Not Implemented", cseq, {}, "");
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
            std::fprintf(stderr, "[rtsp-server] failed to start device %s: %s\n",
                         kv.first.c_str(), ex.what());
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
    if (d.acceptor) return;
    uint16_t bound = 0;
    d.io = std::make_unique<asio::io_context>();
    d.acceptor = open_listener_asio(*d.io, d.spec.lan_ip, d.spec.port,
                                    m_cfg.accept_backlog, bound);
    if (!d.acceptor) {
        d.io.reset();
        throw std::runtime_error(std::string("RtspServer: listen failed for ") +
                                 d.spec.lan_ip + ":" + std::to_string(d.spec.port));
    }
    d.bound_port = bound;
    d.stopped.store(false);

    const RtspServerConfig cfg = m_cfg;

    d.accept_thread = std::thread([&d, cfg]() {
        while (!d.stopped.load()) {
            tcp::socket client_sock(*d.io);
            error_code aec = asio::error::would_block;
            d.acceptor->async_accept(
                client_sock,
                [&aec](const error_code& e) { aec = e; });
            d.io->restart();
            d.io->run_for(std::chrono::milliseconds(200));
            if (aec == asio::error::would_block) {
                error_code ignore;
                d.acceptor->cancel(ignore);
                d.io->run();
                continue;
            }
            if (aec) {
                if (aec == asio::error::operation_aborted) break;
                continue;
            }
            auto native = client_sock.native_handle();
            error_code rel_ec;
            client_sock.release(rel_ec);
            int cfd = static_cast<int>(native);

            int one = 1;
#ifndef _WIN32
            ::setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            if (cfg.io_timeout_seconds > 0) {
                timeval rt{}; rt.tv_sec = cfg.io_timeout_seconds;
                ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
            }
#else
            ::setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE,
                         reinterpret_cast<const char*>(&one), sizeof(one));
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&one), sizeof(one));
            if (cfg.io_timeout_seconds > 0) {
                DWORD rt_ms = cfg.io_timeout_seconds * 1000;
                ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO,
                             reinterpret_cast<const char*>(&rt_ms), sizeof(rt_ms));
            }
#endif

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
#ifdef _WIN32
                    ::closesocket(cfd);
#else
                    ::close(cfd);
#endif
                    continue;
                }
            }

            SSL* ssl = SSL_new(d.ssl_ctx);
            if (!ssl) {
#ifdef _WIN32
                ::closesocket(cfd);
#else
                ::close(cfd);
#endif
                continue;
            }
            SSL_set_fd(ssl, cfd);

            auto sess = std::make_unique<RtspServer::Device::Session>();
            sess->ssl = ssl;
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
    if (d.acceptor) {
        error_code ignore;
        d.acceptor->close(ignore);
    }
    if (d.accept_thread.joinable()) d.accept_thread.join();
    d.acceptor.reset();
    d.io.reset();

    std::vector<std::unique_ptr<Device::Session>> drained;
    {
        std::lock_guard<std::mutex> lk(d.sessions_mu);
        drained = std::move(d.sessions);
        d.sessions.clear();
    }
    for (auto& s : drained) {
        s->stopped.store(true);
        if (s->fd >= 0) {
#ifdef _WIN32
            ::shutdown(s->fd, SD_BOTH);
#else
            ::shutdown(s->fd, SHUT_RDWR);
#endif
        }
        if (s->io_thread.joinable()) s->io_thread.join();
        if (s->ssl) { SSL_free(s->ssl); s->ssl = nullptr; }
        if (s->fd >= 0) {
#ifdef _WIN32
            ::closesocket(s->fd);
#else
            ::close(s->fd);
#endif
            s->fd = -1;
        }
    }
    if (d.spec.source) d.spec.source->close();
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
