// Bambu Bridge — E2E real-printer test: A1 mini (phase 12).
//
// Needs a real A1 mini reachable on the LAN and configured in
// $BAMBU_BRIDGE_E2E_PRINTERS as "a1mini@<ip>[:<access-code>]".
// Skips (rc 77) on any host where that endpoint is absent.

#include "e2e_common.hpp"

int main() {
    using namespace Slic3r::bridge::e2e;
    return run_print_test("a1mini", ModelFamily::A1Like);
}
