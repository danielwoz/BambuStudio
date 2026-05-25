// Bambu Bridge — TraceComparator (harness).
//
// Compares two JSONL trace files emitted by ShimRecorder. The intent:
//
//   * `golden.jsonl` — recorded once against a real A1 / H2S / H2D
//     printer using a fixed slicer script.
//   * `mock.jsonl`   — recorded against the in-process MockPlugin
//     running the same slicer script.
//
// "Match" is defined as identical method sequence, identical argument
// shape and value (after applying per-fn normalisers — timing fields
// dropped, dev_id placeholders renumbered, sequence_id stripped, etc.),
// and identical return values. See test_harness_plan.md §6 for the
// full normaliser table.
//
// Two entry points:
//   - `Result compare_files(path_a, path_b)` returns 0 on match, 1 on
//     mismatch, and 77 when either input is missing (the ctest skip
//     convention).
//   - `Report compare_lines(a, b)` returns a structured diff so test
//     callers can stream a human-readable explanation.

#ifndef SLIC3R_BAMBU_BRIDGE_HARNESS_TRACE_COMPARATOR_HPP
#define SLIC3R_BAMBU_BRIDGE_HARNESS_TRACE_COMPARATOR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

namespace Slic3r {
namespace bridge {
namespace harness {

struct TraceMismatch {
    std::int64_t index    = -1;     // line index (0-based) of the divergence
    std::string  reason;            // human-readable explanation
    nlohmann::json a_line;          // the offending line from trace A
    nlohmann::json b_line;          // the offending line from trace B
};

struct TraceReport {
    bool        match  = false;
    std::size_t a_len  = 0;
    std::size_t b_len  = 0;
    std::vector<TraceMismatch> mismatches;
};

// Normalise a single TraceLine-shaped JSON object for diffing. Drops
// timing fields (`ts_ns`, `seq`, `duration_us`), buckets `delta_ms`
// into ±500ms windows, and renumbers `dev_id` mentions to stable
// placeholders. Returns the normalised JSON.
nlohmann::json normalise_trace_line(const nlohmann::json& line);

// Compare two parsed line-vectors and return a structured report. The
// inputs are expected to be the result of parsing each '\n'-terminated
// JSON object from a JSONL file (one entry per line).
TraceReport compare_lines(const std::vector<nlohmann::json>& a,
                          const std::vector<nlohmann::json>& b);

// File-level entry point. Returns:
//   0 — traces match
//   1 — traces diverge (first mismatch is logged to stderr)
//  77 — one or both input files don't exist (ctest "Not Run")
//   2 — a file exists but couldn't be parsed
int compare_files(const std::string& path_a, const std::string& path_b);

} // namespace harness
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_HARNESS_TRACE_COMPARATOR_HPP
