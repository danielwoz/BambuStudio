// Bambu Bridge — SessionRouter integration test.
//
// Real TLS+MQTT slicer-facing broker on top of SessionRouter, with both
// LAN and Cloud sides going through a single shared MockPluginHandle.
// We can't use a real raw-MQTT "stand-in printer" anymore — after the
// LAN refactor, LanUplink no longer speaks MQTT itself; it routes via
// the plugin. So the printer side is exercised through the same mock
// that backs CloudUplink.
//
//   Slicer (MqttTestClient)
//        │ TLS+MQTT
//        ▼
//   Bridge MqttBroker  ──IUplink──▶  SessionRouter
//                                       │
//                                       ├── LAN  → MockPluginHandle::send_message_to_printer
//                                       └── Cloud → MockPluginHandle::publish_to_device
//
//   Failover is triggered by flipping the mock's `is_local_connected`
//   flag (via deliver_local_connected_for_test). The slicer's broker
//   session must remain up through the route flip.
//
// Skips (rc 77) when loopback bind fails.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "BambuNetworkingPluginHandle.hpp"
#include "router/CloudUplink.hpp"
#include "router/LanUplink.hpp"
#include "router/SessionRouter.hpp"
#include "router/UplinkHealth.hpp"
#include "server/IUplink.hpp"
#include "server/MqttBroker.hpp"
#include "tls/CertFactory.hpp"

#include "support/MqttTestClient.hpp"

using Slic3r::bridge::router::CloudUplink;
using Slic3r::bridge::router::CloudUplinkConfig;
using Slic3r::bridge::router::LanUplink;
using Slic3r::bridge::router::LanUplinkConfig;
using Slic3r::bridge::router::SessionRouter;
using Slic3r::bridge::router::UplinkHealthMonitor;
using Slic3r::bridge::server::IUplink;
using Slic3r::bridge::server::MqttBroker;
using Slic3r::bridge::server::MqttBrokerConfig;
using Slic3r::bridge::server::MqttBrokerVirtualDevice;
using Slic3r::bridge::test::MqttTestClient;
using Slic3r::bridge::tls::CertFactory;
using Slic3r::bridge::tls::CertFactoryConfig;

namespace {

constexpr int kCtestSkip = 77;
int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// Single mock backs both LAN and Cloud sides — that's the real
// architecture (one plugin agent per process).
class MockPluginHandle : public Slic3r::bridge::BambuNetworkingPluginHandle {
public:
    struct Pub { std::string dev_id; std::string json; int qos; };
    mutable std::mutex mu;

    std::atomic<bool>  ready{true};
    std::atomic<bool>  user_logged_in{true};
    std::atomic<bool>  server_up{true};
    std::atomic<bool>  local_up{true};

    std::vector<Pub>   cloud_publishes;
    std::vector<Pub>   lan_publishes;

    MockPluginHandle() : BambuNetworkingPluginHandle({}) {
        this->set_agent_ready_for_test(true);
    }
    bool init() override { return ready.load(); }
    bool agent_ready() const override { return ready.load(); }
    bool is_user_login() const override { return user_logged_in.load(); }
    bool is_server_connected() const override { return server_up.load(); }
    bool get_user_print_info(unsigned int*, std::string*) const override { return false; }

    int subscribe_device(const std::string&) override   { return 0; }
    int unsubscribe_device(const std::string&) override { return 0; }
    int publish_to_device(const std::string& dev_id,
                          const std::string& json, int qos) override {
        std::lock_guard<std::mutex> lk(mu);
        cloud_publishes.push_back({dev_id, json, qos});
        return 0;
    }

