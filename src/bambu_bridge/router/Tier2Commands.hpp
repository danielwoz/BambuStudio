// Bambu Bridge — Tier 2 command builders.
//
// Per PR #61's per-class auth requirements table, three MQTT command
// classes only need the per-printer mTLS cert+key on the TLS handshake
// — they do NOT need the enc_msg signed envelope wrapper:
//
//   - `camera.*`  : ipcam_record_set, ipcam_timelapse, ...
//   - `xcam.*`    : xcam_control_set, ...
//   - `system.*`  : ledctrl, get_access_code, set_accessories, ...
//
// Because the existing Ship 1 cert+key path (raw_mqtt_publish.py) is
// already wired through `LanUplink::on_publish`, the bridge can ship
// these commands today. This module exposes typed builders that produce
// canonical JSON payloads matching OpenBambuAPI/mqtt.md schemas, so
// callers don't have to hand-shape JSON every time.
//
// Callers publish the returned JSON to `device/<dev_id>/request` on
// the bridge's MQTT broker; LanUplink will route it via cert+key. The
// printer's `/report` echoes a success / failure structure (see the
// per-command "Report" sections in OpenBambuAPI/mqtt.md).
//
// All builders take a `sequence_id` parameter so callers can correlate
// their request with the printer's reply. Pass an empty string and the
// builder will substitute "0", matching the OpenBambuAPI examples.
//
// NB: this module is intentionally string-only — no nlohmann::json or
// anything heavier. The schemas are small + flat, and we want to avoid
// dragging the third_party/ json header into bambu_bridge's public ABI.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_TIER2_COMMANDS_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_TIER2_COMMANDS_HPP

#include <string>

namespace Slic3r {
namespace bridge {
namespace tier2 {

// ---- system.* ----------------------------------------------------------

// system.ledctrl — chamber/work light control.
//   led_node: "chamber_light" or "work_light"
//   led_mode: "on", "off", or "flashing"
// For "flashing" mode the on/off/loop/interval timings matter; for
// "on"/"off" they're still required by the schema but ignored.
std::string system_ledctrl(const std::string& sequence_id,
                           const std::string& led_node,
                           const std::string& led_mode,
                           int led_on_time_ms   = 500,
                           int led_off_time_ms  = 500,
                           int loop_times       = 1,
                           int interval_time_ms = 1000);

// system.get_access_code — read-back of the LAN access code. No state
// change; safe to send anywhere as a probe.
std::string system_get_access_code(const std::string& sequence_id);

// system.set_accessories (accessory_type="nozzle") — declare which
// nozzle is installed.
//   nozzle_type: "stainless_steel" or "hardened_steel"
//   nozzle_diameter: 0.2 / 0.4 / 0.6 / 0.8
// Idempotent when re-declaring the currently-installed nozzle.
std::string system_set_accessories_nozzle(const std::string& sequence_id,
                                          const std::string& nozzle_type,
                                          double             nozzle_diameter);

// ---- camera.* ----------------------------------------------------------

// camera.ipcam_record_set — enable/disable recording prints.
//   control: "enable" or "disable"
std::string camera_ipcam_record_set(const std::string& sequence_id,
                                    const std::string& control);

// camera.ipcam_timelapse — enable/disable timelapse capture.
//   control: "enable" or "disable"
std::string camera_ipcam_timelapse(const std::string& sequence_id,
                                   const std::string& control);

// ---- xcam.* ------------------------------------------------------------

// xcam.xcam_control_set — toggle one of the XCam AI modules.
//   module_name: "first_layer_inspector", "buildplate_marker_detector",
//                "printing_monitor", "pileup_detector",
//                "airprint_detector", "clump_detector", "spaghetti_detector"
//   control: true to enable
//   print_halt: true to halt the print on detection
std::string xcam_control_set(const std::string& sequence_id,
                             const std::string& module_name,
                             bool               control,
                             bool               print_halt);

// Convenience: xcam_control_set with the canonical module_name baked in.
inline std::string xcam_first_layer_inspector(const std::string& sequence_id,
                                              bool               enable,
                                              bool               print_halt) {
    return xcam_control_set(sequence_id, "first_layer_inspector",
                            enable, print_halt);
}
inline std::string xcam_spaghetti_detector(const std::string& sequence_id,
                                           bool               enable,
                                           bool               print_halt) {
    return xcam_control_set(sequence_id, "spaghetti_detector",
                            enable, print_halt);
}

} // namespace tier2
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_TIER2_COMMANDS_HPP
