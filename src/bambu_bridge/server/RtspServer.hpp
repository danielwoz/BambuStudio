// Bambu Bridge — per-device RTSPS server for slicer camera re-serve (phase 8).
//
// One RtspServer instance can host many virtual devices, each bound to its
// own (ip, port) endpoint (production: port 322, the Bambu LAN camera
// port). For each device:
//
//   - Implicit TLS on the chosen port (TLS 1.2 only, same self-signed
//     CertMaterial the MqttBroker / FtpsServer use, so the on-the-wire
//     shape is indistinguishable from a real printer's camera channel).
//   - RTSP method subset: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN.
//   - DESCRIBE returns an SDP that advertises ONE video track:
//       m=video 0 RTP/AVP 96
//       a=rtpmap:96 H264/90000
//       a=fmtp:96 packetization-mode=1; profile-level-id=...;
//                 sprop-parameter-sets=<b64(sps)>,<b64(pps)>
//       a=control:streamid=0
//     The SPS / PPS come from the bound ICameraSource's `StreamInfo`.
//   - SETUP only honours `Transport: RTP/AVP/TCP;interleaved=0-1`
//     (RTP+RTCP interleaved over the same TLS RTSP socket). We do NOT
//     implement UDP RTP transport or RTSP-over-UDP — keeping the data
//     on the TLS RTSP channel sidesteps NAT/firewall and lets one socket
//     carry the entire session.
//   - PLAY spawns ONE worker thread per session. The worker drains the
//     bound ICameraSource via `next_frame`, splits Annex-B NAL units
//     out of `nal_data`, RFC-6184-packetises (single-NAL when small,
//     FU-A fragmentation otherwise), and writes each RTP packet as an
//     interleaved frame `$<channel-id><length><payload>` into the TLS
//     stream.
//   - TEARDOWN stops the worker thread + closes the source for that
//     session.
//
// One ICameraSource per device — sessions share it via the bound device
// state. (Phase 8 doesn't multiplex multiple slicers off a single source;
// the bridge's expected concurrency is 1 slicer per printer at a time, and
// camera sources are stateful. Future phases can introduce a per-device
// frame fan-out if needed.)
//
// Linux-only for phase 8 (matches FtpsServer's posture).

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_RTSP_SERVER_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_RTSP_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../tls/CertFactory.hpp"

namespace Slic3r {
namespace bridge {
namespace server {

class ICameraSource;

struct RtspVirtualDevice {
    std::string                   dev_id;
    std::string                   lan_ip = "0.0.0.0";
    uint16_t                      port   = 322;   // real printers use 322
    std::string                   access_code;
    tls::CertMaterial             cert;
    std::shared_ptr<ICameraSource> source;
};

struct RtspServerConfig {
    // Max concurrent slicer sessions per device. Real printers permit
    // exactly one camera client at a time; we mirror that by default but
    // expose a knob for tests / future fan-out.
    int     max_sessions_per_device = 1;

    // listen(2) backlog.
    int     accept_backlog          = 4;

    // Idle timeout on the RTSP control channel (seconds). Slicers send
    // GET_PARAMETER / OPTIONS keepalives every ~30s.
    int     io_timeout_seconds      = 90;

    // RTP packetisation cap. Real cameras tend to fragment around 1400
    // bytes to fit Ethernet MTU; we mimic that. Tests can lower this to
    // force FU-A coverage on small fixtures.
    int     rtp_max_payload         = 1400;

    // Auth posture. Real printers require Basic auth (user `bblp`, pw =
    // access code) on DESCRIBE. Set to false in tests when not exercising
    // auth. Default is false in phase 8 because the test client doesn't
    // synthesise the auth header — phase 11's wire-diff validation flips
    // this on alongside the real-printer comparison.
    bool    require_auth            = false;
};

class RtspServer {
public:
    explicit RtspServer(RtspServerConfig cfg);
    ~RtspServer();

    RtspServer(const RtspServer&)            = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    // Devices can be added before start() (they'll come up with start())
    // or after (brought up immediately). remove_device tears down the
    // listener + any active sessions.
    void add_device   (RtspVirtualDevice dev);
    void remove_device(const std::string& dev_id);

    void start();
    void stop();

    bool running() const noexcept { return m_running.load(); }

    // Reports the actually-bound port for a device. Returns 0 if the
    // device isn't bound. Useful when callers pass port=0 (tests).
    uint16_t bound_port(const std::string& dev_id) const;

    // Internal per-device state; definition lives in RtspServer.cpp.
    struct Device;

private:
    Device* find_locked(const std::string& dev_id);
    void start_device(Device& d);
    void stop_device (Device& d);

    RtspServerConfig                                       m_cfg;
    std::atomic<bool>                                      m_running{false};

    mutable std::mutex                                     m_devices_mu;
    std::unordered_map<std::string,
                       std::unique_ptr<Device>>            m_devices;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_RTSP_SERVER_HPP
