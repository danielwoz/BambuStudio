// Bambu Bridge — LAN control tunnel to printer:6000.
//
// Ported from BambuStudio/src/bambu_net_oss/core/LocalControlTunnel.cpp.
// The only adaptations vs. the OSS source:
//   - Namespace renamed to `Slic3r::bridge::router`.
//   - Logging routed through stderr with `[native-storage]` tags so it
//     interleaves with the bridge's existing log channel without dragging
//     in `boost::log` (the bridge doesn't enable that subsystem).
//
// Wire format and protocol details are unchanged. See
// `BambuStudio/docs/lan_port_6000_observations.md` for the authoritative spec.

#include "LocalControlTunnel.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

constexpr uint8_t kMagic0      = 0x3F;
constexpr uint8_t kMagic1      = 0x01;
constexpr uint8_t kChanAuth    = 0x01;
constexpr uint8_t kChanData    = 0x02;
constexpr uint8_t kFlagRequest = 0x01;
constexpr int     kMTypeStart   = 0x3003;
constexpr int     kMTypeControl = 0x3001;

struct Header {
    uint32_t payload_len;
    uint8_t  magic0;
    uint8_t  magic1;
    uint8_t  channel;
    uint8_t  flags;
    uint32_t seq;
    uint32_t reserved;
};

void encode_header(uint8_t out[16], uint32_t payload_len, uint8_t channel,
                   uint8_t flags, uint32_t seq)
{
    out[0] = (uint8_t)(payload_len & 0xff);
    out[1] = (uint8_t)((payload_len >> 8) & 0xff);
    out[2] = (uint8_t)((payload_len >> 16) & 0xff);
    out[3] = (uint8_t)((payload_len >> 24) & 0xff);
    out[4] = kMagic0;
    out[5] = kMagic1;
    out[6] = channel;
    out[7] = flags;
    out[8]  = (uint8_t)(seq & 0xff);
    out[9]  = (uint8_t)((seq >> 8) & 0xff);
    out[10] = (uint8_t)((seq >> 16) & 0xff);
    out[11] = (uint8_t)((seq >> 24) & 0xff);
    out[12] = out[13] = out[14] = out[15] = 0;
}

bool decode_header(const uint8_t in[16], Header& h)
{
    h.payload_len = (uint32_t) in[0]
                  | ((uint32_t) in[1] << 8)
                  | ((uint32_t) in[2] << 16)
                  | ((uint32_t) in[3] << 24);
    h.magic0  = in[4];
    h.magic1  = in[5];
    h.channel = in[6];
    h.flags   = in[7];
    h.seq     = (uint32_t) in[8]
              | ((uint32_t) in[9] << 8)
              | ((uint32_t) in[10] << 16)
              | ((uint32_t) in[11] << 24);
    h.reserved = (uint32_t) in[12]
               | ((uint32_t) in[13] << 8)
               | ((uint32_t) in[14] << 16)
               | ((uint32_t) in[15] << 24);
    return h.magic0 == kMagic0 && h.magic1 == kMagic1;
}

int wait_writable(int fd, int timeout_ms)
{
    pollfd p{fd, POLLOUT, 0};
    return ::poll(&p, 1, timeout_ms);
}
int wait_readable(int fd, int timeout_ms)
{
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, timeout_ms);
}

int tcp_connect(const std::string& host, int port, int timeout_ms)
{
    addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);
    if (::getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) return -1;

    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd < 0) continue;
        int yes = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) break;
        if (errno == EINPROGRESS) {
            int pr = wait_writable(fd, timeout_ms);
            if (pr > 0) {
                int err = 0;
                socklen_t errlen = sizeof(err);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == 0 && err == 0) {
                    break;
                }
            }
        }
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

}  // namespace

LocalControlTunnel::LocalControlTunnel(std::string host, int port, std::string access_code)
    : host_(std::move(host)), port_(port), access_code_(std::move(access_code))
{}

LocalControlTunnel::~LocalControlTunnel() { close_(); }

