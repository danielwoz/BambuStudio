#include "NetworkAgentPluginAdapter.hpp"

#include "NetworkAgent.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>


namespace Slic3r {

NetworkAgentPluginAdapter::NetworkAgentPluginAdapter(NetworkAgent* agent)
    : bridge::BambuNetworkingPluginHandle(bridge::PluginHandleConfig{}),
      m_agent(agent)
{
    if (!m_agent) return;

    // The tap fires on a plugin worker thread for every non-virtual
    // dev_id message. Forward into the base class's dispatch routines
    // so per-dev_id receivers registered via register_receiver /
    // register_local_message_receiver actually fire.
    m_agent->set_bridge_message_tap(
        [this](const std::string& dev_id,
               const std::string& payload,
               bool               is_local) {
            if (is_local) this->dispatch_local_message(dev_id, payload);
            else          this->dispatch_message      (dev_id, payload);
        });
}

NetworkAgentPluginAdapter::~NetworkAgentPluginAdapter()
{
    if (m_agent) m_agent->set_bridge_message_tap(nullptr);
}

bool NetworkAgentPluginAdapter::init() {
    // No-op. NetworkAgent has already dlopened the plugin and called
    // create_agent / start; constructing a second handle would corrupt
    // the plugin's global state. Returning true keeps every bridge-side
    // "is the handle good?" check happy.
    return m_agent != nullptr;
}

bool NetworkAgentPluginAdapter::agent_ready() const {
    return m_agent != nullptr && m_agent->get_network_agent() != nullptr;
}

bool NetworkAgentPluginAdapter::is_user_login() const {
    return m_agent && m_agent->is_user_login();
}

bool NetworkAgentPluginAdapter::is_server_connected() const {
    return m_agent && m_agent->is_server_connected();
}

bool NetworkAgentPluginAdapter::is_local_connected() const {
    // The plugin has no public is_local_connected() export. We mirror
    // the LAN connection state ourselves: set on a successful
    // connect_printer, cleared on disconnect_printer. Good enough for
    // LanUplink::is_connected — if the plugin drops the LAN session
    // independently, the next publish will surface the error.
    return m_local_connected.load();
}

bool NetworkAgentPluginAdapter::get_user_print_info(
        unsigned int* http_code, std::string* http_body) const {
    if (!m_agent || !http_code || !http_body) return false;
    return m_agent->get_user_print_info(http_code, http_body) == 0;
}

int NetworkAgentPluginAdapter::subscribe_device(const std::string& dev_id) {
    if (!m_agent) return -1;
    return m_agent->add_subscribe(std::vector<std::string>{dev_id});
}

int NetworkAgentPluginAdapter::unsubscribe_device(const std::string& dev_id) {
    if (!m_agent) return -1;
    return m_agent->del_subscribe(std::vector<std::string>{dev_id});
}

int NetworkAgentPluginAdapter::publish_to_device(
        const std::string& dev_id, const std::string& json_payload, int qos) {
    if (!m_agent) return -1;
    // NetworkAgent::send_message hits the plugin's cloud `send_message`
    // export (bambu_network_send_message). `flag` is reserved upstream;
    // pass 0 to match what GUI_App's own publishers do.
    return m_agent->send_message(dev_id, json_payload, qos, 0);
}

int NetworkAgentPluginAdapter::upload_gcode_to_sdcard(
        const CloudUploadParams& params) {
    // Mirror what the GUI's PrintJob does: try the LAN-FTPS-to-printer
    // path first (start_send_gcode_to_sdcard), and on failure fall back
    // to the cloud-relay path (start_print) — the same one PrintJob
    // uses for cloud-bound printers and for printers like A1 that have
    // no LAN FTPS server.
    //
    // Why both: not all real Bambu printers run an FTPS endpoint. H2S,
    // H2D, X1, P1 do; A1/N2S does NOT (per printers.yaml — port 6000
    // on A1 is MJPEG, no file tunnel, no SD-card FTPS). For A1 the
    // start_send_gcode_to_sdcard call returns -5010
    // (BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED) because the
    // plugin can't open the FTPS connection to 192.168.1.6:990. The
    // GUI's PrintJob handles this by routing through start_print
    // (cloud-relay); the bridge needs to do the same so the FFFP
    // FTPS-into-bridge flow remains transparent end-to-end.
    if (!m_agent) return -1;
    PrintParams pp{};
    pp.dev_id           = params.dev_id;
    pp.dev_ip           = params.dev_ip;
    pp.username         = "bblp";
    pp.password         = params.access_code;
    pp.filename         = params.local_file_path;
    pp.project_name     = params.project_name.empty()
                          ? params.local_file_path
                          : params.project_name;
    pp.connection_type  = params.connection_type.empty()
                          ? std::string("cloud")
                          : params.connection_type;
    pp.use_ssl_for_ftp  = params.use_ssl_for_ftp;
    pp.use_ssl_for_mqtt = params.use_ssl_for_mqtt;
    int rc = m_agent->start_send_gcode_to_sdcard(
        pp, /*update_fn=*/nullptr, /*cancel_fn=*/nullptr, /*wait_fn=*/nullptr);
    std::fprintf(stderr,
        "[adapter] upload_gcode_to_sdcard primary "
        "(start_send_gcode_to_sdcard) dev=%s ip=%s rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(), rc);
    std::fflush(stderr);
    if (rc == 0) return 0;
    // Cloud-relay fallback. PrintJob for cloud-bound + FTPS-less
    // printers does this same call.
    int rc2 = m_agent->start_print(
        pp, /*update_fn=*/nullptr, /*cancel_fn=*/nullptr, /*wait_fn=*/nullptr);
    std::fprintf(stderr,
        "[adapter] upload_gcode_to_sdcard fallback "
        "(start_print / cloud-relay) dev=%s ip=%s rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(), rc2);
    std::fflush(stderr);
    return rc2;
}

int NetworkAgentPluginAdapter::connect_printer(
        const std::string& dev_id, const std::string& dev_ip,
        const std::string& username, const std::string& password,
        bool use_ssl) {
    if (!m_agent) return -1;
    int rc = m_agent->connect_printer(dev_id, dev_ip, username, password, use_ssl);
    if (rc == 0) m_local_connected.store(true);
    return rc;
}

int NetworkAgentPluginAdapter::set_user_selected_machine(const std::string& dev_id) {
    if (!m_agent) return -1;
    return m_agent->set_user_selected_machine(dev_id);
}

void NetworkAgentPluginAdapter::install_device_cert(const std::string& dev_id, bool lan_only) {
    if (!m_agent) return;
    m_agent->install_device_cert(dev_id, lan_only);
}

int NetworkAgentPluginAdapter::disconnect_printer() {
    if (!m_agent) return -1;
    int rc = m_agent->disconnect_printer();
    m_local_connected.store(false);
    return rc;
}

int NetworkAgentPluginAdapter::send_message_to_printer(
        const std::string& dev_id, const std::string& json_payload, int qos) {
    if (!m_agent) {
        std::fprintf(stderr,
            "[adapter] send_message_to_printer dev=%s NO AGENT\n",
            dev_id.c_str());
        std::fflush(stderr);
        return -1;
    }
    // Mirror MachineObject::publish_json (DeviceManager.cpp:2354): for
    // cloud-bound printers it routes "print" commands via cloud
    // (`cloud_publish_json` → `send_message`); only LAN-only printers
    // use `send_message_to_printer`. The bridge is cloud-authenticated
    // and owns these dev_ids in the user's account, so cloud is the
    // right path here. Try cloud first; fall back to LAN MQTT if the
    // cloud route fails (which happens for LAN-only printers that
    // have no cloud relay).
    // Try cloud route first (matches MachineObject::cloud_publish_json
    // for cloud-bound printers); LAN fallback for LAN-only ones. The
    // plugin's send_message rejects `print.command=*` (control) payloads
    // with -2/-4 regardless of every prep we've tried; that's a hard
    // limit of the proprietary plugin from outside the GUI's
    // click-driven publish path.
    int rc_cloud = m_agent->send_message(dev_id, json_payload, qos, 0);
    if (rc_cloud == 0) {
        std::fprintf(stderr,
            "[adapter] send_message(CLOUD) dev=%s qos=%d bytes=%zu rc=0\n",
            dev_id.c_str(), qos, json_payload.size());
        std::fflush(stderr);
        return 0;
    }
    int rc_lan = m_agent->send_message_to_printer(dev_id, json_payload, qos, 0);
    std::fprintf(stderr,
        "[adapter] send_message_to_printer dev=%s qos=%d bytes=%zu "
        "rc_cloud=%d rc_lan=%d login=%d server=%d payload_head=%.60s\n",
        dev_id.c_str(), qos, json_payload.size(), rc_cloud, rc_lan,
        int(m_agent->is_user_login()),
        int(m_agent->is_server_connected()),
        json_payload.c_str());
    std::fflush(stderr);
    return rc_lan;
}

int NetworkAgentPluginAdapter::start_local_print_with_record(
        const LocalPrintParams& params) {
    // Same fix as upload_gcode_to_sdcard above — translate the adapter
    // params into PrintParams and delegate to the host NetworkAgent's
    // implementation. The stub return of -2 made every LanUploadSink
    // upload fail with the "plugin missing export" 551 reply at the
    // bridge's FTPS server, which surfaced to Orca as the IP+code
    // dialog reappearing after a successful slice + Send click.
    if (!m_agent) return -1;
    PrintParams pp{};
    pp.dev_id           = params.dev_id;
    pp.dev_ip           = params.dev_ip;
    pp.username         = "bblp";
    pp.password         = params.access_code;
    pp.filename         = params.local_file_path;
    pp.project_name     = params.project_name.empty()
                          ? params.local_file_path
                          : params.project_name;
    pp.connection_type  = params.connection_type.empty()
                          ? std::string("lan")
                          : params.connection_type;
    pp.use_ssl_for_ftp  = params.use_ssl_for_ftp;
    pp.use_ssl_for_mqtt = params.use_ssl_for_mqtt;
    int rc = m_agent->start_local_print_with_record(
        pp, /*update_fn=*/nullptr, /*cancel_fn=*/nullptr, /*wait_fn=*/nullptr);
    std::fprintf(stderr,
        "[adapter] start_local_print_with_record primary "
        "(LAN+FTPS) dev=%s ip=%s rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(), rc);
    std::fflush(stderr);
    if (rc == 0) return 0;
    // Same fallback as upload_gcode_to_sdcard: for printers without an
    // FTPS endpoint (A1) the LAN-with-record path fails with -2130; the
    // GUI's PrintJob recovers by routing through start_print
    // (cloud-relay), and so does the bridge.
    int rc2 = m_agent->start_print(
        pp, /*update_fn=*/nullptr, /*cancel_fn=*/nullptr, /*wait_fn=*/nullptr);
    std::fprintf(stderr,
        "[adapter] start_local_print_with_record fallback "
        "(start_print / cloud-relay) dev=%s ip=%s rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(), rc2);
    std::fflush(stderr);
    return rc2;
}

int NetworkAgentPluginAdapter::get_camera_url(
        const std::string& dev_id, std::string* url_out, int timeout_ms) {
    if (!m_agent || !url_out) return -1;

    std::mutex              mu;
    std::condition_variable cv;
    bool                    done = false;
    std::string             url;
    auto cb = [&](std::string u) {
        std::lock_guard<std::mutex> lk(mu);
        url  = std::move(u);
        done = true;
        cv.notify_one();
    };
    int rc = m_agent->get_camera_url(dev_id, cb);
    if (rc != 0) return -3;

    std::unique_lock<std::mutex> lk(mu);
    if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [&]{ return done; }))
        return -4;
    *url_out = std::move(url);
    return 0;
}

} // namespace Slic3r
