// Bambu Bridge — CloudInventory unit test (phase 1).
//
// Exercises the absent-plugin code path: pointing the inventory at a path
// that doesn't exist must NOT crash, must report `refresh()` failure, and
// must leave the cached snapshot empty. This is the safety contract every
// phase past 1 relies on (a missing proprietary plugin must not take the
// bridge process down).
//
// Real cloud round-trips are exercised by phase 12's E2E suite, which is
// gated behind a separate -DBAMBU_BRIDGE_E2E=ON flag and never runs in
// the default ctest set.

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "CloudInventory.hpp"
#include "BridgeService.hpp"

int main() {
    using namespace Slic3r::bridge;

    // Make sure the env var doesn't accidentally redirect us to a real
    // plugin on the developer machine running this test.
#if defined(_WIN32)
    _putenv_s("BAMBU_BRIDGE_PLUGIN_PATH", "");
#else
    ::unsetenv("BAMBU_BRIDGE_PLUGIN_PATH");
#endif

    CloudInventoryConfig cfg;
    cfg.plugin_path = "/nonexistent/libbambu_networking.so";

    CloudInventory inv(cfg);

    const bool refreshed = inv.refresh();
    if (refreshed) {
        std::fprintf(stderr, "FAIL: refresh() with bogus plugin path returned true\n");
        return 1;
    }
    if (inv.plugin_loaded()) {
        std::fprintf(stderr, "FAIL: plugin_loaded() true with bogus path\n");
        return 1;
    }
    auto snap = inv.snapshot();
    if (!snap.empty()) {
        std::fprintf(stderr,
                     "FAIL: snapshot() with bogus plugin path returned %zu devices\n",
                     snap.size());
        return 1;
    }

    // probe_lan_reachability must be safe to call even on an empty cache.
    inv.probe_lan_reachability();

    // Wire it into BridgeService and confirm the empty-list passthrough.
    BridgeService service;
    auto inv_ptr = std::make_unique<CloudInventory>(cfg);
    service.set_cloud_inventory(std::move(inv_ptr));
    if (!service.devices().empty()) {
        std::fprintf(stderr, "FAIL: BridgeService::devices() non-empty with bogus plugin\n");
        return 1;
    }

    // Detaching the inventory must also leave devices() at empty without
    // crashing.
    service.set_cloud_inventory(nullptr);
    if (!service.devices().empty()) {
        std::fprintf(stderr, "FAIL: BridgeService::devices() non-empty after detach\n");
        return 1;
    }

    std::printf("CloudInventoryTest: ok\n");
    return 0;
}
