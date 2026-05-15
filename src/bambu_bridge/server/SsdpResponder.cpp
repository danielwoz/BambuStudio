// Bambu Bridge — SSDP responder implementation (phase 3).
//
// Built on boost::asio for portability — matches the rest of the slicer's
// networking style (see slic3r/Utils/Bonjour.cpp for the same UDP
// async-receive idiom). Threading: one io_context driven by a single
// worker thread. The periodic announce uses a steady_timer chained from
// itself; M-SEARCH replies happen on the same thread via the recv socket.

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

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

namespace Slic3r {
namespace bridge {
namespace server {

namespace asio = boost::asio;
using asio::ip::udp;
using boost::system::error_code;

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
       << "DevCap.bambu.com: 1\r\n";
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
       << "DevCap.bambu.com: 1\r\n";
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

    m_io = std::make_unique<asio::io_context>();
    m_work = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        m_io->get_executor());

    // 1900 receive socket (also used for sending unicast replies). May fail
    // on locked-down test hosts; we keep running with the announce-only path.
    {
        error_code ec;
        auto sock = std::make_unique<udp::socket>(*m_io);
        sock->open(udp::v4(), ec);
        if (!ec) {
            sock->set_option(asio::socket_base::reuse_address(true), ec);
#ifdef SO_REUSEPORT
            // asio doesn't expose SO_REUSEPORT directly; set it manually
            // so other SSDP listeners on the same host can coexist.
            int one = 1;
            ::setsockopt(sock->native_handle(), SOL_SOCKET, SO_REUSEPORT,
                         &one, sizeof(one));
#endif
        }
        asio::ip::address bind_addr;
        if (m_cfg.bind_address.empty() || m_cfg.bind_address == "0.0.0.0") {
            bind_addr = asio::ip::address_v4::any();
        } else {
            bind_addr = asio::ip::make_address(m_cfg.bind_address, ec);
        }
        if (!ec) {
            sock->bind(udp::endpoint(bind_addr, kSsdpPort), ec);
        }
        if (!ec) {
            // Join the 239.255.255.250 multicast group; non-fatal if it
            // fails (lo-only test environments).
            error_code ignore;
            sock->set_option(
                asio::ip::multicast::join_group(
                    asio::ip::make_address_v4(kSsdpMulticastIPv4)),
                ignore);
            sock->set_option(asio::ip::multicast::hops(4), ignore);
            m_recv_socket = std::move(sock);
        }
        // If bind failed, m_recv_socket stays null and we operate in
        // announce-only mode (same as old code).
    }

    // Separate send socket for multicast announces — we don't bind it so
    // the kernel picks the right outbound interface per-packet.
    {
        error_code ec;
        auto sock = std::make_unique<udp::socket>(*m_io);
        sock->open(udp::v4(), ec);
        if (!ec) {
            error_code ignore;
            sock->set_option(asio::ip::multicast::hops(4), ignore);
            // hear-yourself loopback so the integration test can observe
            // its own announces on lo.
            sock->set_option(asio::ip::multicast::enable_loopback(true), ignore);
            m_multicast_socket = std::move(sock);
        }
    }

    if (m_cfg.enable_bambu_broadcast) {
        error_code ec;
        auto sock = std::make_unique<udp::socket>(*m_io);
        sock->open(udp::v4(), ec);
        if (!ec) {
            error_code ignore;
            sock->set_option(asio::socket_base::broadcast(true), ignore);
            m_bambu_socket = std::move(sock);
        }
    }

    if (m_recv_socket) {
        m_recv_buf.resize(4096);
        start_async_receive();
    }

    // Announce timer — emits an initial ssdp:alive immediately, then on
    // interval until stop().
    m_announce_timer = std::make_unique<asio::steady_timer>(*m_io);
    if (m_cfg.enable_multicast_send) {
        asio::post(*m_io, [this] { emit_notify(/*alive=*/true); });
    }
    schedule_announce();

