// Bambu Bridge — TLS MQTT broker implementation (phase 4b).

#include "MqttBroker.hpp"

#include "IUplink.hpp"
#include "MqttFraming.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace Slic3r {
namespace bridge {
namespace server {

namespace {

// Process-wide monotonic session id. Used by IUplink's per-session
// attach_downstream / detach_downstream so each accepted slicer session
// gets a unique fan-out slot. Starts at 1 so 0 stays reserved for the
// deprecated 2-arg attach_downstream legacy path.
std::atomic<uint64_t> g_next_session_id{1};

// ---------------------------------------------------------------------------
// OpenSSL global init (one-time, idempotent across translation units).
// OpenSSL 1.1+ self-initialises but explicitly loading error strings makes
// the diagnostics from SSL_accept failures human-readable.
// ---------------------------------------------------------------------------
struct OpenSSLInit {
    OpenSSLInit() {
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
    }
};
void ensure_openssl_init() {
    static OpenSSLInit s_init;
    (void)s_init;
}

// Dump the latest OpenSSL error to stderr.
void log_ssl_err(const char* where) {
    unsigned long e = ERR_peek_last_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    ERR_clear_error();
}

// Build a per-device SSL_CTX from CertMaterial PEMs.
//
// TLS profile pinning (per docs/lan_mqtt_command_reference.md + the
// `MQTT_SSL_VERSION_TLS_1_2` constant in LanMqttSession.cpp):
//   - TLS 1.2 only (min == max == TLS1_2_VERSION). Real printers reject
//     TLS 1.3; pinning here makes the bridge match.
//   - No client cert (SSL_VERIFY_NONE).
//   - No SNI requirement (real printers don't check it).
//
// Returns a ref-counted SSL_CTX. Caller owns one reference and is expected
// to SSL_CTX_free() it once.
SSL_CTX* make_device_ctx(const tls::CertMaterial& cert) {
    ensure_openssl_init();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        log_ssl_err("SSL_CTX_new");
        return nullptr;
    }
    // Lock the protocol to TLS 1.2 only. SSL_CTX_set_min/max_proto_version
    // is the post-OpenSSL-1.1 way; the older SSL_OP_NO_TLSvN flags would
    // also work but this is cleaner.
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION) != 1) {
        log_ssl_err("SSL_CTX_set_*_proto_version");
        SSL_CTX_free(ctx);
        return nullptr;
    }
    // No client-cert request. Real printers don't ask for one.
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    // Load cert from PEM in-memory. BIO_new_mem_buf borrows the buffer;
    // we copy data so the BIO is independent of caller storage.
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

// Bind a TCP listening socket to (ip, port). Returns -1 on failure and
// sets `errno`.  On port==0 the kernel picks an ephemeral port; caller
// can recover it with getsockname.
int open_listener(const std::string& ip, uint16_t port, int backlog,
                  uint16_t& bound_port_out) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (ip.empty() || ip == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        errno = EINVAL;
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    if (::listen(fd, backlog) < 0) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    // Recover the actually-bound port (matters when port==0).
    sockaddr_in actual{};
    socklen_t len = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
        bound_port_out = ntohs(actual.sin_port);
    } else {
        bound_port_out = port;
    }
    return fd;
}

// Constant-time string compare for access-code validation. Real LAN
// printers don't actually do this (their MQTT auth is plaintext), but it
// costs nothing here and stops local timing-side-channel paranoia.
bool secure_streq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Per-device + per-session state.
// ---------------------------------------------------------------------------

struct MqttBroker::Device {
    MqttBrokerVirtualDevice spec;
    SSL_CTX*                ssl_ctx       = nullptr;
    int                     listen_fd     = -1;
    uint16_t                bound_port    = 0;
    MqttBroker*             broker        = nullptr; // back-pointer so the
                                                     // session io loop can
                                                     // reach the broker's
                                                     // print-command
                                                     // interceptor.

    std::atomic<bool>       stopped{false};
    std::thread             accept_thread;

    // Sessions currently active for this device. The broker enforces
    // max_clients_per_device by checking size() at accept time.
    struct Session {
        SSL*                ssl       = nullptr;
        int                 fd        = -1;
        std::thread         io_thread;
        std::atomic<bool>   stopped{false};

