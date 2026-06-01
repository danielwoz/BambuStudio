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

    // ---- Multi-subscriber fan-out + retained cache + per-session detach ----
    std::mutex mux;
    std::vector<int> hits(3, 0);
    auto make_pub = [&](int idx) {
        return [&, idx](std::string, std::vector<uint8_t>, uint8_t) {
            std::lock_guard<std::mutex> lk(mux);
            ++hits[idx];
        };
    };
    up.attach_downstream(dev_id_a, /*session_id=*/1001, make_pub(0));
    up.attach_downstream(dev_id_a, /*session_id=*/1002, make_pub(1));
    up.attach_downstream(dev_id_a, /*session_id=*/1003, make_pub(2));

    // Retained cache holds the RUNNING delivery from earlier; each
    // attach replays it synchronously. (Only 1 cached message — the
    // ring is bounded to 2 but only 1 has been delivered so far.)
    {
        std::lock_guard<std::mutex> lk(mux);
        check(hits[0] == 1,
              "retained cache replays 1 message to LAN subscriber 1 at attach");
        check(hits[1] == 1,
              "retained cache replays 1 message to LAN subscriber 2 at attach");
        check(hits[2] == 1,
              "retained cache replays 1 message to LAN subscriber 3 at attach");
    }

    // One live LAN delivery → all 3 subscribers see it.
    mock->deliver_local_message_for_test(dev_id_a,
        "{\"print\":{\"state\":\"FANOUT\"}}");
    {
        std::lock_guard<std::mutex> lk(mux);
        check(hits[0] == 2, "LAN fan-out: subscriber 1 saw delivery");
        check(hits[1] == 2, "LAN fan-out: subscriber 2 saw delivery");
        check(hits[2] == 2, "LAN fan-out: subscriber 3 saw delivery");
    }

    // Per-session detach: drop subscriber 1; the others keep receiving.
    up.detach_downstream(dev_id_a, /*session_id=*/1001);
    mock->deliver_local_message_for_test(dev_id_a,
        "{\"print\":{\"state\":\"AFTER\"}}");
    {
        std::lock_guard<std::mutex> lk(mux);
        check(hits[0] == 2,
              "LAN per-session detach: subscriber 1 received nothing more");
        check(hits[1] == 3,
              "LAN per-session detach: subscriber 2 still receives");
        check(hits[2] == 3,
              "LAN per-session detach: subscriber 3 still receives");
    }

    // Fresh subscriber after deliveries gets the last 2 messages
    // replayed (FANOUT + AFTER).
    std::mutex late_mu;
    std::vector<std::string> late_seen;
    up.attach_downstream(dev_id_a, /*session_id=*/2001,
        [&](std::string /*t*/, std::vector<uint8_t> p, uint8_t /*q*/) {
            std::lock_guard<std::mutex> lk(late_mu);
            late_seen.emplace_back(p.begin(), p.end());
        });
    {
        std::lock_guard<std::mutex> lk(late_mu);
        check(late_seen.size() == 2,
              "LAN fresh subscriber gets 2 retained messages on attach");
        if (late_seen.size() == 2) {
            check(late_seen[0] == "{\"print\":{\"state\":\"FANOUT\"}}",
                  "LAN retained-cache replay order: oldest first");
            check(late_seen[1] == "{\"print\":{\"state\":\"AFTER\"}}",
                  "LAN retained-cache replay order: newest last");
        }
    }

    // Detach the fan-out subscribers so the subsequent assertions about
    // is_connected / disconnect aren't perturbed.
    up.detach_downstream(dev_id_a, 1002);
    up.detach_downstream(dev_id_a, 1003);
    up.detach_downstream(dev_id_a, 2001);

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
