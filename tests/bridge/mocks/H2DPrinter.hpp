// Bambu Bridge — H2D mock printer (harness).
//
// Adds the dual-extruder + dual-AMS dimensions on top of the H2S
// baseline. Per-extruder nozzle temperature; per-AMS humidity. Series
// "series_o".

#ifndef SLIC3R_BAMBU_BRIDGE_MOCKS_H2D_PRINTER_HPP
#define SLIC3R_BAMBU_BRIDGE_MOCKS_H2D_PRINTER_HPP

#include <string>

#include "PrinterFsm.hpp"
#include "PrinterModel.hpp"
#include "third_party/nlohmann/json.hpp"

namespace Slic3r {
namespace bridge {
namespace mocks {

class H2DPrinter {
public:
    explicit H2DPrinter(std::string dev_id)
        : m_dev_id(std::move(dev_id)),
          m_model(make_h2d_model())
    {}

    const std::string& dev_id() const { return m_dev_id; }
    const PrinterModel& model() const { return m_model; }
    PrinterFsm&        fsm()           { return m_fsm; }

    nlohmann::json build_push_status() {
        nlohmann::json p = nlohmann::json::object();
        p["command"]               = "push_status";
        p["msg"]                   = 0;
        p["sequence_id"]           = std::to_string(++m_seq);
        p["gcode_state"]           = gcode_state_str(m_fsm.state());
        p["mc_percent"]            = m_fsm.mc_percent();
        p["mc_remaining_time"]     = m_fsm.mc_remaining_time();
        p["layer_num"]             = m_fsm.layer_num();
        p["total_layer_num"]       = m_fsm.total_layer_num();
        // Dual extruder: nozzle_temper is the active head; per-extruder
        // values exposed in the `device.extruder` map. The slicer reads
        // both.
        p["nozzle_temper"]         = m_nozzle_temper[0];
        p["nozzle_target_temper"]  = m_nozzle_target_temper[0];
        p["bed_temper"]            = m_bed_temper;
        p["bed_target_temper"]     = m_bed_target_temper;
        p["chamber_temper"]        = m_chamber_temper;
        p["fan_gear"]              = m_fan_gear;
        p["big_fan1_speed"]        = m_big_fan1_speed;
        p["sn"]                    = m_dev_id;

        nlohmann::json extruders = nlohmann::json::array();
        for (int i = 0; i < m_model.nozzle_count; ++i) {
            extruders.push_back(nlohmann::json{
                {"id", i},
                {"temp", m_nozzle_temper[i]},
                {"target_temp", m_nozzle_target_temper[i]},
            });
        }
        p["device"] = nlohmann::json{{"extruder", extruders}};

        nlohmann::json units = nlohmann::json::array();
        for (int u = 0; u < m_model.ams_unit_count; ++u) {
            nlohmann::json ams_unit = nlohmann::json::object();
            ams_unit["id"]       = std::to_string(u);
            ams_unit["humidity"] = "30";
            ams_unit["tray"]     = nlohmann::json::array();
            for (int i = 0; i < m_model.ams_slots_per_unit; ++i) {
                ams_unit["tray"].push_back(nlohmann::json{
                    {"id", std::to_string(u * m_model.ams_slots_per_unit + i)},
                    {"remain", 100 - i * 10},
                    {"tray_type", "PLA"},
                    {"tray_sub_brands", "Bambu Lab"},
                });
            }
            units.push_back(std::move(ams_unit));
        }
        p["ams"] = nlohmann::json{{"ams", units}};
        return nlohmann::json{{"print", p}};
    }

private:
    std::string  m_dev_id;
    PrinterModel m_model;
    PrinterFsm   m_fsm;
    int          m_seq                     = 0;
    int          m_nozzle_temper[2]        = {25, 25};
    int          m_nozzle_target_temper[2] = {0, 0};
    int          m_bed_temper              = 25;
    int          m_bed_target_temper       = 0;
    int          m_chamber_temper          = 24;
    int          m_fan_gear                = 0;
    int          m_big_fan1_speed          = 0;
};

} // namespace mocks
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_MOCKS_H2D_PRINTER_HPP
