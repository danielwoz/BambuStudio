// Bambu Bridge — phase 0 smoke test.
//
// Construct the (empty) BridgeService, let it destruct, return 0. This proves
// the bambu_bridge static library compiles and links under -DBAMBU_BRIDGE=ON.

#include "BridgeService.hpp"

int main() {
    Slic3r::bridge::BridgeService service;
    (void)service;
    return 0;
}
