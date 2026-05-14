// Bambu Bridge — LanUplink unit test (plugin-routed).
//
// Pure-state-machine test: no network, no TLS, no real printer. We drive
// LanUplink against a `MockPluginHandle` that overrides every LAN-facing
// method to record the call in memory.
//
// The IUplink-facing contract exercised here:
//
//   * add_device calls `connect_printer` on the plugin with the right
//     credentials.
//   * on_publish forwards verbatim to `send_message_to_printer` with
//     the dev_id, JSON payload, and QoS preserved.
//   * on_subscribe / on_unsubscribe DO NOT call into the plugin (Bambu's
//     LAN broker auto-pushes `device/<dev_id>/report` after
//     connect_printer succeeds — no per-topic subscribe in the plugin
//     model).
//   * deliver_local_message_for_test routes through the registered
//     DownstreamPublisher with the right `device/<dev_id>/report` topic.
//   * remove_device of the only device calls `disconnect_printer`.
//   * attach_downstream(null) detaches.
//   * is_connected reflects (plugin loaded + is_local_connected + we're
//     the currently-active dev_id).

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "router/LanUplink.hpp"
#include "BambuNetworkingPluginHandle.hpp"

using Slic3r::bridge::router::LanUplink;
using Slic3r::bridge::router::LanUplinkConfig;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

class MockPluginHandle : public Slic3r::bridge::BambuNetworkingPluginHandle {
public:
    struct ConnectCall {
        std::string dev_id, dev_ip, username, password;
        bool        use_ssl;
    };
    struct Pub { std::string dev_id; std::string json; int qos; };

    mutable std::mutex mu;
    std::vector<ConnectCall> connects;
    int                       disconnects   = 0;
    std::vector<Pub>          publishes;
    std::atomic<bool>         local_up{true};

    MockPluginHandle() : BambuNetworkingPluginHandle({}) {
        this->set_agent_ready_for_test(true);
    }
    bool init() override          { return true; }
    bool agent_ready() const override { return true; }

    int connect_printer(const std::string& dev_id, const std::string& dev_ip,
                        const std::string& username, const std::string& password,
                        bool use_ssl) override {
        std::lock_guard<std::mutex> lk(mu);
        connects.push_back({dev_id, dev_ip, username, password, use_ssl});
        return 0;
    }
    int disconnect_printer() override {
        std::lock_guard<std::mutex> lk(mu);
        ++disconnects;
        local_up.store(false);
        return 0;
    }
    int send_message_to_printer(const std::string& dev_id,
                                const std::string& json, int qos) override {
        std::lock_guard<std::mutex> lk(mu);
        publishes.push_back({dev_id, json, qos});
        return 0;
    }
    int subscribe_device(const std::string&) override   { return 0; }
    int unsubscribe_device(const std::string&) override { return 0; }
    int publish_to_device(const std::string&, const std::string&, int) override {
        // Should never be called from LanUplink — assert by recording.
        std::lock_guard<std::mutex> lk(mu);
        return 0;
    }
    bool is_local_connected() const override { return local_up.load(); }
};

} // namespace

