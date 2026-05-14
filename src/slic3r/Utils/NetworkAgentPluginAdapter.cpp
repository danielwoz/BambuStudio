#include "NetworkAgentPluginAdapter.hpp"

#include "NetworkAgent.hpp"

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
        const CloudUploadParams& /*params*/) {
    // Not yet implemented — needs a PrintParams construction that lines
    // up with what GUI_App's SendJob feeds the plugin. The slicer's
    // virtual storage tunnel goes through VirtualTunnelServer, not this
    // path. Stub returns the same code the base uses for missing
    // exports so CloudUploadSink falls back gracefully.
    std::fprintf(stderr,
        "[adapter] upload_gcode_to_sdcard not yet implemented\n");
    return -2;
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

int NetworkAgentPluginAdapter::disconnect_printer() {
    if (!m_agent) return -1;
    int rc = m_agent->disconnect_printer();
    m_local_connected.store(false);
    return rc;
}

int NetworkAgentPluginAdapter::send_message_to_printer(
        const std::string& dev_id, const std::string& json_payload, int qos) {
    if (!m_agent) return -1;
    return m_agent->send_message_to_printer(dev_id, json_payload, qos, 0);
}

int NetworkAgentPluginAdapter::start_local_print_with_record(
        const LocalPrintParams& /*params*/) {
    std::fprintf(stderr,
        "[adapter] start_local_print_with_record not yet implemented\n");
    return -2;
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
