// Bambu Bridge — upload sink router (phase 9).
//
// `UploadSinkRouter` is the IUploadSink the FtpsServer is bound to. It owns
// a LAN sink and a cloud sink and, per `deliver()` call, picks one to
// route through; on `ok=false` from the LAN sink it can fall back to the
// cloud sink (and vice-versa if `prefer_lan=false`).
//
// Upload routing is INDEPENDENT of MQTT routing — a print may have its
// command path on LAN MQTT while its (large) .3mf upload goes to cloud
// OSS, or vice-versa. Per-call decision; no caching.
//
// Threading: the FTPS server calls `deliver()` synchronously from its
// per-connection worker. Multiple concurrent uploads for different
// dev_ids may invoke `deliver()` from different threads simultaneously;
// the router takes a short mutex to sample its shared_ptrs, then
// dispatches to the chosen sink. Sub-sinks are themselves thread-safe.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_UPLOAD_SINK_ROUTER_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_UPLOAD_SINK_ROUTER_HPP

#include "../server/IUploadSink.hpp"

#include <memory>
#include <mutex>

namespace Slic3r {
namespace bridge {
namespace router {

class LanUploadSink;
class CloudUploadSink;
class UplinkHealthMonitor;

class UploadSinkRouter : public server::IUploadSink {
public:
    struct Policy {
        bool prefer_lan         = true;
        bool fallback_to_cloud  = true;
    };

    UploadSinkRouter();
    ~UploadSinkRouter() override;

    UploadSinkRouter(const UploadSinkRouter&)            = delete;
    UploadSinkRouter& operator=(const UploadSinkRouter&) = delete;

    void set_lan_sink     (std::shared_ptr<LanUploadSink>       lan);
    void set_cloud_sink   (std::shared_ptr<CloudUploadSink>     cloud);
    void set_health_monitor(std::shared_ptr<UplinkHealthMonitor> monitor);
    void set_policy(Policy p);

    server::UploadResult deliver(server::UploadJob job) override;

private:
    mutable std::mutex                       m_mu;
    std::shared_ptr<LanUploadSink>           m_lan;
    std::shared_ptr<CloudUploadSink>         m_cloud;
    std::shared_ptr<UplinkHealthMonitor>     m_health;
    Policy                                   m_policy;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_UPLOAD_SINK_ROUTER_HPP
