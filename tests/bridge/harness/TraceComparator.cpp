// Bambu Bridge — TraceComparator implementation (harness).

#include "TraceComparator.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace Slic3r {
namespace bridge {
namespace harness {

namespace {

// Drop fields that vary run-to-run by design. They don't carry
// behavioural meaning — only timing / monotonic counters.
const char* kDropKeys[] = {
    "ts_ns",
    "seq",
    "duration_us",
    "thread",
};

// `delta_ms` is preserved but bucketed to the nearest 1s — within
// ±500ms scheduling jitter on either side collapses to the same bucket.
// Plan §6 says "within ±500 ms"; floor-to-1s achieves that for values
// that share an integer-second floor, which is the common case under
// loopback timing.
std::int64_t bucket_delta_ms(std::int64_t v) {
    if (v <= 0) return 0;
    constexpr std::int64_t kBucket = 1000;   // 1 second
    return (v / kBucket) * kBucket;
}

// Bambu serial-number style regex: 15 alnum chars, no separators.
// dev_id placeholders renumbered by first appearance.
struct DevIdRenumberer {
    std::unordered_map<std::string, std::string> mapping;
    std::string canonical(const std::string& sn) {
        auto it = mapping.find(sn);
        if (it != mapping.end()) return it->second;
        const std::string placeholder =
            "<DEV_ID_" + std::to_string(mapping.size()) + ">";
        mapping[sn] = placeholder;
        return placeholder;
    }
};

bool looks_like_sn(const std::string& s) {
    if (s.size() < 12 || s.size() > 20) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)))) return false;
    // Bambu serials all start with "0" + 3 model digits.
    return s[0] == '0';
}

// Walk a JSON value recursively, normalising any string that looks
// like a serial number and any "sequence_id" / "token" / "request_id"
// field. Idempotent.
void normalise_value_inplace(nlohmann::json& v, DevIdRenumberer& devs) {
    if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            const std::string& key = it.key();
            if (key == "sequence_id" || key == "token"
                || key == "request_id") {
                it.value() = "<" + key + ">";
                continue;
            }
            if (key == "created_at" || key == "report_time") {
                it.value() = "<TS>";
                continue;
            }
            if (key == "mc_remaining_time" && it.value().is_number()) {
                // Bucket to 5-minute slabs so trace jitter doesn't
                // break replay-vs-real diffs.
                const std::int64_t v_min =
                    it.value().get<std::int64_t>();
                it.value() = (v_min / 5) * 5;
                continue;
            }
            // Recurse.
            normalise_value_inplace(it.value(), devs);
        }
        return;
    }
    if (v.is_array()) {
        for (auto& e : v) normalise_value_inplace(e, devs);
        return;
    }
    if (v.is_string()) {
        const std::string& s = v.get_ref<const std::string&>();
        // The push_status payload is a string-encoded JSON blob; parse
        // and normalise recursively, then re-serialise.
        if (!s.empty() && (s.front() == '{' || s.front() == '[')) {
            auto parsed = nlohmann::json::parse(s, nullptr, false);
            if (!parsed.is_discarded()) {
                normalise_value_inplace(parsed, devs);
                v = parsed.dump();
                return;
            }
        }
        if (looks_like_sn(s)) {
            v = devs.canonical(s);
        }
    }
}

} // namespace

nlohmann::json normalise_trace_line(const nlohmann::json& line) {
    nlohmann::json out = line;
    for (const char* k : kDropKeys) out.erase(k);
    if (out.contains("delta_ms") && out["delta_ms"].is_number()) {
        out["delta_ms"] = bucket_delta_ms(out["delta_ms"].get<std::int64_t>());
    }
    DevIdRenumberer devs;
    normalise_value_inplace(out, devs);
    return out;
}

TraceReport compare_lines(const std::vector<nlohmann::json>& a,
                          const std::vector<nlohmann::json>& b) {
    TraceReport rep;
    rep.a_len = a.size();
    rep.b_len = b.size();
    rep.match = true;

    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto na = normalise_trace_line(a[i]);
        const auto nb = normalise_trace_line(b[i]);
        if (na == nb) continue;
        rep.match = false;
        TraceMismatch m;
        m.index   = static_cast<std::int64_t>(i);
        // Pinpoint the first differing top-level key for nicer output.
        std::string why = "line " + std::to_string(i) + " differs";
        if (na.is_object() && nb.is_object()) {
            for (auto it = na.begin(); it != na.end(); ++it) {
                if (!nb.contains(it.key()) || nb[it.key()] != it.value()) {
                    why += " at key '" + it.key() + "'";
                    break;
                }
            }
        }
        m.reason = std::move(why);
        m.a_line = na;
        m.b_line = nb;
        rep.mismatches.push_back(std::move(m));
        if (rep.mismatches.size() >= 5) break;  // cap noise
    }
    if (a.size() != b.size()) {
        rep.match = false;
        TraceMismatch m;
        m.index  = static_cast<std::int64_t>(n);
        m.reason = "trace length mismatch: a=" + std::to_string(a.size()) +
                   " b=" + std::to_string(b.size());
        rep.mismatches.push_back(std::move(m));
    }
    return rep;
}

int compare_files(const std::string& path_a, const std::string& path_b) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path_a, ec) || !fs::exists(path_b, ec)) {
        std::fprintf(stderr,
                     "TraceComparator: input missing (a=%s exists=%d, "
                     "b=%s exists=%d) — skipping\n",
                     path_a.c_str(), fs::exists(path_a, ec) ? 1 : 0,
                     path_b.c_str(), fs::exists(path_b, ec) ? 1 : 0);
        return 77;
    }

    auto parse_file = [&](const std::string& p,
                          std::vector<nlohmann::json>& out) -> bool {
        std::ifstream in(p);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded()) {
                std::fprintf(stderr,
                             "TraceComparator: bad JSON in %s\n",
                             p.c_str());
                return false;
            }
            out.push_back(std::move(j));
        }
        return true;
    };

    std::vector<nlohmann::json> a, b;
    if (!parse_file(path_a, a) || !parse_file(path_b, b)) return 2;

    const auto rep = compare_lines(a, b);
    if (rep.match) {
        std::printf("TraceComparator: %zu lines match\n", rep.a_len);
        return 0;
    }
    std::fprintf(stderr,
                 "TraceComparator: %zu mismatches (a=%zu lines, b=%zu lines)\n",
                 rep.mismatches.size(), rep.a_len, rep.b_len);
    for (const auto& m : rep.mismatches) {
        std::fprintf(stderr, "  - %s\n    a: %s\n    b: %s\n",
                     m.reason.c_str(),
                     m.a_line.dump().c_str(),
                     m.b_line.dump().c_str());
    }
    return 1;
}

} // namespace harness
} // namespace bridge
} // namespace Slic3r
