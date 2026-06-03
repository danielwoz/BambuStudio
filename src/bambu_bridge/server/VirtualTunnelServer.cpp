// VirtualTunnelServer implementation. See header for design.

#include "VirtualTunnelServer.hpp"
#include "../BambuSourceHandle.hpp"

#include "nlohmann/json.hpp"

#include "../platform/WinsockShim.hpp"

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
#include <mutex>
#include <thread>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace server {

namespace {

// Base64 decoder. Mirror of the encoder in
// `slic3r/GUI/Printer/BridgeStorageBackend.cpp`. BridgeStorageBackend
// smuggles binary `data` from PFS SUB_FILE replies (thumbnail bytes,
// zipped 3mf metadata) inside the reply JSON as `_bridge_data_b64`; we
// decode it back to bytes here and splice them into the JSON\n\n+bytes
// frame the slicer-side PFS' HandleResponse expects. Non-alphabet
// characters are skipped silently; `=` terminates.
std::string bridge_b64_decode(const std::string& in) {
    static const signed char T_init = -1; (void)T_init;
    static signed char T[256];
    static bool initialised = false;
    if (!initialised) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char alpha[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[(unsigned char)alpha[i]] = static_cast<signed char>(i);
        initialised = true;
    }
    std::string out;
    out.reserve((in.size() / 4) * 3);
    std::uint32_t buf = 0;
    int           bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        const signed char v = T[c];
        if (v < 0) continue;
        buf = (buf << 6) | std::uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((buf >> bits) & 0xff);
        }
    }
    return out;
}

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

int open_listener(const std::string& ip, uint16_t port, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    bambu_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    bambu_setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        bambu_close_socket(fd); return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        bambu_close_socket(fd); return -1;
    }
    if (::listen(fd, backlog) < 0) {
        bambu_close_socket(fd); return -1;
    }
    return fd;
}

uint16_t bound_port_of(int fd) {
    sockaddr_in addr{};
    bridge_socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return 0;
    return ntohs(addr.sin_port);
}

