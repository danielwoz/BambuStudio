// Bambu Bridge — minimal implicit-TLS FTP client for tests (phase 7).
//
// Hand-rolled USER/PASS/PBSZ/PROT/TYPE/PASV/STOR/QUIT issuance over an
// OpenSSL TLS socket. The on-the-wire shape matches what FtpsClient.cpp
// (libcurl with CURLUSESSL_ALL) produces, but here we drive every command
// explicitly so the test can assert exact reply codes and round-trip a
// 1024-byte STOR through PASV.
//
// Not a general-purpose FTP client — it does only what
// `tests/bridge/FtpsServerLoopbackTest.cpp` and `FtpsAuthTest.cpp` need:
//   - Implicit TLS 1.2 connect on the control port (no AUTH TLS upgrade
//     dance); the printer's cert is a per-device self-signed cert from
//     CertFactory, so verification is disabled.
//   - PASV mode only (parses the 227 reply's host:port tuple).
//   - The data channel runs its own TLS handshake on the same SSL_CTX
//     style. STOR streams an arbitrary payload then closes the data
//     channel to signal EOF.
//   - QUIT shuts down cleanly so the broker's session loop returns 221.
//
// Linux-only.

#ifndef SLIC3R_BAMBU_BRIDGE_TEST_FTPS_CLIENT_HPP
#define SLIC3R_BAMBU_BRIDGE_TEST_FTPS_CLIENT_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace test {

class FtpsTestClient {
public:
    FtpsTestClient();
    ~FtpsTestClient();

    FtpsTestClient(const FtpsTestClient&)            = delete;
    FtpsTestClient& operator=(const FtpsTestClient&) = delete;

    // Connect TCP + implicit TLS on the control port. Returns false on
    // any error; the caller can call last_error() for a reason. Reads
    // the 220 welcome banner and stashes it.
    bool connect(const std::string& host, uint16_t port,
                 std::chrono::seconds timeout = std::chrono::seconds(5));

    // Send "USER bblp" then "PASS <pw>". Returns the final PASS reply code
    // (230 on success, 530 on auth failure). -1 on socket/TLS error.
    int login(const std::string& user, const std::string& password);

    // Send "PBSZ 0", "PROT P", "TYPE I" and check each reply is 200.
    bool setup_data_channel_tls();

    // Issue PASV; parses the 227 (h1,h2,h3,h4,p1,p2). Caller receives the
    // returned (ip, port) which is the data endpoint to TCP+TLS to.
    bool pasv(std::string& data_ip_out, uint16_t& data_port_out);

    // STOR <remote_path>: open a TLS data channel to (host_for_data, port),
    // stream `payload`, close the data channel, then read the closing 226.
    // Returns the 226-or-other reply code, or -1 on protocol/socket error.
    int stor(const std::string& remote_path,
             const std::string& host_for_data,
             uint16_t           port_for_data,
             const std::vector<uint8_t>& payload);

    // Send "QUIT"; returns the 221 reply code or -1 on error.
    int quit();

    // Send an arbitrary command; returns the int reply code and fills
    // body_out with whatever followed the code on the final line.
    int raw_command(const std::string& cmd, std::string& body_out);

    void close();

    const std::string& last_error()    const { return m_last_error; }
    const std::string& welcome_banner() const { return m_banner; }

private:
    // Send "cmd\r\n" on control. Returns false on write failure.
    bool send_line(const std::string& cmd);

    // Read one FTP reply, returning the integer code and the trailing
    // body text (post-code, sans CRLF). Returns -1 on socket/TLS error.
    int read_reply(std::string& body_out);

    int   m_fd  = -1;
    void* m_ssl = nullptr;   // SSL*
    void* m_ctx = nullptr;   // SSL_CTX*
    std::vector<uint8_t> m_recv;
    std::string          m_last_error;
    std::string          m_banner;
};

} // namespace test
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_TEST_FTPS_CLIENT_HPP
