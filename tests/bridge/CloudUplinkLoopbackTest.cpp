// Bambu Bridge — CloudUplink loopback test (phase 6).
//
// Drives CloudUplink against a `MockPluginHandle` that overrides every
// network-facing method to record the call in memory. No dlopen, no
// actual cloud — pure plumbing verification:
//
//   * IUplink::on_publish(dev_id, ...) reaches the mock's
//     publish_to_device with the payload converted to a JSON string and
//     the QoS preserved.
//   * IUplink::on_subscribe (and the refcount logic) produces exactly
//     one subscribe_device call per dev_id even with multiple
//     overlapping subscribes, and the matching unsubscribe_device when
//     the last refcount drops.
//   * A simulated incoming cloud message (delivered via
//     deliver_message_for_test) routes through the registered receiver
//     into the broker's DownstreamPublisher with the right
//     `device/<dev_id>/report` topic.
//   * is_connected() reflects (agent_ready && is_user_login &&
//     is_server_connected) — flipping any of the three flips the
//     reported state.

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "router/CloudUplink.hpp"
#include "BambuNetworkingPluginHandle.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// A test-only subclass of the real handle. Overrides every network-
// facing method so we can drive CloudUplink without dlopen. The
// register_receiver / dispatch_message / deliver_message_for_test
// paths inherit from the base class unchanged — that's the seam we
// use to inject a fake printer->bridge PUBLISH.
class MockPluginHandle : public Slic3r::bridge::BambuNetworkingPluginHandle {
public:
    struct Pub {
        std::string dev_id;
        std::string json;
        int         qos;
    };

    mutable std::mutex mu;
    std::atomic<bool>  ready{true};
    std::atomic<bool>  user_logged_in{true};
    std::atomic<bool>  server_up{true};
    std::vector<std::string> subscribes;
    std::vector<std::string> unsubscribes;
    std::vector<Pub>         publishes;

    MockPluginHandle()
        : Slic3r::bridge::BambuNetworkingPluginHandle({}) {
        // Tell the base class we have a "ready" agent so
        // is_server_connected() goes through our overrides.
        this->set_agent_ready_for_test(true);
    }

    bool init() override { return ready.load(); }
    bool agent_ready() const override { return ready.load(); }
    bool is_user_login() const override { return user_logged_in.load(); }
    bool is_server_connected() const override { return server_up.load(); }

    bool get_user_print_info(unsigned int*, std::string*) const override {
        return false; // not used by this test
    }

    int subscribe_device(const std::string& dev_id) override {
        std::lock_guard<std::mutex> lk(mu);
        subscribes.push_back(dev_id);
        return 0;
    }
    int unsubscribe_device(const std::string& dev_id) override {
        std::lock_guard<std::mutex> lk(mu);
        unsubscribes.push_back(dev_id);
        return 0;
    }
    int publish_to_device(const std::string& dev_id,
                          const std::string& json,
                          int qos) override {
        std::lock_guard<std::mutex> lk(mu);
        publishes.push_back({dev_id, json, qos});
        return 0;
    }
};

} // namespace

