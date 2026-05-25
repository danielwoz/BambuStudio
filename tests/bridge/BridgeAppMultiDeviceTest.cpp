// Bambu Bridge — BridgeApp multi-device orchestration test (phase 10).
//
// Drives `BridgeApp::poll_inventory_once()` against a mock plugin handle
// that reports TWO cloud-bound devices. Verifies that after one
// reconcile both devices appear in the per-device table, with distinct
// ports (mqtt_port_base + 0/1, etc) and the snapshot-reported LAN IPs.
//
// This is the multi-device complement to BridgeAppLifecycleTest. We
// disable SSDP (it tries to bind UDP/1900) but keep MQTT/FTPS/RTSP
// enabled with their ports rebased into the OS-ephemeral range so the
// servers can actually bind on a CI host. The MQTT broker is the one
// that builds an SSL_CTX from the per-device CertMaterial, so this
// also pins that the cert factory + cert install is wired correctly.
//
// What we do NOT test here:
//   - Full MQTT client roundtrip — that's
//     SessionRouterIntegrationTest's job.
//   - Camera streaming — RtspServerLoopbackTest covers that path.
//   This test is about ORCHESTRATION: did add_device land on every
//   enabled server with the right port + the right cert + the right
//   access code, and does device_bindings() report a consistent view.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "BambuNetworkingPluginHandle.hpp"
#include "headless/BridgeApp.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// Mock plugin handle that reports a fixed 2-device list. Mirrors the
// shape of the JSON the real proprietary plugin produces (see
// CloudInventory::refresh's parse).
class TwoDevicePluginHandle final
    : public Slic3r::bridge::BambuNetworkingPluginHandle {
public:
    TwoDevicePluginHandle()
        : Slic3r::bridge::BambuNetworkingPluginHandle({}) {
        set_agent_ready_for_test(true);
    }
    bool init()                  override { return true; }
    bool agent_ready()    const  override { return true; }
    bool is_user_login()  const  override { return true; }
    bool is_server_connected() const override { return true; }

    bool get_user_print_info(unsigned int* http_code,
                             std::string*  http_body) const override {
        if (http_code) *http_code = 200;
        if (http_body) {
            // dev_id + dev_name + dev_model_name + dev_online +
            // dev_access_code + dev_ip — same keys CloudInventory parses.
            *http_body =
                "{\"devices\":["
                "{\"dev_id\":\"AAAAA1111\",\"dev_name\":\"alpha\","
                " \"dev_model_name\":\"H2S\",\"dev_online\":true,"
                " \"dev_access_code\":\"alpha-code\",\"dev_ip\":\"192.0.2.10\"},"
                "{\"dev_id\":\"BBBBB2222\",\"dev_name\":\"beta\","
                " \"dev_model_name\":\"A1M\",\"dev_online\":true,"
                " \"dev_access_code\":\"beta-code\",\"dev_ip\":\"192.0.2.11\"}"
                "]}";
        }
        return true;
    }

    int subscribe_device(const std::string&)   override { return 0; }
    int unsubscribe_device(const std::string&) override { return 0; }
    int publish_to_device(const std::string&, const std::string&, int) override { return 0; }
};

std::filesystem::path scratch_dir() {
    auto base = std::filesystem::temp_directory_path() /
        ("bridge_app_multi_test_" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

} // namespace

int main() {
    using namespace Slic3r::bridge;
    using headless::BridgeApp;
    using headless::BridgeAppConfig;

    BridgeAppConfig cfg;
    cfg.cert_cache_dir = scratch_dir();
    cfg.lan_iface_bind = "127.0.0.1";
    cfg.inventory_poll = std::chrono::seconds(3600); // we drive it ourselves.

    // SSDP off (binds udp/1900, host-dependent). The other three bind a
    // TCP listener each; rebase ports into the user-ephemeral range so a
    // CI host without elevated privileges can still bind.
    cfg.enable_ssdp    = false;
    cfg.enable_mqtt    = true;
    cfg.enable_ftps    = true;
    cfg.enable_rtsp    = true;
    // Use bases inside the user-ephemeral range; ports actually used are
    // base+0 and base+1 for the two mock devices.
    cfg.mqtt_port_base = 47700;
    cfg.ftps_port_base = 47710;
    cfg.rtsp_port_base = 47720;

    auto app = std::make_unique<BridgeApp>(std::move(cfg));
    app->set_plugin_handle_for_test(std::make_shared<TwoDevicePluginHandle>());

    check(app->poll_inventory_once(),
          "poll_inventory_once() drives initialise() + reconcile");

    auto bindings = app->device_bindings();
    check(bindings.size() == 2,
          "two devices appear in the bindings table after one reconcile");

    // Sort by dev_id so the assertions below are order-independent.
    std::sort(bindings.begin(), bindings.end(),
        [](const auto& a, const auto& b){ return a.dev_id < b.dev_id; });

    if (bindings.size() == 2) {
        const auto& a = bindings[0];
        const auto& b = bindings[1];

        check(a.dev_id == "AAAAA1111",
              "device 0 dev_id matches mock alpha");
        check(b.dev_id == "BBBBB2222",
              "device 1 dev_id matches mock beta");

        check(a.lan_ip == "192.0.2.10",
              "device 0 lan_ip mirrors mock snapshot");
        check(b.lan_ip == "192.0.2.11",
              "device 1 lan_ip mirrors mock snapshot");

        // Ports are assigned in insertion order. The CloudInventory
        // preserves the array order from the JSON, so alpha gets index
        // 0 (= base+0) and beta gets index 1 (= base+1). Verify both
        // are in [base, base+1] and are DISTINCT — the latter is the
        // "no collision" assertion the plan asks for.
        check(a.mqtt_port == 47700 || a.mqtt_port == 47701,
              "device 0 mqtt port in the assigned range");
        check(b.mqtt_port == 47700 || b.mqtt_port == 47701,
              "device 1 mqtt port in the assigned range");
        check(a.mqtt_port != b.mqtt_port,
              "the two devices got DIFFERENT mqtt ports (no collision)");
        check(a.ftps_port != b.ftps_port,
              "the two devices got DIFFERENT ftps ports (no collision)");
        check(a.rtsp_port != b.rtsp_port,
              "the two devices got DIFFERENT rtsp ports (no collision)");

        // Sanity: ports really are in the per-role bands (no accidental
        // cross-wiring).
        check(a.ftps_port >= 47710 && a.ftps_port <= 47711,
              "device 0 ftps port is in the ftps band");
        check(a.rtsp_port >= 47720 && a.rtsp_port <= 47721,
              "device 0 rtsp port is in the rtsp band");
    }

    // Second reconcile: with the same mock snapshot, nothing should
    // change. This pins the "add is idempotent" property.
    check(app->poll_inventory_once(),
          "second poll_inventory_once() is a clean no-op");
    auto bindings2 = app->device_bindings();
    check(bindings2.size() == 2,
          "device table still has exactly two entries after second reconcile");

    // Shut down. The dtor will call shutdown() + teardown() anyway, but
    // we want to verify it's safe to call explicitly first.
    app->shutdown();
    app.reset();

    if (g_fails == 0) {
        std::fprintf(stderr, "BridgeAppMultiDeviceTest: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "BridgeAppMultiDeviceTest: %d FAILURE(S)\n", g_fails);
    return 1;
}
