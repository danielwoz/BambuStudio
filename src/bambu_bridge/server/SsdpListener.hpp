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
// Threading: one POSIX `select`+`recvfrom` loop on a dedicated thread.
// stop() flips an atomic, closes the socket, and joins.
//
// Coexistence: SO_REUSEADDR + SO_REUSEPORT are both set so the listener
// can run alongside another SSDP consumer on the same host (most notably
// BambuStudio's own bambu_net_oss listener if we ever embed the bridge
// in the GUI). Each consumer receives its own copy of every broadcast.
//
// Linux-only for now — matches SsdpResponder's scope.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_LISTENER_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_LISTENER_HPP

// Windows port: socket types/calls route through platform/WinsockShim.hpp.
#include "../platform/WinsockShim.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>

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
    void recv_loop();

    Config            m_cfg;
    HeardCallback     m_cb;
    std::atomic<bool> m_running{false};
    int               m_fd = -1;
    std::thread       m_thread;
    // dev_ids whose raw NOTIFY payload we've already verbose-dumped.
    // Single-threaded reads/writes (only the recv thread touches it).
    std::unordered_set<std::string> m_dumped;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_LISTENER_HPP
