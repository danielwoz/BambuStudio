// Bambu Bridge — shared CLI argument parser.
//
// Used by BambuStudio's `--bridge-only` headless mode. The parser is
// pure-stdlib + minimal — no Boost, no wx, no getopt — so it can be
// linked into the main BambuStudio binary without dragging extra deps.
//
// There is intentionally no standalone bridge daemon binary: the
// proprietary `bambu_networking` plugin fingerprints its host process
// and refuses to operate when loaded by anything other than
// BambuStudio. Headless mode is therefore `BambuStudio --bridge-only`.
//
// Returned `ParseResult::exit_code`:
//   * 0   — caller should run the daemon with `config`.
//   * 1   — caller should print `help_text` to stdout and exit 0
//           (i.e. user asked for --help; the parser already produced
//           the rendered usage text).
//   * 2   — caller should print `error_message` to stderr and exit 2.
// `usage_text` is always populated so callers can print it themselves
// (e.g. for "unknown option" errors).

#ifndef SLIC3R_BAMBU_BRIDGE_HEADLESS_CLI_ARGS_HPP
#define SLIC3R_BAMBU_BRIDGE_HEADLESS_CLI_ARGS_HPP

#include "BridgeApp.hpp"

#include <string>

namespace Slic3r {
namespace bridge {
namespace headless {

struct ParseResult {
    BridgeAppConfig config;
    int             exit_code     = 0;   // 0 = run, 1 = help (exit 0), 2 = error (exit 2)
    std::string     error_message;       // populated when exit_code == 2
    std::string     help_text;           // populated when exit_code == 1 (i.e. --help)
};

// Static usage text. `program_name` is substituted into the "usage:"
// header so users see the actual entry point in error messages
// (typically `BambuStudio --bridge-only`).
std::string render_usage(const std::string& program_name);

// Parse argv into a BridgeAppConfig. `program_name` is used in error
// messages and the usage header.
//
// Defaults: the parser pulls $BAMBU_BRIDGE_PLUGIN_PATH from the
// environment as a fallback for --plugin if neither is provided.
//
// The returned `BridgeAppConfig` is otherwise default-constructed —
// callers can override fields before passing it to `BridgeApp`.
ParseResult parse_cli_args(const std::string& program_name,
                           int                argc,
                           char**             argv);

} // namespace headless
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_HEADLESS_CLI_ARGS_HPP
