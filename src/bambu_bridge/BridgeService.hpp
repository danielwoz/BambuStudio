// Bambu Bridge — top-level orchestrator service.
//
// Phases 0-1: declarations only. The class body is intentionally minimal;
// per-printer mirroring, server lifecycle, and routing are filled in by later
// phases (see docs/bambu_bridge_plan.md).
//
// Phase 1 adds an optional CloudInventory injection so later phases can wire
// the service against a live cloud view without this header growing the
// CloudInventory implementation surface (forward-declared on purpose).
//
// Phase 2 adds optional CertFactory ownership so the per-device server
// stacks (SsdpResponder / MqttBroker / FtpsServer in phases 3-7) can fetch
// the cert material for a given dev_id through one shared cache without
// re-resolving the XDG path or re-instantiating an OpenSSL context.
//
// Phase 3 adds optional SsdpResponder ownership plus a real start()/stop()
// pair. start()/stop() drive the lifecycle of any installed responder (and
// future MQTT/FTPS/RTSP servers — they'll plug in here in phases 4-7).
//
// Phase 6 adds optional CloudUplink ownership. When both LanUplink and
// CloudUplink are present, phase 9's SessionRouter will pick between
// them at session start; for phase 6 alone, if only CloudUplink is set
// the broker is wired directly to it (useful for cloud-only testing).
//
// Phase 7 adds optional FtpsServer ownership. The server is independent
// of the MQTT broker (file uploads have their own lifecycle); start()/
// stop() drive its listener threads alongside the broker's. The upload
// sink is configured on the FtpsServer itself before installation here;
// phase 9's SessionRouter will replace the sink with a router-shaped
// implementation that fans out per-dev_id.
//
// Phase 9 adds optional SessionRouter / UploadSinkRouter /
// CameraSourceRouter / UplinkHealthMonitor ownership. When a router is
// installed AND its corresponding server component is also installed,
// start() wires the router in front of the single-uplink/single-sink
// path. When NO router is installed the prior single-uplink/sink path
// from phases 5-8 still works (so each phase's tests stay green).

#ifndef SLIC3R_BAMBU_BRIDGE_BRIDGE_SERVICE_HPP
#define SLIC3R_BAMBU_BRIDGE_BRIDGE_SERVICE_HPP

#include <memory>
#include <vector>

namespace Slic3r {
namespace bridge {

class  CloudInventory;   // pimpl/forward — see CloudInventory.hpp
struct CloudDevice;
class  BambuSourceHandle;

namespace tls    { class CertFactory;   }    // forward — see tls/CertFactory.hpp
namespace server { class SsdpResponder; }    // forward — see server/SsdpResponder.hpp
namespace server { class MqttBroker;    }    // forward — see server/MqttBroker.hpp
namespace server { class FtpsServer;    }    // forward — see server/FtpsServer.hpp
namespace server { class RtspServer;    }    // forward — see server/RtspServer.hpp
namespace router { class LanUplink;          }   // forward — see router/LanUplink.hpp
namespace router { class CloudUplink;        }   // forward — see router/CloudUplink.hpp
namespace router { class SessionRouter;      }   // forward — see router/SessionRouter.hpp
namespace router { class UploadSinkRouter;   }   // forward — see router/UploadSinkRouter.hpp
namespace router { class CameraSourceRouter; }   // forward — see router/CameraSourceRouter.hpp
namespace router { class UplinkHealthMonitor;}   // forward — see router/UplinkHealth.hpp

class BridgeService {
public:
    BridgeService();
    ~BridgeService();

    BridgeService(const BridgeService&) = delete;
    BridgeService& operator=(const BridgeService&) = delete;

    // Hands ownership of an inventory to the service. Replaces any previously
    // installed inventory. Pass nullptr to detach. Safe to call before or
    // after start().
    void set_cloud_inventory(std::unique_ptr<CloudInventory> inv);

