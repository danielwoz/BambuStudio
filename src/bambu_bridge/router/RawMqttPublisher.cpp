// See header for the full rationale. Code is intentionally minimal —
// no reconnect, no reader thread, no downstream pump — those belong on
// the plugin's persistent session path.

#include "RawMqttPublisher.hpp"

#include "../server/MqttFraming.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "../platform/WinsockShim.hpp"   // sockets/select/timeval (winsock2 first)
#ifndef _WIN32
#  include <signal.h>
#endif

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

void ensure_openssl_init() {
    static std::once_flag once;
    std::call_once(once, []{
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
#ifndef _WIN32
        // SIGPIPE-on-EPIPE protection: SSL_write into a half-closed
        // TCP socket would otherwise raise SIGPIPE process-wide. Winsock
        // has no SIGPIPE, so this is POSIX-only.
        struct sigaction cur{};
        if (::sigaction(SIGPIPE, nullptr, &cur) == 0
            && cur.sa_handler == SIG_DFL) {
            struct sigaction sa{};
            sa.sa_handler = SIG_IGN;
            ::sigemptyset(&sa.sa_mask);
            ::sigaction(SIGPIPE, &sa, nullptr);
        }
#endif
    });
}

void log_ssl_err(const char* where, const std::string& dev_id) {
    unsigned long e = ERR_peek_last_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    std::fprintf(stderr, "[raw-mqtt dev=%s] ssl-err at %s: %s\n",
                 dev_id.c_str(), where, buf[0] ? buf : "no-error");
    ERR_clear_error();
}

int tcp_connect(const std::string& ip, uint16_t port,
                std::chrono::seconds timeout) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        bambu_close_socket(fd); return -1;
    }
    bambu_set_nonblocking(fd, true);
    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int cerr = bambu_last_socket_error();
    if (rc < 0 && cerr != EINPROGRESS && cerr != EWOULDBLOCK) { bambu_close_socket(fd); return -1; }
    if (rc < 0) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        timeval tv{}; tv.tv_sec = static_cast<long>(timeout.count());
        int sr = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (sr <= 0) { bambu_close_socket(fd); return -1; }
        int so_err = 0; int len = sizeof(so_err);
        if (bambu_getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) < 0
            || so_err != 0) { bambu_close_socket(fd); return -1; }
    }
    bambu_set_nonblocking(fd, false);   // restore blocking
    int one = 1;
    bambu_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    bambu_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return fd;
}

SSL_CTX* make_client_ctx(const std::string& cert_path,
                         const std::string& key_path,
                         const std::string& dev_id) {
    ensure_openssl_init();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return nullptr;
    // Minimal: trust OpenSSL 1.1.1k defaults (statically linked).
    // Only thing we need is to disable verify since printer cert is
    // self-signed.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    // The printer's TLS server is ECDHE-only. Without an explicit ECDH
    // curve set the handshake aborts with
    // `tls_process_ske_ecdhe:unable to find ecdh parameters`.
    //
    // Curve choice (P-521): the bridge's BUNDLED OpenSSL 1.1.1k was built
    // with `no-asm` (see deps/OpenSSL/OpenSSL.cmake). In that
    // configuration P-256 and P-384's curve parameter tables are
    // corrupted — EC_GROUP_new_by_curve_name returns "unknown group" and
    // `EC_POINT_set_affine_coordinates: point is not on curve` for them.
    // P-521 uses the generic GFp_simple method which IS intact, and the
    // printer accepts it (verified: `Server Temp Key: ECDH, secp521r1`).
    // X25519 works in the bundled OpenSSL but the printer rejects it
    // (TLS alert 40 = handshake failure). So P-521 is the one curve that
    // both ends agree on.
    //
    // Don't add P-256/P-384 back without first verifying the bundled
    // OpenSSL is rebuilt with-asm — see /tmp/ssl_repro.c for the
    // reproducer that documented this.
    //
    // (SSL_CTX_set_ecdh_auto was deprecated in OpenSSL 1.1.0;
    // SSL_CTX_set1_groups_list is the modern equivalent.)
    SSL_CTX_set_options(ctx, SSL_OP_SINGLE_ECDH_USE);
    SSL_CTX_set1_groups_list(ctx, "P-521");
    // Optional mTLS: load the bridge-cached client cert + key so the
    // printer's LAN broker accepts `print.command=*` and other
    // mTLS-enforced control payloads. Empty paths = leave plain TLS in
    // place (read-only paths don't need mTLS).
    if (!cert_path.empty() && !key_path.empty()) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cert_path.c_str()) != 1) {
            log_ssl_err("SSL_CTX_use_certificate_chain_file", dev_id);
            SSL_CTX_free(ctx);
            return nullptr;
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, key_path.c_str(),
                                        SSL_FILETYPE_PEM) != 1) {
            log_ssl_err("SSL_CTX_use_PrivateKey_file", dev_id);
            SSL_CTX_free(ctx);
            return nullptr;
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            log_ssl_err("SSL_CTX_check_private_key", dev_id);
            SSL_CTX_free(ctx);
            return nullptr;
        }
    }
    return ctx;
}