    int connect_printer(const std::string&, const std::string&,
                        const std::string&, const std::string&, bool) override {
        return 0;
    }
    int disconnect_printer() override { local_up.store(false); return 0; }
    int send_message_to_printer(const std::string& dev_id,
                                const std::string& json, int qos) override {
        std::lock_guard<std::mutex> lk(mu);
        lan_publishes.push_back({dev_id, json, qos});
        return 0;
    }
    bool is_local_connected() const override { return local_up.load(); }
};

struct TestCertMaterial {
    Slic3r::bridge::tls::CertMaterial cert;
    std::filesystem::path             cache_dir;
};
TestCertMaterial mint_test_cert(const std::string& dev_id, const char* tag) {
    TestCertMaterial r;
    r.cache_dir = std::filesystem::temp_directory_path() /
                  ("bambu-bridge-srint-" + std::string(tag) + "-" +
                   std::to_string(::getpid()));
    std::filesystem::create_directories(r.cache_dir);
    CertFactoryConfig cfg;
    cfg.cache_dir = r.cache_dir;
    CertFactory factory(cfg);
    r.cert = factory.get_or_create(dev_id);
    return r;
}

std::unique_ptr<MqttBroker> try_start_broker(
    const std::string& bind_ip, uint16_t port,
    const std::string& dev_id, const std::string& access_code,
    const Slic3r::bridge::tls::CertMaterial& cert,
    std::shared_ptr<IUplink> uplink, uint16_t& bound_out) {
    MqttBrokerConfig cfg;
    cfg.uplink                 = std::move(uplink);
    cfg.max_clients_per_device = 1;
    cfg.accept_backlog         = 4;
    auto broker = std::make_unique<MqttBroker>(cfg);
    try {
        MqttBrokerVirtualDevice dev;
        dev.dev_id      = dev_id;
        dev.lan_ip      = bind_ip;
        dev.port        = port;
        dev.access_code = access_code;
        dev.cert        = cert;
        broker->add_device(dev);
        broker->start();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[srint] broker bind %s:%u failed: %s\n",
                     bind_ip.c_str(), port, ex.what());
        return nullptr;
    }
    bound_out = broker->bound_port(dev_id);
    if (bound_out == 0) { broker->stop(); return nullptr; }
    return broker;
}

template <typename F>
bool wait_until(F&& pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

} // namespace

