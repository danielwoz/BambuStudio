// Bambu Bridge — camera source router (phase 9).
//
// `CameraSourceRouter` is the ICameraSource the RtspServer's
// `RtspVirtualDevice::source` is bound to. It owns up to three sub-sources
// (LAN, Cloud, Null) and, on `open()`, picks ONE — sticky for the
// lifetime of that open()/close() pair.
//
// Why sticky:
//   Mid-stream failover would require the H.264 decoder to resync against
//   a new SPS/PPS pair, drop intermediate frames, and possibly re-encode
//   the slicer's view. That's a worse user experience than just tearing
//   down the RTSP session and letting the slicer reconnect (which will
//   re-open this router → re-pick → re-PLAY against whichever source is
//   currently healthy). So if the chosen source's `next_frame` returns
//   nullopt for "stream died", we propagate the nullopt and let the
//   RtspServer TEARDOWN.
//
// Picking happens in `open()`:
//   * If LAN healthy and `policy.prefer_lan`, try LAN's open(). If it
//     succeeds, we're locked to LAN.
//   * Else (or on LAN open() failure) try Cloud's open(). Lock to Cloud.
//   * Else (or on Cloud open() failure) try Null if `policy.allow_null_
//     fallback`. Useful for tests and demos.
//   * Otherwise open() returns false and is_open() stays false.
//
// Fallback only fires on `open()` failure — once a stream is up, a
// subsequent `next_frame` returning nullopt is propagated.
//
// Threading: RtspServer ties one source to one session and only one
// thread calls `next_frame()` at a time. `open()` / `close()` come from
// that thread too. The router takes a short mutex around the chosen
// pointer and the open flag.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_CAMERA_SOURCE_ROUTER_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_CAMERA_SOURCE_ROUTER_HPP

#include "../server/ICameraSource.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace Slic3r {
namespace bridge {
namespace router {

class LanCameraSource;
class CloudCameraSource;
class NullCameraSource;
class UplinkHealthMonitor;

class CameraSourceRouter final : public server::ICameraSource {
public:
    enum class Choice { None, Lan, Cloud, Null };

    struct Policy {
        bool prefer_lan          = true;
        bool allow_null_fallback = false;
    };

    explicit CameraSourceRouter(std::string dev_id);
    ~CameraSourceRouter() override;

    CameraSourceRouter(const CameraSourceRouter&)            = delete;
    CameraSourceRouter& operator=(const CameraSourceRouter&) = delete;

    void set_lan_source   (std::shared_ptr<LanCameraSource>     lan);
    void set_cloud_source (std::shared_ptr<CloudCameraSource>   cloud);
    void set_null_source  (std::shared_ptr<NullCameraSource>    null);
    void set_health_monitor(std::shared_ptr<UplinkHealthMonitor> monitor);
    void set_policy(Policy p);

    // ---- ICameraSource ------------------------------------------------
    bool open() override;
    void close() override;
    bool is_open() const override;

    std::optional<server::VideoFrame> next_frame(int timeout_ms) override;
    server::ICameraSource::StreamInfo info() const override;

    // Inspection: which source did open() pick? `None` before open() /
    // after close() / on open()-failure-with-no-fallback. Useful for tests
    // and `bridge-cli proxy` status logging.
    Choice current_choice() const;

private:
    // Returns the chosen source's shared_ptr (or nullptr if c == None).
    std::shared_ptr<server::ICameraSource> pick_locked(Choice c) const;

    std::string                                m_dev_id;

    mutable std::mutex                         m_mu;
    std::shared_ptr<LanCameraSource>           m_lan;
    std::shared_ptr<CloudCameraSource>         m_cloud;
    std::shared_ptr<NullCameraSource>          m_null;
    std::shared_ptr<UplinkHealthMonitor>       m_health;
    Policy                                     m_policy;

    Choice                                     m_choice = Choice::None;
    std::shared_ptr<server::ICameraSource>     m_chosen;
    bool                                       m_open   = false;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_CAMERA_SOURCE_ROUTER_HPP
