// Bambu Bridge — CloudUplink unit test (phase 6).
//
// Pure no-I/O test: build a CloudUplink with no plugin handle attached,
// drive the full IUplink interface, and assert the calls return without
// crashing and is_connected() stays false. This is the safety contract
// SessionRouter (phase 9) will rely on — when the cloud agent isn't
// loaded the bridge should silently fall back to LAN.
//
// Real cloud round-trips are exercised by CloudUplinkLoopbackTest (which
// uses a MockPluginHandle) and ultimately phase-12's E2E suite.

#include <cstdio>
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

} // namespace

int main() {
    using namespace Slic3r::bridge;
    using router::CloudUplink;
    using router::CloudUplinkConfig;

    const std::string dev_id = "EXAMPLESERIAL01";

    CloudUplink up;

    // No plugin attached at all.
    check(up.plugin_handle() == nullptr,
          "plugin_handle() null before attach_plugin");
    check(!up.is_connected(dev_id),
          "is_connected() false without a plugin");

    CloudUplinkConfig cfg;
    cfg.dev_id      = dev_id;
    cfg.access_code = "ACCESS123";
    up.add_device(cfg);

    // Adding a device without a plugin should still not crash + still
    // report disconnected (the device is registered but unreachable).
    check(!up.is_connected(dev_id),
          "is_connected() still false after add_device w/o plugin");

    // Exercise the full IUplink surface; everything should be a no-op.
    const std::string topic_report = "device/" + dev_id + "/report";
    const std::string topic_req    = "device/" + dev_id + "/request";

    up.on_subscribe (dev_id, topic_report);
    up.on_subscribe (dev_id, topic_report); // dup — refcount
    up.on_publish   (dev_id, topic_req,
                     std::vector<uint8_t>{'{','}','\0'}, 0);
    up.on_unsubscribe(dev_id, topic_report);
    up.on_unsubscribe(dev_id, topic_report);

    // attach_downstream / on_disconnect with no plugin must be safe.
    bool downstream_invoked = false;
    up.attach_downstream(dev_id,
        [&](std::string, std::vector<uint8_t>, uint8_t) {
            downstream_invoked = true;
        });
    up.on_disconnect(dev_id);
    check(!downstream_invoked,
          "no spurious downstream invocations without a plugin");

    // Detach via null publisher.
    up.attach_downstream(dev_id, nullptr);

    // remove_device must be safe.
    up.remove_device(dev_id);

    // Attaching a real (but plugin-less) handle keeps everything safe.
    auto handle = std::make_shared<BambuNetworkingPluginHandle>();
    up.attach_plugin(handle);
    check(up.plugin_handle() == handle,
          "plugin_handle() returns the attached handle");
    check(!up.is_connected(dev_id),
          "is_connected() still false against an uninitialized handle");

    // Re-add and re-publish — handle has no agent, so publish is a
    // no-op at the plugin layer, but CloudUplink must not crash.
    up.add_device(cfg);
    up.on_publish(dev_id, topic_req,
                  std::vector<uint8_t>{'p','i','n','g'}, 0);
    up.remove_device(dev_id);

    // Detach via nullptr.
    up.attach_plugin(nullptr);
    check(up.plugin_handle() == nullptr,
          "attach_plugin(nullptr) detaches");

    if (g_fails) {
        std::fprintf(stderr, "CloudUplinkUnitTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("CloudUplinkUnitTest: ok\n");
    return 0;
}