    // Snapshot of the most recently observed cloud-bound devices. Returns
    // an empty vector if no inventory has been installed (or if the
    // inventory itself has never been refreshed).
    std::vector<CloudDevice> devices() const;

    // Phase 2 — optional per-device TLS cert factory. Phases 3+ pull
    // cert material from this when they bring up SSDP/MQTT/FTPS for a
    // given dev_id. Pass nullptr to detach. Raw getter is nullable.
    void set_cert_factory(std::unique_ptr<tls::CertFactory> factory);
    tls::CertFactory*       cert_factory();
    const tls::CertFactory* cert_factory() const;

    // Phase 3 — optional SSDP responder. When installed, start()/stop()
    // drive its lifecycle.
    void set_ssdp_responder(std::unique_ptr<server::SsdpResponder> responder);
    server::SsdpResponder*       ssdp_responder();
    const server::SsdpResponder* ssdp_responder() const;

    // Phase 4 — optional MQTT broker. When installed, start()/stop()
    // drive its lifecycle. Devices are added to the broker out-of-band
    // (per-printer mirroring is the caller's job).
    void set_mqtt_broker(std::unique_ptr<server::MqttBroker> broker);
    server::MqttBroker*       mqtt_broker();
    const server::MqttBroker* mqtt_broker() const;

    // Phase 5 — optional LAN uplink (direct-LAN passthrough to the real
    // printer). Outlives the broker so reports can keep flowing while
    // a slicer reconnects. If both an MqttBroker AND a LanUplink are
    // installed at start() time the broker's uplink is wired to the
    // LAN uplink before either component is brought up.
    //
    // The LanUplink itself is shared as a std::shared_ptr because the
    // broker holds one too (via MqttBrokerConfig::uplink). Phase 9's
    // SessionRouter will replace this shared_ptr with a router-shaped
    // implementation that fans out to either LAN or cloud.
    void set_lan_uplink(std::shared_ptr<router::LanUplink> uplink);
    router::LanUplink*       lan_uplink();
    const router::LanUplink* lan_uplink() const;

    // Phase 6 — optional CloudUplink (fallback to Bambu Cloud's MQTT
    // broker via the proprietary plugin). Outlives the broker for the
    // same reason as the LAN uplink: reports must keep flowing while a
    // slicer reconnects. When ONLY the CloudUplink is set (no
    // LanUplink), start() wires the broker's uplink directly to it. When
    // both are set, the broker's uplink remains untouched here — phase
    // 9's SessionRouter is expected to slot in over the top.
    void set_cloud_uplink(std::shared_ptr<router::CloudUplink> uplink);
    router::CloudUplink*       cloud_uplink();
    const router::CloudUplink* cloud_uplink() const;

    // Phase 7 — optional FTPS server (slicer-facing file uploads). When
    // installed, start()/stop() drive its listener thread(s) alongside the
    // MQTT broker. The server keeps its own IUploadSink; phase 9's
    // SessionRouter will install a fan-out sink that picks between
    // LanUploadSink and CloudUploadSink per dev_id.
    void set_ftps_server(std::unique_ptr<server::FtpsServer> server);
    server::FtpsServer*       ftps_server();
    const server::FtpsServer* ftps_server() const;

    // Phase 8 — optional RTSPS camera re-serve server. When installed,
    // start()/stop() drive its listener thread(s) alongside the other
    // servers. Per-device camera sources are bound onto the RtspServer
    // via add_device(); BridgeService just owns the lifecycle.
    void set_rtsp_server(std::unique_ptr<server::RtspServer> server);
    server::RtspServer*       rtsp_server();
    const server::RtspServer* rtsp_server() const;

    // Phase 9 — optional UplinkHealthMonitor. Shared by all three routers
    // below so they observe one coherent (lan, cloud) per-device view.
    // Setting / clearing is idempotent; setting after start() is fine
    // because the monitor is consulted at lookup time, not at start().
    void set_uplink_health_monitor(std::shared_ptr<router::UplinkHealthMonitor> mon);
    router::UplinkHealthMonitor*       uplink_health_monitor();
    const router::UplinkHealthMonitor* uplink_health_monitor() const;

