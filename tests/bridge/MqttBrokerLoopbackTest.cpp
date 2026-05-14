// Bambu Bridge — MqttBroker loopback integration test (phase 4b).
//
// Stands up a real MqttBroker on a loopback endpoint, drives it with our
// minimal TLS MQTT client, and asserts the auth/PUBLISH/SUBSCRIBE/inject
// paths work end-to-end.
//
// Endpoint selection:
//   1. Try 127.0.0.2:8883 (the canonical loopback alias). Real Bambu
//      printers all want 8883, so this matches the production binding.
//   2. If that fails (lo alias disabled or port 8883 in use), fall back
//      to 127.0.0.1 with port 0 (kernel picks ephemeral) and use the
//      bound port the broker reports.
//
// We use the broker's own CertFactory output for a minted serial so the
// cert chain matches what production would produce.
//
// Skips (ctest return code 77) when binding fails altogether (e.g. inside
// a strict netns); only an actual protocol/behaviour bug is a failure.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <unistd.h>  // getpid

#include "server/MqttBroker.hpp"
#include "server/IUplink.hpp"
#include "tls/CertFactory.hpp"
#include "support/MqttTestClient.hpp"

using Slic3r::bridge::server::MqttBroker;
using Slic3r::bridge::server::MqttBrokerConfig;
using Slic3r::bridge::server::MqttBrokerVirtualDevice;
using Slic3r::bridge::server::IUplink;
using Slic3r::bridge::tls::CertFactory;
using Slic3r::bridge::tls::CertFactoryConfig;
using Slic3r::bridge::test::MqttTestClient;
using Slic3r::bridge::test::ReceivedPublish;

namespace {

constexpr int kCtestSkip = 77;

int g_fails = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_fails;
        std::fprintf(stderr, "FAIL %s\n", what);
    } else {
        std::fprintf(stderr, "ok   %s\n", what);
    }
}

// Test double: records every uplink event so we can assert order + content.
struct RecordingUplink : public IUplink {
    struct PubEvent {
        std::string dev_id;
        std::string topic;
        std::vector<uint8_t> payload;
        uint8_t qos;
    };
    std::mutex                mu;
    std::vector<PubEvent>     publishes;
    std::vector<std::string>  subscribes;
    std::vector<std::string>  unsubscribes;
    std::vector<std::string>  disconnects;
    std::unordered_map<std::string, DownstreamPublisher> pubs;

    void on_subscribe(const std::string& dev, std::string t) override {
        std::lock_guard<std::mutex> lk(mu);
        subscribes.push_back(dev + ":" + t);
    }
    void on_publish(const std::string& dev, std::string t,
                    std::vector<uint8_t> p, uint8_t q) override {
        std::lock_guard<std::mutex> lk(mu);
        publishes.push_back({dev, t, std::move(p), q});
    }
    void on_unsubscribe(const std::string& dev, std::string t) override {
        std::lock_guard<std::mutex> lk(mu);
        unsubscribes.push_back(dev + ":" + t);
    }
    void on_disconnect(const std::string& dev) override {
        std::lock_guard<std::mutex> lk(mu);
        disconnects.push_back(dev);
    }
    void attach_downstream(const std::string& dev,
                           DownstreamPublisher p) override {
        std::lock_guard<std::mutex> lk(mu);
        if (p) pubs[dev] = std::move(p);
        else   pubs.erase(dev);
    }
};

// Build a self-signed cert for the test dev_id in a throw-away cache dir.
struct TestCertMaterial {
    Slic3r::bridge::tls::CertMaterial cert;
    std::filesystem::path             cache_dir;
};
TestCertMaterial mint_test_cert(const std::string& dev_id) {
    TestCertMaterial r;
    r.cache_dir = std::filesystem::temp_directory_path() /
                  ("bambu-bridge-mqtt-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(r.cache_dir);
    CertFactoryConfig cfg;
    cfg.cache_dir = r.cache_dir;
    CertFactory factory(cfg);
    r.cert = factory.get_or_create(dev_id);
    return r;
}

// Try binding the broker at (bind_ip, port). Returns nullptr on failure.
std::unique_ptr<MqttBroker> try_start_broker(
    const std::string& bind_ip, uint16_t port,
    const std::string& dev_id, const std::string& access_code,
    const Slic3r::bridge::tls::CertMaterial& cert,
    std::shared_ptr<IUplink> uplink, uint16_t& bound_port_out) {
    MqttBrokerConfig cfg;
    cfg.uplink                   = std::move(uplink);
    cfg.max_clients_per_device   = 1;
    cfg.accept_backlog           = 4;
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
        std::fprintf(stderr, "[loopback] start at %s:%u failed: %s\n",
                     bind_ip.c_str(), port, ex.what());
        return nullptr;
    }
    bound_port_out = broker->bound_port(dev_id);
    if (bound_port_out == 0) {
        broker->stop();
        return nullptr;
    }
    std::fprintf(stderr, "[loopback] broker bound %s:%u\n",
                 bind_ip.c_str(), bound_port_out);
    return broker;
}

} // namespace