        // Subscribed topic filters. We don't implement full wildcard
        // routing (Bambu doesn't use it on LAN), but we remember the set
        // so unsubscribe can be acked correctly.
        std::unordered_set<std::string> subscriptions;

        // Downstream send queue. The TLS write side is single-threaded
        // (only the I/O thread writes), so producers append + signal and
        // the I/O loop drains under the same mutex.
        std::mutex                                send_mu;
        std::deque<std::vector<uint8_t>>          send_queue;

        // Per-session client-id from CONNECT, for logging.
        std::string client_id;

        // Broker-assigned unique session id. Used by IUplink's per-session
        // attach_downstream / detach_downstream so multiple slicer sessions
        // for the same dev_id each get their own fan-out slot.
        uint64_t    session_id = 0;

        // Lifetime sentinel held by both this Session AND the downstream
        // publisher lambda. Flipped to true by Cleanup when this session
        // tears down; the publisher checks it before touching `ssl`/`fd`,
        // so a stale publisher entry that survives past session destruction
        // (because attach_downstream stays wired until the NEXT session
        // overwrites it — required to avoid the slicer-reconnect race that
        // otherwise blackholes push_status for 30 s) can no-op safely.
        std::shared_ptr<std::atomic<bool>> dead { std::make_shared<std::atomic<bool>>(false) };
    };

    // Pointers, not values, because std::thread inside Session makes
    // moves awkward and we need stable addresses for the I/O thread.
    std::vector<std::unique_ptr<Session>> sessions;
    std::mutex                            sessions_mu;
};

// ---------------------------------------------------------------------------
// MqttBroker ctor/dtor + device registry.
// ---------------------------------------------------------------------------

MqttBroker::MqttBroker(MqttBrokerConfig cfg) : m_cfg(std::move(cfg)) {
    ensure_openssl_init();
}

MqttBroker::~MqttBroker() {
    stop();
}

MqttBroker::Device* MqttBroker::find_locked(const std::string& dev_id) {
    auto it = m_devices.find(dev_id);
    return it == m_devices.end() ? nullptr : it->second.get();
}

void MqttBroker::add_device(MqttBrokerVirtualDevice dev) {
    auto d        = std::make_unique<Device>();
    d->spec       = std::move(dev);
    d->broker     = this;
    d->ssl_ctx    = make_device_ctx(d->spec.cert);
    if (!d->ssl_ctx) {
        throw std::runtime_error("MqttBroker: failed to build SSL_CTX for dev_id="
                                 + d->spec.dev_id);
    }

    Device* raw = d.get();
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        m_devices[d->spec.dev_id] = std::move(d);
    }
    // If we're already running, bring this device up immediately.
    if (m_running.load()) {
        start_device(*raw);
    }
}

void MqttBroker::remove_device(const std::string& dev_id) {
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
        if (evicted->ssl_ctx) {
            SSL_CTX_free(evicted->ssl_ctx);
            evicted->ssl_ctx = nullptr;
        }
    }
}

uint16_t MqttBroker::bound_port(const std::string& dev_id) const {
    std::lock_guard<std::mutex> lk(m_devices_mu);
    auto it = m_devices.find(dev_id);
    if (it == m_devices.end()) return 0;
    return it->second->bound_port;
}

// ---------------------------------------------------------------------------
// Forward declaration of the per-connection I/O loop.
// ---------------------------------------------------------------------------

