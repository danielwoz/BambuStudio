// Bambu Bridge — minimal implicit-TLS FTP client for tests (phase 7).

#include "FtpsTestClient.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

FtpsTestClient::FtpsTestClient() { init_openssl_once(); }

FtpsTestClient::~FtpsTestClient() { close(); }

void FtpsTestClient::close() {
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

bool FtpsTestClient::connect(const std::string& host, uint16_t port,
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

    // Read 220 banner.
    int code = read_reply(m_banner);
    if (code != 220) {
        m_last_error = "no 220 banner; got " + std::to_string(code);
        return false;
    }
    return true;
}

bool FtpsTestClient::send_line(const std::string& cmd) {
    std::string wire = cmd + "\r\n";
    return ssl_write_all(reinterpret_cast<SSL*>(m_ssl), wire.data(), wire.size());
}

int FtpsTestClient::read_reply(std::string& body_out) {
    body_out.clear();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto read_one_line = [&](std::string& out) -> bool {
        out.clear();
        for (;;) {
            for (size_t i = 0; i + 1 < m_recv.size(); ++i) {
                if (m_recv[i] == '\r' && m_recv[i+1] == '\n') {
                    out.assign(reinterpret_cast<const char*>(m_recv.data()), i);
                    m_recv.erase(m_recv.begin(), m_recv.begin() + i + 2);
                    return true;
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                m_last_error = "read timeout";
                return false;
            }
            SSL* ssl = reinterpret_cast<SSL*>(m_ssl);
            if (SSL_pending(ssl) == 0) {
                fd_set rfds; FD_ZERO(&rfds); FD_SET(m_fd, &rfds);
                timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
                int s = ::select(m_fd + 1, &rfds, nullptr, nullptr, &tv);
                if (s < 0) { m_last_error = std::strerror(errno); return false; }
                if (s == 0) continue;
            }
            uint8_t tmp[1024];
            int n = SSL_read(ssl, tmp, sizeof(tmp));
            if (n <= 0) {
                int e = SSL_get_error(ssl, n);
                if (e == SSL_ERROR_WANT_READ) continue;
                m_last_error = "SSL_read: " + ssl_err_str();
                return false;
            }
            m_recv.insert(m_recv.end(), tmp, tmp + n);
        }
    };
    std::string ln;
    if (!read_one_line(ln)) return -1;
    if (ln.size() < 4) { m_last_error = "short reply"; return -1; }
    char code_buf[4] = { ln[0], ln[1], ln[2], 0 };
    int code = std::atoi(code_buf);
    if (ln.size() >= 4 && ln[3] == '-') {
        std::string prefix = code_buf;
        prefix.push_back(' ');
        for (;;) {
            std::string more;
            if (!read_one_line(more)) return -1;
            if (more.size() >= 4 && more.compare(0, 4, prefix) == 0) {
                body_out = more.substr(4);
                return code;
            }
        }
    }
    body_out = ln.size() > 4 ? ln.substr(4) : std::string();
    return code;
}

int FtpsTestClient::raw_command(const std::string& cmd, std::string& body_out) {
    if (!send_line(cmd)) { m_last_error = "SSL_write"; return -1; }
    return read_reply(body_out);
}

int FtpsTestClient::login(const std::string& user, const std::string& password) {
    std::string body;
    int code = raw_command("USER " + user, body);
    if (code < 0) return -1;
    if (code == 230) return 230;  // some servers allow no PASS
    if (code != 331) return code; // unexpected — return for caller to inspect
    return raw_command("PASS " + password, body);
}

bool FtpsTestClient::setup_data_channel_tls() {
    std::string body;
    int c1 = raw_command("PBSZ 0", body);
    int c2 = raw_command("PROT P", body);
    int c3 = raw_command("TYPE I", body);
    return c1 == 200 && c2 == 200 && c3 == 200;
}

bool FtpsTestClient::pasv(std::string& data_ip_out, uint16_t& data_port_out) {
    std::string body;
    int code = raw_command("PASV", body);
    if (code != 227) {
        m_last_error = "PASV reply " + std::to_string(code);
        return false;
    }
    auto lp = body.find('(');
    auto rp = body.find(')', lp == std::string::npos ? 0 : lp);
    if (lp == std::string::npos || rp == std::string::npos) {
        m_last_error = "PASV body unparseable: " + body;
        return false;
    }
    std::string inner = body.substr(lp + 1, rp - lp - 1);
    int parts[6] = {0,0,0,0,0,0};
    if (std::sscanf(inner.c_str(), "%d,%d,%d,%d,%d,%d",
                    &parts[0], &parts[1], &parts[2],
                    &parts[3], &parts[4], &parts[5]) != 6) {
        m_last_error = "PASV scanf failed";
        return false;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                  parts[0], parts[1], parts[2], parts[3]);
    data_ip_out   = buf;
    data_port_out = static_cast<uint16_t>((parts[4] << 8) | parts[5]);
    return true;
}

int FtpsTestClient::stor(const std::string& remote_path,
                         const std::string& host_for_data,
                         uint16_t           port_for_data,
                         const std::vector<uint8_t>& payload) {
    // Establish the data-channel TCP connection BEFORE STOR so the kernel
    // accept() on the server side has something to hand off. We delay the
    // TLS handshake until AFTER STOR is sent + 150 is received, because the
    // server only calls SSL_accept() on the data channel after replying 150
    // — running SSL_connect now would block waiting for ServerHello while
    // the server is still blocked reading STOR on the control channel.
    std::string derr;
    int dfd = tcp_connect(host_for_data, port_for_data,
                          std::chrono::seconds(5), derr);
    if (dfd < 0) { m_last_error = "data connect: " + derr; return -1; }

    if (!send_line("STOR " + remote_path)) {
        m_last_error = "STOR write";
        ::close(dfd);
        return -1;
    }
    std::string body;
    int code = read_reply(body);
    if (code != 150 && code != 125) {
        ::close(dfd);
        return code;
    }

    // Server has begun SSL_accept on the data port; run our SSL_connect now.
    SSL_CTX* dctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(dctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(dctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(dctx, SSL_VERIFY_NONE, nullptr);
    SSL* dssl = SSL_new(dctx);
    SSL_set_fd(dssl, dfd);
    if (SSL_connect(dssl) != 1) {
        m_last_error = "data SSL_connect: " + ssl_err_str();
        SSL_free(dssl); SSL_CTX_free(dctx); ::close(dfd);
        return -1;
    }

    if (!payload.empty() && !ssl_write_all(dssl, payload.data(), payload.size())) {
        m_last_error = "data SSL_write: " + ssl_err_str();
        SSL_shutdown(dssl); SSL_free(dssl); SSL_CTX_free(dctx); ::close(dfd);
        return -1;
    }
    SSL_shutdown(dssl); SSL_free(dssl); SSL_CTX_free(dctx); ::close(dfd);

    return read_reply(body);
}

int FtpsTestClient::quit() {
    std::string body;
    return raw_command("QUIT", body);
}

} // namespace test
} // namespace bridge
} // namespace Slic3r