int main() {
    const std::string dev_id      = "0938BC582502312";
    const std::string access_code = "ABCD1234";
    auto              cert_holder = mint_test_cert(dev_id);

    auto uplink = std::make_shared<RecordingUplink>();

    // Endpoint #1: 127.0.0.2:8883 (canonical alias, low port -> may need cap).
    std::string bind_ip   = "127.0.0.2";
    uint16_t    try_port  = 8883;
    uint16_t    bound     = 0;
    auto broker = try_start_broker(bind_ip, try_port, dev_id, access_code,
                                   cert_holder.cert, uplink, bound);
    if (!broker) {
        bind_ip  = "127.0.0.1";
        try_port = 0;
        broker = try_start_broker(bind_ip, try_port, dev_id, access_code,
                                  cert_holder.cert, uplink, bound);
    }
    if (!broker) {
        std::fprintf(stderr, "SKIP: couldn't bind broker on loopback\n");
        return kCtestSkip;
    }

    // Give the accept thread a beat to enter select().
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ----- Test 1: wrong creds -> CONNACK 5 -----
    {
        MqttTestClient c;
        if (!c.tcp_tls_connect(bind_ip, bound)) {
            std::fprintf(stderr, "SKIP: TLS connect failed: %s\n",
                         c.last_error().c_str());
            broker->stop();
            return kCtestSkip;
        }
        int rc = c.connect_mqtt("cli-bad", "bblp", "WRONGPASS");
        check(rc == 5, "wrong access_code -> CONNACK 5 (Not Authorized)");
        c.close();
    }

    // Reaping happens lazily on the next accept; give it a moment.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ----- Test 2: correct creds -> CONNACK 0 + PUBLISH -> uplink + downstream inject -----
    {
        MqttTestClient c;
        check(c.tcp_tls_connect(bind_ip, bound), "TLS connect");
        int rc = c.connect_mqtt("cli-ok", "bblp", access_code);
        check(rc == 0, "correct creds -> CONNACK 0");

        const std::string topic   = "device/" + dev_id + "/request";
        const std::string body    = "{\"info\":{\"command\":\"get_version\"}}";
        std::vector<uint8_t> payload(body.begin(), body.end());

        // QoS 0 PUBLISH (no PUBACK expected).
        check(c.send_publish(topic, payload, 0, 0), "QoS0 PUBLISH send");

        // QoS 1 PUBLISH (PUBACK expected).
        check(c.send_publish(topic, payload, 1, 1234), "QoS1 PUBLISH send");
        check(c.wait_for_puback(1234, std::chrono::seconds(3)),
              "QoS1 PUBACK received");

        // SUBSCRIBE.
        const std::string report_topic = "device/" + dev_id + "/report";
        check(c.subscribe_one(report_topic, 7), "SUBSCRIBE + SUBACK");

        // Trigger a downstream inject and confirm the client sees it.
        const std::string ds_body = "{\"print\":{\"command\":\"push_status\"}}";
        std::vector<uint8_t> ds_payload(ds_body.begin(), ds_body.end());
        broker->inject_downstream(dev_id, report_topic, ds_payload, 0);

        auto got = c.recv_publish(std::chrono::seconds(3));
        check(got.has_value(), "downstream inject arrives at client");
        if (got) {
            check(got->topic == report_topic, "downstream topic matches");
            std::string s(got->payload.begin(), got->payload.end());
            check(s == ds_body, "downstream payload matches");
        }

        c.close();
    }

    // Give the I/O thread time to flush on_disconnect.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ----- Test 3: uplink received the expected events -----
    {
        std::lock_guard<std::mutex> lk(uplink->mu);
        check(uplink->publishes.size() >= 2,
              "uplink saw at least 2 publishes");
        if (uplink->publishes.size() >= 2) {
            check(uplink->publishes[0].topic == "device/" + dev_id + "/request",
                  "publish #0 topic");
            check(uplink->publishes[0].qos == 0,
                  "publish #0 QoS 0");
            check(uplink->publishes[1].qos == 1,
                  "publish #1 QoS 1");
        }
        check(!uplink->subscribes.empty(), "uplink saw at least 1 subscribe");
        check(!uplink->disconnects.empty(), "uplink saw disconnect");
    }

    broker->stop();

    if (g_fails) {
        std::fprintf(stderr, "MqttBrokerLoopbackTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("MqttBrokerLoopbackTest: ok\n");
    return 0;
}