namespace {
void session_io_loop(MqttBroker::Device* dev,
                     MqttBroker::Device::Session* sess,
                     IUplink* uplink);

// Serve a minimal UPnP device descriptor (HTTP/1.1) so slicers that
// follow our SSDP LOCATION header (e.g. Orca's UPnP fetch of
// `http://<ip>:<mqtt_port>/upnp/desc.xml`) get a valid XML back
// instead of a TLS error. The slicer then proceeds with MQTT-over-TLS
// on a fresh TCP connection to the same port.
void serve_http_descriptor(int fd, const MqttBrokerVirtualDevice& spec) {
    // Drain the request line + headers; we don't actually parse them.
    {
        char drain[2048];
        // Best-effort, single recv. If the slicer pipelined later
        // requests we just close after the first one.
        (void) ::recv(fd, drain, sizeof(drain), MSG_DONTWAIT);
    }

    const std::string sn   = spec.virtual_dev_id.empty()
                             ? spec.dev_id : spec.virtual_dev_id;
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
        << "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
        << "  <specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
        << "  <device>\r\n"
        << "    <deviceType>urn:bambulab-com:device:3dprinter:1</deviceType>\r\n"
        << "    <friendlyName>Bambu Bridge</friendlyName>\r\n"
        << "    <manufacturer>Bambu Lab</manufacturer>\r\n"
        << "    <modelName>Bridge</modelName>\r\n"
        << "    <serialNumber>" << sn << "</serialNumber>\r\n"
        << "    <UDN>uuid:" << sn << "</UDN>\r\n"
        << "  </device>\r\n"
        << "</root>\r\n";
    const std::string body = xml.str();

    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: text/xml; charset=\"utf-8\"\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Server: Bambu/Bridge\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << body;
    const std::string r = resp.str();
    (void) ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
}
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

void MqttBroker::start() {
    bool was_running = m_running.exchange(true);
    if (was_running) return;
    std::lock_guard<std::mutex> lk(m_devices_mu);
    for (auto& kv : m_devices) {
        try {
            start_device(*kv.second);
        } catch (const std::exception& ex) {
        }
    }
}

void MqttBroker::stop() {
    bool was_running = m_running.exchange(false);
    if (!was_running) return;
    // Snapshot under lock so device destructors don't race with stop().
    std::vector<Device*> all;
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        all.reserve(m_devices.size());
        for (auto& kv : m_devices) all.push_back(kv.second.get());
    }
    for (Device* d : all) stop_device(*d);
}