// Build MQTT 3.1.1 CONNECT (encoders in MqttFraming are server-side
// only — CONNECT is normally decoded by us, not encoded, so hand-roll).
std::vector<uint8_t> build_connect(const std::string& client_id,
                                   const std::string& username,
                                   const std::string& password,
                                   uint16_t keepalive_s,
                                   bool clean_session) {
    using namespace server::mqtt;
    std::vector<uint8_t> body;
    append_mqtt_string(body, "MQTT");
    body.push_back(0x04); // protocol level 4 = MQTT 3.1.1
    uint8_t flags = 0;
    if (!username.empty()) flags |= 0x80;
    if (!password.empty()) flags |= 0x40;
    if (clean_session)     flags |= 0x02;
    body.push_back(flags);
    append_uint16_be(body, keepalive_s);
    append_mqtt_string(body, client_id);
    if (!username.empty()) append_mqtt_string(body, username);
    if (!password.empty()) append_mqtt_string(body, password);

    std::vector<uint8_t> full;
    full.push_back(0x10); // CONNECT
    uint8_t rl[4];
    size_t n = encode_varint(static_cast<uint32_t>(body.size()), rl);
    full.insert(full.end(), rl, rl + n);
    full.insert(full.end(), body.begin(), body.end());
    return full;
}

