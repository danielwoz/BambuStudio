// Bambu Bridge — LAN control tunnel to printer:6000.
//
// Ported from BambuStudio/src/bambu_net_oss/core/LocalControlTunnel.{hpp,cpp}
// (Slic3r::oss namespace) into the bridge's `Slic3r::bridge::router` namespace
// so the bridge can speak the legacy BambuTunnel storage protocol directly
// (without going through the proprietary plugin).
//
// One LocalControlTunnel == one TLS 1.2 session on port 6000 implementing
// the auth + start-of-stream + framed-JSON protocol documented in
// `BambuStudio/docs/lan_port_6000_observations.md`.
//
// Threading: `request()` serializes via an internal mutex so multiple callers
// can share one tunnel. The class is otherwise NOT thread-safe — construct,
// connect, and destruct from a single owning context.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_LOCAL_CONTROL_TUNNEL_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_LOCAL_CONTROL_TUNNEL_HPP

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

typedef struct ssl_st     SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace Slic3r {
namespace bridge {
namespace router {

class LocalControlTunnel {
public:
    LocalControlTunnel(std::string host, int port, std::string access_code);
    ~LocalControlTunnel();

    LocalControlTunnel(const LocalControlTunnel&)            = delete;
    LocalControlTunnel& operator=(const LocalControlTunnel&) = delete;

    // Connect TCP + TLS + perform auth handshake + start-of-stream.
    // Returns 0 on success; -1 on failure (already logged via stderr).
    int connect_and_open(int timeout_ms = 10000);

    // Send `json_body` (will get `\n\n` terminator appended if absent) on the
    // data channel and read one reply frame. On success returns 0 and fills
    // out_json + out_binary with the printer's reply body (binary tail empty
    // for the JSON-only commands we exchange).
    int request(const std::string&    json_body,
                std::string&          out_json,
                std::vector<uint8_t>& out_binary,
                int                   timeout_ms = 5000);

    void shutdown();

    bool is_open() const { return ssl_ != nullptr; }

    const std::string& host() const { return host_; }
    int                port() const { return port_; }

private:
    int  send_frame_(uint8_t channel, const void* body, std::size_t len);
    int  recv_frame_(uint8_t& channel, uint8_t& flags, uint32_t& seq,
                     std::vector<uint8_t>& body, int timeout_ms);
    int  do_auth_();
    int  do_start_of_stream_();
    void close_();

    std::string host_;
    int         port_;
    std::string access_code_;

    int       fd_           = -1;
    SSL_CTX*  ssl_ctx_      = nullptr;
    SSL*      ssl_          = nullptr;
    uint32_t  outbound_seq_ = 0xb3a6db4cu;

    std::mutex io_mutex_;
};

}  // namespace router
}  // namespace bridge
}  // namespace Slic3r

#endif  // SLIC3R_BAMBU_BRIDGE_ROUTER_LOCAL_CONTROL_TUNNEL_HPP
