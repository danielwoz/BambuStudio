// Bambu Bridge — verbose-logging gate.
//
// One-time env probe of `BAMBU_BRIDGE_VERBOSE`. Per-poll diagnostics
// (cloud-inventory body previews, reconcile snapshot lines) are wrapped
// in `if (Slic3r::bridge::verbose())` so they're silent in normal runs
// and chatty when the user opts in. Startup-once logs (plugin symbol
// resolution, servers up, listener bound) stay unconditional.

#ifndef SLIC3R_BAMBU_BRIDGE_VERBOSE_HPP
#define SLIC3R_BAMBU_BRIDGE_VERBOSE_HPP

#include <cstdlib>
#include <cstring>

namespace Slic3r {
namespace bridge {

inline bool verbose() {
    static const bool v = []() {
        const char* e = std::getenv("BAMBU_BRIDGE_VERBOSE");
        return e && *e && std::strcmp(e, "0") != 0;
    }();
    return v;
}

} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_VERBOSE_HPP
