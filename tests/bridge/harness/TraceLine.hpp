// Bambu Bridge — TraceLine schema (harness).
//
// One JSON-per-line envelope shared by ShimRecorder (writer) and
// TraceComparator (reader). The harness records every call into the
// proprietary `libbambu_networking.so` / `libBambuSource.so` shims so
// that:
//
//   - replays can be diffed against a captured-from-real-printer fixture
//     trace to prove the in-process mocks behave equivalently;
//   - bridge engineers can read a trace by hand, since each line is
//     valid JSON and unix tools (grep, jq, wc -l) work directly on the
//     file.
//
// The schema is intentionally small and stable — keys here MUST match
// the comparator's normaliser table (see test_harness_plan.md §6). Any
// schema change is a comparator-fixture-fixture change.

#ifndef SLIC3R_BAMBU_BRIDGE_HARNESS_TRACE_LINE_HPP
#define SLIC3R_BAMBU_BRIDGE_HARNESS_TRACE_LINE_HPP

#include <cstdint>
#include <string>

#include "third_party/nlohmann/json.hpp"

namespace Slic3r {
namespace bridge {
namespace harness {

// Library tag. Carried verbatim into the JSONL `lib` field. Two values:
// the proprietary cloud/LAN plugin (`bambu_networking`) and the camera
// transport library (`bambu_source`).
enum class TraceLib {
    BambuNetworking,
    BambuSource,
};

inline const char* trace_lib_str(TraceLib lib) {
    switch (lib) {
    case TraceLib::BambuNetworking: return "bambu_networking";
    case TraceLib::BambuSource:     return "bambu_source";
    }
    return "unknown";
}

// In-memory envelope. The ShimRecorder builds one of these per shimmed
// call, fills `args` / `ret` / `ret_out_params` with whatever the call
// site provides, and serialises to a single JSONL line.
//
// `args` is a free-form JSON object whose shape is fn-specific (see
// test_harness_plan.md §4 "What the shim records"). Callbacks captured
// by-value at registration time are encoded as
// `{"_callback": "OnMessageFn", "id": <int>}` so a later invocation
// can be cross-referenced by id.
struct TraceLine {
    std::int64_t                seq           = 0;       // monotonic per-trace
    std::int64_t                ts_ns         = 0;       // wall-clock ns
    std::int64_t                delta_ms      = 0;       // since previous call
    std::string                 thread;                  // thread name / tid
    TraceLib                    lib           = TraceLib::BambuNetworking;
    std::string                 fn;                      // C function name w/o prefix
    nlohmann::json              args          = nlohmann::json::object();
    nlohmann::json              ret;                     // null/int/bool/string
    nlohmann::json              ret_out_params = nlohmann::json::object();
    std::int64_t                duration_us   = 0;
    // Only set on lines that represent a registered callback firing.
    // Unset (== -1) means "this line is a forward call, not a callback".
    std::int64_t                cb_id         = -1;

    // Serialise to a single-line JSON object terminated by '\n'.
    // Stable key order so trace fixtures diff cleanly.
    std::string to_jsonl() const {
        nlohmann::json j;
        j["seq"]      = seq;
        j["ts_ns"]    = ts_ns;
        j["delta_ms"] = delta_ms;
        j["thread"]   = thread;
        j["lib"]      = trace_lib_str(lib);
        j["fn"]       = fn;
        j["args"]     = args;
        if (!ret.is_null())
            j["ret"] = ret;
        if (!ret_out_params.empty())
            j["ret_out_params"] = ret_out_params;
        j["duration_us"] = duration_us;
        if (cb_id >= 0)
            j["cb_id"] = cb_id;
        std::string s = j.dump();
        s.push_back('\n');
        return s;
    }
};

} // namespace harness
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_HARNESS_TRACE_LINE_HPP