int LocalControlTunnel::connect_and_open(int timeout_ms)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (ssl_) return 0;

    fd_ = tcp_connect(host_, port_, timeout_ms);
    if (fd_ < 0) {
        std::fprintf(stderr,
                     "[native-storage] tcp_connect failed %s:%d errno=%d\n",
                     host_.c_str(), port_, errno);
        std::fflush(stderr);
        return -1;
    }

    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) { close_(); return -1; }
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx_, TLS1_2_VERSION);
    SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
    // X1C / P1S / H2S all accept AES256-GCM-SHA384 with RSA key exchange.
    SSL_CTX_set_cipher_list(ssl_ctx_, "AES256-GCM-SHA384");

    ssl_ = SSL_new(ssl_ctx_);
    if (!ssl_) { close_(); return -1; }
    SSL_set_fd(ssl_, fd_);
    // Empty SNI; printers reject the handshake if SNI is set.
    SSL_set_tlsext_host_name(ssl_, nullptr);

    while (true) {
        int r = SSL_connect(ssl_);
        if (r == 1) break;
        int e = SSL_get_error(ssl_, r);
        if (e == SSL_ERROR_WANT_READ) {
            if (wait_readable(fd_, timeout_ms) <= 0) { close_(); return -1; }
        } else if (e == SSL_ERROR_WANT_WRITE) {
            if (wait_writable(fd_, timeout_ms) <= 0) { close_(); return -1; }
        } else {
            std::fprintf(stderr,
                         "[native-storage] SSL_connect err=%d host=%s\n",
                         e, host_.c_str());
            std::fflush(stderr);
            close_();
            return -1;
        }
    }

    if (do_auth_() != 0) { close_(); return -1; }
    if (do_start_of_stream_() != 0) { close_(); return -1; }

    std::fprintf(stderr, "[native-storage] connected %s:%d\n",
                 host_.c_str(), port_);
    std::fflush(stderr);
    return 0;
}

int LocalControlTunnel::do_auth_()
{
    uint8_t hdr[16];
    encode_header(hdr, 16, kChanAuth, kFlagRequest, outbound_seq_++);

    uint8_t creds[16] = {0};
    creds[0] = 'b'; creds[1] = 'b'; creds[2] = 'l'; creds[3] = 'p';
    // bytes 4..7 zero (already memset).
    // bytes 8..15 = first 8 chars of access code, NUL-padded.
    std::size_t copy = std::min<std::size_t>(8, access_code_.size());
    std::memcpy(&creds[8], access_code_.data(), copy);

    int r = SSL_write(ssl_, hdr, 16);
    if (r != 16) return -1;
    r = SSL_write(ssl_, creds, 16);
    if (r != 16) return -1;

    uint8_t              rch, rflags;
    uint32_t             rseq;
    std::vector<uint8_t> body;
    if (recv_frame_(rch, rflags, rseq, body, 5000) != 0) return -1;
    if (rch != kChanAuth || body.size() < 4) {
        std::fprintf(stderr,
                     "[native-storage] auth reply unexpected ch=%d body=%zu\n",
                     (int) rch, body.size());
        std::fflush(stderr);
        return -1;
    }
    uint32_t status = (uint32_t) body[0]
                    | ((uint32_t) body[1] << 8)
                    | ((uint32_t) body[2] << 16)
                    | ((uint32_t) body[3] << 24);
    if (status != 0) {
        std::fprintf(stderr, "[native-storage] auth status=%u (non-zero)\n",
                     status);
        std::fflush(stderr);
        return -1;
    }
    return 0;
}

