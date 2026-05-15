// Bambu Bridge — SSDP listener implementation.
//
// boost::asio async UDP receive. One io_context driven by one worker
// thread; the receive handler reschedules itself until stop() closes the
// socket (operation_aborted → no rearm).

#include "SsdpListener.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#ifndef _WIN32
#  include <sys/socket.h>     // for SO_REUSEPORT
#endif

namespace Slic3r {
namespace bridge {
namespace server {

namespace asio = boost::asio;
using asio::ip::udp;
using boost::system::error_code;

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

} // namespace

SsdpListener::SsdpListener(Config cfg, HeardCallback cb)
    : m_cfg(std::move(cfg)), m_cb(std::move(cb)) {}

SsdpListener::~SsdpListener() { stop(); }

bool SsdpListener::start() {
    if (m_running.exchange(true)) return true;

    m_io = std::make_unique<asio::io_context>();
    m_work = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        m_io->get_executor());

    m_socket = std::make_unique<udp::socket>(*m_io);
    error_code ec;
    m_socket->open(udp::v4(), ec);
    if (ec) {
        std::fprintf(stderr, "[ssdp-listener] open failed: %s\n", ec.message().c_str());
        m_running.store(false);
        m_socket.reset();
        m_work.reset();
        m_io.reset();
        return false;
    }
    m_socket->set_option(asio::socket_base::reuse_address(true), ec);
    m_socket->set_option(asio::socket_base::broadcast(true), ec);
#ifdef SO_REUSEPORT
    // asio doesn't expose SO_REUSEPORT directly; set it manually so other
    // SSDP listeners on the same host can coexist.
    int one = 1;
    ::setsockopt(m_socket->native_handle(), SOL_SOCKET, SO_REUSEPORT,
                 &one, sizeof(one));
#endif

    asio::ip::address bind_addr;
    if (m_cfg.bind_address.empty() || m_cfg.bind_address == "0.0.0.0") {
        bind_addr = asio::ip::address_v4::any();
    } else {
        bind_addr = asio::ip::make_address(m_cfg.bind_address, ec);
        if (ec) {
            std::fprintf(stderr,
                "[ssdp-listener] bad bind addr %s: %s\n",
                m_cfg.bind_address.c_str(), ec.message().c_str());
            m_running.store(false);
            m_socket.reset();
            m_work.reset();
            m_io.reset();
            return false;
        }
    }
    m_socket->bind(udp::endpoint(bind_addr, m_cfg.port), ec);
    if (ec) {
        std::fprintf(stderr,
            "[ssdp-listener] bind %s:%u failed: %s\n",
            m_cfg.bind_address.c_str(), m_cfg.port, ec.message().c_str());
        m_running.store(false);
        m_socket.reset();
        m_work.reset();
        m_io.reset();
        return false;
    }
    std::fprintf(stderr,
        "[ssdp-listener] listening on udp/%s:%u for Bambu NOTIFYs\n",
        m_cfg.bind_address.c_str(), m_cfg.port);

    m_buf.resize(2048);
    start_async_receive();

    m_thread = std::thread([this] {
        try { m_io->run(); }
        catch (const std::exception& ex) {
            std::fprintf(stderr, "[ssdp-listener] io thread exception: %s\n",
                         ex.what());
        }
    });
    return true;
}

void SsdpListener::stop() {
    if (!m_running.exchange(false)) return;
    if (m_io) {
        asio::post(*m_io, [this] {
            error_code ignore;
            if (m_socket) m_socket->close(ignore);
        });
        if (m_work) m_work.reset();
        m_io->stop();
    }
    if (m_thread.joinable()) m_thread.join();
    m_socket.reset();
    m_work.reset();
    m_io.reset();
}

void SsdpListener::start_async_receive() {
    if (!m_socket) return;
    m_socket->async_receive_from(
        asio::buffer(m_buf), m_sender,
        [this](const error_code& ec, std::size_t bytes) {
            on_receive(ec, bytes);
        });
}

void SsdpListener::on_receive(const error_code& ec, std::size_t bytes) {
    if (!m_running.load()) return;
    if (ec) {
        if (ec == asio::error::operation_aborted) return;
        start_async_receive();
        return;
    }
    if (bytes == 0) {
        start_async_receive();
        return;
    }

    const std::string payload(m_buf.data(), m_buf.data() + bytes);

    // Filter: only Bambu NOTIFYs. Real firmwares emit NT containing
    // "bambulab-com"; anything else is noise (UPnP routers, smart
    // home devices, our own M-SEARCH echoes, etc.).
    const std::string nt = header_value(payload, "nt");
    if (nt.find("bambulab-com") == std::string::npos) {
        start_async_receive();
        return;
    }

    SsdpHeardDevice dev;
    dev.dev_id   = header_value(payload, "usn");
    dev.name     = header_value(payload, "devname.bambu.com");
    dev.model    = header_value(payload, "devmodel.bambu.com");
    dev.firmware = header_value(payload, "devversion.bambu.com");
    dev.nts      = header_value(payload, "nts");
    dev.lan_ip   = m_sender.address().to_string();

    if (dev.dev_id.empty() || dev.lan_ip.empty()) {
        start_async_receive();
        return;
    }

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

    start_async_receive();
}

} // namespace server
} // namespace bridge
} // namespace Slic3r
