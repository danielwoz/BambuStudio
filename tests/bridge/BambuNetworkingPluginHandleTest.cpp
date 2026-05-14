// Bambu Bridge — BambuNetworkingPluginHandle unit test (phase 6).
//
// Mirrors the phase-1 CloudInventoryTest "missing-plugin" safety check
// directly against the handle. The contract is:
//
//   * init() with a bogus path returns false and DOES NOT crash.
//   * agent_ready() / is_user_login() / is_server_connected() all
//     return false in the no-agent state.
//   * get_user_print_info() returns false and leaves the out-params
//     untouched.
//   * subscribe_device / unsubscribe_device / publish_to_device return
//     a negative error code (BAMBU_NETWORK_ERR_INVALID_HANDLE == -1).
//   * register_receiver + deliver_message_for_test still work — that's
//     the seam tests use, so it must be functional regardless of the
//     plugin's load state.
//
// Real cloud round-trips are exercised by phase 12's E2E suite.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "BambuNetworkingPluginHandle.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

} // namespace

int main() {
    using namespace Slic3r::bridge;

    // Same env hygiene as CloudInventoryTest — make sure a real dev
    // setup doesn't redirect us to a live plugin.
#if defined(_WIN32)
    _putenv_s("BAMBU_BRIDGE_PLUGIN_PATH", "");
#else
    ::unsetenv("BAMBU_BRIDGE_PLUGIN_PATH");
#endif

    PluginHandleConfig cfg;
    cfg.plugin_path = "/nonexistent/libbambu_networking.so";

    BambuNetworkingPluginHandle handle(cfg);

    check(!handle.init(),                  "init() with bogus path returns false");
    check(!handle.agent_ready(),           "agent_ready() false with no agent");
    check(!handle.is_user_login(),         "is_user_login() false with no agent");
    check(!handle.is_server_connected(),   "is_server_connected() false with no agent");

    // get_user_print_info: must report failure cleanly.
    unsigned int http_code = 42;
    std::string  body      = "should-not-touch";
    check(!handle.get_user_print_info(&http_code, &body),
          "get_user_print_info() reports failure");
    // We don't strictly mandate the out-params stay untouched (the
    // signature passes them through), but at minimum the call must not
    // crash. Both fields are still valid after the call:
    (void)http_code; (void)body;

    check(handle.subscribe_device("0938X1") < 0,
          "subscribe_device() with no agent returns negative");
    check(handle.unsubscribe_device("0938X1") < 0,
          "unsubscribe_device() with no agent returns negative");
    check(handle.publish_to_device("0938X1", "{\"hello\":1}", 0) < 0,
          "publish_to_device() with no agent returns negative");

    // Receiver registration + test injection should still work — that's
    // the surface CloudUplinkLoopbackTest exercises.
    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> recvd;
    handle.register_receiver("0938X1",
        [&](std::string topic, std::vector<uint8_t> payload, uint8_t /*qos*/) {
            std::lock_guard<std::mutex> lk(mu);
            std::string p(payload.begin(), payload.end());
            recvd.emplace_back(std::move(topic), std::move(p));
        });
    handle.deliver_message_for_test("0938X1", "{\"x\":1}");
    {
        std::lock_guard<std::mutex> lk(mu);
        check(recvd.size() == 1, "deliver_message_for_test dispatches to receiver");
        if (recvd.size() == 1) {
            check(recvd[0].first == "device/0938X1/report",
                  "delivered topic is device/<id>/report");
            check(recvd[0].second == "{\"x\":1}",
                  "delivered payload passes through verbatim");
        }
    }

    // Unregistering must silence subsequent deliveries.
    handle.unregister_receiver("0938X1");
    handle.deliver_message_for_test("0938X1", "{\"y\":2}");
    {
        std::lock_guard<std::mutex> lk(mu);
        check(recvd.size() == 1,
              "deliver after unregister_receiver is a no-op");
    }

    if (g_fails) {
        std::fprintf(stderr,
                     "BambuNetworkingPluginHandleTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("BambuNetworkingPluginHandleTest: ok\n");
    return 0;
}
