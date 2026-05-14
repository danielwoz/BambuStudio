// Bambu Bridge — H2S mock printer (harness).
//
// Differs from A1: chamber heater + chamber temperature sensor + AMS
// humidity sensor. Single nozzle, single AMS unit. Series == "series_o".

#ifndef SLIC3R_BAMBU_BRIDGE_MOCKS_H2S_PRINTER_HPP
#define SLIC3R_BAMBU_BRIDGE_MOCKS_H2S_PRINTER_HPP

#include <string>

#include "PrinterFsm.hpp"
#include "PrinterModel.hpp"
#include "third_party/nlohmann/json.hpp"

namespace Slic3r {
namespace bridge {
namespace mocks {

class H2SPrinter {
public:
    explicit H2SPrinter(std::string dev_id)
        : m_dev_id(std::move(dev_id)),
          m_model(make_h2s_model())
    {}

    const std::string& dev_id() const { return m_dev_id; }
    const PrinterModel& model() const { return m_model; }
    PrinterFsm&        fsm()           { return m_fsm; }

    nlohmann::json build_push_status() {
        nlohmann::json p = nlohmann::json::object();
        p["command"]              = "push_status";
        p["msg"]                  = 0;
        p["sequence_id"]          = std::to_string(++m_seq);
        p["gcode_state"]          = gcode_state_str(m_fsm.state());
        p["mc_percent"]           = m_fsm.mc_percent();
        p["mc_remaining_time"]    = m_fsm.mc_remaining_time();
        p["layer_num"]            = m_fsm.layer_num();
        p["total_layer_num"]      = m_fsm.total_layer_num();
        p["nozzle_temper"]        = m_nozzle_temper;
        p["nozzle_target_temper"] = m_nozzle_target_temper;
        p["bed_temper"]           = m_bed_temper;
        p["bed_target_temper"]    = m_bed_target_temper;
        p["chamber_temper"]       = m_chamber_temper;
        p["fan_gear"]             = m_fan_gear;
        p["big_fan1_speed"]       = m_big_fan1_speed;
        p["sn"]                   = m_dev_id;

        nlohmann::json ams_unit = nlohmann::json::object();
        ams_unit["id"]       = "0";
        ams_unit["humidity"] = "30";  // H2S exposes AMS humidity
        ams_unit["tray"]     = nlohmann::json::array();
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
    int          m_chamber_temper       = 24;  // H2S chamber sensor
    int          m_fan_gear             = 0;
    int          m_big_fan1_speed       = 0;
};

} // namespace mocks
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_MOCKS_H2S_PRINTER_HPP
