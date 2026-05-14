// Bambu Bridge — shared E2E driver (phase 12).
//
// All four per-printer tests do the same thing: parse the env var, spin
// up the harness, drive a print/pause/cancel via E2EPrinterClient,
// wait for the matching status keyword. The model name parameterises:
//   - which entry in $BAMBU_BRIDGE_E2E_PRINTERS we pick,
//   - which status keywords the wait_for_status predicates accept
//     (A1/A1mini lack AMS so status payload shape differs slightly),
//   - the test executable name reported in the PASS/FAIL line.

#ifndef SLIC3R_BAMBU_BRIDGE_E2E_COMMON_HPP
#define SLIC3R_BAMBU_BRIDGE_E2E_COMMON_HPP

#include <string>

namespace Slic3r {
namespace bridge {
namespace e2e {

// Per-model status predicate flavour. The H2/H2S X1-family use BBL-style
// "RUNNING/PAUSE/IDLE" keywords; A1 / A1mini use the same surface for
// the keywords the bridge cares about, but they don't report AMS state
// so any predicate that pokes into ams.* fields would hang.
enum class ModelFamily {
    H2Like,    // h2, h2s (P1/X1 family code path on the printer)
    A1Like,    // a1, a1mini (no AMS by default)
};

// Drives one end-to-end print run for the given model tag.
// Returns:
//   0  — PASS
//   1  — FAIL (real failure: print didn't run, status never matched, ...)
//   77 — SKIP (no endpoint for this model, no access code, fixture is
//        still the stub .3mf — anything where the test can't even start)
int run_print_test(const std::string& model_tag, ModelFamily family);

} // namespace e2e
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_E2E_COMMON_HPP
