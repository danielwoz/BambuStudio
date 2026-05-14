// Bambu Bridge — E2E harness empty-endpoints sentry (phase 12).
//
// The real e2e_<model>_print binaries only build with -DBAMBU_BRIDGE_E2E=ON
// AND only run meaningfully when BAMBU_BRIDGE_E2E_PRINTERS is populated.
// In the standard configure, neither holds — which means the E2EHarness
// code could quietly bit-rot. This sentry test exists to catch that:
// it ALWAYS builds (regardless of -DBAMBU_BRIDGE_E2E) and exercises the
// empty-endpoint contract:
//
//   - E2EHarness({}).start() returns false without spawning a process.
//   - ports_for("anything") returns nullopt.
//   - stop() on a never-started harness is a no-op (no crash, no leak).
//
// It does NOT spawn the daemon, does NOT touch sockets, and exits 0 on
// a host with no real printers anywhere near it.

#include "e2e/E2EHarness.hpp"

#include <chrono>
#include <cstdio>

namespace {
int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}
} // namespace

int main() {
    using namespace Slic3r::bridge::e2e;

    // Empty endpoints: start() must short-circuit to false without
    // touching fork/exec/pipes. We give it a tight timeout so any
    // future regression that DOES spawn is noticed at test time.
    E2EHarness harness({});
    const bool started = harness.start(std::chrono::seconds(2));
    check(started == false,
          "E2EHarness({}).start() returns false (empty endpoints)");
    check(harness.ports_for("h2s") == std::nullopt,
          "ports_for() on never-bound harness yields nullopt");
    check(harness.ports_for("anything") == std::nullopt,
          "ports_for() with unknown model yields nullopt");

    // stop() must be a clean no-op when start() never succeeded.
    harness.stop();
    check(true, "stop() on a never-started harness did not throw");

    // Second start() with still-empty endpoints must remain false (no
    // state-machine surprise from the previous attempt).
    check(harness.start(std::chrono::seconds(2)) == false,
          "repeat E2EHarness({}).start() still returns false");

    if (g_fails == 0) {
        std::fprintf(stderr, "E2EHarnessSkipTest: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr,
        "E2EHarnessSkipTest: %d FAILURE(S)\n", g_fails);
    return 1;
}
