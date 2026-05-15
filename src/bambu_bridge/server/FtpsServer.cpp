// Bambu Bridge — per-device FTPS server implementation (phase 7).
//
// Raw POSIX sockets + OpenSSL. Implicit TLS on the control channel: the
// socket goes straight into SSL_accept the moment a TCP client lands, no
// AUTH-TLS upgrade. Verified against the real-printer behaviour in
// `~/BambuStudio/src/bambu_net_oss/core/LocalPrintOrchestrator.cpp`
// (`kFtpPort=990`) and against the libcurl options FtpsClient.cpp sets
// (`CURLUSESSL_ALL` + `CURLFTPSSL_CCC_NONE` + `CURLOPT_FTP_USE_EPSV=1`).
//
// The protocol surface we implement is the FTP subset BambuStudio's
// FtpsClient.cpp drives via libcurl:
//
//     USER bblp                              -> 331
//     PASS <access-code>                     -> 230
//     PBSZ 0                                 -> 200
//     PROT P                                 -> 200
//     TYPE I                                 -> 200
//     CWD /model (or MKD /model)             -> 250 / 257
//     PASV  | EPSV                           -> 227 / 229
//     STOR /model/<file>.3mf                 -> 150 ... 226
//     QUIT                                   -> 221
//
// LIST / NLST return an empty body (real slicers issue LIST mainly to
// confirm /model/ exists before STOR; an empty body is a valid answer).
// DELE responds 250 (the file isn't actually in our virtual FS — we
// drop it on the floor, because the broker forwards the upload to the
// real printer or cloud and DELE on the bridge is meaningless).
//
// The PASV data channel:
//   - We bind a free port from [pasv_port_min, pasv_port_max], 5 attempts.
//   - The 227 reply uses the configured pasv_advertise_ip if set,
//     otherwise the device's lan_ip (and for the loopback test we want
//     127.0.0.x rather than the wildcard zeros, so empty -> "127.0.0.1").
//   - We accept() one TLS connection on it (with a 30s timeout), reuse
//     the device's SSL_CTX, SSL_accept, then read until EOF for STOR or
//     send nothing for LIST/NLST.
//
// Auth: username MUST be "bblp"; password MUST match the device's access
// code (constant-time compare). Empty access_code is treated as
// accept-all (handy for tests).
//
// Size cap: STOR refuses (551) when the upload exceeds max_upload_bytes.

#include "FtpsServer.hpp"

#include "IUploadSink.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
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
// OpenSSL global init (one-time, idempotent across translation units).
// ---------------------------------------------------------------------------
struct OpenSSLInit {
    OpenSSLInit() {
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
        // SIGPIPE on a broken TLS write would kill the process; ignore it.
        // MqttBroker relies on the same precondition (set up in LanUplink
        // earlier in the link order, but we re-arm here defensively).
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
    std::fprintf(stderr, "[ftps-server] ssl-err at %s: %s\n",
                 where, buf[0] ? buf : "no-error");
    ERR_clear_error();
}

// Build a per-device SSL_CTX (server) for both control + data channels.
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
            X509_free(x);
            SSL_CTX_free(ctx);
            return nullptr;
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
            EVP_PKEY_free(k);
            SSL_CTX_free(ctx);
            return nullptr;
        }
        EVP_PKEY_free(k);
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        log_ssl_err("SSL_CTX_check_private_key");
        SSL_CTX_free(ctx);
        return nullptr;
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

// Open + bind a PASV data listener. Tries random ports in
// [min, max] up to 16 times, then falls back to a kernel-picked
// ephemeral port. Returns nullptr on total exhaustion.
std::unique_ptr<tcp::acceptor> open_pasv_listener_asio(
        asio::io_context& io,
        uint16_t pmin, uint16_t pmax,
        const std::string& bind_ip,
        uint16_t& bound_port_out)
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    if (pmax < pmin) std::swap(pmin, pmax);
    std::uniform_int_distribution<uint16_t> dist(pmin, pmax);
    for (int i = 0; i < 16; ++i) {
        uint16_t p = dist(rng);
        uint16_t b = 0;
        auto acc = open_listener_asio(io, bind_ip, p, /*backlog=*/1, b);
        if (acc) { bound_port_out = b; return acc; }
    }
    uint16_t b = 0;
    auto acc = open_listener_asio(io, bind_ip, 0, /*backlog=*/1, b);
    if (acc) bound_port_out = b;
    return acc;
}

