// Bambu Bridge — implementation of PrintDispatcherInputs.
//
// All four capability lookups in this file mirror what the GUI's
// PrintJob (and SelectMachine / SendToPrinter / ReleaseNote) read.
// See PrintDispatcherInputs.hpp for the per-field source-of-truth map.

#include "PrintDispatcherInputs.hpp"

#include "slic3r/GUI/DeviceManager.hpp"      // MachineObject
#include "slic3r/GUI/DeviceCore/DevManager.h" // DeviceManager
#include "slic3r/GUI/DeviceCore/DevStorage.h" // DevStorage (sdcard state)
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h" // DevPrinterConfigUtil

namespace Slic3r {
namespace PrintDispatcherInputsFromMachineObject {

PrintDispatcher::Inputs from_obj(const MachineObject* obj,
                                 bool                 app_lan_mode_only,
                                 const std::string&   verify_temp_path) {
    PrintDispatcher::Inputs in;
    in.app_lan_mode_only = app_lan_mode_only;
    in.verify_temp_path  = verify_temp_path;

    if (!obj) {
        // Safe fallback — matches PrintJob's view of a printer that
        // hasn't pushed a status frame yet (all capability fields
        // default-initialise to false in MachineObject's declaration:
        // DeviceManager.hpp:528, 554; storage_state defaults to
        // NO_SDCARD).
        return in;
    }

    // cloud_print_only — see DeviceManager.cpp:2784. Parsed from
    //   `print.support_cloud_print_only` in the pushall report.
    in.cloud_print_only = obj->is_support_cloud_print_only;

    // could_emmc_print — see DeviceManager.cpp:4292. Parsed from
    //   bit 0 of the pushall `fun2` (bit-string) field via
    //   get_flag_bits_no_border().
    in.could_emmc_print = obj->is_support_print_with_emmc;

    // has_sdcard — see SelectMachine.cpp:3113, ReleaseNote.cpp:1994,
    //   SendToPrinter.cpp:997. Always compared against
    //   HAS_SDCARD_NORMAL exactly (not HAS_SDCARD_ABNORMAL or
    //   NO_SDCARD). Storage state is set from bits 8..9 of the
    //   pushall `home_flag` field in DeviceManager.cpp:1022 via
    //   `m_storage->set_sdcard_state(get_flag_bits(flag, 8, 2))`.
    //
    //   Note: A1 (model N2S) reports HAS_SDCARD_NORMAL even though it
    //   has no physical SD-card slot — the bit reflects "internal
    //   storage available", not a literal SD card.
    if (auto* storage = obj->GetStorage()) {
        in.has_sdcard =
            (storage->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_NORMAL);
    }

    return in;
}

PrintDispatcher::Inputs from_dev_id(DeviceManager*       dm,
                                    const std::string&   dev_id,
                                    bool                 app_lan_mode_only,
                                    const std::string&   verify_temp_path) {
    if (!dm) return from_obj(nullptr, app_lan_mode_only, verify_temp_path);
    const auto list = dm->get_user_machinelist();
    auto it = list.find(dev_id);
    if (it == list.end())
        return from_obj(nullptr, app_lan_mode_only, verify_temp_path);
    return from_obj(it->second, app_lan_mode_only, verify_temp_path);
}

std::string get_ftp_folder_for_model(const std::string& model_code) {
    // Direct wrapper around the GUI's static printer-config lookup.
    // See DevConfigUtil.h:73 — `DevPrinterConfigUtil::get_ftp_folder`
    // reads the `"ftp_folder"` key from resources/printers/<code>.json.
    //
    // Sample values observed in the JSONs that ship with this build:
    //   O1D (H2D): omitted from JSON → returns ""
    //   N2S (A1):  "sdcard/"
    //   C11 (X1C): "sdcard/"
    return DevPrinterConfigUtil::get_ftp_folder(model_code);
}

} // namespace PrintDispatcherInputsFromMachineObject
} // namespace Slic3r
