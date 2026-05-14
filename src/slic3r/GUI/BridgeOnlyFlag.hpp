// Bambu Bridge --bridge-only headless flag shared between BambuStudio.cpp
// (which parses --bridge-only on the very early CLI path) and GUI_App
// (which short-circuits MainFrame creation and instead drives the
// bridge worker + DeviceManager push pump from OnInit).
//
// When `g_bridge_only` is true the slicer's wxApp still constructs --
// including NetworkAgent + DeviceManager -- but never opens MainFrame /
// Plater / preset bundle UI. GUI_App::OnInit takes the headless branch
// and runs the same bridge bootstrap the GUI worker thread uses,
// driven by `g_bridge_only_cfg` (populated from --bridge-only CLI).
//
// Both globals default to {false, default-constructed config}; if
// --bridge-only is absent they're untouched and the slicer comes up
// in normal GUI mode.

#ifndef SLIC3R_GUI_BRIDGE_ONLY_FLAG_HPP
#define SLIC3R_GUI_BRIDGE_ONLY_FLAG_HPP

#if defined(BAMBU_BRIDGE)

#include "bambu_bridge/headless/BridgeApp.hpp"

namespace Slic3r {
namespace GUI {

extern bool                                  g_bridge_only;
extern ::Slic3r::bridge::headless::BridgeAppConfig g_bridge_only_cfg;

} // namespace GUI
} // namespace Slic3r

#endif // BAMBU_BRIDGE

#endif // SLIC3R_GUI_BRIDGE_ONLY_FLAG_HPP