bool secure_streq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
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

// Format + send "<code> <text>\r\n" on the control SSL channel.
bool reply(SSL* ssl, int code, const std::string& text) {
    char buf[1024];
    int n = std::snprintf(buf, sizeof(buf), "%d %s\r\n", code, text.c_str());
    if (n <= 0) return false;
    return ssl_write_all(ssl, buf, static_cast<size_t>(n));
}

// Multi-line reply: a list of intermediate lines (sent as "code- line\r\n")
// terminated by the canonical "code text\r\n". Used for FEAT.
bool reply_multi(SSL* ssl, int code,
                 const std::vector<std::string>& lines,
                 const std::string& final_text) {
    std::string out;
    out.reserve(64);
    for (const auto& l : lines) {
        char hdr[8];
        std::snprintf(hdr, sizeof(hdr), "%d-", code);
        out += hdr;
        out += l;
        out += "\r\n";
    }
    char tail[1024];
    std::snprintf(tail, sizeof(tail), "%d %s\r\n", code, final_text.c_str());
    out += tail;
    return ssl_write_all(ssl, out.data(), out.size());
}

// Pull a single \r\n-terminated line from the SSL control channel into `out`.
// `recv_buf` may already contain queued bytes from prior reads.
// Returns true on success, false on EOF / TLS error / timeout.
//
// Uses a select() on the SSL's underlying socket for the wait-for-bytes
// poll. select() with Winsock + POSIX fds is identical at the C API level
// so this is portable; if we ever needed to drop select() entirely we'd
// poll the asio socket via async_read with a deadline timer instead.
bool read_line(SSL* ssl, int fd, std::vector<uint8_t>& recv_buf,
               std::string& out, std::chrono::seconds timeout) {
    out.clear();
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        // Drain any complete line from the buffer.
        for (size_t i = 0; i + 1 < recv_buf.size(); ++i) {
            if (recv_buf[i] == '\r' && recv_buf[i+1] == '\n') {
                out.assign(reinterpret_cast<const char*>(recv_buf.data()), i);
                recv_buf.erase(recv_buf.begin(), recv_buf.begin() + i + 2);
                return true;
            }
        }
        // Need more bytes.
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (SSL_pending(ssl) == 0) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            timeval tv{}; tv.tv_sec = 1; tv.tv_usec = 0;
            int s = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
            if (s < 0) return false;
            if (s == 0) continue;
        }
        uint8_t tmp[1024];
        int n = SSL_read(ssl, tmp, sizeof(tmp));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ) continue;
            return false;
        }
        recv_buf.insert(recv_buf.end(), tmp, tmp + n);
    }
}

// Decode an FTP command line into (CMD, args). FTP commands are
// case-insensitive; we upper-case the verb.
void split_command(const std::string& line,
                   std::string& cmd, std::string& args) {
    cmd.clear(); args.clear();
    size_t i = 0;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
        char c = line[i++];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        cmd.push_back(c);
    }
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    args = line.substr(i);
}

std::string basename_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string normalize_remote_path(const std::string& cwd, const std::string& arg) {
    if (arg.empty()) return cwd;
    if (arg.front() == '/') return arg;
    if (cwd.empty() || cwd.back() != '/') return cwd + "/" + arg;
    return cwd + arg;
}

} // namespace

// ---------------------------------------------------------------------------
// Per-device + per-session state.
// ---------------------------------------------------------------------------

struct FtpsServer::Device {
    FtpsVirtualDevice spec;
    SSL_CTX*          ssl_ctx     = nullptr;

    // asio plumbing per-device.
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<tcp::acceptor>    acceptor;
    uint16_t          bound_port  = 0;

    std::atomic<bool> stopped{false};
    std::thread       accept_thread;

    struct Session {
        SSL*              ssl = nullptr;
        int               fd  = -1;        // native fd owned after release
        std::thread       io_thread;
        std::atomic<bool> stopped{false};
    };
    std::vector<std::unique_ptr<Session>> sessions;
    std::mutex                            sessions_mu;
};

