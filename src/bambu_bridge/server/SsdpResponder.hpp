// Bambu Bridge — SSDP responder (phase 3).
//
// One SsdpResponder runs per BridgeService and answers discovery on behalf
// of every mirrored cloud printer the bridge owns. The goal is *byte-for-
// byte indistinguishability*: a downstream BambuStudio / Orca listener on
// the LAN must see our virtual device the same way it sees a real printer.
//
// What real Bambu printers actually emit (verified against
// `~/BambuStudio/src/bambu_net_oss/core/SsdpListener.cpp`, May 2026):
//
//   - They DO NOT respond to standard SSDP M-SEARCH on udp/1900. Their
//     discovery is a broadcast NOTIFY on **udp/2021** to 255.255.255.255
//     emitted periodically.
//   - The headers the host parser actually keys off (case-folded for
//     map lookup) are:
//       USN                       → raw serial / dev_id
//       NT                        → must contain "bambulab-com"
//       NTS                       → ssdp:alive | ssdp:byebye
//       DevName.bambu.com         → friendly name
//       DevModel.bambu.com        → model code (H2S, A1M, …)
//       DevConnect.bambu.com      → "lan" (or "farm" → folded to "lan" upstream)
//       DevBind.bambu.com         → "free" | "occupied"
//       Devseclink.bambu.com      → "secure" | "free"   (note lower-case 'sec')
//       DevVersion.bambu.com      → firmware version string
//       DevSignal.bambu.com       → optional WiFi signal string
//       DevCap.bambu.com          → optional capability int
//
//   The spec from the implementation prompt used `DevSecure.bambu.com: 1/0`,
//   which the upstream listener silently ignores — we emit
//   `Devseclink.bambu.com: secure|free` instead so it round-trips through
//   the real parser. The literal-spec header is *also* emitted for
//   forward-compat with any future hard match.
//
// Two send paths are exposed, both controlled by SsdpResponderConfig:
//
//   1. The classic 1900-multicast SSDP path (M-SEARCH + periodic NOTIFY)
//      — useful for standard UPnP scanners and unit tests, and harmless
//      against real BambuStudio (which ignores 1900 NOTIFYs it can't
//      attribute to a bambulab-com NT).
//   2. The Bambu broadcast path (NOTIFY to 255.255.255.255:2021) — this
//      is what makes downstream BambuStudio listeners actually pick up
//      our virtual device.
//
// Implementation: built on boost::asio (matches the rest of the slicer's
// networking stack — see `slic3r/Utils/Bonjour.cpp` for the same UDP
// idiom). One io_context, one worker thread, one steady_timer for the
// periodic announce. asio gives us portable POSIX/Winsock sockets — the
// previous raw-fd implementation was Linux-only.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_RESPONDER_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_RESPONDER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

namespace Slic3r {
namespace bridge {
namespace server {

// One virtual device the responder advertises. All fields except the trailing
// bool/uint16_t flags are required for the emitted NOTIFY / 200 OK to be
// parseable by the upstream listener.
struct SsdpVirtualDevice {
    std::string dev_id;          // serial = USN value (raw, no uuid: prefix)
    std::string name;            // DevName.bambu.com
    std::string model;           // DevModel.bambu.com
    std::string firmware;        // DevVersion.bambu.com
    std::string lan_ip;          // bridge host IP this virtual device owns
    uint16_t    http_port = 80;  // LOCATION header port
    bool        bound     = true; // DevBind.bambu.com: bound=occupied, !bound=free
    bool        secure    = true; // Devseclink.bambu.com: secure=secure, !secure=free
};

struct SsdpResponderConfig {
    // Cadence for periodic NOTIFY (ssdp:alive) on both 1900 multicast and
    // 2021 broadcast.
    std::chrono::seconds notify_interval{30};

    // Optional outbound interface for multicast send (empty = system default).
    // For phase 3 this is informational; the socket binds INADDR_ANY.
    std::string multicast_iface;

    // If false, we still answer M-SEARCH but skip the periodic announce.
    // Tests use this to keep the recv loop deterministic.
    bool enable_multicast_send = true;

    // Bind addr for the 1900 receive socket. Default "0.0.0.0" matches
    // real-printer behaviour; tests use "127.0.0.1" to stay on lo.
    std::string bind_address = "0.0.0.0";

    // If true, also broadcast on UDP/2021 (real Bambu printers do this —
    // see SsdpListener.cpp lines 23-28). Off by default in tests so the
    // recv loop assertions don't race with broadcasts.
    bool enable_bambu_broadcast = true;
};

class SsdpResponder {
public:
    explicit SsdpResponder(SsdpResponderConfig cfg);
    ~SsdpResponder();

    SsdpResponder(const SsdpResponder&) = delete;
    SsdpResponder& operator=(const SsdpResponder&) = delete;

    void add_device   (SsdpVirtualDevice dev);
    void remove_device(const std::string& dev_id);
    std::vector<SsdpVirtualDevice> devices() const;

    void start();   // spawns recv + announce threads; non-blocking
    void stop();    // sends byebye for each device, joins threads

    // True iff start() succeeded and the responder is currently running.
    bool running() const noexcept { return m_running.load(); }

    // ---- Static format helpers (testable without sockets) ----

    // Build the unicast HTTP/1.1 200 OK reply to an M-SEARCH for this device.
    static std::string format_search_response(const SsdpVirtualDevice& dev);

    // Build a multicast NOTIFY (ssdp:alive | ssdp:byebye) for this device.
    static std::string format_notify(const SsdpVirtualDevice& dev,
                                     bool alive /* true=alive, false=byebye */);

private:
    void start_async_receive();
    void on_receive(const boost::system::error_code& ec, std::size_t bytes);
    void handle_search(const std::string& payload,
                       const boost::asio::ip::udp::endpoint& sender);
    void schedule_announce();
    void emit_notify(bool alive);   // multicast + bambu broadcast for every device

    SsdpResponderConfig            m_cfg;
    std::atomic<bool>              m_running{false};

    // asio plumbing — owned for the lifetime of the responder. work_guard
    // keeps io_context::run() pinned until stop() resets it.
    std::unique_ptr<boost::asio::io_context> m_io;
    std::unique_ptr<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>> m_work;

    std::unique_ptr<boost::asio::ip::udp::socket> m_recv_socket;   // bound to *:1900
    std::unique_ptr<boost::asio::ip::udp::socket> m_multicast_socket; // unbound, for 239.255.255.250:1900
    std::unique_ptr<boost::asio::ip::udp::socket> m_bambu_socket;  // unbound, for 255.255.255.255:2021
    std::unique_ptr<boost::asio::steady_timer>    m_announce_timer;

    // Receive buffer + sender endpoint for the current async_receive_from.
    std::vector<char>                     m_recv_buf;
    boost::asio::ip::udp::endpoint        m_recv_sender;

    std::thread                    m_io_thread;

    mutable std::mutex             m_devices_mutex;
    std::vector<SsdpVirtualDevice> m_devices;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_SSDP_RESPONDER_HPP
