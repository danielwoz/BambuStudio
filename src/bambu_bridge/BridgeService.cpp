// Bambu Bridge — top-level orchestrator service.
//
// Phases 0-1: trivial lifecycle plus optional CloudInventory ownership.
// Real per-device orchestration arrives in phases 3+.

#include "BridgeService.hpp"
#include "BambuSourceHandle.hpp"
#include "CloudInventory.hpp"
#include "tls/CertFactory.hpp"
#include "server/SsdpResponder.hpp"
#include "server/MqttBroker.hpp"
#include "server/FtpsServer.hpp"
#include "server/RtspServer.hpp"
#include "router/LanUplink.hpp"
#include "router/CloudUplink.hpp"
#include "router/SessionRouter.hpp"
#include "router/UploadSinkRouter.hpp"
#include "router/CameraSourceRouter.hpp"
#include "router/UplinkHealth.hpp"

namespace Slic3r {
namespace bridge {

BridgeService::BridgeService() = default;

BridgeService::~BridgeService() {
    stop();
}

void BridgeService::set_cloud_inventory(std::unique_ptr<CloudInventory> inv) {
    m_inventory = std::move(inv);
}

std::vector<CloudDevice> BridgeService::devices() const {
    if (!m_inventory) return {};
    return m_inventory->snapshot();
}

void BridgeService::set_cert_factory(std::unique_ptr<tls::CertFactory> factory) {
    m_cert_factory = std::move(factory);
}

tls::CertFactory* BridgeService::cert_factory() {
    return m_cert_factory.get();
}

const tls::CertFactory* BridgeService::cert_factory() const {
    return m_cert_factory.get();
}

void BridgeService::set_ssdp_responder(std::unique_ptr<server::SsdpResponder> responder) {
    // If we already had one running, stop it before swapping.
    if (m_ssdp_responder && m_running) {
        m_ssdp_responder->stop();
    }
    m_ssdp_responder = std::move(responder);
    if (m_ssdp_responder && m_running) {
        m_ssdp_responder->start();
    }
}

server::SsdpResponder* BridgeService::ssdp_responder() {
    return m_ssdp_responder.get();
}

const server::SsdpResponder* BridgeService::ssdp_responder() const {
    return m_ssdp_responder.get();
}

void BridgeService::set_mqtt_broker(std::unique_ptr<server::MqttBroker> broker) {
    // Mirror set_ssdp_responder's behaviour: if the bridge is running,
    // stop the outgoing one and start the incoming one cleanly.
    if (m_mqtt_broker && m_running) {
        m_mqtt_broker->stop();
    }
    m_mqtt_broker = std::move(broker);
    if (m_mqtt_broker && m_running) {
        m_mqtt_broker->start();
    }
}

server::MqttBroker* BridgeService::mqtt_broker() {
    return m_mqtt_broker.get();
}

const server::MqttBroker* BridgeService::mqtt_broker() const {
    return m_mqtt_broker.get();
}

void BridgeService::set_lan_uplink(std::shared_ptr<router::LanUplink> uplink) {
    // No lifecycle to drive on the uplink itself — its sessions are
    // started per-device when caller invokes add_device(). We just
    // hold the shared_ptr so its lifetime tracks the service's.
    m_lan_uplink = std::move(uplink);
}

router::LanUplink* BridgeService::lan_uplink() {
    return m_lan_uplink.get();
}

const router::LanUplink* BridgeService::lan_uplink() const {
    return m_lan_uplink.get();
}

void BridgeService::set_cloud_uplink(std::shared_ptr<router::CloudUplink> uplink) {
    m_cloud_uplink = std::move(uplink);
}

router::CloudUplink* BridgeService::cloud_uplink() {
    return m_cloud_uplink.get();
}

const router::CloudUplink* BridgeService::cloud_uplink() const {
    return m_cloud_uplink.get();
}

void BridgeService::set_ftps_server(std::unique_ptr<server::FtpsServer> server) {
    // Mirror set_mqtt_broker's behaviour: if the bridge is running, stop the
    // outgoing one and start the incoming one cleanly.
    if (m_ftps_server && m_running) {
        m_ftps_server->stop();
    }
    m_ftps_server = std::move(server);
    if (m_ftps_server && m_running) {
        m_ftps_server->start();
    }
}

server::FtpsServer* BridgeService::ftps_server() {
    return m_ftps_server.get();
}

const server::FtpsServer* BridgeService::ftps_server() const {
    return m_ftps_server.get();
}

void BridgeService::set_rtsp_server(std::unique_ptr<server::RtspServer> server) {
    if (m_rtsp_server && m_running) {
        m_rtsp_server->stop();
    }
    m_rtsp_server = std::move(server);
    if (m_rtsp_server && m_running) {
        m_rtsp_server->start();
    }
}

server::RtspServer* BridgeService::rtsp_server() {
    return m_rtsp_server.get();
}

const server::RtspServer* BridgeService::rtsp_server() const {
    return m_rtsp_server.get();
}

void BridgeService::set_uplink_health_monitor(std::shared_ptr<router::UplinkHealthMonitor> mon) {
    m_uplink_health = std::move(mon);
}

router::UplinkHealthMonitor* BridgeService::uplink_health_monitor() {
    return m_uplink_health.get();
}

const router::UplinkHealthMonitor* BridgeService::uplink_health_monitor() const {
    return m_uplink_health.get();
}

void BridgeService::set_session_router(std::shared_ptr<router::SessionRouter> router) {
    m_session_router = std::move(router);
}

router::SessionRouter* BridgeService::session_router() {
    return m_session_router.get();
}

const router::SessionRouter* BridgeService::session_router() const {
    return m_session_router.get();
}

void BridgeService::set_upload_sink_router(std::shared_ptr<router::UploadSinkRouter> router) {
    m_upload_router = std::move(router);
}

router::UploadSinkRouter* BridgeService::upload_sink_router() {
    return m_upload_router.get();
}

const router::UploadSinkRouter* BridgeService::upload_sink_router() const {
    return m_upload_router.get();
}

void BridgeService::set_camera_source_router(std::shared_ptr<router::CameraSourceRouter> router) {
    m_camera_router = std::move(router);
}

router::CameraSourceRouter* BridgeService::camera_source_router() {
    return m_camera_router.get();
}

const router::CameraSourceRouter* BridgeService::camera_source_router() const {
    return m_camera_router.get();
}

void BridgeService::set_bambu_source_handle(std::shared_ptr<BambuSourceHandle> handle) {
    m_bambu_source = std::move(handle);
}

BambuSourceHandle* BridgeService::bambu_source_handle() {
    return m_bambu_source.get();
}

const BambuSourceHandle* BridgeService::bambu_source_handle() const {
    return m_bambu_source.get();
}

void BridgeService::start() {
    if (m_running) return;

    // Phase 9: if a SessionRouter is installed, plumb it into the
    // broker INSTEAD of LanUplink/CloudUplink directly. The router holds
    // its own shared_ptrs to those sub-uplinks (assumed wired up by the
    // caller); the broker only sees the router as its IUplink. The
    // SessionRouter is also the recipient of the UplinkHealthMonitor so
    // it can pick a route at every call.
    //
    // Otherwise, fall back to the phase-5/6 single-uplink behaviour: if
    // both LAN and cloud are present without a router, prefer LAN (which
    // matches phase 5's wiring). This preserves prior tests' setup.
    if (m_mqtt_broker && m_session_router) {
        if (m_lan_uplink)    m_session_router->set_lan_uplink(m_lan_uplink);
        if (m_cloud_uplink)  m_session_router->set_cloud_uplink(m_cloud_uplink);
        if (m_uplink_health) m_session_router->set_health_monitor(m_uplink_health);
        // The UplinkHealthMonitor wants to know about the same uplinks.
        if (m_uplink_health) {
            if (m_lan_uplink)   m_uplink_health->set_lan_uplink(m_lan_uplink);
            if (m_cloud_uplink) m_uplink_health->set_cloud_uplink(m_cloud_uplink);
        }
        m_mqtt_broker->set_uplink(m_session_router);
    } else if (m_mqtt_broker && m_lan_uplink) {
        m_mqtt_broker->set_uplink(m_lan_uplink);
    } else if (m_mqtt_broker && m_cloud_uplink) {
        // Phase 6: cloud-only path when a session router isn't installed.
        m_mqtt_broker->set_uplink(m_cloud_uplink);
    }

    // Phase 9: if an UploadSinkRouter is installed and we have an FTPS
    // server, swap its sink for the router before start. Same pattern as
    // the session router: the router internally fans out LAN-vs-cloud
    // per-call. The router is expected to already hold its sub-sink
    // pointers; we also forward the health monitor in case the caller
    // didn't already.
    if (m_ftps_server && m_upload_router) {
        if (m_uplink_health) m_upload_router->set_health_monitor(m_uplink_health);
        m_ftps_server->set_sink(m_upload_router);
    }

    m_running = true;
    if (m_ssdp_responder) m_ssdp_responder->start();
    if (m_mqtt_broker)    m_mqtt_broker->start();
    if (m_ftps_server)    m_ftps_server->start();
    if (m_rtsp_server)    m_rtsp_server->start();
}

void BridgeService::stop() {
    if (!m_running) return;
    if (m_ssdp_responder) m_ssdp_responder->stop();
    if (m_mqtt_broker)    m_mqtt_broker->stop();
    if (m_ftps_server)    m_ftps_server->stop();
    if (m_rtsp_server)    m_rtsp_server->stop();
    // LanUplink stops itself in its destructor; if the service is being
    // explicitly stop()'d, the per-device sessions will be torn down
    // when the LanUplink shared_ptr drops its last ref. Nothing to do.
    m_running = false;
}

} // namespace bridge
} // namespace Slic3r