// Read exactly n bytes from `ssl`, blocking. Returns false on EOF/error.
bool ssl_read_exact(SSL* ssl, uint8_t* buf, size_t n) {
    size_t got = 0;
    static thread_local int call_seq = 0;
    int my_seq = ++call_seq;
    const int fd0 = SSL_get_fd(ssl);
    auto now_ms = []() {
        return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    long long t_enter = now_ms();
    while (got < n) {
        int r = SSL_read(ssl, buf + got, static_cast<int>(n - got));
        if (r <= 0) {
            const int err = SSL_get_error(ssl, r);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                continue;
            const int fd = SSL_get_fd(ssl);
            unsigned long avail = 0;
            unsigned long ssl_err_q = ERR_peek_error();
            char ssl_err_buf[256] = {0};
            if (ssl_err_q) ERR_error_string_n(ssl_err_q, ssl_err_buf, sizeof(ssl_err_buf));
            uint8_t peek[16] = {0};
            ssize_t peek_n = -1;
            int peek_errno = 0;
            if (fd >= 0) {
                bambu_bytes_available(fd, &avail);
                bambu_set_nonblocking(fd, true);
                peek_n = ::recv(fd, reinterpret_cast<char*>(peek), sizeof(peek), MSG_PEEK);
                bambu_set_nonblocking(fd, false);
                peek_errno = bambu_last_socket_error();
            }
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
    int                        listen_fd   = -1;
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
}

void VirtualTunnelServer::update_printer_lan_ip(
        const std::string& dev_id,
        const std::string& printer_lan_ip) {
    std::lock_guard<std::mutex> lk(m_mu);
    Device* d = find_locked(dev_id);
    if (!d) return;
    if (d->spec.printer_lan_ip == printer_lan_ip) return;
    d->spec.printer_lan_ip = printer_lan_ip;
}

void VirtualTunnelServer::update_printer_firmware_ver(
        const std::string& dev_id,
        const std::string& firmware_ver) {
    std::lock_guard<std::mutex> lk(m_mu);
    Device* d = find_locked(dev_id);
    if (!d) return;
    if (d->spec.printer_firmware_ver == firmware_ver) return;
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
    SSL* slicer_ssl = SSL_new(server_ctx);
    if (!slicer_ssl) { bambu_close_socket(client_fd); return; }
    SSL_set_fd(slicer_ssl, client_fd);
    const int acc_rc = SSL_accept(slicer_ssl);
    if (acc_rc != 1) {
        log_ssl_err("SSL_accept");
        SSL_free(slicer_ssl);
        bambu_close_socket(client_fd);
        return;
    }
    {
        unsigned long avail_after_accept = 0;
        bambu_bytes_available(client_fd, &avail_after_accept);
        int sock_err = 0; int se_len = sizeof(sock_err);
        bambu_getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &sock_err, &se_len);
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
            break;
        }
        std::vector<uint8_t> payload(n);
        if (!ssl_read_exact(slicer_ssl, payload.data(), n)) break;

        // Parse the slicer-virtual envelope. PFS-shaped: {cmdtype, sequence, req}.
        nlohmann::json env;
        try {
            env = nlohmann::json::parse(payload.begin(), payload.end());
        } catch (const std::exception& ex) {
            break;
        }
        const int  cmdtype  = env.value("cmdtype",  -1);
        const int  sequence = env.value("sequence",  0);
        nlohmann::json req_body = env.value("req", nlohmann::json::object());
        if (cmdtype < 0) {
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
                reply = nlohmann::json::object();
            }
            // If BridgeStorageBackend smuggled binary `data` from the
            // upstream PFS SUB_FILE response, strip the sidecar field
            // and splice the bytes into the wire frame after `\n\n`.
            // The slicer-side PFS' HandleResponse (PrinterFileSystem.cpp
            // ~line 1629) finds the `\n\n` delimiter and treats every
            // byte past it as the `data` portion of the sample, exactly
            // matching libBambuSource's on-wire shape. Without this the
            // slicer parses thumbnail bytes / 3mf metadata zip bytes as
            // zero, so the Storage tab loses timelapse previews and
            // model weight + print-time.
            std::string binary_data;
            if (reply.contains("_bridge_data_b64")) {
                try {
                    binary_data = bridge_b64_decode(
                        reply["_bridge_data_b64"].get<std::string>());
                } catch (const std::exception&) {
                    binary_data.clear();
                }
                reply.erase("_bridge_data_b64");
            }
            nlohmann::json envelope = {
                {"result",   rc},
                {"sequence", sequence},
                {"reply",    std::move(reply)},
            };
            const std::string body = envelope.dump();
            const char sep[2] = {'\n', '\n'};
            const std::size_t total =
                body.size()
              + (binary_data.empty() ? std::size_t{0}
                                     : sizeof(sep) + binary_data.size());
            const uint32_t nb = static_cast<uint32_t>(total);
            uint8_t hdr[4] = {
                static_cast<uint8_t>((nb >> 24) & 0xff),
                static_cast<uint8_t>((nb >> 16) & 0xff),
                static_cast<uint8_t>((nb >>  8) & 0xff),
                static_cast<uint8_t>( nb        & 0xff),
            };
            std::lock_guard<std::mutex> lk(*mu);
            if (!alive->load()) return;
            if (!ssl_write_all(ssl_capture, hdr, 4)) {
                return;
            }
            if (!ssl_write_all(ssl_capture,
                               reinterpret_cast<const uint8_t*>(body.data()),
                               body.size())) {
                return;
            }
            if (!binary_data.empty()) {
                if (!ssl_write_all(ssl_capture,
                                   reinterpret_cast<const uint8_t*>(sep),
                                   sizeof(sep))) {
                    return;
                }
                if (!ssl_write_all(ssl_capture,
                                   reinterpret_cast<const uint8_t*>(
                                       binary_data.data()),
                                   binary_data.size())) {
                    return;
                }
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
    bambu_close_socket(client_fd);
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
        bambu_close_socket(client_fd);
        return;
    }
    session_loop_backend(server_ctx, client_fd, spec, delegate,
                         slicer_net_ver, slicer_cli_id, slicer_cli_ver);
}

static void accept_loop(VirtualTunnelServer::Device* d, int io_timeout_s) {
    (void) io_timeout_s; // not used in Phase 1; sessions are stream-driven
    while (d->accepting.load()) {
        sockaddr_in caddr{};
        bridge_socklen_t clen = sizeof(caddr);
        // Use select so we can wake on shutdown.
        fd_set rfds; FD_ZERO(&rfds); FD_SET(d->listen_fd, &rfds);
        timeval tv{1, 0}; // 1s tick
        int sel = ::select(d->listen_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        int cfd = ::accept(d->listen_fd,
                           reinterpret_cast<sockaddr*>(&caddr), &clen);
        if (cfd < 0) {
            if (bambu_last_socket_error() == EINTR) continue;
            break;
        }
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
    if (d.listen_fd >= 0) return;
    d.storage_delegate = m_storage_delegate;
    d.slicer_net_ver = m_cfg.slicer_net_ver;
    d.slicer_cli_id  = m_cfg.slicer_cli_id;
    d.slicer_cli_ver = m_cfg.slicer_cli_ver;
    d.listen_fd = open_listener(d.spec.lan_ip, d.spec.port,
                                m_cfg.accept_backlog);
    if (d.listen_fd < 0) {
        return;
    }
    d.bound_port = bound_port_of(d.listen_fd);
    d.accepting.store(true);
    Device* dp = &d;
    int io_to = m_cfg.io_timeout_seconds;
    d.accept_thread = std::thread([dp, io_to]() { accept_loop(dp, io_to); });
}

void VirtualTunnelServer::stop_device(Device& d) {
    d.accepting.store(false);
    if (d.listen_fd >= 0) {
        ::shutdown(d.listen_fd, SHUT_RDWR);
        bambu_close_socket(d.listen_fd);
        d.listen_fd = -1;
    }
    if (d.accept_thread.joinable())
        d.accept_thread.join();
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
