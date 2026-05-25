// Bambu Bridge — Tier 2 command builders (impl).
//
// String-only JSON construction. The payloads we shape here are tiny,
// flat, and 100% known shape — there's no benefit to pulling in a full
// JSON library + recompile cost for callers. We keep escaping minimal:
// the only string values are well-known enums ("enable"/"disable",
// "stainless_steel", etc.) and module names from the OpenBambuAPI
// catalog. Callers should not pass attacker-controlled strings (none of
// these commands has a free-form string parameter anyway).

#include "Tier2Commands.hpp"

#include <cstdio>
#include <string>

namespace Slic3r {
namespace bridge {
namespace tier2 {

namespace {

// Render the sequence_id field. Empty input → "0" per OpenBambuAPI
// convention. We quote it as a string (matches every example in
// mqtt.md).
std::string seq_or_default(const std::string& seq) {
    return seq.empty() ? std::string("0") : seq;
}

// Format a double like the slicer / firmware does: short, no trailing
// zeros, no scientific notation. 0.4 → "0.4"; 0.6 → "0.6"; 0.25 → "0.25".
// Matches the canonical examples in OpenBambuAPI/mqtt.md.
std::string fmt_double(double v) {
    char buf[32];
    // %g picks the shorter of %e / %f, which gives us "0.4" not "0.400000".
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}

} // namespace

// ---- system.* ----------------------------------------------------------

std::string system_ledctrl(const std::string& sequence_id,
                           const std::string& led_node,
                           const std::string& led_mode,
                           int led_on_time_ms,
                           int led_off_time_ms,
                           int loop_times,
                           int interval_time_ms) {
    std::string s;
    s.reserve(256);
    s += "{\"system\":{";
    s += "\"sequence_id\":\"" + seq_or_default(sequence_id) + "\",";
    s += "\"command\":\"ledctrl\",";
    s += "\"led_node\":\"" + led_node + "\",";
    s += "\"led_mode\":\"" + led_mode + "\",";
    s += "\"led_on_time\":"   + std::to_string(led_on_time_ms)   + ",";
    s += "\"led_off_time\":"  + std::to_string(led_off_time_ms)  + ",";
    s += "\"loop_times\":"    + std::to_string(loop_times)       + ",";
    s += "\"interval_time\":" + std::to_string(interval_time_ms);
    s += "}}";
    return s;
}

std::string system_get_access_code(const std::string& sequence_id) {
    std::string s;
    s.reserve(96);
    s += "{\"system\":{";
    s += "\"sequence_id\":\"" + seq_or_default(sequence_id) + "\",";
    s += "\"command\":\"get_access_code\"";
    s += "}}";
    return s;
}

std::string system_set_accessories_nozzle(const std::string& sequence_id,
                                          const std::string& nozzle_type,
                                          double             nozzle_diameter) {
    std::string s;
    s.reserve(192);
    s += "{\"system\":{";
    s += "\"sequence_id\":\"" + seq_or_default(sequence_id) + "\",";
    s += "\"accessory_type\":\"nozzle\",";
    s += "\"command\":\"set_accessories\",";
    s += "\"nozzle_diameter\":" + fmt_double(nozzle_diameter) + ",";
    s += "\"nozzle_type\":\"" + nozzle_type + "\"";
    s += "}}";
    return s;
}

// ---- camera.* ----------------------------------------------------------

std::string camera_ipcam_record_set(const std::string& sequence_id,
                                    const std::string& control) {
    std::string s;
    s.reserve(128);
    s += "{\"camera\":{";
    s += "\"sequence_id\":\"" + seq_or_default(sequence_id) + "\",";
    s += "\"command\":\"ipcam_record_set\",";
    s += "\"control\":\"" + control + "\"";
    s += "}}";
    return s;
}

std::string camera_ipcam_timelapse(const std::string& sequence_id,
                                   const std::string& control) {
    std::string s;
    s.reserve(128);
    s += "{\"camera\":{";
    s += "\"sequence_id\":\"" + seq_or_default(sequence_id) + "\",";
    s += "\"command\":\"ipcam_timelapse\",";
    s += "\"control\":\"" + control + "\"";
    s += "}}";
    return s;
}

// ---- xcam.* ------------------------------------------------------------

std::string xcam_control_set(const std::string& sequence_id,
                             const std::string& module_name,
                             bool               control,
                             bool               print_halt) {
    std::string s;
    s.reserve(192);
    s += "{\"xcam\":{";
    s += "\"sequence_id\":\"" + seq_or_default(sequence_id) + "\",";
    s += "\"command\":\"xcam_control_set\",";
    s += "\"module_name\":\"" + module_name + "\",";
    s += std::string("\"control\":") + (control ? "true" : "false") + ",";
    s += std::string("\"print_halt\":") + (print_halt ? "true" : "false");
    s += "}}";
    return s;
}

} // namespace tier2
} // namespace bridge
} // namespace Slic3r
