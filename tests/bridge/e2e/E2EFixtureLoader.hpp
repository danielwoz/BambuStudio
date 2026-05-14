// Bambu Bridge — E2E fixture loader (phase 12).
//
// Tiny helper that locates the `cube_5mm.3mf` fixture and reads its
// metadata sidecar. Tests use the metadata file's `plate_idx` so the
// per-printer test stays oblivious to whether the cube was sliced for
// plate 1 (default) or some other slot.
//
// The fixture path is baked in by CMake at configure time
// (BAMBU_BRIDGE_E2E_FIXTURE_DIR) so tests don't need to chase the
// source tree from their cwd.

#ifndef SLIC3R_BAMBU_BRIDGE_E2E_FIXTURE_LOADER_HPP
#define SLIC3R_BAMBU_BRIDGE_E2E_FIXTURE_LOADER_HPP

#include <string>

namespace Slic3r {
namespace bridge {
namespace e2e {

struct FixtureMetadata {
    std::string fixture_path;    // absolute path to .3mf
    int         plate_idx = 1;   // which plate to print
    bool        is_stub   = true;  // true if the .3mf is a placeholder
    std::string notes;            // free-form description from the JSON
};

// Loads the bundled `cube_5mm.3mf` and its `.metadata.json` sidecar.
// Returns a FixtureMetadata with a meaningful `is_stub` flag and a
// short `notes` blurb on what the file actually is.
FixtureMetadata load_cube_5mm_fixture();

} // namespace e2e
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_E2E_FIXTURE_LOADER_HPP
