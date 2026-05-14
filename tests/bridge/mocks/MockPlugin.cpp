// Bambu Bridge — MockPlugin implementation (harness).

#include "MockPlugin.hpp"

#include "third_party/nlohmann/json.hpp"

namespace Slic3r {
namespace bridge {
namespace mocks {

MockPlugin::MockPlugin()
    : BambuNetworkingPluginHandle({}) {
    // Flip the base-class flag so any code that asks "do you have a
    // plugin loaded" gets a truthful "yes". Subclass overrides then
    // take over for the actual behaviour.
    this->set_agent_ready_for_test(true);
}

void MockPlugin::add_printer(std::shared_ptr<IPrinterBehaviour> behaviour) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_printers.push_back(std::move(behaviour));
}

IPrinterBehaviour* MockPlugin::find_printer_locked(
    const std::string& dev_id) const {
    for (const auto& p : m_printers)
        if (p->dev_id() == dev_id) return p.get();
    return nullptr;
}

bool MockPlugin::get_user_print_info(unsigned int* http_code,
                                     std::string*  http_body) const {
    std::lock_guard<std::mutex> lk(m_mu);
    nlohmann::json devices = nlohmann::json::array();
    for (const auto& p : m_printers) {
        devices.push_back(nlohmann::json{
            {"dev_id",   p->dev_id()},
            {"dev_name", p->model().display_name},
            {"dev_model_name", p->model().model_id},
            {"dev_product_name", p->model().display_name},
            {"online", true},
            {"print_status", gcode_state_str(
                const_cast<IPrinterBehaviour*>(p.get())->fsm().state())},
            {"dev_access_code", "BBLP"},
        });
    }
    nlohmann::json body = nlohmann::json{
        {"message", "success"},
        {"code", 0},
        {"error", ""},
        {"devices", devices},
    };
    if (http_code) *http_code = 200;
    if (http_body) *http_body = body.dump();
    return true;
}

int MockPlugin::subscribe_device(const std::string& dev_id) {
    IPrinterBehaviour* p = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_subscribed[dev_id] = true;
        p = find_printer_locked(dev_id);
    }
    // Outside the mutex: emit the first push_status so the
    // CloudUplink-registered receiver sees the device.
    if (p) emit_push_status(p);
    return 0;
}

int MockPlugin::unsubscribe_device(const std::string& dev_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_subscribed[dev_id] = false;
    return 0;
}

int MockPlugin::publish_to_device(const std::string& dev_id,
                                  const std::string& json_payload,
                                  int                qos) {
    IPrinterBehaviour* p = nullptr;
    bool subscribed      = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_publishes.push_back({dev_id, json_payload, qos});
        subscribed = m_subscribed[dev_id];
        p = find_printer_locked(dev_id);
    }
    if (!p || !subscribed) return 0;

    nlohmann::json j = nlohmann::json::parse(json_payload, nullptr,
                                             /*allow_exceptions=*/false);
    if (j.is_discarded()) return 0;

    // Recognise both `"print"` and `"system"` envelopes. Each carries a
    // `"command"` field that the FSM keys on.
    std::string cmd;
    if (j.contains("print") && j["print"].is_object()
        && j["print"].contains("command"))
        cmd = j["print"]["command"].get<std::string>();
    else if (j.contains("system") && j["system"].is_object()
        && j["system"].contains("command"))
        cmd = j["system"]["command"].get<std::string>();

    if (!cmd.empty()) {
        p->fsm().apply_command(cmd);
        emit_push_status(p);
    }
    return 0;
}

int MockPlugin::get_camera_url(const std::string& dev_id,
                               std::string*       url_out,
                               int /*timeout_ms*/) {
    if (!url_out) return -3;
    std::lock_guard<std::mutex> lk(m_mu);
    if (!find_printer_locked(dev_id)) return -3;
    // Match the LAN-discovered URL shape MediaPlayCtrl receives — the
    // bridge's CloudCameraSource hands this verbatim to BambuSource.
    *url_out = "bambu:///rtsps___bblp:BBLP@127.0.0.1/streaming/live/1?proto=rtsps";
    return 0;
}

void MockPlugin::emit_push_status(IPrinterBehaviour* p) {
    if (!p) return;
    const std::string payload = p->build_push_status_str();
    // Drive into the base class's per-dev_id receiver registry. Same
    // path the real plugin's worker thread would take.
    this->deliver_message_for_test(p->dev_id(), payload);
}

std::vector<MockPlugin::PublishRecord> MockPlugin::publishes_for(
    const std::string& dev_id) const {
    std::lock_guard<std::mutex> lk(m_mu);
    std::vector<PublishRecord> out;
    for (const auto& p : m_publishes)
        if (p.dev_id == dev_id) out.push_back(p);
    return out;
}

std::vector<MockPlugin::Subscription> MockPlugin::subscriptions() const {
    std::lock_guard<std::mutex> lk(m_mu);
    std::vector<Subscription> out;
    out.reserve(m_subscribed.size());
    for (const auto& kv : m_subscribed)
        out.push_back({kv.first, kv.second});
    return out;
}

int MockPlugin::publish_count() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return static_cast<int>(m_publishes.size());
}

} // namespace mocks
} // namespace bridge
} // namespace Slic3r
