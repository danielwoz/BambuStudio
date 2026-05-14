// Bambu Bridge — minimal TLS RTSP client for tests (phase 8).

#include "RtspTestClient.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace Slic3r {
namespace bridge {
namespace test {

namespace {

void init_openssl_once() {
    static struct Init {
        Init() {
            SSL_load_error_strings();
            OpenSSL_add_ssl_algorithms();
        }
    } s_init;
    (void)s_init;
}

std::string ssl_err_str() {
    unsigned long e = ERR_peek_last_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    ERR_clear_error();
    return buf[0] ? std::string(buf) : "no-error";
}

int tcp_connect(const std::string& host, uint16_t port,
                std::chrono::seconds timeout, std::string& err_out) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        err_out = "inet_pton failed for " + host;
        return -1;
    }
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err_out = std::strerror(errno); return -1; }
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        err_out = std::strerror(errno); ::close(fd); return -1;
    }
    if (rc < 0) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        timeval tv{}; tv.tv_sec = timeout.count(); tv.tv_usec = 0;
        rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (rc <= 0) { err_out = "connect timeout"; ::close(fd); return -1; }
        int so_err = 0; socklen_t sl = sizeof(so_err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &sl);
        if (so_err != 0) { err_out = std::strerror(so_err); ::close(fd); return -1; }
    }
    ::fcntl(fd, F_SETFL, flags);
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
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

} // namespace

RtspTestClient::RtspTestClient() { init_openssl_once(); }

RtspTestClient::~RtspTestClient() { close(); }

void RtspTestClient::close() {
    if (m_ssl) {
        SSL_shutdown(reinterpret_cast<SSL*>(m_ssl));
        SSL_free(reinterpret_cast<SSL*>(m_ssl));
        m_ssl = nullptr;
    }
    if (m_ctx) {
        SSL_CTX_free(reinterpret_cast<SSL_CTX*>(m_ctx));
        m_ctx = nullptr;
    }
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
    m_recv.clear();
}

bool RtspTestClient::connect(const std::string& host, uint16_t port,
                             std::chrono::seconds timeout) {
    m_fd = tcp_connect(host, port, timeout, m_last_error);
    if (m_fd < 0) return false;

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { m_last_error = "SSL_CTX_new"; close(); return false; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); m_last_error = "SSL_new"; close(); return false; }
    SSL_set_fd(ssl, m_fd);
    if (SSL_connect(ssl) != 1) {
        m_last_error = "SSL_connect: " + ssl_err_str();
        SSL_free(ssl); SSL_CTX_free(ctx);
        close();
        return false;
    }
    m_ctx = ctx;
    m_ssl = ssl;
    return true;
}

bool RtspTestClient::read_some(std::chrono::seconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    SSL* ssl = reinterpret_cast<SSL*>(m_ssl);
    while (std::chrono::steady_clock::now() < deadline) {
        if (SSL_pending(ssl) == 0) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(m_fd, &rfds);
            timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
            int s = ::select(m_fd + 1, &rfds, nullptr, nullptr, &tv);
            if (s < 0) { m_last_error = std::strerror(errno); return false; }
            if (s == 0) continue;
        }
        uint8_t tmp[2048];
        int n = SSL_read(ssl, tmp, sizeof(tmp));
        if (n > 0) {
            m_recv.insert(m_recv.end(), tmp, tmp + n);
            return true;
        }
        int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ) continue;
        m_last_error = "SSL_read: " + ssl_err_str();
        return false;
    }
    m_last_error = "read timeout";
    return false;
}

