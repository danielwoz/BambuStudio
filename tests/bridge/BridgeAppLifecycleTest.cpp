// Bambu Bridge — BridgeApp lifecycle test (phase 10).
//
// Two lifecycle paths covered:
//
//   1. Init-failure path: hand a deliberately-bad plugin_path. run() on
//      a worker thread; shutdown() within 500ms. Assert the worker
//      thread joins within 2s and run() returned non-zero. This pins
//      the "refuse to start if init fails" contract.
//
//   2. Clean-shutdown path: disable every server (--no-ssdp/mqtt/ftps/
//      rtsp via the config struct), inject a mock plugin handle whose
//      `init()` returns true but whose get_user_print_info returns no
//      devices. run(); poll_inventory_once() (returns true with no
//      devices); shutdown(). Assert clean rc=0.
//
// The second test exercises the "happy path" without ever binding a
// network listener — that's the lifecycle gate the daemon's
// SIGINT/SIGTERM handler depends on (signal handler -> shutdown() ->
// poll thread joins -> servers stop in order -> teardown).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
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

// MockPluginHandle: never opens the .so, claims agent_ready=true,
// returns an empty device list from get_user_print_info. The
// "clean shutdown" test uses this so initialise() succeeds without
// requiring any real cloud plumbing.
class MockEmptyPluginHandle : public Slic3r::bridge::BambuNetworkingPluginHandle {
public:
    MockEmptyPluginHandle()
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
        if (http_body) *http_body = "{\"devices\":[]}";
        return true;
    }

    int subscribe_device(const std::string&)   override { return 0; }
    int unsubscribe_device(const std::string&) override { return 0; }
    int publish_to_device(const std::string&, const std::string&, int) override { return 0; }
};

// Pick a per-test scratch dir so the cert factory doesn't dirty $HOME.
std::filesystem::path scratch_dir(const char* tag) {
    auto base = std::filesystem::temp_directory_path() /
        ("bridge_app_test_" + std::string(tag) + "_" +
         std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

} // namespace

int main() {
    using namespace Slic3r::bridge;
    using headless::BridgeApp;
    using headless::BridgeAppConfig;

    // ------------------------------------------------------------------
    // 1) Init-failure path: bad plugin_path → run() returns non-zero.
    // ------------------------------------------------------------------
    {
        BridgeAppConfig cfg;
        cfg.plugin_path     = "/nonexistent/libbambu_networking.so";
        cfg.cert_cache_dir  = scratch_dir("fail");
        // Inventory poll cadence is irrelevant here; init fails first.
        cfg.inventory_poll  = std::chrono::seconds(60);
        // Disabling everything is also irrelevant — initialise() bails
        // before the server section.
        cfg.enable_ssdp = cfg.enable_mqtt = cfg.enable_ftps = cfg.enable_rtsp = false;
        // Opt out of host-driven mode so initialise() actually tries to
        // dlopen the (deliberately-bad) plugin path; in host-driven mode
        // it would skip the dlopen entirely and init would succeed.
        cfg.host_drives_inventory = false;

        auto app = std::make_unique<BridgeApp>(std::move(cfg));

        std::promise<int> rcp;
        auto fut = rcp.get_future();
        std::thread runner([&]{ rcp.set_value(app->run()); });

        // Give run() a moment to attempt initialise(); it should bail
        // synchronously and return non-zero (in which case the thread
        // will already be done by the time we even call shutdown()).
        // Either way: shutdown() is documented as idempotent / safe.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        app->shutdown();

        const auto status = fut.wait_for(std::chrono::seconds(2));
        check(status == std::future_status::ready,
              "run() joined within 2s on init-failure path");
        if (status == std::future_status::ready) {
            const int rc = fut.get();
            check(rc != 0, "init-failure path returns non-zero rc");
        }
        if (runner.joinable()) runner.join();
    }

    // ------------------------------------------------------------------
    // 2) Clean shutdown with all servers disabled + mock plugin.
    // ------------------------------------------------------------------
    {
        BridgeAppConfig cfg;
        cfg.cert_cache_dir = scratch_dir("happy");
        cfg.inventory_poll = std::chrono::seconds(60);
        cfg.enable_ssdp    = false;
        cfg.enable_mqtt    = false;
        cfg.enable_ftps    = false;
        cfg.enable_rtsp    = false;

        auto app = std::make_unique<BridgeApp>(std::move(cfg));
        app->set_plugin_handle_for_test(std::make_shared<MockEmptyPluginHandle>());

        std::promise<int> rcp;
        auto fut = rcp.get_future();
        std::thread runner([&]{ rcp.set_value(app->run()); });

        // Wait a beat for run() to bring up the (empty) routers etc.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // One synchronous reconcile — with the mock returning an empty
        // device list this should be a clean no-op and leave the table
        // empty.
        check(app->poll_inventory_once(),
              "poll_inventory_once() runs with mock plugin");
        check(app->device_bindings().empty(),
              "no devices known after empty-snapshot reconcile");

        app->shutdown();

        const auto status = fut.wait_for(std::chrono::seconds(2));
        check(status == std::future_status::ready,
              "run() joined within 2s on clean-shutdown path");
        if (status == std::future_status::ready) {
            const int rc = fut.get();
            check(rc == 0, "clean-shutdown path returns rc=0");
        }
        if (runner.joinable()) runner.join();
    }

    if (g_fails == 0) {
        std::fprintf(stderr, "BridgeAppLifecycleTest: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "BridgeAppLifecycleTest: %d FAILURE(S)\n", g_fails);
    return 1;
}