// ---------------------------------------------------------------------------
// FtpsServer ctor/dtor + device registry.
// ---------------------------------------------------------------------------

FtpsServer::FtpsServer(FtpsServerConfig cfg) : m_cfg(std::move(cfg)) {
    ensure_openssl_init();
}

FtpsServer::~FtpsServer() { stop(); }

FtpsServer::Device* FtpsServer::find_locked(const std::string& dev_id) {
    auto it = m_devices.find(dev_id);
    return it == m_devices.end() ? nullptr : it->second.get();
}

void FtpsServer::add_device(FtpsVirtualDevice dev) {
    auto d        = std::make_unique<Device>();
    d->spec       = std::move(dev);
    d->ssl_ctx    = make_device_ctx(d->spec.cert);
    if (!d->ssl_ctx) {
        throw std::runtime_error("FtpsServer: failed to build SSL_CTX for dev_id="
                                 + d->spec.dev_id);
    }
    Device* raw = d.get();
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        m_devices[d->spec.dev_id] = std::move(d);
    }
    if (m_running.load()) start_device(*raw);
}

void FtpsServer::remove_device(const std::string& dev_id) {
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

uint16_t FtpsServer::bound_port(const std::string& dev_id) const {
    std::lock_guard<std::mutex> lk(m_devices_mu);
    auto it = m_devices.find(dev_id);
    if (it == m_devices.end()) return 0;
    return it->second->bound_port;
}

// ---------------------------------------------------------------------------
// Per-connection I/O loop forward decl.
// ---------------------------------------------------------------------------

namespace {

void session_io_loop(FtpsServer::Device* dev,
                     FtpsServer::Device::Session* sess,
                     IUploadSink* sink,
                     const FtpsServerConfig& cfg);

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

void FtpsServer::set_sink(std::shared_ptr<IUploadSink> sink) {
    if (m_running.load()) {
        std::fprintf(stderr,
            "[ftps-server] set_sink ignored: server already running\n");
        return;
    }
    m_cfg.sink = std::move(sink);
}

void FtpsServer::start() {
    if (m_running.exchange(true)) return;
    std::lock_guard<std::mutex> lk(m_devices_mu);
    for (auto& kv : m_devices) {
        try { start_device(*kv.second); }
        catch (const std::exception& ex) {
            std::fprintf(stderr, "[ftps-server] failed to start device %s: %s\n",
                         kv.first.c_str(), ex.what());
        }
    }
}

void FtpsServer::stop() {
    if (!m_running.exchange(false)) return;
    std::vector<Device*> all;
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        all.reserve(m_devices.size());
        for (auto& kv : m_devices) all.push_back(kv.second.get());
    }
    for (Device* d : all) stop_device(*d);
}

void FtpsServer::start_device(Device& d) {
    if (d.acceptor) return;
    uint16_t bound = 0;
    d.io = std::make_unique<asio::io_context>();
    d.acceptor = open_listener_asio(*d.io, d.spec.lan_ip, d.spec.port,
                                    m_cfg.accept_backlog, bound);
    if (!d.acceptor) {
        d.io.reset();
        throw std::runtime_error(std::string("FtpsServer: listen failed for ") +
                                 d.spec.lan_ip + ":" + std::to_string(d.spec.port));
    }
    d.bound_port = bound;
    d.stopped.store(false);

    IUploadSink* sink = m_cfg.sink.get();
    const FtpsServerConfig cfg = m_cfg;

    d.accept_thread = std::thread([&d, sink, cfg]() {
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

            // Release the asio socket's native handle into the I/O
            // thread so the asio dtor doesn't close the fd that SSL
            // now owns.
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

            // Reap any finished sessions.
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

            auto sess = std::make_unique<FtpsServer::Device::Session>();
            sess->ssl = ssl;
            sess->fd  = cfd;
            sess->stopped.store(false);

            FtpsServer::Device::Session* raw = sess.get();
            {
                std::lock_guard<std::mutex> lk(d.sessions_mu);
                d.sessions.push_back(std::move(sess));
            }
            raw->io_thread = std::thread(session_io_loop, &d, raw, sink, cfg);
        }
    });
}

