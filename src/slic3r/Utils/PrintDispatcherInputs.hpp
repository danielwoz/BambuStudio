// Bambu Bridge — helpers for sourcing `PrintDispatcher::Inputs` from
// the same places the GUI's PrintJob does.
//
// The four capabilities the dispatcher needs (`cloud_print_only`,
// `has_sdcard`, `could_emmc_print`, `ftp_folder`) live in two different
// places in BambuStudio:
//
//   - cloud_print_only      — MachineObject field, parsed from
//                             pushall's `print.support_cloud_print_only`
//                             JSON bool. See
//                             slic3r/GUI/DeviceManager.cpp:2784.
//   - has_sdcard            — MachineObject's storage state, derived
//                             from bits 8..9 of the pushall `home_flag`
//                             integer. See
//                             slic3r/GUI/DeviceManager.cpp:1022.
//                             Sourced via
//                             `obj->GetStorage()->get_sdcard_state() ==
//                              DevStorage::SdcardState::HAS_SDCARD_NORMAL`.
//                             Callers: SelectMachine.cpp:3113,
//                             ReleaseNote.cpp:1994, SendToPrinter.cpp:997.
//   - could_emmc_print      — MachineObject field, derived from bit 0
//                             of the pushall `fun2` (bit-string)
//                             field. See
//                             slic3r/GUI/DeviceManager.cpp:4292.
//                             Caller: SelectMachine.cpp:3114.
//   - ftp_folder (for the
//     start_local_print_with_record / start_local_print params, NOT
//     part of dispatcher Inputs) — static config lookup by
//     `printer_type` model code in `resources/printers/<code>.json`.
//     See `MachineObject::get_ftp_folder()` at
//     slic3r/GUI/DeviceManager.cpp:417 → `DevPrinterConfigUtil::
//     get_ftp_folder(type_str)` at
//     slic3r/GUI/DeviceCore/DevConfigUtil.h:73.
//     Caller: SelectMachine.cpp:3060.
//
// Reading these via the same code paths the GUI uses means future
// BambuStudio updates (new pushall fields, new model JSON entries)
// flow through to the bridge automatically. The bridge does NOT
// maintain its own per-model capability table.

#pragma once

#include "PrintDispatcher.hpp"

#include <string>

namespace Slic3r {

// Forward decls — DeviceManager / MachineObject are heavyweight slicer
// types we don't want to pull into PrintDispatcher.hpp.
class DeviceManager;
class MachineObject;

namespace PrintDispatcherInputsFromMachineObject {

// Fill a PrintDispatcher::Inputs from a live MachineObject. Pushall
// reports must already have been parsed for the booleans to be
// meaningful — caller's responsibility.
//
// If `obj` is null the returned Inputs is the safe fallback
// (cloud_print_only=false, has_sdcard=false, could_emmc_print=false),
// which matches what PrintJob would see for a brand-new printer that
// hasn't pushed yet.
PrintDispatcher::Inputs from_obj(const MachineObject* obj,
                                 bool                  app_lan_mode_only,
                                 const std::string&    verify_temp_path);

// Convenience: look up by dev_id in DeviceManager's user machine list.
// Returns the safe-fallback Inputs if either the manager or the
// dev_id isn't present.
PrintDispatcher::Inputs from_dev_id(DeviceManager*        dm,
                                    const std::string&    dev_id,
                                    bool                  app_lan_mode_only,
                                    const std::string&    verify_temp_path);

// Lookup `ftp_folder` for a given model code (e.g. "O1D", "N2S"). Wraps
// `DevPrinterConfigUtil::get_ftp_folder()` so callers don't need to
// pull in the GUI's config-util header.
//
// Returns "" if the model isn't in resources/printers/*.json — same
// fallback the GUI gets when the JSON omits the key.
std::string get_ftp_folder_for_model(const std::string& model_code);

} // namespace PrintDispatcherInputsFromMachineObject
} // namespace Slic3r
