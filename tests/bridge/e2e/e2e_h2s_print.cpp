// Bambu Bridge — E2E real-printer test: H2S (phase 12).
//
// Needs a real H2S reachable on the LAN and configured in
// $BAMBU_BRIDGE_E2E_PRINTERS as "h2s@<ip>[:<access-code>]".
// Skips (rc 77) on any host where that endpoint is absent.

#include "e2e_common.hpp"

int main() {
    using namespace Slic3r::bridge::e2e;
    return run_print_test("h2s", ModelFamily::H2Like);
}
