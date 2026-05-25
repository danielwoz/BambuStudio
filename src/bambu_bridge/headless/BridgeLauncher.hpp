// Bambu Bridge — multi-process launcher.
//
// `BambuStudio --bridge-multi` fork+execs one `BambuStudio --bridge-only
// --only-dev-id <X> --mqtt-port-base N --ftps-port-base N --rtsp-port-base N`
// child per real printer, then supervises them: forwards SIGINT/SIGTERM,
// waitpids, returns the worst child exit code.
//
// Each child has its own plugin instance and therefore its own private
// LAN slot — `BambuNetworkingPluginHandle::connect_printer` only supports
// one active LAN connection per process, so N concurrent real-printer
// monitors requires N processes. Every child still goes through the full
// GUI init path (wxApp + plugin load + cloud session) so the proprietary
// plugin's startup fingerprinting is identical to a real slicer launch.
//
// Usage:
//   BambuStudio --bridge-multi \
//       --printer EXAMPLESERIAL01 \
//       --printer EXAMPLESERIAL02 \
//       [--mqtt-port-base 8883] \
//       [--ftps-port-base 39990] \
//       [--rtsp-port-base 38322]
//
// If no --printer flags are given, the launcher reads the `access_code`
// section of ~/.config/BambuStudio/BambuStudio.conf and spawns one child
// per cloud-bound device (real serials, not the FFFF mangled forms).

#ifndef BAMBU_BRIDGE_HEADLESS_BRIDGE_LAUNCHER_HPP
#define BAMBU_BRIDGE_HEADLESS_BRIDGE_LAUNCHER_HPP

namespace Slic3r {
namespace bridge {
namespace headless {

// Returns true if argv contains "--bridge-multi". Callers use this to
// route from main() / CLI::run() before any wx/locale init runs (the
// children handle their own).
bool is_bridge_multi(int argc, char** argv);

// Parse + fork+exec + supervise loop. Returns the exit code to hand to
// _Exit(). Never returns until all children have reaped (or signal
// caused us to kill them).
int run_bridge_multi(int argc, char** argv);

} // namespace headless
} // namespace bridge
} // namespace Slic3r

#endif // BAMBU_BRIDGE_HEADLESS_BRIDGE_LAUNCHER_HPP