bool RtspTestClient::consume_one_response(RtspResponse& out) {
    // Skip any leading interleaved RTP/RTCP frames in the buffer until we
    // find an RTSP response (starts with "RTSP/").
    for (;;) {
        if (m_recv.empty()) return false;
        if (m_recv[0] != '$') break;  // not interleaved
        if (m_recv.size() < 4) return false;
        uint16_t len = (static_cast<uint16_t>(m_recv[2]) << 8) | m_recv[3];
        if (m_recv.size() < static_cast<size_t>(4 + len)) return false;
        m_recv.erase(m_recv.begin(), m_recv.begin() + 4 + len);
    }
    // Find "\r\n\r\n" header terminator.
    ssize_t hend = -1;
    for (size_t i = 0; i + 3 < m_recv.size(); ++i) {
        if (m_recv[i] == '\r' && m_recv[i+1] == '\n' &&
            m_recv[i+2] == '\r' && m_recv[i+3] == '\n') {
            hend = static_cast<ssize_t>(i);
            break;
        }
    }
    if (hend < 0) return false;

    std::string head(reinterpret_cast<const char*>(m_recv.data()),
                     static_cast<size_t>(hend));

    // Parse status line.
    auto first_eol = head.find("\r\n");
    if (first_eol == std::string::npos) first_eol = head.size();
    std::string sl = head.substr(0, first_eol);
    // RTSP/1.0 200 OK
    auto sp1 = sl.find(' ');
    auto sp2 = (sp1 == std::string::npos) ? std::string::npos
                                          : sl.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    out.code   = std::atoi(sl.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
    out.status = sl.substr(sp2 + 1);
    out.headers.clear();

    size_t p = first_eol + 2;
    while (p < head.size()) {
        auto eol = head.find("\r\n", p);
        if (eol == std::string::npos) eol = head.size();
        std::string line = head.substr(p, eol - p);
        p = eol + 2;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        auto trim = [](std::string& s) {
            size_t a = 0, b = s.size();
            while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
            while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
            s = s.substr(a, b - a);
        };
        trim(k); trim(v);
        for (auto& c : k) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        out.headers[k] = v;
    }

    size_t clen = 0;
    auto it = out.headers.find("content-length");
    if (it != out.headers.end()) {
        clen = static_cast<size_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    }
    // Drop the parsed header bytes.
    m_recv.erase(m_recv.begin(), m_recv.begin() + hend + 4);
    while (m_recv.size() < clen) {
        if (!read_some(std::chrono::seconds(5))) return false;
    }
    out.body.assign(reinterpret_cast<const char*>(m_recv.data()), clen);
    m_recv.erase(m_recv.begin(), m_recv.begin() + clen);

    // Stash Session id.
    auto sit = out.headers.find("session");
    if (sit != out.headers.end()) {
        std::string sid = sit->second;
        auto semi = sid.find(';');
        if (semi != std::string::npos) sid = sid.substr(0, semi);
        size_t a = 0, b = sid.size();
        while (a < b && (sid[a] == ' ' || sid[a] == '\t')) ++a;
        while (b > a && (sid[b-1] == ' ' || sid[b-1] == '\t')) --b;
        m_session_id = sid.substr(a, b - a);
    }
    return true;
}

bool RtspTestClient::request(const std::string& verb,
                             const std::string& target,
                             const std::vector<std::string>& extra_headers,
                             RtspResponse& resp) {
    if (!m_ssl) { m_last_error = "not connected"; return false; }
    std::ostringstream os;
    os << verb << " " << target << " RTSP/1.0\r\n";
    os << "CSeq: " << (m_cseq++) << "\r\n";
    if (!m_session_id.empty() && verb != "OPTIONS" && verb != "DESCRIBE") {
        os << "Session: " << m_session_id << "\r\n";
    }
    for (const auto& h : extra_headers) {
        os << h << "\r\n";
    }
    os << "\r\n";
    std::string s = os.str();
    if (!ssl_write_all(reinterpret_cast<SSL*>(m_ssl), s.data(), s.size())) {
        m_last_error = "SSL_write: " + ssl_err_str();
        return false;
    }
    // Read responses until one parses (skipping any interleaved frames).
    while (!consume_one_response(resp)) {
        if (!read_some(std::chrono::seconds(5))) return false;
    }
    return true;
}

bool RtspTestClient::options(const std::string& target, RtspResponse& resp) {
    return request("OPTIONS", target, {}, resp);
}
bool RtspTestClient::describe(const std::string& target, RtspResponse& resp) {
    return request("DESCRIBE", target, {"Accept: application/sdp"}, resp);
}
bool RtspTestClient::setup(const std::string& target,
                           int lo, int hi, RtspResponse& resp) {
    char tbuf[128];
    std::snprintf(tbuf, sizeof(tbuf),
        "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d", lo, hi);
    return request("SETUP", target, {tbuf}, resp);
}
bool RtspTestClient::play(const std::string& target, RtspResponse& resp) {
    return request("PLAY", target, {"Range: npt=0.000-"}, resp);
}
bool RtspTestClient::teardown(const std::string& target, RtspResponse& resp) {
    return request("TEARDOWN", target, {}, resp);
}

int RtspTestClient::read_interleaved_rtp(std::vector<RtpFrame>& out,
                                         int max_frames,
                                         std::chrono::seconds deadline_s) {
    auto deadline = std::chrono::steady_clock::now() + deadline_s;
    while ((int)out.size() < max_frames &&
           std::chrono::steady_clock::now() < deadline) {
        // Peel any complete interleaved frame off the buffer.
        if (m_recv.size() >= 4 && m_recv[0] == '$') {
            uint16_t len = (static_cast<uint16_t>(m_recv[2]) << 8) | m_recv[3];
            if (m_recv.size() >= static_cast<size_t>(4 + len)) {
                RtpFrame f;
                f.channel = m_recv[1];
                f.data.assign(m_recv.begin() + 4, m_recv.begin() + 4 + len);
                out.push_back(std::move(f));
                m_recv.erase(m_recv.begin(), m_recv.begin() + 4 + len);
                continue;
            }
        } else if (!m_recv.empty() && m_recv[0] != '$') {
            // Possibly an RTSP response interleaved between frames.
            // Try to consume it; if it fails for want of bytes we'll
            // read more below.
            RtspResponse junk;
            (void)consume_one_response(junk);
            if (!m_recv.empty() && m_recv[0] != '$') {
                // Couldn't make progress — read more.
            }
        }
        const auto rem_s = std::chrono::duration_cast<std::chrono::seconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (rem_s <= 0) break;
        if (!read_some(std::chrono::seconds(std::max<int64_t>(1, rem_s)))) break;
    }
    return static_cast<int>(out.size());
}

} // namespace test
} // namespace bridge
} // namespace Slic3r
