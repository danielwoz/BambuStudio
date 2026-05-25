// Bambu Bridge — SSDP responder implementation (phase 3).

#include "SsdpResponder.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace Slic3r {
namespace bridge {
namespace server {

namespace {

constexpr const char* kSsdpMulticastIPv4 = "239.255.255.250";
constexpr uint16_t    kSsdpPort          = 1900;
constexpr uint16_t    kBambuBroadcastPort = 2021;

// Bambu-flavoured headers for an M-SEARCH 200 OK reply. Mostly
// vestigial — real Bambu printers don't reply to M-SEARCH at all, so
// this only fires for generic UPnP scanners. Kept for backwards-compat.
std::string build_search_response_headers(const SsdpVirtualDevice& dev) {
    std::ostringstream os;
    os << "CACHE-CONTROL: max-age=1800\r\n"
       << "EXT:\r\n"
       << "LOCATION: " << dev.lan_ip << "\r\n"
       << "SERVER: UPnP/1.0\r\n"
       << "ST: urn:bambulab-com:device:3dprinter:1\r\n"
       << "USN: " << dev.dev_id << "\r\n"
       << "DevModel.bambu.com: "   << dev.model     << "\r\n"
       << "DevName.bambu.com: "    << dev.name      << "\r\n"
       << "DevConnect.bambu.com: lan\r\n"
       << "DevBind.bambu.com: "    << (dev.bound  ? "occupied" : "free")  << "\r\n"
       << "Devseclink.bambu.com: " << (dev.secure ? "secure"   : "free")  << "\r\n"
       << "DevVersion.bambu.com: " << dev.firmware  << "\r\n"
       << "DevCap.bambu.com: 1\r\n"
       << "Bambu-Mqtt-Port: "      << dev.http_port << "\r\n";
    return os.str();
}

// NOTIFY headers byte-for-byte matched against an A1 firmware
// 01.08.00.00 broadcast captured at runtime. Notable choices:
//   * Location: <ip>   — NOT a URL. Slicers that interpret this as a
//                        UPnP-spec URL (e.g. Orca) will HTTP-GET the
//                        value; emitting a URL causes that GET to land
//                        on our MQTT port and trigger
//                        ssl3_get_record:http request. Plain IP makes
//                        slicers skip the descriptor fetch and go
//                        straight to MQTT-over-TLS on port 8883.
//   * Server: UPnP/1.0 — Literal. The real firmware does not
//                        include a Bambu-versioned Server string.
//   * No EXT:, no ST:, no DevSecure.bambu.com:
//   * Mixed-case header names match the real wire format.
//   * Field order matches A1 exactly.
//   * NTS is included on alive; on byebye we still emit it because
//     spec-compliant SSDP clients use NTS to distinguish.
std::string build_notify_headers(const SsdpVirtualDevice& dev, bool alive) {
    std::ostringstream os;
    os << "Server: UPnP/1.0\r\n"
       << "Location: " << dev.lan_ip << "\r\n"
       << "NT: urn:bambulab-com:device:3dprinter:1\r\n"
       << "NTS: " << (alive ? "ssdp:alive" : "ssdp:byebye") << "\r\n"
       << "USN: " << dev.dev_id << "\r\n"
       << "Cache-Control: max-age=1800\r\n"
       << "DevModel.bambu.com: "   << dev.model     << "\r\n"
       << "DevName.bambu.com: "    << dev.name      << "\r\n"
       << "DevSignal.bambu.com: -60\r\n"
       << "DevConnect.bambu.com: lan\r\n"
       << "DevBind.bambu.com: "    << (dev.bound  ? "occupied" : "free")  << "\r\n"
       << "Devseclink.bambu.com: " << (dev.secure ? "secure"   : "free")  << "\r\n"
       << "DevVersion.bambu.com: " << dev.firmware  << "\r\n"
       << "DevCap.bambu.com: 1\r\n"
       << "Bambu-Mqtt-Port: "      << dev.http_port << "\r\n";
    return os.str();
}

bool is_msearch(const std::string& payload) {
    // First line should start with "M-SEARCH ".
    return payload.size() >= 9
        && std::memcmp(payload.data(), "M-SEARCH ", 9) == 0;
}

// Lower-cased, trimmed value for one header key, or empty string if absent.
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

// True iff the M-SEARCH's ST header is one we should answer. We answer
// `ssdp:all`, `upnp:rootdevice`, and any ST containing "bambulab-com".
bool st_matches(const std::string& payload) {
    std::string st = header_value(payload, "st");
    if (st.empty()) return true;          // permissive — some scanners omit ST
    if (st == "ssdp:all") return true;
    if (st == "upnp:rootdevice") return true;
    if (st.find("bambulab-com") != std::string::npos) return true;
    return false;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// Open a UDP socket bound to bind_addr:port with SO_REUSEADDR + SO_REUSEPORT
// (when available). Returns the fd or -1 on failure.
int open_udp_bound(const std::string& bind_addr, uint16_t port,
                   bool join_multicast, bool allow_broadcast)
{
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    // Allow other SSDP listeners (e.g. avahi, BambuStudio itself) to coexist.
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    if (allow_broadcast) {
        ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    }

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

    if (join_multicast) {
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = ::inet_addr(kSsdpMulticastIPv4);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        // Non-fatal if it fails (lo-only test environments etc.) — recorded
        // through the caller's error path.
        ::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        // TTL=4 is what most upnp implementations use for SSDP; setting
        // it gives us reach across one router hop without flooding.
        unsigned char ttl = 4;
        ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        // Disable loopback only for non-test code paths; we want loopback
        // *on* so the integration test can hear its own announces on lo.
    }
    return fd;
}

// Open an unbound UDP socket suitable for sending broadcasts.
int open_udp_send_broadcast() {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    return fd;
}

} // namespace

// ---------------------------------------------------------------------------
// Public format helpers
// ---------------------------------------------------------------------------

std::string SsdpResponder::format_search_response(const SsdpVirtualDevice& dev) {
    std::ostringstream os;
    os << "HTTP/1.1 200 OK\r\n"
       << build_search_response_headers(dev)
       << "\r\n";
    return os.str();
}

std::string SsdpResponder::format_notify(const SsdpVirtualDevice& dev, bool alive) {
    // Real Bambu printers emit `HOST: 239.255.255.250:1900` literally,
    // even though they broadcast the packet on UDP/2021. Mirror that
    // so slicers parsing the HOST field match the expected literal.
    std::ostringstream os;
    os << "NOTIFY * HTTP/1.1\r\n"
       << "HOST: " << kSsdpMulticastIPv4 << ':' << kSsdpPort << "\r\n"
       << build_notify_headers(dev, alive)
       << "\r\n";
    return os.str();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

SsdpResponder::SsdpResponder(SsdpResponderConfig cfg) : m_cfg(std::move(cfg)) {}

SsdpResponder::~SsdpResponder() {
    stop();
}

void SsdpResponder::add_device(SsdpVirtualDevice dev) {
    std::lock_guard<std::mutex> lock(m_devices_mutex);
    // Replace existing by dev_id, else append.
    for (auto& d : m_devices) {
        if (d.dev_id == dev.dev_id) { d = std::move(dev); return; }
    }
    m_devices.push_back(std::move(dev));
}

void SsdpResponder::remove_device(const std::string& dev_id) {
    std::lock_guard<std::mutex> lock(m_devices_mutex);
    m_devices.erase(
        std::remove_if(m_devices.begin(), m_devices.end(),
                       [&](const SsdpVirtualDevice& d) { return d.dev_id == dev_id; }),
        m_devices.end());
}

std::vector<SsdpVirtualDevice> SsdpResponder::devices() const {
    std::lock_guard<std::mutex> lock(m_devices_mutex);
    return m_devices;
}

void SsdpResponder::start() {
    if (m_running.exchange(true)) return;

    // 1900 receive socket (also used for sending unicast replies). May fail
    // on locked-down test hosts; we keep running with the announce-only path.
    m_recv_fd_1900 = open_udp_bound(m_cfg.bind_address, kSsdpPort,
                                    /*join_multicast=*/true,
                                    /*allow_broadcast=*/false);

    // Separate send socket for multicast announces — we don't bind it so
    // the kernel picks the right outbound interface per-packet.
    m_multicast_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_multicast_fd >= 0) {
        unsigned char ttl = 4;
        ::setsockopt(m_multicast_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        unsigned char loop = 1;  // hear-yourself for loopback tests
        ::setsockopt(m_multicast_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    }

    if (m_cfg.enable_bambu_broadcast) {
        m_bambu_send_fd = open_udp_send_broadcast();
    }

    // Recv loop only if we managed to bind 1900.
    if (m_recv_fd_1900 >= 0) {
        m_recv_thread = std::thread([this] { recv_loop(); });
    }

    // Announce loop — emits an initial ssdp:alive immediately, then on
    // interval until stop().
    m_announce_thread = std::thread([this] { announce_loop(); });
}

void SsdpResponder::stop() {
    if (!m_running.exchange(false)) return;

    // Emit one final ssdp:byebye per device on a best-effort basis. This
    // has to happen *before* we close the send sockets.
    emit_notify(/*alive=*/false);

    // Closing the recv socket interrupts the blocked select() in recv_loop.
    close_fd(m_recv_fd_1900);

    if (m_recv_thread.joinable())     m_recv_thread.join();
    if (m_announce_thread.joinable()) m_announce_thread.join();

    close_fd(m_multicast_fd);
    close_fd(m_bambu_send_fd);
}

// ---------------------------------------------------------------------------
// Receive / respond
// ---------------------------------------------------------------------------

void SsdpResponder::recv_loop() {
    std::array<char, 4096> buf{};
    while (m_running.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_recv_fd_1900, &rfds);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 250 * 1000;        // 250 ms — bounded shutdown latency
        int rc = ::select(m_recv_fd_1900 + 1, &rfds, nullptr, nullptr, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;
        if (!FD_ISSET(m_recv_fd_1900, &rfds)) continue;

        sockaddr_in sender{};
        socklen_t   slen = sizeof(sender);
        ssize_t n = ::recvfrom(m_recv_fd_1900, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&sender), &slen);
        if (n <= 0) continue;
        std::string payload(buf.data(), buf.data() + n);
        if (!is_msearch(payload))   continue;
        if (!st_matches(payload))   continue;
        handle_search(payload, sender, m_recv_fd_1900);
    }
}

void SsdpResponder::handle_search(const std::string& /*payload*/,
                                  const ::sockaddr_in& sender,
                                  int reply_fd)
{
    // One unicast 200-OK reply per virtual device.
    auto devs = devices();
    for (const auto& dev : devs) {
        const std::string body = format_search_response(dev);
        ::sendto(reply_fd, body.data(), body.size(), 0,
                 reinterpret_cast<const sockaddr*>(&sender), sizeof(sender));
    }
}

// ---------------------------------------------------------------------------
// Announce
// ---------------------------------------------------------------------------

void SsdpResponder::announce_loop() {
    // Initial alive immediately on start (real printers do the same — first
    // NOTIFY hits the wire within a second of boot).
    if (m_cfg.enable_multicast_send) emit_notify(/*alive=*/true);

    const auto period = m_cfg.notify_interval;
    auto next = std::chrono::steady_clock::now() + period;
    while (m_running.load()) {
        // Sleep in small slices so stop() doesn't have to wait `period`.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!m_running.load()) break;
        if (!m_cfg.enable_multicast_send) continue;
        if (std::chrono::steady_clock::now() < next) continue;
        emit_notify(/*alive=*/true);
        next = std::chrono::steady_clock::now() + period;
    }
}

void SsdpResponder::emit_notify(bool alive) {
    // Snapshot under lock then send unlocked.
    std::vector<SsdpVirtualDevice> devs;
    {
        std::lock_guard<std::mutex> lock(m_devices_mutex);
        devs = m_devices;
    }

    // Multicast destination 239.255.255.250:1900.
    sockaddr_in mcast{};
    mcast.sin_family = AF_INET;
    mcast.sin_port   = htons(kSsdpPort);
    mcast.sin_addr.s_addr = ::inet_addr(kSsdpMulticastIPv4);

    // Bambu broadcast destination 255.255.255.255:2021.
    sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port   = htons(kBambuBroadcastPort);
    bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    for (const auto& dev : devs) {
        const std::string body = format_notify(dev, alive);
        // One-shot dump of the actual NOTIFY we'll send, so it's easy to
        // diff against a captured real-printer NOTIFY when fingerprinting
        // discrepancies. Gated on BAMBU_BRIDGE_VERBOSE so it doesn't
        // spam every 30 s.
        {
            static std::mutex sm;
            static std::set<std::string> dumped;
            const bool verbose = []() {
                const char* e = std::getenv("BAMBU_BRIDGE_VERBOSE");
                return e && *e && std::strcmp(e, "0") != 0;
            }();
            if (verbose) {
                std::lock_guard<std::mutex> lk(sm);
                if (dumped.insert(dev.dev_id).second) {
                }
            }
        }
        if (m_multicast_fd >= 0) {
            ::sendto(m_multicast_fd, body.data(), body.size(), 0,
                     reinterpret_cast<const sockaddr*>(&mcast), sizeof(mcast));
        }
        if (m_bambu_send_fd >= 0) {
            ::sendto(m_bambu_send_fd, body.data(), body.size(), 0,
                     reinterpret_cast<const sockaddr*>(&bcast), sizeof(bcast));
        }
    }
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
