// Bambu Bridge — SSDP listener implementation.

#include "SsdpListener.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Slic3r {
namespace bridge {
namespace server {

namespace {

// Lower-cased value of one HTTP-style header, or empty if absent.
std::string header_value(const std::string& payload, const std::string& lc_key) {
    std::istringstream iss(payload);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) { first = false; continue; }
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        auto p = v.find_first_not_of(" \t");
        if (p != std::string::npos) v.erase(0, p);
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (k == lc_key) return v;
    }
    return {};
}

int open_udp_listener(const std::string& bind_addr, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (bind_addr.empty() || bind_addr == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

SsdpListener::SsdpListener(Config cfg, HeardCallback cb)
    : m_cfg(std::move(cfg)), m_cb(std::move(cb)) {}

SsdpListener::~SsdpListener() { stop(); }

bool SsdpListener::start() {
    if (m_running.exchange(true)) return true;
    m_fd = open_udp_listener(m_cfg.bind_address, m_cfg.port);
    if (m_fd < 0) {
        std::fprintf(stderr,
            "[ssdp-listener] bind %s:%u failed: %s\n",
            m_cfg.bind_address.c_str(), m_cfg.port, std::strerror(errno));
        m_running.store(false);
        return false;
    }
    std::fprintf(stderr,
        "[ssdp-listener] listening on udp/%s:%u for Bambu NOTIFYs\n",
        m_cfg.bind_address.c_str(), m_cfg.port);
    m_thread = std::thread(&SsdpListener::recv_loop, this);
    return true;
}

void SsdpListener::stop() {
    if (!m_running.exchange(false)) return;
    if (m_fd >= 0) { ::shutdown(m_fd, SHUT_RDWR); ::close(m_fd); m_fd = -1; }
    if (m_thread.joinable()) m_thread.join();
}

void SsdpListener::recv_loop() {
    std::array<char, 2048> buf{};
    while (m_running.load()) {
        // 200ms select so we observe the stop flag promptly without
        // requiring the socket to be shutdown from another thread.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_fd, &rfds);
        timeval tv{0, 200 * 1000};
        int r = ::select(m_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        if (!FD_ISSET(m_fd, &rfds)) continue;

        sockaddr_in src{};
        socklen_t sl = sizeof(src);
        ssize_t n = ::recvfrom(m_fd, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&src), &sl);
        if (n <= 0) continue;

        const std::string payload(buf.data(), buf.data() + n);

        // Filter: only Bambu NOTIFYs. Real firmwares emit NT containing
        // "bambulab-com"; anything else is noise (UPnP routers, smart
        // home devices, our own M-SEARCH echoes, etc.).
        const std::string nt = header_value(payload, "nt");
        if (nt.find("bambulab-com") == std::string::npos) continue;

        SsdpHeardDevice dev;
        dev.dev_id   = header_value(payload, "usn");
        dev.name     = header_value(payload, "devname.bambu.com");
        dev.model    = header_value(payload, "devmodel.bambu.com");
        dev.firmware = header_value(payload, "devversion.bambu.com");
        dev.nts      = header_value(payload, "nts");

        char ip[INET_ADDRSTRLEN] = {0};
        if (::inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip)))
            dev.lan_ip = ip;

        if (dev.dev_id.empty() || dev.lan_ip.empty()) continue;

        // Verbose-only: dump the raw NOTIFY exactly once per dev_id, so
        // it's easy to capture a real printer's emitted headers
        // (including LOCATION) without having tcpdump on hand. This
        // also helps us mimic the real device's HTTP descriptor format
        // (see MqttBroker::serve_http_descriptor).
        if (m_dumped.find(dev.dev_id) == m_dumped.end() &&
            (std::getenv("BAMBU_BRIDGE_VERBOSE") &&
             std::strcmp(std::getenv("BAMBU_BRIDGE_VERBOSE"), "0") != 0)) {
            m_dumped.insert(dev.dev_id);
            std::fprintf(stderr,
                "[ssdp-listener] raw NOTIFY from %s dev_id=%s "
                "(first-occurrence dump, %zu bytes):\n%s\n",
                dev.lan_ip.c_str(), dev.dev_id.c_str(), payload.size(),
                payload.c_str());
        }

        if (m_cb) {
            try { m_cb(dev); }
            catch (const std::exception& ex) {
                std::fprintf(stderr,
                    "[ssdp-listener] callback threw: %s\n", ex.what());
            }
        }
    }
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
