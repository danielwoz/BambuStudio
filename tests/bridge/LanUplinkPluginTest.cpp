// Bambu Bridge — LanUplink plugin-route loopback test.
//
// Drives LanUplink against a MockPluginHandle subclass that overrides
// every LAN-facing method to record the call in memory. Replaces the
// old `LanUplinkLoopbackTest` — there's no raw TLS+MQTT for the test to
// drive anymore because LanUplink now goes through the plugin.
//
// Assertions:
//   * add_device triggers connect_printer with the right credentials.
//   * on_publish reaches send_message_to_printer with the payload bytes
//     converted to JSON and QoS preserved.
//   * on_subscribe does NOT call into the plugin (no per-topic subscribe
//     in the LAN model — the broker auto-pushes /report).
//   * deliver_local_message_for_test routes through the registered
//     DownstreamPublisher.
//   * remove_device of the only device calls disconnect_printer.
//   * Multiple registered devices — only the most recently-added one
//     "owns" the plugin's single LAN slot; the bridge tracks this in
//     `current_connected_dev_id`.
//   * is_connected reflects (plugin loaded + LAN connect rc=0 +
//     plugin's is_local_connected() + dev_id is current owner).

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

    mutable std::mutex        mu;
    std::vector<ConnectCall>  connects;
    int                       disconnects = 0;
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
        local_up.store(true);
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
        return 0;
    }
    bool is_local_connected() const override { return local_up.load(); }
};

} // namespace

int main() {
    const std::string dev_id_a = "0938BC582502312";
    const std::string dev_id_b = "0938BC582502999";
    const std::string topic_a  = "device/" + dev_id_a + "/report";

    auto mock = std::make_shared<MockPluginHandle>();
    LanUplink up;
    up.attach_plugin(mock);

    // ---- add_device → connect_printer ----
    LanUplinkConfig cfg_a;
    cfg_a.dev_id      = dev_id_a;
    cfg_a.printer_ip  = "192.0.2.10";
    cfg_a.access_code = "AAAA";
    up.add_device(cfg_a);

    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->connects.size() == 1,
              "add_device #1 → connect_printer called once");
        if (!mock->connects.empty()) {
            check(mock->connects[0].dev_id == dev_id_a, "dev_id matches");
            check(mock->connects[0].dev_ip == "192.0.2.10", "dev_ip matches");
            check(mock->connects[0].password == "AAAA", "access_code as password");
            check(mock->connects[0].use_ssl, "use_ssl true");
        }
    }
    check(up.is_connected(dev_id_a), "LAN up for dev_a after connect rc=0");

    // ---- on_subscribe is a no-op on the plugin wire ----
    up.on_subscribe(dev_id_a, topic_a);
    up.on_subscribe(dev_id_a, "device/" + dev_id_a + "/alt");
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->connects.size() == 1,
              "on_subscribe added no extra plugin call");
    }

    // ---- on_publish → send_message_to_printer, payload preserved ----
    const std::string body = "{\"info\":{\"command\":\"print\"}}";
    std::vector<uint8_t> payload(body.begin(), body.end());
    up.on_publish(dev_id_a, "device/" + dev_id_a + "/request", payload, /*qos=*/0);
    up.on_publish(dev_id_a, "device/" + dev_id_a + "/request", payload, /*qos=*/1);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->publishes.size() == 2,
              "two send_message_to_printer calls observed");
        if (mock->publishes.size() >= 2) {
            check(mock->publishes[0].json == body,
                  "publish payload bytes pass through verbatim");
            check(mock->publishes[0].qos == 0, "qos=0 preserved");
            check(mock->publishes[1].qos == 1, "qos=1 preserved");
        }
    }

    // ---- downstream dispatch ----
    std::mutex ds_mu;
    std::vector<std::pair<std::string, std::string>> downstream;
    up.attach_downstream(dev_id_a,
        [&](std::string topic, std::vector<uint8_t> p, uint8_t /*q*/) {
            std::lock_guard<std::mutex> lk(ds_mu);
            downstream.emplace_back(std::move(topic),
                                    std::string(p.begin(), p.end()));
        });
    mock->deliver_local_message_for_test(dev_id_a,
        "{\"print\":{\"state\":\"RUNNING\"}}");
    {
        std::lock_guard<std::mutex> lk(ds_mu);
        check(downstream.size() == 1, "downstream PUBLISH dispatched");
        if (!downstream.empty()) {
            check(downstream[0].first == topic_a,
                  "downstream topic synthesised as device/<id>/report");
            check(downstream[0].second == "{\"print\":{\"state\":\"RUNNING\"}}",
                  "downstream JSON pass-through");
        }
    }

    // ---- Second device: only the most recently-added is "current" ----
    LanUplinkConfig cfg_b;
    cfg_b.dev_id      = dev_id_b;
    cfg_b.printer_ip  = "192.0.2.20";
    cfg_b.access_code = "BBBB";
    up.add_device(cfg_b);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->connects.size() == 2,
              "add_device #2 → second connect_printer call");
    }
    check(up.is_connected(dev_id_b), "LAN reports up for dev_b (the new owner)");
    check(!up.is_connected(dev_id_a),
          "LAN reports DOWN for dev_a (single-LAN-slot serialisation)");

    // ---- remove_device of the owner triggers disconnect_printer ----
    up.remove_device(dev_id_b);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->disconnects == 1,
              "remove_device of current owner → disconnect_printer");
    }
    check(!up.is_connected(dev_id_b), "is_connected false after remove of dev_b");

    // ---- remove_device of a non-owner is silent on the plugin ----
    up.remove_device(dev_id_a);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->disconnects == 1,
              "remove_device of non-owner does NOT fire another disconnect");
    }

    if (g_fails) {
        std::fprintf(stderr, "LanUplinkPluginTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("LanUplinkPluginTest: ok\n");
    return 0;
}