int LocalControlTunnel::do_start_of_stream_()
{
    char body[256];
    int  n = std::snprintf(body, sizeof(body),
                           "{\"sequence\":0,\"mtype\":%d,\"req\":{\"t_av\":0,\"mtype\":%d,\"peer_t\":3,\"pid\":\"\",\"ver\":\"\"}}\n\n",
                           kMTypeStart, kMTypeControl);
    if (n <= 0 || n >= (int) sizeof(body)) return -1;

    if (send_frame_(kChanData, body, (std::size_t) n) != 0) return -1;

    uint8_t              rch, rflags;
    uint32_t             rseq;
    std::vector<uint8_t> reply;
    if (recv_frame_(rch, rflags, rseq, reply, 5000) != 0) return -1;
    if (rch != kChanData) {
        std::fprintf(stderr,
                     "[native-storage] start-of-stream wrong ch=%d\n",
                     (int) rch);
        std::fflush(stderr);
        return -1;
    }
    std::string s((const char*) reply.data(), reply.size());
    if (s.find("\"result\":0") == std::string::npos) {
        std::fprintf(stderr,
                     "[native-storage] start-of-stream rejected: %s\n",
                     s.c_str());
        std::fflush(stderr);
        return -1;
    }
    return 0;
}

int LocalControlTunnel::send_frame_(uint8_t channel, const void* body, std::size_t len)
{
    uint8_t hdr[16];
    encode_header(hdr, (uint32_t) len, channel, kFlagRequest, outbound_seq_++);
    int r = SSL_write(ssl_, hdr, 16);
    if (r != 16) return -1;
    r = SSL_write(ssl_, body, (int) len);
    if (r != (int) len) return -1;
    return 0;
}

int LocalControlTunnel::recv_frame_(uint8_t& channel, uint8_t& flags, uint32_t& seq,
                                    std::vector<uint8_t>& body, int timeout_ms)
{
    auto ssl_read_full = [&](uint8_t* dst, std::size_t want) -> int {
        std::size_t off = 0;
        while (off < want) {
            int r = SSL_read(ssl_, dst + off, (int) (want - off));
            if (r > 0) { off += (std::size_t) r; continue; }
            int e = SSL_get_error(ssl_, r);
            if (e == SSL_ERROR_WANT_READ) {
                if (wait_readable(fd_, timeout_ms) <= 0) return -1;
                continue;
            }
            if (e == SSL_ERROR_WANT_WRITE) {
                if (wait_writable(fd_, timeout_ms) <= 0) return -1;
                continue;
            }
            return -1;
        }
        return 0;
    };

    uint8_t hdr[16];
    if (ssl_read_full(hdr, 16) != 0) return -1;
    Header h;
    if (!decode_header(hdr, h)) {
        std::fprintf(stderr, "[native-storage] bad header magic\n");
        std::fflush(stderr);
        return -1;
    }
    channel = h.channel;
    flags   = h.flags;
    seq     = h.seq;

    body.resize(h.payload_len);
    if (h.payload_len > 0 && ssl_read_full(body.data(), body.size()) != 0) {
        return -1;
    }
    return 0;
}

int LocalControlTunnel::request(const std::string&    json_body,
                                std::string&          out_json,
                                std::vector<uint8_t>& out_binary,
                                int                   timeout_ms)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ssl_) return -1;

    std::string framed = json_body;
    if (framed.size() < 2 || framed.substr(framed.size() - 2) != "\n\n") {
        framed += "\n\n";
    }
    if (send_frame_(kChanData, framed.data(), framed.size()) != 0) return -1;

    uint8_t              ch, fl;
    uint32_t             seq;
    std::vector<uint8_t> body;
    if (recv_frame_(ch, fl, seq, body, timeout_ms) != 0) return -1;
    if (ch != kChanData) return -1;

    // Reply body is JSON terminated with \n\n; an optional binary tail
    // follows. Find the JSON terminator.
    std::size_t json_end = body.size();
    for (std::size_t i = 0; i + 1 < body.size(); ++i) {
        if (body[i] == '\n' && body[i + 1] == '\n') { json_end = i; break; }
    }
    out_json.assign((const char*) body.data(), json_end);
    if (json_end + 2 < body.size()) {
        out_binary.assign(body.begin() + (long) json_end + 2, body.end());
    } else {
        out_binary.clear();
    }
    return 0;
}

void LocalControlTunnel::shutdown()
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    close_();
}

void LocalControlTunnel::close_()
{
    if (ssl_)     { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
    if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_);             ssl_ctx_ = nullptr; }
    if (fd_ >= 0) { ::close(fd_);                        fd_ = -1; }
}

}  // namespace router
}  // namespace bridge
}  // namespace Slic3r