int main() {
    using namespace Slic3r::bridge;
    using router::CloudUplink;
    using router::CloudUplinkConfig;

    const std::string dev_id        = "0938BC582502312";
    const std::string topic_report  = "device/" + dev_id + "/report";
    const std::string topic_request = "device/" + dev_id + "/request";

    auto mock = std::make_shared<MockPluginHandle>();
    CloudUplink up;
    up.attach_plugin(mock);

    CloudUplinkConfig cfg;
    cfg.dev_id      = dev_id;
    cfg.access_code = "ACCESS-CODE";
    up.add_device(cfg);

    // ---- is_connected reflects the mock's reported state ----
    check(up.is_connected(dev_id),
          "is_connected true when agent+login+server all up");
    mock->server_up.store(false);
    check(!up.is_connected(dev_id),
          "is_connected false when server drops");
    mock->server_up.store(true);
    mock->user_logged_in.store(false);
    check(!up.is_connected(dev_id),
          "is_connected false when user logs out");
    mock->user_logged_in.store(true);
    mock->ready.store(false);
    check(!up.is_connected(dev_id),
          "is_connected false when agent goes away");
    mock->ready.store(true);
    check(up.is_connected(dev_id),
          "is_connected recovers once all three are up again");

    // ---- subscribe refcount: one wire SUBSCRIBE per dev_id ----
    up.on_subscribe(dev_id, topic_report);
    up.on_subscribe(dev_id, topic_report);   // duplicate — coalesced
    up.on_subscribe(dev_id, "device/" + dev_id + "/alt");
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->subscribes.size() == 1,
              "multiple slicer subscribes coalesce to one cloud subscribe");
        if (!mock->subscribes.empty()) {
            check(mock->subscribes[0] == dev_id,
                  "cloud subscribe carries the dev_id, not the topic");
        }
    }

    // ---- publish forwarding ----
    const std::string body = "{\"info\":{\"command\":\"get_version\"}}";
    std::vector<uint8_t> payload(body.begin(), body.end());
    up.on_publish(dev_id, topic_request, payload, 1);
    up.on_publish(dev_id, topic_request, payload, 0);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->publishes.size() == 2, "two publishes forwarded");
        if (mock->publishes.size() >= 2) {
            check(mock->publishes[0].dev_id == dev_id,
                  "publish #0 dev_id matches");
            check(mock->publishes[0].json == body,
                  "publish #0 JSON payload passes through verbatim");
            check(mock->publishes[0].qos == 1,
                  "publish #0 qos=1 preserved");
            check(mock->publishes[1].qos == 0,
                  "publish #1 qos=0 preserved");
        }
    }

    // ---- incoming cloud message routes through DownstreamPublisher ----
    std::mutex ds_mu;
    std::vector<std::pair<std::string, std::string>> downstream;
    up.attach_downstream(dev_id,
        [&](std::string topic, std::vector<uint8_t> p, uint8_t /*q*/) {
            std::lock_guard<std::mutex> lk(ds_mu);
            std::string s(p.begin(), p.end());
            downstream.emplace_back(std::move(topic), std::move(s));
        });

    mock->deliver_message_for_test(dev_id, "{\"print\":{\"state\":\"RUNNING\"}}");
    mock->deliver_message_for_test(dev_id, "{\"print\":{\"state\":\"PAUSED\"}}");
    {
        std::lock_guard<std::mutex> lk(ds_mu);
        check(downstream.size() == 2,
              "two cloud-side messages routed to downstream slicer");
        if (downstream.size() == 2) {
            check(downstream[0].first == topic_report,
                  "downstream topic synthesised as device/<id>/report");
            check(downstream[0].second == "{\"print\":{\"state\":\"RUNNING\"}}",
                  "downstream payload passes through verbatim");
        }
    }

    // attach_downstream(nullptr) detaches — subsequent deliveries no-op.
    up.attach_downstream(dev_id, nullptr);
    mock->deliver_message_for_test(dev_id, "{\"print\":{\"state\":\"IDLE\"}}");
    {
        std::lock_guard<std::mutex> lk(ds_mu);
        check(downstream.size() == 2,
              "delivery after detach is a no-op");
    }

    // ---- unsubscribe refcount: only the LAST emits wire unsubscribe ----
    up.on_unsubscribe(dev_id, topic_report);                       // 2->1
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->unsubscribes.empty(),
              "first unsubscribe doesn't drop the cloud subscription");
    }
    up.on_unsubscribe(dev_id, topic_report);                       // 1->0
    up.on_unsubscribe(dev_id, "device/" + dev_id + "/alt");        // alt 1->0
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->unsubscribes.size() == 1,
              "cloud unsubscribe fires once all topic refs drop");
        if (!mock->unsubscribes.empty()) {
            check(mock->unsubscribes[0] == dev_id,
                  "cloud unsubscribe carries the dev_id");
        }
    }

    // on_disconnect from slicer must not touch the cloud agent.
    up.on_disconnect(dev_id);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->unsubscribes.size() == 1,
              "on_disconnect doesn't fire an extra cloud unsubscribe");
    }

    // remove_device should be a clean teardown.
    up.remove_device(dev_id);

    if (g_fails) {
        std::fprintf(stderr, "CloudUplinkLoopbackTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("CloudUplinkLoopbackTest: ok\n");
    return 0;
}