int main() {
    const std::string dev_id      = "0938BC582502312";
    const std::string access_code = "ABCD1234";
    auto              cert_a      = mint_test_cert(dev_id, "slicerbroker");

    // ---- 1. Shared MockPluginHandle drives LAN + cloud ----
    auto mock = std::make_shared<MockPluginHandle>();

    // ---- 2. LanUplink wired against the mock ----
    auto lan = std::make_shared<LanUplink>();
    lan->attach_plugin(mock);
    LanUplinkConfig lcfg;
    lcfg.dev_id      = dev_id;
    lcfg.printer_ip  = "127.0.0.1";
    lcfg.access_code = access_code;
    lan->add_device(lcfg);
    check(lan->is_connected(dev_id), "LanUplink connected via mock plugin");

    // ---- 3. CloudUplink with the same mock ----
    auto cloud = std::make_shared<CloudUplink>();
    cloud->attach_plugin(mock);
    CloudUplinkConfig ccfg;
    ccfg.dev_id      = dev_id;
    ccfg.access_code = access_code;
    cloud->add_device(ccfg);
    check(cloud->is_connected(dev_id), "cloud (mock) reports connected");

    // ---- 4. SessionRouter wiring ----
    auto health = std::make_shared<UplinkHealthMonitor>();
    health->set_lan_uplink(lan);
    health->set_cloud_uplink(cloud);

    auto router = std::make_shared<SessionRouter>();
    router->set_lan_uplink(lan);
    router->set_cloud_uplink(cloud);
    router->set_health_monitor(health);
    SessionRouter::Policy pol;
    pol.prefer_lan         = true;
    pol.rerouting_debounce = std::chrono::milliseconds(100);
    router->set_policy(pol);

    // ---- 5. Slicer-facing MqttBroker, uplink = the router ----
    uint16_t slicer_bound = 0;
    auto slicer_broker = try_start_broker("127.0.0.1", 0, dev_id, access_code,
                                          cert_a.cert, router, slicer_bound);
    if (!slicer_broker) {
        std::fprintf(stderr, "SKIP: couldn't bind slicer-facing broker\n");
        lan->remove_device(dev_id);
        cloud->remove_device(dev_id);
        return kCtestSkip;
    }
    std::fprintf(stderr, "[srint] slicer-facing broker at 127.0.0.1:%u\n",
                 slicer_bound);

    // ---- 6. Test client connects to the slicer-facing broker ----
    MqttTestClient client;
    bool tls_ok = client.tcp_tls_connect("127.0.0.1", slicer_bound,
                                         std::chrono::seconds(5));
    check(tls_ok, "slicer client TLS-connected to bridge broker");
    if (!tls_ok) {
        std::fprintf(stderr, "[srint] %s\n", client.last_error().c_str());
        slicer_broker->stop();
        lan->remove_device(dev_id);
        return 1;
    }
    int conn_rc = client.connect_mqtt("test-slicer", "bblp", access_code);
    check(conn_rc == 0, "MQTT CONNACK accepted");

    // ---- 7. First PUBLISH while LAN healthy → goes to LAN (mock) ----
    const std::string topic = "device/" + dev_id + "/request";
    const std::string body1 = "{\"info\":{\"command\":\"phase9-lan\"}}";
    std::vector<uint8_t> payload1(body1.begin(), body1.end());
    bool sent1 = client.send_publish(topic, payload1, /*qos=*/0);
    check(sent1, "slicer published #1");

    bool saw_lan = wait_until([&]{
        std::lock_guard<std::mutex> lk(mock->mu);
        for (auto& p : mock->lan_publishes) {
            if (p.json == body1) return true;
        }
        return false;
    }, std::chrono::seconds(3));
    check(saw_lan, "mock LAN side observed PUBLISH #1");
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        check(mock->cloud_publishes.empty(),
              "cloud side did NOT see PUBLISH #1 (LAN won)");
    }

    // ---- 8. Failover: flip LAN's is_local_connected to false ----
    mock->local_up.store(false);
    bool lan_down = wait_until([&]{ return !lan->is_connected(dev_id); },
                               std::chrono::seconds(2));
    check(lan_down, "LanUplink reports disconnected after mock flip");

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Slicer's broker session must still be up. Next PUBLISH should
    // route via the cloud mock.
    const std::string body2 = "{\"info\":{\"command\":\"phase9-cloud\"}}";
    std::vector<uint8_t> payload2(body2.begin(), body2.end());
    bool sent2 = client.send_publish(topic, payload2, /*qos=*/0);
    check(sent2, "slicer published #2 over SAME broker session");

    bool saw_cloud = wait_until([&]{
        std::lock_guard<std::mutex> lk(mock->mu);
        for (auto& p : mock->cloud_publishes) {
            if (p.json == body2) return true;
        }
        return false;
    }, std::chrono::seconds(3));
    check(saw_cloud, "cloud mock observed PUBLISH #2 (failover)");
    {
        std::lock_guard<std::mutex> lk(mock->mu);
        bool body2_on_lan = false;
        for (auto& p : mock->lan_publishes) {
            if (p.json == body2) body2_on_lan = true;
        }
        check(!body2_on_lan,
              "LAN side did NOT see PUBLISH #2 (failover left it on cloud)");
    }

    // Slicer's session is still alive — issue one more publish.
    const std::string body3 = "{\"info\":{\"command\":\"phase9-still-up\"}}";
    std::vector<uint8_t> payload3(body3.begin(), body3.end());
    check(client.send_publish(topic, payload3, /*qos=*/0),
          "slicer can still publish after failover (session uninterrupted)");

    // ---- Tear down ----
    client.close();
    slicer_broker->stop();
    cloud->remove_device(dev_id);
    lan->remove_device(dev_id);

    if (g_fails) {
        std::fprintf(stderr, "SessionRouterIntegrationTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("SessionRouterIntegrationTest: ok\n");
    return 0;
}
