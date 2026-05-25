// Bambu Bridge — SessionRouter unit test (phase 9, plugin-routed LAN).
//
// Pure state-machine test: no sockets, no TLS, no real plugin. Both LAN
// and Cloud sides go through the same `BambuNetworkingPluginHandle` —
// we use a single `MockPluginHandle` that overrides every network call
// (subscribe/publish for cloud, connect_printer/send_message_to_printer/
// is_local_connected for LAN). Flipping the mock's atomics drives both
// sub-uplinks' `is_connected(dev_id)` exactly the way the loopback tests
// established.
//
// Scenarios exercised:
//   1. LAN up, cloud up                → route = LAN, PUBLISH goes to LAN.
//   2. LAN down, cloud up              → route = Cloud, PUBLISH goes to cloud.
//   3. Both down                       → route = None, PUBLISH dropped.
//   4. LAN flap within debounce window → route doesn't bounce back.
//   5. attach_downstream registers with both sub-uplinks (a LAN-side
//      report AND a cloud-side report both reach the slicer publisher).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "router/CloudUplink.hpp"
#include "router/LanUplink.hpp"
#include "router/SessionRouter.hpp"
#include "router/UplinkHealth.hpp"
#include "BambuNetworkingPluginHandle.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

using Slic3r::bridge::router::CloudUplink;
using Slic3r::bridge::router::CloudUplinkConfig;
using Slic3r::bridge::router::LanUplink;
using Slic3r::bridge::router::LanUplinkConfig;
using Slic3r::bridge::router::SessionRouter;
using Slic3r::bridge::router::UplinkHealthMonitor;

// ---- MockPluginHandle ----------------------------------------------------
//
// Records every send for both LAN and cloud sides so we can assert which
// side an MQTT PUBLISH was actually routed to. The cloud side hits
// `publish_to_device` (CloudUplink's path), the LAN side hits
// `send_message_to_printer` (LanUplink's path) — even though upstream
// both wrap the same plugin symbol, the bridge keeps them as separate
// virtual methods so tests can distinguish them.
class MockPluginHandle : public Slic3r::bridge::BambuNetworkingPluginHandle {
public:
    struct Pub { std::string dev_id; std::string json; int qos; };
    mutable std::mutex mu;

    // Cloud-side knobs.
    std::atomic<bool>  ready{true};
    std::atomic<bool>  user_logged_in{true};
    std::atomic<bool>  server_up{true};

    // LAN-side knobs.
    std::atomic<bool>  local_up{true};
    std::atomic<int>   connect_printer_rc{0};
    std::vector<std::string> lan_connects;   // dev_ids passed to connect_printer
    int                lan_disconnects = 0;

    std::vector<Pub>   cloud_publishes;
    std::vector<Pub>   lan_publishes;

    MockPluginHandle()
        : Slic3r::bridge::BambuNetworkingPluginHandle({}) {
        this->set_agent_ready_for_test(true);
    }
    bool init() override { return ready.load(); }
    bool agent_ready() const override { return ready.load(); }
    bool is_user_login() const override { return user_logged_in.load(); }
    bool is_server_connected() const override { return server_up.load(); }
    bool get_user_print_info(unsigned int*, std::string*) const override { return false; }

    // Cloud path.
    int subscribe_device(const std::string&) override   { return 0; }
    int unsubscribe_device(const std::string&) override { return 0; }
    int publish_to_device(const std::string& dev_id,
                          const std::string& json,
                          int qos) override {
        std::lock_guard<std::mutex> lk(mu);
        cloud_publishes.push_back({dev_id, json, qos});
        return 0;
    }

    // LAN path.
    int connect_printer(const std::string& dev_id, const std::string&,
                        const std::string&, const std::string&, bool) override {
        std::lock_guard<std::mutex> lk(mu);
        lan_connects.push_back(dev_id);
        // The bridge stores `current_connected_dev_id` only on rc==0.
        return connect_printer_rc.load();
    }
    int disconnect_printer() override {
        std::lock_guard<std::mutex> lk(mu);
        ++lan_disconnects;
        local_up.store(false);
        return 0;
    }
    int send_message_to_printer(const std::string& dev_id,
                                const std::string& json,
                                int qos) override {
        std::lock_guard<std::mutex> lk(mu);
        lan_publishes.push_back({dev_id, json, qos});
        return 0;
    }
    bool is_local_connected() const override { return local_up.load(); }
};

} // namespace

