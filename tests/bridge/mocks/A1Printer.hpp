// Bambu Bridge — A1 mock printer (harness).
//
// Builds the push_status JSON payload an A1 would emit, parameterised
// by the shared PrinterFsm. The smallest-realism set of mutating
// fields lives here; everything else (printer_type, sn, AMS slot
// colours, etc.) is pinned at construction time from PrinterModel.
//
// The slicer's DeviceManager::parse_json reads ~30 keys from each
// push_status. Most are pinned; the FSM mutates only the eight listed
// in test_harness_plan.md §3 ("push_status payload realism").

#ifndef SLIC3R_BAMBU_BRIDGE_MOCKS_A1_PRINTER_HPP
#define SLIC3R_BAMBU_BRIDGE_MOCKS_A1_PRINTER_HPP

#include <string>

#include "PrinterFsm.hpp"
#include "PrinterModel.hpp"
#include "third_party/nlohmann/json.hpp"

namespace Slic3r {
namespace bridge {
namespace mocks {

class A1Printer {
public:
    explicit A1Printer(std::string dev_id)
        : m_dev_id(std::move(dev_id)),
          m_model(make_a1_model())
    {}

    const std::string& dev_id() const { return m_dev_id; }
    const PrinterModel& model() const { return m_model; }
    PrinterFsm&        fsm()           { return m_fsm; }

    // Build a "print" push_status payload reflecting the current FSM
    // state. The slicer's `DeviceManager::parse_json` walks this exact
    // shape; A1 lacks chamber and humidity, so those keys are omitted.
    nlohmann::json build_push_status() {
        nlohmann::json p = nlohmann::json::object();
        p["command"]            = "push_status";
        p["msg"]                = 0;
        p["sequence_id"]        = std::to_string(++m_seq);
        p["gcode_state"]        = gcode_state_str(m_fsm.state());
        p["mc_percent"]         = m_fsm.mc_percent();
        p["mc_remaining_time"]  = m_fsm.mc_remaining_time();
        p["layer_num"]          = m_fsm.layer_num();
        p["total_layer_num"]    = m_fsm.total_layer_num();
        p["nozzle_temper"]      = m_nozzle_temper;
        p["nozzle_target_temper"] = m_nozzle_target_temper;
        p["bed_temper"]         = m_bed_temper;
        p["bed_target_temper"]  = m_bed_target_temper;
        p["fan_gear"]           = m_fan_gear;
        p["big_fan1_speed"]     = m_big_fan1_speed;
        p["sn"]                 = m_dev_id;
        // Single AMS (Lite), 4 slots.
        nlohmann::json ams_unit = nlohmann::json::object();
        ams_unit["id"]   = "0";
        ams_unit["tray"] = nlohmann::json::array();
        for (int i = 0; i < m_model.ams_slots_per_unit; ++i) {
            ams_unit["tray"].push_back(nlohmann::json{
                {"id", std::to_string(i)},
                {"remain", 100 - i * 10},
                {"tray_type", "PLA"},
                {"tray_sub_brands", "Bambu Lab"},
            });
        }
        p["ams"] = nlohmann::json{
            {"ams", nlohmann::json::array({ams_unit})},
        };
        return nlohmann::json{{"print", p}};
    }

private:
    std::string  m_dev_id;
    PrinterModel m_model;
    PrinterFsm   m_fsm;
    int          m_seq                  = 0;
    int          m_nozzle_temper        = 25;
    int          m_nozzle_target_temper = 0;
    int          m_bed_temper           = 25;
    int          m_bed_target_temper    = 0;
    int          m_fan_gear             = 0;
    int          m_big_fan1_speed       = 0;
};

} // namespace mocks
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_MOCKS_A1_PRINTER_HPP
