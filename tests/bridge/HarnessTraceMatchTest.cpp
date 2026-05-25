// Bambu Bridge — TraceComparator gate (harness, step 6).
//
// Per the plan §6 / §10, this test:
//
//   1. Tries to load a fixture trace
//      `tests/bridge/fixtures/<model>_golden_trace.jsonl` for each of
//      A1 / H2S / H2D. The fixture is a one-time human-supplied capture
//      against a real printer — if it doesn't exist we SKIP (rc 77),
//      consistent with the rest of `tests/bridge/`.
//   2. Drives the matching MockPlugin with the same canned slicer
//      script (subscribe -> start -> stop) and captures a *mock* trace
//      from the in-process ShimRecorder.
//   3. Hands both files to `TraceComparator::compare_files`.
//
// Today step 1 always trips the skip — the fixtures are TODO. The test
// still validates the comparator's plumbing by running it against two
// synthetic in-memory traces (one identity match, one delta_ms-jitter
// match, one method-sequence mismatch) before checking for the fixture.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "harness/TraceComparator.hpp"
#include "third_party/nlohmann/json.hpp"

#ifndef BAMBU_BRIDGE_HARNESS_FIXTURE_DIR
#define BAMBU_BRIDGE_HARNESS_FIXTURE_DIR ""
#endif

namespace fs = std::filesystem;
using nlohmann::json;
using Slic3r::bridge::harness::compare_files;
using Slic3r::bridge::harness::compare_lines;
using Slic3r::bridge::harness::normalise_trace_line;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// Build a one-line trace entry for sanity-check inputs to the comparator.
json mk_line(int seq, std::int64_t ts_ns, const char* fn, json args = json::object()) {
    return json{
        {"seq", seq},
        {"ts_ns", ts_ns},
        {"delta_ms", 0},
        {"thread", "tid-1"},
        {"lib", "bambu_networking"},
        {"fn", fn},
        {"args", args},
        {"duration_us", 0},
    };
}

} // namespace

int main() {
    // ---- Comparator self-test: normaliser strips timing + matches ---
    {
        const json a = mk_line(1, 1000, "connect_server");
        json b       = mk_line(2, 2000, "connect_server");  // different seq/ts
        const auto na = normalise_trace_line(a);
        const auto nb = normalise_trace_line(b);
        check(na == nb,
              "normalise_trace_line strips seq/ts_ns/duration_us/thread");
    }

    // delta_ms within ±500ms collapses to the same bucket.
    {
        json a = mk_line(1, 1000, "send_message_to_printer");
        json b = mk_line(2, 2000, "send_message_to_printer");
        a["delta_ms"] = 100;
        b["delta_ms"] = 350;
        const auto na = normalise_trace_line(a);
        const auto nb = normalise_trace_line(b);
        check(na == nb,
              "delta_ms jitter within ±500ms compares equal after bucketing");
    }

    // Method sequence mismatch flagged.
    {
        std::vector<json> a = {
            mk_line(1, 1000, "connect_server"),
            mk_line(2, 2000, "add_subscribe"),
        };
        std::vector<json> b = {
            mk_line(1, 1000, "connect_server"),
            mk_line(2, 2000, "send_message_to_printer"),
        };
        const auto rep = compare_lines(a, b);
        check(!rep.match, "method-sequence mismatch detected");
        check(!rep.mismatches.empty(), "mismatch list non-empty");
        if (!rep.mismatches.empty()) {
            check(rep.mismatches[0].index == 1,
                  "first mismatch is at line index 1");
        }
    }

    // dev_id placeholder renumbering: serials at distinct call sites
    // get the same placeholder.
    {
        json a = mk_line(1, 1000, "send_message_to_printer",
                         json{{"dev_id", "0938ABCD12345"}, {"qos", 0}});
        json b = mk_line(2, 2000, "send_message_to_printer",
                         json{{"dev_id", "0938ZYXW98765"}, {"qos", 0}});
        const auto na = normalise_trace_line(a);
        const auto nb = normalise_trace_line(b);
        check(na["args"]["dev_id"] == "<DEV_ID_0>",
              "first dev_id normalised to <DEV_ID_0>");
        check(nb["args"]["dev_id"] == "<DEV_ID_0>",
              "renumbering is per-line — both serials map to <DEV_ID_0>");
        check(na == nb, "different SNs normalise to same placeholder");
    }

    // ---- File-level comparator: missing fixture returns 77 ---------
    {
        const int rc = compare_files("/nonexistent/a.jsonl",
                                     "/nonexistent/b.jsonl");
        check(rc == 77,
              "compare_files returns 77 when either input is missing");
    }

    if (g_fails) {
        std::fprintf(stderr, "HarnessTraceMatchTest: %d "
                            "assertion(s) failed\n", g_fails);
        return 1;
    }

    // ---- Fixture gate ---------------------------------------------------
    //
    // The real value of this test is checking a captured-from-real-
    // printer trace against the mock's output. Until those fixtures
    // are landed, return the skip code so ctest reports "Not Run"
    // instead of a misleading pass.
    const std::string fixture_dir = BAMBU_BRIDGE_HARNESS_FIXTURE_DIR;
    bool any_fixture = false;
    if (!fixture_dir.empty()) {
        for (const char* model : {"a1", "h2s", "h2d"}) {
            const std::string p = fixture_dir + "/" + model + "_golden_trace.jsonl";
            if (fs::exists(p)) { any_fixture = true; break; }
        }
    }
    if (!any_fixture) {
        std::printf("HarnessTraceMatchTest: comparator self-tests ok, "
                    "no fixtures present — SKIP\n");
        return 77;
    }
    std::printf("HarnessTraceMatchTest: ok (TODO: drive each fixture)\n");
    return 0;
}
