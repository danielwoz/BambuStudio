// Bambu Bridge — SSDP listener.
//
// Listens for Bambu broadcast NOTIFY packets on UDP/2021 and reports the
// printer's serial + LAN IP back via a callback. Real Bambu printers
// announce themselves on the LAN periodically (the firmware's own SSDP
// emitter; see `~/BambuStudio/src/bambu_net_oss/core/SsdpListener.cpp`
// for the parser the firmware speaks against). The bridge uses this to
// discover the LAN IP of cloud-bound printers — Bambu's cloud REST
// endpoint (get_user_print_info) does not include `dev_ip` for devices
// that haven't recently phoned home from inside the user's LAN, so
// without this listener we'd never know where to point LanUplink /
// MQTT-over-TLS at.
//
// Implementation: built on boost::asio. One io_context, one worker
// thread, async_receive_from chained from itself. stop() closes the
// socket which triggers operation_aborted in the handler.
//
// Coexistence: SO_REUSEADDR + SO_REUSEPORT are both set so the listener
// can run alongside another SSDP consumer on the same host (most notably
// BambuStudio's own bambu_net_oss listener if we ever embed the bridge
// in the GUI). Each consumer receives its own copy of every broadcast.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_LISTENER_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_LISTENER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>

namespace Slic3r {
namespace bridge {
namespace server {

// Snapshot of a single Bambu NOTIFY we heard on the LAN.
struct SsdpHeardDevice {
    std::string dev_id;     // USN
    std::string lan_ip;     // sender IP of the UDP datagram
    std::string name;       // DevName.bambu.com
    std::string model;      // DevModel.bambu.com
    std::string firmware;   // DevVersion.bambu.com
    std::string nts;        // ssdp:alive | ssdp:byebye
};

class SsdpListener {
public:
    using HeardCallback = std::function<void(const SsdpHeardDevice&)>;

    struct Config {
        // Bind address for the recv socket. Default 0.0.0.0 so we hear
        // broadcasts on every interface. Tests use 127.0.0.1.
        std::string bind_address = "0.0.0.0";
        // Bambu broadcast port. 2021 is what real printers actually use.
        uint16_t    port         = 2021;
    };

    SsdpListener(Config cfg, HeardCallback cb);
    ~SsdpListener();

    SsdpListener(const SsdpListener&)            = delete;
    SsdpListener& operator=(const SsdpListener&) = delete;

    bool start();
    void stop();

    bool running() const noexcept { return m_running.load(); }

private:
    void start_async_receive();
    void on_receive(const boost::system::error_code& ec, std::size_t bytes);

    Config            m_cfg;
    HeardCallback     m_cb;
    std::atomic<bool> m_running{false};

    std::unique_ptr<boost::asio::io_context>           m_io;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>       m_work;
    std::unique_ptr<boost::asio::ip::udp::socket>      m_socket;

    std::vector<char>                                  m_buf;
    boost::asio::ip::udp::endpoint                     m_sender;

    std::thread       m_thread;
    // dev_ids whose raw NOTIFY payload we've already verbose-dumped.
    // Single-threaded reads/writes (only the io thread touches it).
    std::unordered_set<std::string> m_dumped;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_LISTENER_HPP