bool ssl_write_all(SSL* ssl, const std::vector<uint8_t>& buf) {
    size_t off = 0;
    while (off < buf.size()) {
        int n = SSL_write(ssl, buf.data() + off,
                          static_cast<int>(buf.size() - off));
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// Read exactly n bytes from SSL with timeout (whole-buffer deadline).
bool ssl_read_exact(SSL* ssl, int fd, void* out, size_t n,
                    std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    uint8_t* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < n) {
        int r = SSL_read(ssl, p + got, static_cast<int>(n - got));
        if (r > 0) { got += static_cast<size_t>(r); continue; }
        int err = SSL_get_error(ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            timeval tv{};
            auto remain = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - now);
            tv.tv_sec  = remain.count() / 1000000;
            tv.tv_usec = remain.count() % 1000000;
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            int sr = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
            if (sr <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

// Decode MQTT remaining-length from a contiguous buffer position.
// Returns true and advances *off by the number of bytes consumed.
bool read_remaining_length(SSL* ssl, int fd, uint32_t* out_rl,
                           std::chrono::milliseconds timeout) {
    uint32_t value = 0;
    int      mult  = 1;
    for (int i = 0; i < 4; ++i) {
        uint8_t b;
        if (!ssl_read_exact(ssl, fd, &b, 1, timeout)) return false;
        value += (b & 0x7F) * mult;
        if (!(b & 0x80)) { *out_rl = value; return true; }
        mult *= 128;
    }
    return false;
}

} // namespace

int raw_mqtt_publish_oneshot(const RawMqttPublishConfig& cfg,
                             const std::string&          topic,
                             const std::vector<uint8_t>& payload,
                             uint8_t                     qos) {
    using namespace server::mqtt;
    static std::mt19937 s_rng{std::random_device{}()};
    static std::mutex   s_rng_mu;

    const std::string client_id = std::string("bridge-raw-") + cfg.dev_id;
    int fd = tcp_connect(cfg.printer_ip, cfg.printer_port, cfg.connect_timeout);
    if (fd < 0) {
        std::fprintf(stderr,
            "[raw-mqtt dev=%s] tcp_connect %s:%u failed: %s\n",
            cfg.dev_id.c_str(), cfg.printer_ip.c_str(),
            unsigned(cfg.printer_port), std::strerror(errno));
        std::fflush(stderr);
        return -1;
    }

    SSL_CTX* ctx = make_client_ctx(cfg.mtls_cert_path,
                                   cfg.mtls_key_path,
                                   cfg.dev_id);
    if (!ctx) { bambu_close_socket(fd); return -2; }
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); bambu_close_socket(fd); return -2; }
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        log_ssl_err("SSL_connect", cfg.dev_id);
        SSL_free(ssl); SSL_CTX_free(ctx); bambu_close_socket(fd);
        return -2;
    }

    // CONNECT
    auto connect_pkt = build_connect(client_id, cfg.username,
                                     cfg.access_code,
                                     /*keepalive*/ 30,
                                     /*clean_session*/ true);
    if (!ssl_write_all(ssl, connect_pkt)) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); bambu_close_socket(fd);
        return -3;
    }
    // CONNACK: 0x20, RL=2, [flags, rc]
    uint8_t hdr[2];
    if (!ssl_read_exact(ssl, fd, hdr, 2,
        std::chrono::milliseconds(cfg.io_timeout))
        || hdr[0] != 0x20) {
        std::fprintf(stderr,
            "[raw-mqtt dev=%s] CONNACK header read fail (got 0x%02x)\n",
            cfg.dev_id.c_str(), hdr[0]);
        std::fflush(stderr);
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); bambu_close_socket(fd);
        return -3;
    }
    uint8_t connack[2];
    if (!ssl_read_exact(ssl, fd, connack, 2,
        std::chrono::milliseconds(cfg.io_timeout))) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); bambu_close_socket(fd);
        return -3;
    }
    if (connack[1] != 0) {
        std::fprintf(stderr,
            "[raw-mqtt dev=%s] CONNACK rc=%u (expected 0)\n",
            cfg.dev_id.c_str(), unsigned(connack[1]));
        std::fflush(stderr);
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); bambu_close_socket(fd);
        return -3;
    }

    // PUBLISH
    uint16_t pid = 0;
    if (qos > 0) {
        std::lock_guard<std::mutex> lk(s_rng_mu);
        pid = static_cast<uint16_t>(
            std::uniform_int_distribution<int>(1, 0xFFFE)(s_rng));
    }
    auto pub_pkt = encode_publish(topic, payload, qos, /*retain=*/false,
                                  /*dup=*/false, pid);
    if (!ssl_write_all(ssl, pub_pkt)) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); bambu_close_socket(fd);
        return -4;
    }

    int rc = 0;
    if (qos == 1) {
        // PUBACK: 0x40, RL=2, [pid hi, pid lo]
        uint8_t puback_hdr[2];
        if (!ssl_read_exact(ssl, fd, puback_hdr, 2,
            std::chrono::milliseconds(cfg.io_timeout))
            || puback_hdr[0] != 0x40) {
            std::fprintf(stderr,
                "[raw-mqtt dev=%s] PUBACK header read fail "
                "(got 0x%02x)\n",
                cfg.dev_id.c_str(), puback_hdr[0]);
            std::fflush(stderr);
            rc = -5;
        } else {
            uint8_t pidbuf[2];
            if (!ssl_read_exact(ssl, fd, pidbuf, 2,
                std::chrono::milliseconds(cfg.io_timeout))) {
                rc = -5;
            } else {
                uint16_t got_pid = (uint16_t(pidbuf[0]) << 8) | pidbuf[1];
                if (got_pid != pid) {
                    std::fprintf(stderr,
                        "[raw-mqtt dev=%s] PUBACK pid mismatch sent=%u got=%u\n",
                        cfg.dev_id.c_str(), unsigned(pid), unsigned(got_pid));
                    std::fflush(stderr);
                    rc = -5;
                }
            }
        }
    }

    // DISCONNECT (0xE0 0x00) — polite shutdown.
    std::vector<uint8_t> disc{0xE0, 0x00};
    (void)ssl_write_all(ssl, disc);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    bambu_close_socket(fd);
    return rc;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