void MqttBroker::start_device(Device& d) {
    if (d.listen_fd >= 0) return;
    uint16_t bound = 0;
    int fd = open_listener(d.spec.lan_ip, d.spec.port,
                           m_cfg.accept_backlog, bound);
    if (fd < 0) {
        const int saved = errno;
        throw std::runtime_error(std::string("MqttBroker: listen failed for ") +
                                 d.spec.lan_ip + ":" +
                                 std::to_string(d.spec.port) +
                                 ": " + std::strerror(saved));
    }
    d.listen_fd  = fd;
    d.bound_port = bound;
    d.stopped.store(false);

    IUplink* uplink = m_cfg.uplink.get();
    const int max_clients = m_cfg.max_clients_per_device;
    const int io_timeout  = m_cfg.io_timeout_seconds;

    d.accept_thread = std::thread([&d, uplink, max_clients, io_timeout]() {
        while (!d.stopped.load()) {
            // select() on the listener with a short timeout so stop() can
            // wake us promptly without needing a self-pipe.
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(d.listen_fd, &rfds);
            timeval tv{};
            tv.tv_sec  = 0;
            tv.tv_usec = 200 * 1000;  // 200 ms

            int rc = ::select(d.listen_fd + 1, &rfds, nullptr, nullptr, &tv);
            if (rc <= 0) continue;

            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            int cfd = ::accept(d.listen_fd,
                               reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (cfd < 0) continue;

            // Protocol-detect: TLS ClientHello starts with 0x16 (SSL3/
            // TLS Handshake content type). Anything else on this port
            // is almost certainly a slicer following our SSDP LOCATION
            // header (`http://<ip>:8883/upnp/desc.xml`) with a plain
            // HTTP GET. We serve a minimal UPnP descriptor and close;
            // the slicer then proceeds with MQTT-over-TLS on a fresh
            // TCP connection.
            unsigned char first = 0;
            {
                int peeked = ::recv(cfd, &first, 1, MSG_PEEK);
                if (peeked <= 0) { ::close(cfd); continue; }
            }
            if (first != 0x16) {
                serve_http_descriptor(cfd, d.spec);
                ::close(cfd);
                continue;
            }

            // Enforce max-concurrent-clients before doing the TLS handshake.
            {
                std::lock_guard<std::mutex> lk(d.sessions_mu);
                // Reap any sessions whose I/O thread has finished.
                for (auto it = d.sessions.begin(); it != d.sessions.end();) {
                    if ((*it)->stopped.load() && (*it)->io_thread.joinable()) {
                        (*it)->io_thread.join();
                        it = d.sessions.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (static_cast<int>(d.sessions.size()) >= max_clients) {
                    ::close(cfd);
                    continue;
                }
            }

            // Socket-level options: keep-alive + receive timeout so a dead
            // peer eventually gets reaped.
            int one = 1;
            ::setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
            ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            if (io_timeout > 0) {
                timeval rt{};
                rt.tv_sec = io_timeout;
                ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
            }

            // Build SSL object on this device's CTX.
            SSL* ssl = SSL_new(d.ssl_ctx);
            if (!ssl) {
                ::close(cfd);
                continue;
            }
            SSL_set_fd(ssl, cfd);

            auto sess        = std::make_unique<MqttBroker::Device::Session>();
            sess->ssl        = ssl;
            sess->fd         = cfd;
            sess->stopped.store(false);
            sess->session_id = g_next_session_id.fetch_add(1);

            MqttBroker::Device::Session* raw = sess.get();
            {
                std::lock_guard<std::mutex> lk(d.sessions_mu);
                d.sessions.push_back(std::move(sess));
            }
            raw->io_thread = std::thread(session_io_loop, &d, raw, uplink);
        }
    });
}

void MqttBroker::stop_device(Device& d) {
    d.stopped.store(true);
    if (d.accept_thread.joinable()) d.accept_thread.join();
    if (d.listen_fd >= 0) {
        ::close(d.listen_fd);
        d.listen_fd = -1;
    }
    // Tear down active sessions.
    std::vector<std::unique_ptr<Device::Session>> drained;
    {
        std::lock_guard<std::mutex> lk(d.sessions_mu);
        drained = std::move(d.sessions);
        d.sessions.clear();
    }
    for (auto& s : drained) {
        s->stopped.store(true);
        // Closing the fd kicks SSL_read out of select.
        if (s->fd >= 0) ::shutdown(s->fd, SHUT_RDWR);
        if (s->io_thread.joinable()) s->io_thread.join();
        if (s->ssl) { SSL_free(s->ssl); s->ssl = nullptr; }
        if (s->fd >= 0) { ::close(s->fd); s->fd = -1; }
    }
}

// ---------------------------------------------------------------------------
// Per-connection I/O loop.
// ---------------------------------------------------------------------------

namespace {

// Send everything currently in the session's send_queue.
// Returns false if a TLS write fails (caller should tear the session down).
bool flush_send_queue(MqttBroker::Device::Session* s) {
    std::deque<std::vector<uint8_t>> drained;
    {
        std::lock_guard<std::mutex> lk(s->send_mu);
        drained.swap(s->send_queue);
    }
    for (auto& buf : drained) {
        size_t off = 0;
        while (off < buf.size()) {
            int n = SSL_write(s->ssl,
                              buf.data() + off,
                              static_cast<int>(buf.size() - off));
            if (n <= 0) {
                log_ssl_err("SSL_write");
                return false;
            }
            off += static_cast<size_t>(n);
        }
    }
    return true;
}

// Enqueue a packet for the session and (if `urgent`) flush immediately.
void enqueue_send(MqttBroker::Device::Session* s,
                  std::vector<uint8_t> packet) {
    std::lock_guard<std::mutex> lk(s->send_mu);
    s->send_queue.push_back(std::move(packet));
}

void session_io_loop(MqttBroker::Device* dev,
                     MqttBroker::Device::Session* sess,
                     IUplink* uplink) {
    using namespace mqtt;

    // RAII close + uplink notify on exit. We don't free SSL/fd here —
    // that's done by stop_device when it reaps the session — because
    // the session pointer may still be in the device's sessions list.
    struct Cleanup {
        MqttBroker::Device*           dev;
        MqttBroker::Device::Session*  sess;
        IUplink*                      uplink;
        bool                          notified = false;
        ~Cleanup() {
            sess->stopped.store(true);
            // Flip the publisher's lifetime sentinel BEFORE notifying the
            // uplink. A stale publisher lambda still in the downstreams
            // map will now no-op instead of touching the soon-to-be-freed
            // ssl/fd.
            if (sess->dead) sess->dead->store(true);
            if (!notified && uplink) {
                // Per-session detach: only THIS session's entry leaves
                // the downstreams vector. Concurrent sessions for the
                // same dev_id keep receiving printer pushes. This
                // replaces the old "leave it wired, let the next attach
                // overwrite" hack — which was correct under single-slot
                // semantics but is wrong under multi-subscriber fan-out.
                if (sess->session_id != 0) {
                    uplink->detach_downstream(dev->spec.dev_id,
                                              sess->session_id);
                }
                uplink->on_disconnect(dev->spec.dev_id);
            }
        }
    } cleanup{dev, sess, uplink};

    // TLS handshake. SSL_accept may need multiple reads/writes; we let
    // OpenSSL block on the underlying fd by default (no SSL_set_nonblock).
    int rc = SSL_accept(sess->ssl);
    if (rc != 1) {
        log_ssl_err("SSL_accept");
        return;
    }

    // ---- CONNECT must be the first packet ---------------------------------
    std::vector<uint8_t> recv_buf;
    recv_buf.reserve(4096);

    // Poll-style read: returns true if bytes arrived, false on EOF/error,
    // and "no data within poll window" simply loops back (caller checks
    // stopped_/send_queue between calls). Using SSL_pending + select on
    // the underlying fd would be more efficient, but a 100ms select is
    // plenty for the bridge's traffic shape.
    auto try_read_some = [&]() -> int {
        // -1 = error/EOF, 0 = no data within poll window, 1 = got bytes.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sess->fd, &rfds);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 100 * 1000; // 100 ms
        // If OpenSSL has data already buffered (TLS record contained more
        // than what SSL_read pulled), skip select entirely.
        if (SSL_pending(sess->ssl) == 0) {
            int sr = ::select(sess->fd + 1, &rfds, nullptr, nullptr, &tv);
            if (sr < 0)  return -1;
            if (sr == 0) return 0;
        }
        uint8_t tmp[4096];
        int n = SSL_read(sess->ssl, tmp, sizeof(tmp));
        if (n <= 0) {
            int err = SSL_get_error(sess->ssl, n);
            if (err == SSL_ERROR_WANT_READ) return 0;
            return -1;
        }
        recv_buf.insert(recv_buf.end(), tmp, tmp + n);
        return 1;
    };

    // Used only for the initial CONNECT phase where we want to block
    // until CONNECT actually arrives. Loops on try_read_some with stop
    // checks so we never hang forever.
    auto read_more_blocking = [&]() -> bool {
        while (!sess->stopped.load() && !dev->stopped.load()) {
            int r = try_read_some();
            if (r < 0) return false;
            if (r > 0) return true;
        }
        return false;
    };

    // Pump until we have a full CONNECT packet.
    std::optional<MqttPacket> first;
    while (!sess->stopped.load()) {
        first = decode_packet(recv_buf.data(), recv_buf.size());
        if (first) break;
        if (!read_more_blocking()) return;
    }
    if (!first || first->type != PacketType::Connect) {
        return;
    }
    if (first->error != DecodeError::Ok) {
        // Best-effort: send CONNACK BadCredentials/UnacceptableProtocol so the
        // client gets a defined response, then close.
        ConnackReturnCode rc_send = ConnackReturnCode::UnacceptableProtocol;
        if (first->error == DecodeError::ProtocolViolation)
            rc_send = ConnackReturnCode::UnacceptableProtocol;
        auto pkt = encode_connack(rc_send);
        SSL_write(sess->ssl, pkt.data(), static_cast<int>(pkt.size()));
        return;
    }

    const ConnectPacket& con = first->connect;
    sess->client_id = con.client_id;

    // Auth: username "bblp", password = device access code.
    // Bambu printers use the dev access code as the MQTT password; if a
    // device record has an empty access_code we treat it as an
    // accept-all-passwords dev (handy for tests; not for production).
    const std::string supplied_pass(con.password.begin(), con.password.end());
    const bool auth_ok =
        con.has_username && con.has_password &&
        con.username == "bblp" &&
        (dev->spec.access_code.empty() ||
         secure_streq(supplied_pass, dev->spec.access_code));
    if (!auth_ok) {
        std::fprintf(stderr,
            "[mqtt-broker] CONNECT auth fail dev_id=%s client_id=%s user='%s' pass_len=%zu expected_len=%zu\n",
            dev->spec.dev_id.c_str(), con.client_id.c_str(),
            (con.has_username ? con.username.c_str() : "-"),
            (con.has_password ? supplied_pass.size() : 0),
            dev->spec.access_code.size());
        std::fflush(stderr);
        auto pkt = encode_connack(ConnackReturnCode::NotAuthorized);
        SSL_write(sess->ssl, pkt.data(), static_cast<int>(pkt.size()));
        return;
    }

    // Accept the session. session_present=false because we don't persist
    // sessions across reconnects.
    {
        auto pkt = encode_connack(ConnackReturnCode::Accepted);
        if (SSL_write(sess->ssl, pkt.data(), static_cast<int>(pkt.size())) <= 0) {
            log_ssl_err("SSL_write CONNACK");
            return;
        }
    }

    // Wire the downstream injector. The uplink (and inject_downstream)
    // call into this lambda from arbitrary threads; we serialise via the
    // session send_mu inside enqueue_send + flush_send_queue.
    if (uplink) {
        // Capture the real/virtual SN pair so the publisher can rewrite
        // `device/<real_sn>/...` -> `device/<virtual_sn>/...` before
        // pushing to the slicer. The plugin's local-message receiver
        // hands us the real_sn in the topic; the slicer subscribed
        // with the virtual_sn we advertised via SSDP, so the topic
        // strings have to match the slicer's filter or the message is
        // silently filtered out at the broker.
        const std::string real_sn    = dev->spec.dev_id;
        const std::string virtual_sn = dev->spec.virtual_dev_id;
        // Capture the session's lifetime sentinel by VALUE (shared_ptr).
        // The shared_ptr keeps the atomic<bool> alive even if this Session
        // struct is reaped and destroyed — Cleanup flips it true before
        // teardown, and this lambda short-circuits without touching the
        // dangling sess pointer.
        auto dead_flag = sess->dead;
        IUplink::DownstreamPublisher publisher =
            [sess, real_sn, virtual_sn, dead_flag]
            (std::string topic, std::vector<uint8_t> payload, uint8_t qos) {
                if (dead_flag && dead_flag->load()) return;
                if (sess->stopped.load()) return;
                if (!virtual_sn.empty() && virtual_sn != real_sn) {
                    auto pos = topic.find(real_sn);
                    if (pos != std::string::npos)
                        topic.replace(pos, real_sn.size(), virtual_sn);
                }
                // Use packet_id=0 for QoS 0; the broker doesn't yet
                // generate ids for downstream QoS 1 publishes.
                auto pkt = encode_publish(topic, payload, qos,
                                          /*retain=*/false,
                                          /*packet_id=*/0, /*dup=*/false);
                enqueue_send(sess, std::move(pkt));
            };
        uplink->attach_downstream(dev->spec.dev_id, sess->session_id, publisher);
    }

    // Consumed bytes for the CONNECT packet.
    recv_buf.erase(recv_buf.begin(), recv_buf.begin() + first->bytes_consumed);

    // ---- Main packet loop -------------------------------------------------
    while (!sess->stopped.load() && !dev->stopped.load()) {
        // First, flush anything queued by inject_downstream / uplink. We
        // re-check on every iteration so downstream injects from other
        // threads land at the slicer within ~100 ms (the read poll window).
        if (!flush_send_queue(sess)) break;

        // Decode whatever's in the buffer; on truncation, poll for more
        // bytes with a short window so the next loop iteration can drain
        // the send_queue even if the client is idle.
        auto pk = decode_packet(recv_buf.data(), recv_buf.size());
        if (!pk) {
            int r = try_read_some();
            if (r < 0) break;
            continue;
        }
        if (pk->error != DecodeError::Ok) {
            break;
        }

        switch (pk->type) {
        case PacketType::Publish: {
            // QoS 1 requires a PUBACK before we ack the uplink. We don't
            // implement duplicate suppression by packet_id (Bambu firmware
            // doesn't reuse ids within a session) but we do honour the
            // DUP flag: if set, the upstream side may receive a duplicate
            // — that's the printer's problem to dedup. Real LAN sessions
            // observed in `LanMqttSession` use QoS 0 for /report and a
            // mix of 0/1 for /request — the broker handles both.
            if (uplink) {
                // Diagnostic: log every slicer→printer PUBLISH so we can
                // confirm filament/print/AMS commands are reaching the
                // bridge at all + whether the SN rewrite fired.
                {
                    const std::string& v = dev->spec.virtual_dev_id;
                    size_t rewrite_hits = 0;
                    if (!v.empty() && v != dev->spec.dev_id
                        && v.size() == dev->spec.dev_id.size()) {
                        const auto& buf = pk->publish.payload;
                        const uint8_t* needle = reinterpret_cast<const uint8_t*>(v.data());
                        const size_t   nlen   = v.size();
                        for (size_t i = 0; i + nlen <= buf.size(); ++i) {
                            if (std::memcmp(buf.data() + i, needle, nlen) == 0) {
                                ++rewrite_hits;
                                i += nlen - 1;
                            }
                        }
                    }
                    std::fprintf(stderr,
                        "[mqtt-broker] UPSTREAM dev=%s topic=%s qos=%u bytes=%zu virtual_sn_hits=%zu\n",
                        dev->spec.dev_id.c_str(), pk->publish.topic.c_str(),
                        unsigned(pk->publish.qos), pk->publish.payload.size(),
                        rewrite_hits);
                    std::fflush(stderr);
                }
                // Rewrite virtual_sn → real_sn in the JSON payload. The
                // slicer's MachineObject is keyed on the FFFF-mangled
                // dev_id and embeds that virtual SN throughout its
                // command JSON ("dev_id", "user_id", target identifiers,
                // etc.). Real Bambu printer firmware rejects commands
                // whose payload SN doesn't match the printer's own
                // serial — which is why AMS/filament/print commands
                // appear to do nothing even though the topic + plugin
                // routing is fine. SNs are 15-char ASCII, so this is a
                // length-preserving rewrite — safe to do in-place
                // without re-allocating.
                const std::string& real_sn    = dev->spec.dev_id;
                const std::string& virtual_sn = dev->spec.virtual_dev_id;
                if (!virtual_sn.empty()
                    && virtual_sn != real_sn
                    && virtual_sn.size() == real_sn.size()) {
                    auto& buf = pk->publish.payload;
                    if (buf.size() >= virtual_sn.size()) {
                        const uint8_t* needle =
                            reinterpret_cast<const uint8_t*>(virtual_sn.data());
                        const size_t   nlen   = virtual_sn.size();
                        for (size_t i = 0; i + nlen <= buf.size(); ++i) {
                            if (std::memcmp(buf.data() + i, needle, nlen) == 0) {
                                std::memcpy(buf.data() + i,
                                            real_sn.data(), nlen);
                                i += nlen - 1;
                            }
                        }
                    }
                }
                // Interceptor for `print.command=gcode_file`. The
                // slicer publishes this on `device/<sn>/request` right
                // after the matching FTPS upload. LanUploadSink registered
                // the upload's spool; the interceptor we set up in
                // BridgeApp pulls that spool path out and drives the
                // plugin's start_local_print_with_record (LAN) or
                // start_print (cloud fallback) with the FULL AMS context
                // from this JSON. If the callback returns 0 (handled),
                // suppress the verbatim forward — the plugin's call
                // sends its own equivalent MQTT command to the printer.
                // On nonzero (not spooled, parse failure, plugin error),
                // fall through to the verbatim forward so behaviour
                // degrades gracefully.
                bool intercepted = false;
                if (dev->broker) {
                    const auto& buf = pk->publish.payload;
                    // Cheap pre-filter: avoid JSON-parsing every
                    // heartbeat / status message. The `gcode_file`
                    // command always appears as `"command":"gcode_file"`
                    // in the payload (virtual_lan_print_ uses
                    // j.dump() which emits compact JSON without
                    // whitespace).
                    static constexpr const char kNeedle[] =
                        "\"command\":\"gcode_file\"";
                    const size_t nlen = sizeof(kNeedle) - 1;
                    bool maybe = false;
                    if (buf.size() >= nlen) {
                        for (size_t i = 0; i + nlen <= buf.size(); ++i) {
                            if (std::memcmp(buf.data() + i, kNeedle, nlen) == 0) {
                                maybe = true;
                                break;
                            }
                        }
                    }
                    if (maybe) {
                        std::string json_str(
                            reinterpret_cast<const char*>(buf.data()),
                            buf.size());
                        int rc = dev->broker->try_intercept_print_command(
                            dev->spec.dev_id, dev->spec.virtual_dev_id,
                            json_str);
                        if (rc == 0) {
                            intercepted = true;
                            std::fprintf(stderr,
                                "[mqtt-broker] dev=%s gcode_file "
                                "INTERCEPTED (no verbatim forward)\n",
                                dev->spec.dev_id.c_str());
                            std::fflush(stderr);
                        } else if (rc == 1) {
                            // No interceptor — fall through to verbatim.
                        } else {
                            std::fprintf(stderr,
                                "[mqtt-broker] dev=%s gcode_file "
                                "interceptor rc=%d — falling back to "
                                "verbatim forward\n",
                                dev->spec.dev_id.c_str(), rc);
                            std::fflush(stderr);
                        }
                    }
                }
                if (!intercepted) {
                    uplink->on_publish(dev->spec.dev_id, pk->publish.topic,
                                       std::move(pk->publish.payload),
                                       pk->publish.qos);
                }
            }
            if (pk->publish.qos == 1) {
                auto ack = encode_puback(pk->publish.packet_id);
                enqueue_send(sess, std::move(ack));
            }
            // QoS 2 not supported; the decoder already rejected
            // PUBREC/REL/COMP. A QoS-2 PUBLISH would surface here as
            // qos==2 — we ack as best we can but don't run the full
            // 4-way handshake. Bambu doesn't use QoS 2.
            break;
        }
        case PacketType::Subscribe: {
            std::vector<uint8_t> rcs;
            rcs.reserve(pk->subscribe.filters.size());
            for (auto& f : pk->subscribe.filters) {
                sess->subscriptions.insert(f.topic);
                // Granted-QoS = requested (we don't downgrade).
                rcs.push_back(f.qos);
                if (uplink) {
                    uplink->on_subscribe(dev->spec.dev_id, f.topic);
                }
            }
            auto pkt = encode_suback(pk->subscribe.packet_id, rcs);
            enqueue_send(sess, std::move(pkt));
            break;
        }
        case PacketType::Unsubscribe: {
            for (auto& t : pk->unsubscribe.topics) {
                sess->subscriptions.erase(t);
                if (uplink) {
                    uplink->on_unsubscribe(dev->spec.dev_id, t);
                }
            }
            auto pkt = encode_unsuback(pk->unsubscribe.packet_id);
            enqueue_send(sess, std::move(pkt));
            break;
        }
        case PacketType::Pingreq: {
            auto pkt = encode_pingresp();
            enqueue_send(sess, std::move(pkt));
            break;
        }
        case PacketType::Puback: {
            // The client is acking a QoS-1 PUBLISH we sent downstream.
            // We don't yet track in-flight downstream packets so just
            // log the id for visibility.
            (void)pk->puback.packet_id;
            break;
        }
        case PacketType::Disconnect: {
            // Clean shutdown. Don't bubble on_disconnect twice — the
            // Cleanup struct's destructor handles it.
            return;
        }
        default:
            return;
        }

        // Advance the recv buffer.
        recv_buf.erase(recv_buf.begin(), recv_buf.begin() + pk->bytes_consumed);

        // Flush any acks/responses we just queued.
        if (!flush_send_queue(sess)) break;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public test/integration hooks.
// ---------------------------------------------------------------------------

void MqttBroker::set_uplink(std::shared_ptr<IUplink> uplink) {
    // Refuse mid-run swaps; the accept thread captured the raw IUplink*
    // and changing it under it is a data race.
    if (m_running.load()) {
        return;
    }
    m_cfg.uplink = std::move(uplink);
}

void MqttBroker::set_print_command_interceptor(PrintCommandInterceptor cb) {
    std::lock_guard<std::mutex> lk(m_intercept_mu);
    m_print_intercept = std::move(cb);
}

int MqttBroker::try_intercept_print_command(const std::string& dev_id,
                                            const std::string& virtual_dev_id,
                                            const std::string& json_payload) {
    PrintCommandInterceptor cb;
    {
        std::lock_guard<std::mutex> lk(m_intercept_mu);
        cb = m_print_intercept;
    }
    if (!cb) return 1; // no interceptor installed; caller forwards verbatim
    return cb(dev_id, virtual_dev_id, json_payload);
}

void MqttBroker::inject_downstream(const std::string& dev_id,
                                   std::string topic,
                                   std::vector<uint8_t> payload,
                                   uint8_t qos) {
    using namespace mqtt;
    auto pkt = encode_publish(topic, payload, qos,
                              /*retain=*/false, /*packet_id=*/0, /*dup=*/false);
    std::lock_guard<std::mutex> lk(m_devices_mu);
    auto* dev = find_locked(dev_id);
    if (!dev) return;
    std::lock_guard<std::mutex> sk(dev->sessions_mu);
    for (auto& s : dev->sessions) {
        if (s->stopped.load()) continue;
        std::lock_guard<std::mutex> qk(s->send_mu);
        s->send_queue.push_back(pkt);
    }
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