void FtpsServer::stop_device(Device& d) {
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
}

// ---------------------------------------------------------------------------
// Per-connection I/O loop.
// ---------------------------------------------------------------------------

namespace {

struct DataChannel {
    SSL* ssl = nullptr;
    int  fd  = -1;
    void close() {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); ssl = nullptr; }
        if (fd >= 0) {
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
            fd = -1;
        }
    }
};

// Holds the open PASV listener PLUS a background thread that eagerly
// accepts a single client + runs SSL_accept on it. Why eager: libcurl
// (and Bambu printers) initiate the TLS handshake on the data channel
// *before* sending STOR on the control channel, so the server can't
// gate accept/SSL_accept on STOR — that would deadlock.
//
// The control-channel I/O loop calls `take_channel()` once it's ready to
// stream/receive bytes; that returns the established DataChannel (or
// {nullptr,-1} on failure / timeout).
struct PasvState {
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<tcp::acceptor>    acceptor;
    uint16_t        port      = 0;
    SSL_CTX*        ctx       = nullptr;

    std::mutex      mu;
    std::condition_variable cv;
    DataChannel     dc;            // filled by acceptor thread
    bool            done = false;  // accept + SSL_accept finished
    std::thread     thr;
    std::atomic<bool> stop_req{false};

    // True iff a PASV listener is currently bound. Equivalent to the old
    // `listen_fd >= 0` predicate.
    bool armed() const { return static_cast<bool>(acceptor); }

    void start_accept_thread(SSL_CTX* c, std::chrono::seconds timeout) {
        ctx = c;
        thr = std::thread([this, timeout]() {
            DataChannel d;
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!stop_req.load() &&
                   std::chrono::steady_clock::now() < deadline) {
                tcp::socket client_sock(*io);
                error_code aec = asio::error::would_block;
                acceptor->async_accept(
                    client_sock,
                    [&aec](const error_code& e) { aec = e; });
                io->restart();
                io->run_for(std::chrono::milliseconds(200));
                if (aec == asio::error::would_block) {
                    error_code ignore;
                    acceptor->cancel(ignore);
                    io->run();
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
                SSL* ssl = SSL_new(ctx);
                if (!ssl) {
#ifdef _WIN32
                    ::closesocket(cfd);
#else
                    ::close(cfd);
#endif
                    break;
                }
                SSL_set_fd(ssl, cfd);
                if (SSL_accept(ssl) != 1) {
                    log_ssl_err("SSL_accept(data)");
                    SSL_free(ssl);
#ifdef _WIN32
                    ::closesocket(cfd);
#else
                    ::close(cfd);
#endif
                    break;
                }
                d.ssl = ssl;
                d.fd  = cfd;
                break;
            }
            std::lock_guard<std::mutex> lk(mu);
            dc   = d;
            done = true;
            cv.notify_all();
        });
    }

    // Wait until the acceptor finishes; returns the (possibly empty)
    // DataChannel. Caller takes ownership of the SSL+fd.
    DataChannel take_channel(std::chrono::seconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, timeout, [&]{ return done; });
        DataChannel out = dc;
        dc = DataChannel{}; // moved
        return out;
    }

    void close() {
        stop_req.store(true);
        if (acceptor) {
            error_code ignore;
            acceptor->close(ignore);
        }
        if (thr.joinable()) thr.join();
        // dc may still hold a live SSL+fd if take_channel wasn't called.
        dc.close();
        acceptor.reset();
        io.reset();
        port = 0;
        ctx  = nullptr;
        done = false;
        stop_req.store(false);
    }
    ~PasvState() { close(); }
};

// DataChannel::close uses ::close on POSIX. On Windows we'd need
// closesocket, but the data SSL is freed first so we use the same
// pattern as control sessions. Patch DataChannel::close to be portable.