int main() {
    const std::string dev_id = "EXAMPLESERIAL01";

    // Single shared plugin handle drives both LAN and cloud sides.
    auto mock = std::make_shared<MockPluginHandle>();

    // ---- Build LAN side: real LanUplink against mock plugin ----
    auto lan = std::make_shared<LanUplink>();
    lan->attach_plugin(mock);
    LanUplinkConfig lcfg;
    lcfg.dev_id      = dev_id;
    lcfg.printer_ip  = "192.0.2.1";   // documented test-net (RFC 5737)
    lcfg.access_code = "ACCESS";
    lan->add_device(lcfg);
    check(lan->is_connected(dev_id), "LAN reports connected initially");

    // ---- Build cloud side: real CloudUplink with same mock ----
    auto cloud = std::make_shared<CloudUplink>();
    cloud->attach_plugin(mock);
    CloudUplinkConfig ccfg;
    ccfg.dev_id      = dev_id;
    ccfg.access_code = "ACCESS";
    cloud->add_device(ccfg);
    check(cloud->is_connected(dev_id), "cloud reports connected initially");

    // ---- Build the health monitor + router ----
    auto health = std::make_shared<UplinkHealthMonitor>();
    health->set_lan_uplink(lan);
    health->set_cloud_uplink(cloud);

    SessionRouter router;
    router.set_lan_uplink(lan);
    router.set_cloud_uplink(cloud);
    router.set_health_monitor(health);
    SessionRouter::Policy pol;
    pol.prefer_lan         = true;
    pol.rerouting_debounce = std::chrono::milliseconds(50);
    router.set_policy(pol);

    const std::string topic_req = "device/" + dev_id + "/request";
    const std::string body      = "{\"info\":{\"command\":\"get_version\"}}";
    std::vector<uint8_t> payload(body.begin(), body.end());

    // ---- Scenario 1: LAN up, cloud up → prefer LAN ----
    auto r = router.current_route(dev_id);
    check(r == SessionRouter::Route::Lan,
          "LAN+cloud both healthy with prefer_lan → route = LAN");
    router.on_publish(dev_id, topic_req, payload, 0);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->lan_publishes.size() == 1,
              "PUBLISH routed to LAN (mock send_message_to_printer)");
        check(mock->cloud_publishes.empty(),
              "PUBLISH did NOT reach cloud (mock publish_to_device)");
    }

    // ---- Scenario 2: LAN down, cloud up → route = Cloud ----
    mock->local_up.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    r = router.current_route(dev_id);
    check(r == SessionRouter::Route::Cloud,
          "LAN down + cloud up → route = Cloud");
    router.on_publish(dev_id, topic_req, payload, 0);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->cloud_publishes.size() == 1,
              "PUBLISH routed to cloud once LAN drops");
        check(mock->lan_publishes.size() == 1,
              "LAN-side got no further PUBLISH");
    }

    // ---- Scenario 3: both down → route = None, PUBLISH dropped ----
    mock->server_up.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    r = router.current_route(dev_id);
    check(r == SessionRouter::Route::None,
          "both down → route = None");
    router.on_publish(dev_id, topic_req, payload, 0);
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->cloud_publishes.size() == 1,
              "PUBLISH dropped: cloud count unchanged");
        check(mock->lan_publishes.size() == 1,
              "PUBLISH dropped: LAN count unchanged");
    }

    // ---- Scenario 4: LAN flap within debounce window → no re-route ----
    mock->server_up.store(true);
    mock->local_up.store(false);
    SessionRouter::Policy long_dbn;
    long_dbn.prefer_lan         = true;
    long_dbn.rerouting_debounce = std::chrono::seconds(10);
    router.set_policy(long_dbn);

    r = router.current_route(dev_id);
    check(r == SessionRouter::Route::Cloud,
          "with LAN down + cloud up, first pick is Cloud (fresh cache)");

    mock->local_up.store(true);
    r = router.current_route(dev_id);
    check(r == SessionRouter::Route::Cloud,
          "LAN flapping up within debounce window does NOT bounce route");

    mock->local_up.store(false);
    r = router.current_route(dev_id);
    check(r == SessionRouter::Route::Cloud,
          "LAN flapping again within debounce window stays on Cloud");

    // ---- attach_downstream registers with both sub-uplinks ----
    std::mutex ds_mu;
    std::vector<std::string> ds_topics;
    router.attach_downstream(dev_id,
        [&](std::string t, std::vector<uint8_t> /*p*/, uint8_t /*q*/) {
            std::lock_guard<std::mutex> lk(ds_mu);
            ds_topics.push_back(std::move(t));
        });

    // Fire one report on the LAN-side (via deliver_local_message_for_test)
    // and one on the cloud side (via deliver_message_for_test). The router
    // must have registered downstream callbacks on BOTH sub-uplinks so
    // both flow through to the slicer-side publisher.
    mock->deliver_local_message_for_test(dev_id, "{\"print\":{\"lan\":1}}");
    mock->deliver_message_for_test     (dev_id, "{\"print\":{\"cloud\":1}}");
    {
        std::lock_guard<std::mutex> lk(ds_mu);
        check(ds_topics.size() == 2,
              "BOTH sub-uplinks routed downstream PUBLISHes to the slicer");
    }

    // ---- on_disconnect clears cache + forwards to both ----
    router.on_disconnect(dev_id);
    r = router.current_route(dev_id);
    check(r == SessionRouter::Route::Cloud,
          "after on_disconnect, fresh pick lands on the healthy uplink");

    // Tear down.
    lan->remove_device(dev_id);
    cloud->remove_device(dev_id);

    if (g_fails) {
        std::fprintf(stderr, "SessionRouterTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("SessionRouterTest: ok\n");
    return 0;
}
