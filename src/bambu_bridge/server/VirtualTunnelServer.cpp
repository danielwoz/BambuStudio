// VirtualTunnelServer implementation. See header for design.

#include "VirtualTunnelServer.hpp"
#include "../BambuSourceHandle.hpp"

#include "nlohmann/json.hpp"

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#  include <sys/ioctl.h>
#endif

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace server {

namespace asio = boost::asio;
using asio::ip::tcp;
using boost::system::error_code;

namespace {

// ABI mirror of `Bambu_Sample` from BambuTunnel.h. The BambuSourceHandle
// public API erases this as `void*` so we don't drag the proprietary
// header into every TU; the layout MUST match BambuTunnel.h byte-for-byte.
// Same mirror pattern lives inside BambuSourceHandle.cpp.
struct MirrorBambu_Sample {
    int                    itrack;
    int                    size;
    int                    flags;
    unsigned char const*   buffer;
    unsigned long long     decode_time;
};

// Bambu_Error enum values — matches BambuTunnel.h. Kept local so we
// don't expose the proprietary enum names through bridge headers.
constexpr int kBambuSuccess     = 0;
constexpr int kBambuStreamEnd   = 1;
constexpr int kBambuWouldBlock  = 2;

// CTRL_TYPE the slicer's PrinterFileSystem uses for storage JSON-RPC.
// See `src/slic3r/GUI/Printer/PrinterFileSystem.h` (`CTRL_TYPE = 0x3001`).
constexpr int kCtrlTypeStorage  = 0x3001;

void log_ssl_err(const char* tag) {
    const unsigned long e = ERR_get_error();
    char buf[256] = {};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    std::fprintf(stderr, "[virtual-tunnel] %s: %s\n", tag,
                 e ? buf : "(no error in queue)");
}

SSL_CTX* make_device_ctx(const tls::CertMaterial& cert) {
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { log_ssl_err("SSL_CTX_new"); return nullptr; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);

    {
        BIO* bio = BIO_new_mem_buf(cert.cert_pem.data(),
                                   static_cast<int>(cert.cert_pem.size()));
        if (!bio) { log_ssl_err("BIO cert"); SSL_CTX_free(ctx); return nullptr; }
        X509* x = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!x) { log_ssl_err("PEM cert"); SSL_CTX_free(ctx); return nullptr; }
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
        if (!k) { log_ssl_err("PEM key"); SSL_CTX_free(ctx); return nullptr; }
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
        asio::io_context& io, const std::string& ip, uint16_t port, int backlog)
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
#ifdef SO_REUSEPORT
    if (!ec) {
        int one = 1;
        ::setsockopt(acc->native_handle(), SOL_SOCKET, SO_REUSEPORT,
                     &one, sizeof(one));
    }
#endif
    if (!ec) acc->bind(ep, ec);
    if (!ec) acc->listen(backlog, ec);
    if (ec) return nullptr;
    return acc;
}

// Read exactly n bytes from `ssl`, blocking. Returns false on EOF/error.
bool ssl_read_exact(SSL* ssl, uint8_t* buf, size_t n) {
    size_t got = 0;
    static thread_local int call_seq = 0;
    int my_seq = ++call_seq;
    const int fd0 = SSL_get_fd(ssl);
    auto now_ms = []() {
        timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
        return (long long)(ts.tv_sec * 1000) + ts.tv_nsec / 1000000;
    };
    long long t_enter = now_ms();
    std::fprintf(stderr,
        "[virtual-tunnel] ssl_read_exact ENTER seq=%d fd=%d want=%zu t=%lld\n",
        my_seq, fd0, n, t_enter);
    while (got < n) {
        int r = SSL_read(ssl, buf + got, static_cast<int>(n - got));
        if (r <= 0) {
            const int err = SSL_get_error(ssl, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue;
            const int fd = SSL_get_fd(ssl);
            unsigned long ssl_err_q = ERR_peek_error();
            char ssl_err_buf[256] = {0};
            if (ssl_err_q) ERR_error_string_n(ssl_err_q, ssl_err_buf, sizeof(ssl_err_buf));
#ifndef _WIN32
            int avail = -1;
            uint8_t peek[16] = {0};
            ssize_t peek_n = -1;
            int peek_errno = 0;
            if (fd >= 0) {
                ::ioctl(fd, FIONREAD, &avail);
                peek_n = ::recv(fd, peek, sizeof(peek), MSG_DONTWAIT|MSG_PEEK);
                peek_errno = errno;
            }
            std::fprintf(stderr,
                "[virtual-tunnel] ssl_read_exact failed seq=%d r=%d ssl_err=%d "
                "errno=%d got=%zu/%zu fd=%d FIONREAD=%d "
                "peek_recv=%zd peek_errno=%d ssl_err_q=0x%lx (%s) "
                "dt=%lldms\n",
                my_seq, r, err, errno, got, n, fd, avail, peek_n, peek_errno,
                ssl_err_q, ssl_err_buf, now_ms() - t_enter);
#else
            std::fprintf(stderr,
                "[virtual-tunnel] ssl_read_exact failed seq=%d r=%d ssl_err=%d "
                "errno=%d got=%zu/%zu fd=%d ssl_err_q=0x%lx (%s) dt=%lldms\n",
                my_seq, r, err, errno, got, n, fd,
                ssl_err_q, ssl_err_buf, now_ms() - t_enter);
#endif
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return true;
}

bool ssl_write_all(SSL* ssl, const uint8_t* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int r = SSL_write(ssl, buf + sent, static_cast<int>(n - sent));
        if (r <= 0) {
            const int err = SSL_get_error(ssl, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue;
            return false;
        }
        sent += static_cast<size_t>(r);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Device — per-device listener + accepted-session state.
// ---------------------------------------------------------------------------

struct VirtualTunnelServer::Device {
    VirtualTunnelVirtualDevice spec;
    SSL_CTX*                   ssl_ctx     = nullptr;

    // asio plumbing per-device.
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<tcp::acceptor>    acceptor;
    uint16_t                   bound_port  = 0;
    std::atomic<bool>          accepting{false};
    std::thread                accept_thread;

    // Snapshot of the server's storage delegate.
    // session_loop hands JSON-RPC frames to the delegate
    // (PrinterFileSystem-via-BridgeStorageBackend path) for relaying.
    StorageDelegate                           storage_delegate;

    // Slicer-identity URL fields (snapshot of m_cfg.slicer_*). Empty
    // in headless mode.
    std::string                               slicer_net_ver;
    std::string                               slicer_cli_id;
    std::string                               slicer_cli_ver;

    // Active session threads. Joined on stop_device — each one exits when
    // its client closes the socket.
    std::mutex                                session_mu;
    std::vector<std::thread>                  sessions;
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

VirtualTunnelServer::VirtualTunnelServer(VirtualTunnelServerConfig cfg)
    : m_cfg(std::move(cfg)) {}

VirtualTunnelServer::~VirtualTunnelServer() { stop(); }

void VirtualTunnelServer::add_device(VirtualTunnelVirtualDevice dev) {
    const std::string dev_id = dev.dev_id;
    auto d = std::make_unique<Device>();
    d->spec = std::move(dev);
    d->ssl_ctx = make_device_ctx(d->spec.cert);
    if (!d->ssl_ctx) {
        std::fprintf(stderr,
            "[virtual-tunnel] add_device dev_id=%s: SSL_CTX init failed\n",
            dev_id.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_devices[dev_id] = std::move(d);
    }
    if (m_running.load()) {
        std::lock_guard<std::mutex> lk(m_mu);
        Device* dp = find_locked(dev_id);
        if (dp) start_device(*dp);
    }
}

void VirtualTunnelServer::attach_storage_delegate(StorageDelegate delegate) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_storage_delegate = std::move(delegate);
    std::fprintf(stderr,
        "[virtual-tunnel] attach_storage_delegate set=%d\n",
        m_storage_delegate ? 1 : 0);
}

void VirtualTunnelServer::update_printer_lan_ip(
        const std::string& dev_id,
        const std::string& printer_lan_ip) {
    std::lock_guard<std::mutex> lk(m_mu);
    Device* d = find_locked(dev_id);
    if (!d) return;
    if (d->spec.printer_lan_ip == printer_lan_ip) return;
    std::fprintf(stderr,
        "[virtual-tunnel] dev_id=%s printer_lan_ip %s -> %s\n",
        dev_id.c_str(),
        d->spec.printer_lan_ip.empty() ? "(none)" : d->spec.printer_lan_ip.c_str(),
        printer_lan_ip.empty()         ? "(none)" : printer_lan_ip.c_str());
    d->spec.printer_lan_ip = printer_lan_ip;
}

void VirtualTunnelServer::update_printer_firmware_ver(
        const std::string& dev_id,
        const std::string& firmware_ver) {
    std::lock_guard<std::mutex> lk(m_mu);
    Device* d = find_locked(dev_id);
    if (!d) return;
    if (d->spec.printer_firmware_ver == firmware_ver) return;
    std::fprintf(stderr,
        "[virtual-tunnel] dev_id=%s printer_firmware_ver %s -> %s\n",
        dev_id.c_str(),
        d->spec.printer_firmware_ver.empty() ? "(none)" : d->spec.printer_firmware_ver.c_str(),
        firmware_ver.empty()                 ? "(none)" : firmware_ver.c_str());
    d->spec.printer_firmware_ver = firmware_ver;
}

void VirtualTunnelServer::remove_device(const std::string& dev_id) {
    std::unique_ptr<Device> doomed;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_devices.find(dev_id);
        if (it == m_devices.end()) return;
        doomed = std::move(it->second);
        m_devices.erase(it);
    }
    stop_device(*doomed);
    if (doomed->ssl_ctx) SSL_CTX_free(doomed->ssl_ctx);
}

void VirtualTunnelServer::start() {
    bool was = m_running.exchange(true);
    if (was) return;
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& kv : m_devices) start_device(*kv.second);
}

void VirtualTunnelServer::stop() {
    bool was = m_running.exchange(false);
    if (!was) return;
    std::vector<Device*> snap;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        snap.reserve(m_devices.size());
        for (auto& kv : m_devices) snap.push_back(kv.second.get());
    }
    for (auto* d : snap) stop_device(*d);
}

uint16_t VirtualTunnelServer::bound_port(const std::string& dev_id) const {
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_devices.find(dev_id);
    if (it == m_devices.end()) return 0;
    return it->second->bound_port;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

VirtualTunnelServer::Device* VirtualTunnelServer::find_locked(
        const std::string& dev_id) {
    auto it = m_devices.find(dev_id);
    return (it == m_devices.end()) ? nullptr : it->second.get();
}


// Storage-delegate session loop. Reads length-prefixed JSON frames from
// the virtual slicer, parses cmdtype/sequence/req, hands the request
// to the delegate (PrinterFileSystem-via-BridgeStorageBackend in the
// GUI case), and writes the {result, sequence, reply} envelope back
// when the delegate's reply callback fires.
static void session_loop_backend(SSL_CTX* server_ctx, int client_fd,
                                 const VirtualTunnelVirtualDevice& spec,
                                 const StorageDelegate& delegate,
                                 const std::string& slicer_net_ver,
                                 const std::string& slicer_cli_id,
                                 const std::string& slicer_cli_ver) {
    const std::string& dev_id = spec.dev_id;
    std::fprintf(stderr,
        "[virtual-tunnel] session_loop_backend enter dev_id=%s fd=%d\n",
        dev_id.c_str(), client_fd);
    SSL* slicer_ssl = SSL_new(server_ctx);
    if (!slicer_ssl) { ::close(client_fd); return; }
    SSL_set_fd(slicer_ssl, client_fd);
    const int acc_rc = SSL_accept(slicer_ssl);
    if (acc_rc != 1) {
        log_ssl_err("SSL_accept");
        SSL_free(slicer_ssl);
        ::close(client_fd);
        return;
    }
    {
        int sock_err = 0; socklen_t se_len = sizeof(sock_err);
#ifdef _WIN32
        ::getsockopt(client_fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&sock_err), &se_len);
        std::fprintf(stderr,
            "[virtual-tunnel] session up dev_id=%s — backend delegation mode "
            "tls=%s cipher=%s SSL_pending=%d SSL_has_pending=%d "
            "SO_ERROR=%d fd=%d\n",
            dev_id.c_str(),
            SSL_get_version(slicer_ssl), SSL_get_cipher(slicer_ssl),
            SSL_pending(slicer_ssl), SSL_has_pending(slicer_ssl),
            sock_err, client_fd);
#else
        int avail_after_accept = -1;
        ::ioctl(client_fd, FIONREAD, &avail_after_accept);
        ::getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &sock_err, &se_len);
        std::fprintf(stderr,
            "[virtual-tunnel] session up dev_id=%s — backend delegation mode "
            "tls=%s cipher=%s SSL_pending=%d SSL_has_pending=%d "
            "FIONREAD=%d SO_ERROR=%d fd=%d\n",
            dev_id.c_str(),
            SSL_get_version(slicer_ssl), SSL_get_cipher(slicer_ssl),
            SSL_pending(slicer_ssl), SSL_has_pending(slicer_ssl),
            avail_after_accept, sock_err, client_fd);
#endif
    }

    // Shared write-side state. The backend's reply callback can fire on
    // an arbitrary thread (PFS' recv thread), so we use a mutex around
    // SSL_write and an alive-flag so callbacks that arrive after the
    // session has gone become no-ops.
    auto write_mu    = std::make_shared<std::mutex>();
    auto session_alive = std::make_shared<std::atomic<bool>>(true);

    while (true) {
        uint8_t lenbuf[4];
        if (!ssl_read_exact(slicer_ssl, lenbuf, 4)) break;
        const uint32_t n = (static_cast<uint32_t>(lenbuf[0]) << 24) |
                           (static_cast<uint32_t>(lenbuf[1]) << 16) |
                           (static_cast<uint32_t>(lenbuf[2]) <<  8) |
                            static_cast<uint32_t>(lenbuf[3]);
        if (n == 0 || n > 4u * 1024u * 1024u) {
            std::fprintf(stderr,
                "[virtual-tunnel] dev_id=%s: bad frame length %u — closing\n",
                dev_id.c_str(), unsigned(n));
            break;
        }
        std::vector<uint8_t> payload(n);
        if (!ssl_read_exact(slicer_ssl, payload.data(), n)) break;

        // Parse the slicer-virtual envelope. PFS-shaped: {cmdtype, sequence, req}.
        nlohmann::json env;
        try {
            env = nlohmann::json::parse(payload.begin(), payload.end());
        } catch (const std::exception& ex) {
            std::fprintf(stderr,
                "[virtual-tunnel] dev_id=%s: malformed JSON frame: %s\n",
                dev_id.c_str(), ex.what());
            break;
        }
        const int  cmdtype  = env.value("cmdtype",  -1);
        const int  sequence = env.value("sequence",  0);
        nlohmann::json req_body = env.value("req", nlohmann::json::object());
        if (cmdtype < 0) {
            std::fprintf(stderr,
                "[virtual-tunnel] dev_id=%s: frame without cmdtype, dropping\n",
                dev_id.c_str());
            continue;
        }

        // Serialize the inner req body for the cross-ABI hand-off.
        std::string req_body_json = req_body.dump();

        // The reply callback writes the envelope back over the SSL
        // session. Captures shared_ptrs only (no raw refs to stack-
        // owned state) so it's safe to fire after the session has
        // already torn down — `alive` gates everything.
        SSL* ssl_capture = slicer_ssl;
        auto cb = [sequence, ssl_capture,
                   mu = write_mu, alive = session_alive,
                   dev_id_copy = dev_id]
                  (int rc, std::string reply_json) {
            if (!alive->load()) return;
            // Re-parse the GUI-supplied reply JSON so we can splice it
            // back into the wire envelope without escaping it as a
            // string. Bad JSON from the GUI side gets logged and
            // converted to an empty object — better than corrupting
            // the wire format.
            nlohmann::json reply;
            try {
                reply = reply_json.empty()
                    ? nlohmann::json::object()
                    : nlohmann::json::parse(reply_json);
            } catch (const std::exception& ex) {
                std::fprintf(stderr,
                    "[virtual-tunnel] dev_id=%s: malformed reply JSON "
                    "from delegate: %s — substituting {}\n",
                    dev_id_copy.c_str(), ex.what());
                reply = nlohmann::json::object();
            }
            nlohmann::json envelope = {
                {"result",   rc},
                {"sequence", sequence},
                {"reply",    std::move(reply)},
            };
            const std::string body = envelope.dump();
            const uint32_t    nb   = static_cast<uint32_t>(body.size());
            uint8_t hdr[4] = {
                static_cast<uint8_t>((nb >> 24) & 0xff),
                static_cast<uint8_t>((nb >> 16) & 0xff),
                static_cast<uint8_t>((nb >>  8) & 0xff),
                static_cast<uint8_t>( nb        & 0xff),
            };
            std::lock_guard<std::mutex> lk(*mu);
            if (!alive->load()) return;
            if (!ssl_write_all(ssl_capture, hdr, 4)) {
                std::fprintf(stderr,
                    "[virtual-tunnel] dev_id=%s: write header failed in cb\n",
                    dev_id_copy.c_str());
                return;
            }
            if (!ssl_write_all(ssl_capture,
                               reinterpret_cast<const uint8_t*>(body.data()),
                               body.size())) {
                std::fprintf(stderr,
                    "[virtual-tunnel] dev_id=%s: write body failed in cb\n",
                    dev_id_copy.c_str());
                return;
            }
        };

        delegate(
            spec.dev_id,
            spec.printer_lan_ip,
            spec.access_code,
            spec.printer_firmware_ver,
            slicer_net_ver,
            slicer_cli_id,
            slicer_cli_ver,
            cmdtype,
            std::move(req_body_json),
            std::move(cb));
    }

    std::fprintf(stderr,
        "[virtual-tunnel] session down (backend mode) dev_id=%s\n",
        dev_id.c_str());

    // Disarm any in-flight callbacks BEFORE freeing the SSL.
    session_alive->store(false);
    {
        // Briefly take the write mutex so any callback currently inside
        // ssl_write_all completes before we tear down.
        std::lock_guard<std::mutex> lk(*write_mu);
        SSL_shutdown(slicer_ssl);
        SSL_free(slicer_ssl);
    }
    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
}

// Dispatch: hand each accepted vtun session to the
// PrinterFileSystem-via-BridgeStorageBackend delegate. The bridge
// always installs one on bootstrap (GUI_App::init_bridge_only_headless
// and the in-GUI bridge worker both call attach_storage_delegate).
static void session_loop(SSL_CTX* server_ctx, int client_fd,
                         const VirtualTunnelVirtualDevice& spec,
                         StorageDelegate delegate,
                         const std::string& slicer_net_ver,
                         const std::string& slicer_cli_id,
                         const std::string& slicer_cli_ver) {
    if (!delegate) {
        std::fprintf(stderr,
            "[virtual-tunnel] dev_id=%s: no storage delegate attached — "
            "refusing connection\n",
            spec.dev_id.c_str());
        ::close(client_fd);
        return;
    }
    session_loop_backend(server_ctx, client_fd, spec, delegate,
                         slicer_net_ver, slicer_cli_id, slicer_cli_ver);
}

static void accept_loop(VirtualTunnelServer::Device* d, int io_timeout_s) {
    (void) io_timeout_s; // not used in Phase 1; sessions are stream-driven
    while (d->accepting.load()) {
        tcp::socket client_sock(*d->io);
        error_code aec = asio::error::would_block;
        d->acceptor->async_accept(
            client_sock,
            [&aec](const error_code& e) { aec = e; });
        d->io->restart();
        d->io->run_for(std::chrono::seconds(1));
        if (aec == asio::error::would_block) {
            error_code ignore;
            d->acceptor->cancel(ignore);
            d->io->run();
            continue;
        }
        if (aec) {
            if (aec == asio::error::operation_aborted) break;
            std::fprintf(stderr,
                "[virtual-tunnel] accept failed dev_id=%s ec=%s\n",
                d->spec.dev_id.c_str(), aec.message().c_str());
            break;
        }

        // Detach the native fd so the asio socket dtor doesn't close it
        // out from under the SSL_set_fd in session_loop.
        auto native = client_sock.native_handle();
        error_code rel_ec;
        client_sock.release(rel_ec);
        int cfd = static_cast<int>(native);

        std::fprintf(stderr,
            "[virtual-tunnel] accept_loop accept -> cfd=%d "
            "dev_id=%s ssl_ctx=%p\n",
            cfd, d->spec.dev_id.c_str(), (void*)d->ssl_ctx);

        SSL_CTX*                                  ctx    = d->ssl_ctx;
        VirtualTunnelVirtualDevice                spec   = d->spec;
        StorageDelegate                           del    = d->storage_delegate;
        std::string                               net_ver = d->slicer_net_ver;
        std::string                               cli_id  = d->slicer_cli_id;
        std::string                               cli_ver = d->slicer_cli_ver;
        std::lock_guard<std::mutex> lk(d->session_mu);
        d->sessions.emplace_back([ctx, cfd, spec, del, net_ver, cli_id, cli_ver]() {
            session_loop(ctx, cfd, spec, del, net_ver, cli_id, cli_ver);
        });
    }
}

void VirtualTunnelServer::start_device(Device& d) {
    if (d.acceptor) return;
    d.storage_delegate = m_storage_delegate;
    d.slicer_net_ver = m_cfg.slicer_net_ver;
    d.slicer_cli_id  = m_cfg.slicer_cli_id;
    d.slicer_cli_ver = m_cfg.slicer_cli_ver;
    d.io = std::make_unique<asio::io_context>();
    d.acceptor = open_listener_asio(*d.io, d.spec.lan_ip, d.spec.port,
                                    m_cfg.accept_backlog);
    if (!d.acceptor) {
        std::fprintf(stderr,
            "[virtual-tunnel] listen failed dev_id=%s ip=%s port=%u\n",
            d.spec.dev_id.c_str(), d.spec.lan_ip.c_str(),
            unsigned(d.spec.port));
        d.io.reset();
        return;
    }
    d.bound_port = d.acceptor->local_endpoint().port();
    std::fprintf(stderr,
        "[virtual-tunnel] dev_id=%s listening on %s:%u\n",
        d.spec.dev_id.c_str(), d.spec.lan_ip.c_str(),
        unsigned(d.bound_port));
    d.accepting.store(true);
    Device* dp = &d;
    int io_to = m_cfg.io_timeout_seconds;
    d.accept_thread = std::thread([dp, io_to]() { accept_loop(dp, io_to); });
}

void VirtualTunnelServer::stop_device(Device& d) {
    d.accepting.store(false);
    if (d.acceptor) {
        error_code ignore;
        d.acceptor->close(ignore);
    }
    if (d.accept_thread.joinable())
        d.accept_thread.join();
    d.acceptor.reset();
    d.io.reset();
    std::vector<std::thread> snap;
    {
        std::lock_guard<std::mutex> lk(d.session_mu);
        snap.swap(d.sessions);
    }
    for (auto& t : snap) if (t.joinable()) t.join();
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
