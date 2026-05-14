// Bambu Bridge — PrinterModel capability tables.
//
// Pure-data per-printer struct used by the harness mocks. The fields
// mirror what the slicer's DeviceManager::parse_json + downstream
// MachineObject consumers read from each push_status JSON: nozzle count
// (single vs dual), AMS unit count (one vs two), chamber heater
// presence, etc.
//
// Capability values were cross-checked against
//   ~/BambuStudio/src/slic3r/GUI/DeviceManager.cpp series_o / series-N
//   branches
// and against the model_id strings in the Bambu Studio profile JSONs:
//   - resources/profiles/BBL/machine/Bambu Lab A1.json      -> "N2S"
//   - resources/profiles/BBL/machine/Bambu Lab A1 mini.json -> "N1"
//   - resources/profiles/BBL/machine/Bambu Lab H2S.json     -> "O1S"
//   - resources/profiles/BBL/machine/Bambu Lab H2D.json     -> "O1D"
//
// Note: test_harness_plan.md §3 lists the model codes as "N1/N2S/N2D".
// That is a doc error — the real codes (from the live profiles above)
// are N2S/O1S/O1D for A1/H2S/H2D respectively. The plan's status section
// notes the divergence.

#ifndef SLIC3R_BAMBU_BRIDGE_MOCKS_PRINTER_MODEL_HPP
#define SLIC3R_BAMBU_BRIDGE_MOCKS_PRINTER_MODEL_HPP

#include <string>

namespace Slic3r {
namespace bridge {
namespace mocks {

struct PrinterModel {
    std::string sn_prefix;            // matches Bambu's serial namespace
    int         nozzle_count;         // A1/H2S = 1, H2D = 2
    int         ams_unit_count;       // A1 = 1 (Lite), H2S = 1, H2D = 2
    int         ams_slots_per_unit;   // all = 4
    bool        ams_humidity;         // A1 = false, H2S = true, H2D = true
    bool        chamber_heater;       // A1 = false, H2S = true, H2D = true
    bool        chamber_temp_sensor;  // A1 = false, H2S = true, H2D = true
    bool        laser_accessory;      // H2D only (optional)
    std::string model_id;             // BBL internal: "N2S", "O1S", "O1D"
    std::string printer_type;         // matches DeviceManager::printer_type
    std::string series;               // "series_n" or "series_o"
    std::string display_name;         // user-facing
};

// Static capability table for the three printer flavours the harness
// covers in v1.
inline PrinterModel make_a1_model() {
    return PrinterModel{
        /*sn_prefix*/           "0938",  // A1 serial prefix (BBL)
        /*nozzle_count*/        1,
        /*ams_unit_count*/      1,
        /*ams_slots_per_unit*/  4,
        /*ams_humidity*/        false,
        /*chamber_heater*/      false,
        /*chamber_temp_sensor*/ false,
        /*laser_accessory*/     false,
        /*model_id*/            "N2S",
        /*printer_type*/        "3DPrinter-A1",
        /*series*/              "series_n",
        /*display_name*/        "Bambu Lab A1",
    };
}

inline PrinterModel make_h2s_model() {
    return PrinterModel{
        /*sn_prefix*/           "0950",  // H2S serial prefix (BBL)
        /*nozzle_count*/        1,
        /*ams_unit_count*/      1,
        /*ams_slots_per_unit*/  4,
        /*ams_humidity*/        true,
        /*chamber_heater*/      true,
        /*chamber_temp_sensor*/ true,
        /*laser_accessory*/     false,
        /*model_id*/            "O1S",
        /*printer_type*/        "3DPrinter-H2S",
        /*series*/              "series_o",
        /*display_name*/        "Bambu Lab H2S",
    };
}

inline PrinterModel make_h2d_model() {
    return PrinterModel{
        /*sn_prefix*/           "0951",  // H2D serial prefix (BBL)
        /*nozzle_count*/        2,
        /*ams_unit_count*/      2,
        /*ams_slots_per_unit*/  4,
        /*ams_humidity*/        true,
        /*chamber_heater*/      true,
        /*chamber_temp_sensor*/ true,
        /*laser_accessory*/     true,
        /*model_id*/            "O1D",
        /*printer_type*/        "3DPrinter-H2D",
        /*series*/              "series_o",
        /*display_name*/        "Bambu Lab H2D",
    };
}

} // namespace mocks
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_MOCKS_PRINTER_MODEL_HPP