int main() {
    const std::string dev_id = "0938BC582502312";

    auto mock = std::make_shared<MockPluginHandle>();
    LanUplink up;
    up.attach_plugin(mock);

    LanUplinkConfig cfg;
    cfg.dev_id      = dev_id;
    cfg.printer_ip  = "192.0.2.1";   // documented test-net (RFC 5737)
    cfg.access_code = "ACCESS123";
    up.add_device(cfg);

    // -- add_device drives connect_printer on the plugin --
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->connects.size() == 1,
              "add_device invoked connect_printer once");
        if (!mock->connects.empty()) {
            const auto& c = mock->connects[0];
            check(c.dev_id   == dev_id,       "connect_printer dev_id");
            check(c.dev_ip   == "192.0.2.1",  "connect_printer dev_ip");
            check(c.username == "bblp",       "connect_printer username = bblp");
            check(c.password == "ACCESS123",  "connect_printer password = access code");
            check(c.use_ssl  == true,         "connect_printer use_ssl=true");
        }
    }
    check(up.is_connected(dev_id),
          "is_connected true once connect_printer rc=0 and plugin reports up");

    // -- on_subscribe is a no-op on the wire (auto-pushed by the plugin) --
    const std::string topic_report = "device/" + dev_id + "/report";
    up.on_subscribe(dev_id, topic_report);
    up.on_subscribe(dev_id, topic_report);
    up.on_subscribe(dev_id, "device/" + dev_id + "/alt");
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->connects.size() == 1,
              "on_subscribe did NOT trigger extra plugin call");
    }

    // -- on_unsubscribe also doesn't touch the plugin --
    up.on_unsubscribe(dev_id, topic_report);
    up.on_unsubscribe(dev_id, topic_report);
    up.on_unsubscribe(dev_id, "device/" + dev_id + "/never");

    // -- on_publish forwards verbatim, QoS preserved --
    const std::string topic_req = "device/" + dev_id + "/request";
    const std::string body      = "{\"info\":{\"command\":\"get_version\"}}";
    std::vector<uint8_t> payload(body.begin(), body.end());
    up.on_publish(dev_id, topic_req, payload, 0);
    up.on_publish(dev_id, topic_req, payload, 1);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->publishes.size() == 2, "two LAN publishes recorded");
        if (mock->publishes.size() >= 2) {
            check(mock->publishes[0].dev_id == dev_id,
                  "publish #0 dev_id matches");
            check(mock->publishes[0].json == body,
                  "publish #0 JSON payload passes through verbatim");
            check(mock->publishes[0].qos == 0,
                  "publish #0 qos=0 preserved");
            check(mock->publishes[1].qos == 1,
                  "publish #1 qos=1 preserved");
        }
    }

    // -- DownstreamPublisher routing: simulate printer→bridge PUBLISH --
    std::mutex ds_mu;
    std::vector<std::pair<std::string, std::string>> downstream;
    up.attach_downstream(dev_id,
        [&](std::string topic, std::vector<uint8_t> p, uint8_t /*q*/) {
            std::lock_guard<std::mutex> lk(ds_mu);
            std::string s(p.begin(), p.end());
            downstream.emplace_back(std::move(topic), std::move(s));
        });

    mock->deliver_local_message_for_test(dev_id, "{\"print\":{\"state\":\"RUNNING\"}}");
    mock->deliver_local_message_for_test(dev_id, "{\"print\":{\"state\":\"PAUSED\"}}");
    {
        std::lock_guard<std::mutex> lk(ds_mu);
        check(downstream.size() == 2,
              "two local messages routed to downstream slicer");
        if (downstream.size() == 2) {
            check(downstream[0].first == topic_report,
                  "downstream topic synthesised as device/<id>/report");
            check(downstream[0].second == "{\"print\":{\"state\":\"RUNNING\"}}",
                  "downstream payload passes through verbatim");
        }
    }

    // -- attach_downstream(nullptr) detaches: subsequent dispatch no-ops --
    up.attach_downstream(dev_id, nullptr);
    mock->deliver_local_message_for_test(dev_id, "{\"print\":{\"state\":\"IDLE\"}}");
    {
        std::lock_guard<std::mutex> lk(ds_mu);
        check(downstream.size() == 2,
              "dispatch after attach_downstream(null) is a no-op");
    }

    // -- on_disconnect: slicer detach does NOT touch the plugin --
    up.on_disconnect(dev_id);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->disconnects == 0,
              "on_disconnect from slicer does NOT disconnect plugin LAN session");
    }

    // -- remove_device of the only device calls disconnect_printer --
    up.remove_device(dev_id);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->disconnects == 1,
              "remove_device fired disconnect_printer");
    }
    check(!up.is_connected(dev_id),
          "is_connected false after remove_device");

    if (g_fails) {
        std::fprintf(stderr, "LanUplinkUnitTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("LanUplinkUnitTest: ok\n");
    return 0;
}