    // Drive the io_context on a dedicated thread.
    m_io_thread = std::thread([this] {
        try { m_io->run(); }
        catch (const std::exception& ex) {
            std::fprintf(stderr, "[ssdp-responder] io thread exception: %s\n",
                         ex.what());
        }
    });
}

void SsdpResponder::stop() {
    if (!m_running.exchange(false)) return;

    // Emit one final ssdp:byebye per device on a best-effort basis. Done
    // synchronously on the calling thread before tearing down sockets.
    emit_notify(/*alive=*/false);

    if (m_io) {
        // Cancel outstanding async ops and stop the io_context so the
        // worker thread can join.
        asio::post(*m_io, [this] {
            error_code ignore;
            if (m_recv_socket)      m_recv_socket->close(ignore);
            if (m_announce_timer)   m_announce_timer->cancel(ignore);
        });
        if (m_work) m_work.reset();
        m_io->stop();
    }

    if (m_io_thread.joinable()) m_io_thread.join();

    // Now safe to destroy asio objects.
    m_recv_socket.reset();
    m_multicast_socket.reset();
    m_bambu_socket.reset();
    m_announce_timer.reset();
    m_io.reset();
}

// ---------------------------------------------------------------------------
// Receive / respond (asio async chain)
// ---------------------------------------------------------------------------

void SsdpResponder::start_async_receive() {
    if (!m_recv_socket) return;
    m_recv_socket->async_receive_from(
        asio::buffer(m_recv_buf), m_recv_sender,
        [this](const error_code& ec, std::size_t bytes) {
            on_receive(ec, bytes);
        });
}

void SsdpResponder::on_receive(const error_code& ec, std::size_t bytes) {
    if (!m_running.load()) return;
    if (ec) {
        // socket closed by stop(): bail out without rescheduling.
        if (ec == asio::error::operation_aborted) return;
        // Transient — try again.
        start_async_receive();
        return;
    }
    if (bytes > 0) {
        std::string payload(m_recv_buf.data(), m_recv_buf.data() + bytes);
        if (is_msearch(payload) && st_matches(payload)) {
            handle_search(payload, m_recv_sender);
        }
    }
    start_async_receive();
}

void SsdpResponder::handle_search(const std::string& /*payload*/,
                                  const udp::endpoint& sender)
{
    if (!m_recv_socket) return;
    // One unicast 200-OK reply per virtual device.
    auto devs = devices();
    for (const auto& dev : devs) {
        const std::string body = format_search_response(dev);
        error_code ignore;
        m_recv_socket->send_to(asio::buffer(body), sender, 0, ignore);
    }
}

// ---------------------------------------------------------------------------
// Announce
// ---------------------------------------------------------------------------

void SsdpResponder::schedule_announce() {
    if (!m_announce_timer) return;
    m_announce_timer->expires_after(m_cfg.notify_interval);
    m_announce_timer->async_wait([this](const error_code& ec) {
        if (ec) return;
        if (!m_running.load()) return;
        if (m_cfg.enable_multicast_send) emit_notify(/*alive=*/true);
        schedule_announce();
    });
}

void SsdpResponder::emit_notify(bool alive) {
    // Snapshot under lock then send unlocked.
    std::vector<SsdpVirtualDevice> devs;
    {
        std::lock_guard<std::mutex> lock(m_devices_mutex);
        devs = m_devices;
    }

    udp::endpoint mcast(asio::ip::make_address_v4(kSsdpMulticastIPv4), kSsdpPort);
    udp::endpoint bcast(asio::ip::address_v4::broadcast(), kBambuBroadcastPort);

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
                    std::fprintf(stderr,
                        "[ssdp-responder] outbound NOTIFY for dev_id=%s "
                        "(first-occurrence dump, %zu bytes):\n%s",
                        dev.dev_id.c_str(), body.size(), body.c_str());
                }
            }
        }
        error_code ignore;
        if (m_multicast_socket && m_multicast_socket->is_open()) {
            m_multicast_socket->send_to(asio::buffer(body), mcast, 0, ignore);
        }
        if (m_bambu_socket && m_bambu_socket->is_open()) {
            m_bambu_socket->send_to(asio::buffer(body), bcast, 0, ignore);
        }
    }
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