    // Phase 9 — optional SessionRouter. When installed (and a MqttBroker
    // is also installed), start() wires it as the broker's permanent
    // IUplink instead of LanUplink/CloudUplink directly. Replaces the
    // single-uplink phase-5/6 path. The router uses the LanUplink /
    // CloudUplink shared_ptrs the service already holds.
    void set_session_router(std::shared_ptr<router::SessionRouter> router);
    router::SessionRouter*       session_router();
    const router::SessionRouter* session_router() const;

    // Phase 9 — optional UploadSinkRouter. When installed (and a
    // FtpsServer is also installed), start() replaces the FTPS server's
    // sink with the router. Caller is expected to have wired the
    // LanUploadSink / CloudUploadSink shared_ptrs into the router
    // beforehand.
    void set_upload_sink_router(std::shared_ptr<router::UploadSinkRouter> router);
    router::UploadSinkRouter*       upload_sink_router();
    const router::UploadSinkRouter* upload_sink_router() const;

    // Phase 9 — optional CameraSourceRouter factory hook (per dev_id).
    // Unlike the other routers there is NO single per-service instance —
    // each RtspVirtualDevice on the RtspServer needs its own
    // CameraSourceRouter (since each picks one source on open()). The
    // service just remembers the factory the caller provides; the
    // RtspServer wires up devices itself. The factory is consulted by
    // bridge-cli `proxy`; BridgeService holds the shared_ptr so it
    // outlives the service.
    //
    // For phase-9-internal use only: BridgeService doesn't iterate
    // devices itself (that's the caller's job today), but we DO hold the
    // shared_ptrs so they live as long as the service. Pass nullptr to
    // detach.
    void set_camera_source_router(std::shared_ptr<router::CameraSourceRouter> router);
    router::CameraSourceRouter*       camera_source_router();
    const router::CameraSourceRouter* camera_source_router() const;

    // Shared BambuSourceHandle ownership. Used by LanCameraSource and
    // CloudCameraSource (both go through libBambuSource.so for the
    // actual stream pump). BridgeService just holds the shared_ptr so
    // it outlives the per-device camera sources; consumers attach it
    // onto each source directly via `attach_source_handle(...)`.
    void set_bambu_source_handle(std::shared_ptr<BambuSourceHandle> handle);
    BambuSourceHandle*       bambu_source_handle();
    const BambuSourceHandle* bambu_source_handle() const;

    // Phase 3 — top-level lifecycle. Both are idempotent and safe to call
    // before/after subordinate components are attached. Today they only
    // drive the SSDP responder; phases 4-7 will tack on additional
    // start()/stop() calls for MqttBroker / FtpsServer / RtspServer.
    void start();
    void stop();

    bool running() const { return m_running; }

private:
    std::unique_ptr<CloudInventory>          m_inventory;
    std::unique_ptr<tls::CertFactory>        m_cert_factory;
    std::unique_ptr<server::SsdpResponder>   m_ssdp_responder;
    std::unique_ptr<server::MqttBroker>      m_mqtt_broker;
    std::shared_ptr<router::LanUplink>       m_lan_uplink;
    std::shared_ptr<router::CloudUplink>     m_cloud_uplink;
    std::unique_ptr<server::FtpsServer>      m_ftps_server;
    std::unique_ptr<server::RtspServer>      m_rtsp_server;
    std::shared_ptr<router::UplinkHealthMonitor> m_uplink_health;
    std::shared_ptr<router::SessionRouter>      m_session_router;
    std::shared_ptr<router::UploadSinkRouter>   m_upload_router;
    std::shared_ptr<router::CameraSourceRouter> m_camera_router;
    std::shared_ptr<BambuSourceHandle>          m_bambu_source;
    bool                                     m_running = false;
};

} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_BRIDGE_SERVICE_HPP