void session_io_loop(FtpsServer::Device* dev,
                     FtpsServer::Device::Session* sess,
                     IUploadSink* sink,
                     const FtpsServerConfig& cfg) {
    struct Cleanup {
        FtpsServer::Device::Session* sess;
        ~Cleanup() { sess->stopped.store(true); }
    } cleanup{sess};

    // Implicit TLS — handshake straight away. Real printers do this on 990.
    if (SSL_accept(sess->ssl) != 1) {
        log_ssl_err("SSL_accept(control)");
        return;
    }

    // RFC-959 220 welcome banner. Bambu printers emit something like
    // "220 (vsFTPd 3.0.3)"; libcurl is happy with anything that starts
    // with "220 ". We deliberately don't mimic vsFTPd's exact string —
    // libcurl doesn't parse it, and bridging a different banner is
    // immaterial to the upload's success.
    if (!reply(sess->ssl, 220, "Bambu Bridge FTPS ready.")) return;

    std::vector<uint8_t> recv_buf;
    std::string line;
    std::string cmd, args;

    bool        authed_user = false;
    std::string user;
    std::string cwd = "/";
    PasvState   pasv;

    const auto rd_timeout = std::chrono::seconds(cfg.io_timeout_seconds > 0
                                                  ? cfg.io_timeout_seconds : 90);

    // PASV reply needs a 4-byte IPv4 + 2-byte port. We resolve the IP
    // from cfg.pasv_advertise_ip (preferred) then dev->spec.lan_ip,
    // falling back to 127.0.0.1 for the empty/wildcard case.
    auto resolve_pasv_ip = [&](uint32_t& nbo_out) -> bool {
        std::string ip = cfg.pasv_advertise_ip.empty() ? dev->spec.lan_ip
                                                       : cfg.pasv_advertise_ip;
        if (ip.empty() || ip == "0.0.0.0") ip = "127.0.0.1";
        error_code ec;
        auto addr_v4 = asio::ip::make_address_v4(ip, ec);
        if (ec) return false;
        // PASV reply wants the IP in network byte order (4 bytes high to
        // low as printed). asio's to_uint() returns host-byte-order; we
        // convert to NBO via htonl.
        nbo_out = htonl(addr_v4.to_uint());
        return true;
    };

    while (!sess->stopped.load() && !dev->stopped.load()) {
        if (!read_line(sess->ssl, sess->fd, recv_buf, line, rd_timeout)) break;
        split_command(line, cmd, args);
        if (cmd.empty()) continue;

        if (cmd == "USER") {
            user        = args;
            authed_user = false;
            reply(sess->ssl, 331, "Password required.");
        }
        else if (cmd == "PASS") {
            if (user != "bblp") {
                reply(sess->ssl, 530, "Authentication failed.");
                std::fprintf(stderr,
                    "[ftps-server] auth fail dev=%s user=%s (not bblp)\n",
                    dev->spec.dev_id.c_str(), user.c_str());
                return;
            }
            const bool pw_ok = dev->spec.access_code.empty() ||
                               secure_streq(args, dev->spec.access_code);
            if (!pw_ok) {
                reply(sess->ssl, 530, "Authentication failed.");
                std::fprintf(stderr,
                    "[ftps-server] auth fail dev=%s wrong access code\n",
                    dev->spec.dev_id.c_str());
                return;
            }
            authed_user = true;
            reply(sess->ssl, 230, "Login successful.");
        }
        else if (!authed_user) {
            reply(sess->ssl, 530, "Please login with USER and PASS.");
        }
        else if (cmd == "FEAT") {
            // Advertise just what we actually support, in the multi-line
            // form RFC 2389 specifies.
            reply_multi(sess->ssl, 211,
                        {" PBSZ", " PROT", " EPSV", " UTF8"},
                        "End.");
        }
        else if (cmd == "OPTS") {
            // libcurl issues "OPTS UTF8 ON" — accept silently.
            reply(sess->ssl, 200, "OK.");
        }
        else if (cmd == "SYST") {
            reply(sess->ssl, 215, "UNIX Type: L8");
        }
        else if (cmd == "NOOP") {
            reply(sess->ssl, 200, "OK.");
        }
        else if (cmd == "PBSZ") {
            // libcurl sends "PBSZ 0" — we accept any value.
            reply(sess->ssl, 200, "PBSZ=0");
        }
        else if (cmd == "PROT") {
            // libcurl sends "PROT P" — accept P (private). Anything else
            // would still work because we run the data channel through
            // SSL_accept regardless, but answer 504 for clarity.
            if (!args.empty() && (args[0] == 'P' || args[0] == 'p')) {
                reply(sess->ssl, 200, "PROT now Private.");
            } else {
                reply(sess->ssl, 504, "Only PROT P supported.");
            }
        }
        else if (cmd == "TYPE") {
            // I (binary) is what we actually use; A (ASCII) is meaningless
            // for .3mf but we accept it to be polite.
            reply(sess->ssl, 200, "Type set.");
        }
        else if (cmd == "PWD" || cmd == "XPWD") {
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "\"%s\" is current directory.",
                          cwd.c_str());
            reply(sess->ssl, 257, buf);
        }
        else if (cmd == "CWD" || cmd == "XCWD") {
            // We don't maintain a real FS; accept any CWD.
            std::string target = normalize_remote_path(cwd, args);
            cwd = target.empty() ? "/" : target;
            reply(sess->ssl, 250, "Directory changed.");
        }
        else if (cmd == "CDUP" || cmd == "XCUP") {
            if (cwd.size() > 1) {
                auto pos = cwd.find_last_of('/');
                cwd = (pos == 0) ? "/" : cwd.substr(0, pos);
            }
            reply(sess->ssl, 250, "CDUP ok.");
        }
        else if (cmd == "MKD" || cmd == "XMKD") {
            // CURLOPT_FTP_CREATE_MISSING_DIRS triggers MKD before STOR.
            std::string target = normalize_remote_path(cwd, args);
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "\"%s\" created.", target.c_str());
            reply(sess->ssl, 257, buf);
        }
        else if (cmd == "RMD" || cmd == "XRMD") {
            reply(sess->ssl, 250, "RMD ok.");
        }
        else if (cmd == "DELE") {
            // Bridge has no virtual filesystem; pretend success.
            reply(sess->ssl, 250, "Deleted.");
        }
        else if (cmd == "PASV") {
            pasv.close();
            uint16_t pport = 0;
            const std::string& bind_ip =
                cfg.pasv_advertise_ip.empty() ? dev->spec.lan_ip
                                              : cfg.pasv_advertise_ip;
            // We bind PASV listener on the SAME ip we advertise so a
            // client connecting back to that ip can reach us.
            std::string pasv_bind = bind_ip.empty() ? std::string("0.0.0.0") : bind_ip;
            pasv.io = std::make_unique<asio::io_context>();
            pasv.acceptor = open_pasv_listener_asio(*pasv.io,
                                                    cfg.pasv_port_min,
                                                    cfg.pasv_port_max,
                                                    pasv_bind, pport);
            if (!pasv.acceptor) {
                pasv.io.reset();
                reply(sess->ssl, 425, "Can't open data connection.");
                continue;
            }
            pasv.port = pport;
            uint32_t ip_nbo = 0;
            if (!resolve_pasv_ip(ip_nbo)) {
                pasv.close();
                reply(sess->ssl, 425, "Bad PASV address.");
                continue;
            }
            // Start the data-channel acceptor immediately — libcurl
            // (CURLUSESSL_ALL) initiates TLS on the data port BEFORE
            // sending STOR.
            pasv.start_accept_thread(dev->ssl_ctx, std::chrono::seconds(30));
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&ip_nbo);
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
                          p[0], p[1], p[2], p[3],
                          static_cast<unsigned>(pport >> 8),
                          static_cast<unsigned>(pport & 0xFF));
            reply(sess->ssl, 227, buf);
        }
        else if (cmd == "EPSV") {
            // RFC 2428 extended passive. libcurl prefers this with
            // CURLOPT_FTP_USE_EPSV=1; it falls back to PASV on 522/501.
            pasv.close();
            uint16_t pport = 0;
            const std::string& bind_ip =
                cfg.pasv_advertise_ip.empty() ? dev->spec.lan_ip
                                              : cfg.pasv_advertise_ip;
            std::string pasv_bind = bind_ip.empty() ? std::string("0.0.0.0") : bind_ip;
            pasv.io = std::make_unique<asio::io_context>();
            pasv.acceptor = open_pasv_listener_asio(*pasv.io,
                                                    cfg.pasv_port_min,
                                                    cfg.pasv_port_max,
                                                    pasv_bind, pport);
            if (!pasv.acceptor) {
                pasv.io.reset();
                reply(sess->ssl, 425, "Can't open data connection.");
                continue;
            }
            pasv.port = pport;
            pasv.start_accept_thread(dev->ssl_ctx, std::chrono::seconds(30));
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                          "Entering Extended Passive Mode (|||%u|)", pport);
            reply(sess->ssl, 229, buf);
        }
        else if (cmd == "LIST" || cmd == "NLST") {
            if (!pasv.armed()) {
                reply(sess->ssl, 425, "Use PASV first.");
                continue;
            }
            reply(sess->ssl, 150, "Opening data connection.");
            DataChannel dc = pasv.take_channel(std::chrono::seconds(30));
            // No body — bridge has no virtual filesystem. Just close.
            dc.close();
            pasv.close();
            reply(sess->ssl, 226, "Directory send OK.");
        }
        else if (cmd == "STOR") {
            if (!pasv.armed()) {
                reply(sess->ssl, 425, "Use PASV first.");
                continue;
            }
            const std::string remote = normalize_remote_path(cwd, args);
            const std::string fname  = basename_of(remote);
            if (!reply(sess->ssl, 150, "Ok to send data.")) {
                pasv.close();
                return;
            }
            DataChannel dc = pasv.take_channel(std::chrono::seconds(30));
            if (!dc.ssl) {
                reply(sess->ssl, 425, "Can't open data connection.");
                continue;
            }
            std::vector<uint8_t> body;
            body.reserve(64 * 1024);
            bool overflow = false;
            uint8_t buf[16 * 1024];
            for (;;) {
                int n = SSL_read(dc.ssl, buf, sizeof(buf));
                if (n <= 0) break;
                if (body.size() + static_cast<size_t>(n) > cfg.max_upload_bytes) {
                    overflow = true;
                    break;
                }
                body.insert(body.end(), buf, buf + n);
            }
            dc.close();

            if (overflow) {
                std::fprintf(stderr,
                    "[ftps-server] STOR %s rejected: > %zu bytes\n",
                    fname.c_str(), cfg.max_upload_bytes);
                reply(sess->ssl, 552,
                      "Exceeded storage allocation.");
                continue;
            }

            UploadJob job;
            job.dev_id      = dev->spec.dev_id;
            job.filename    = fname;
            job.remote_path = remote;
            job.content     = std::move(body);
            job.received_at = std::chrono::system_clock::now();

            UploadResult res;
            if (sink) {
                try {
                    res = sink->deliver(std::move(job));
                } catch (const std::exception& ex) {
                    res.ok = false;
                    res.error_message = std::string("sink threw: ") + ex.what();
                }
            } else {
                res.ok = true;  // No sink -> drop silently; tests can null.
            }

            if (res.ok) {
                reply(sess->ssl, 226, "Transfer complete.");
            } else {
                std::string msg = "Upload failed";
                if (!res.error_message.empty()) {
                    msg += ": ";
                    msg += res.error_message;
                }
                reply(sess->ssl, 551, msg);
            }
            pasv.close();  // PASV is single-shot.
        }
        else if (cmd == "REST") {
            // libcurl emits REST 0 for resume support. Accept the
            // zero-offset case; refuse non-zero (we don't implement resume).
            if (args == "0") reply(sess->ssl, 350, "Restart at 0.");
            else             reply(sess->ssl, 502, "REST not implemented.");
        }
        else if (cmd == "SIZE") {
            // Bridge has no FS; refuse with 550 so the client falls back.
            reply(sess->ssl, 550, "SIZE not available.");
        }
        else if (cmd == "MDTM") {
            reply(sess->ssl, 550, "MDTM not available.");
        }
        else if (cmd == "QUIT") {
            reply(sess->ssl, 221, "Goodbye.");
            return;
        }
        else {
            reply(sess->ssl, 502, "Command not implemented.");
        }
    }
}

} // namespace

} // namespace server
} // namespace bridge
} // namespace Slic3r
