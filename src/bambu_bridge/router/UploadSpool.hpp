// Bambu Bridge — shared upload spool helper.
//
// Both CloudUploadSink and LanUploadSink route through the proprietary
// plugin's print exports (`start_send_gcode_to_sdcard` /
// `start_local_print_with_record`), which take a file path rather than
// an in-memory buffer. The slicer-facing FtpsServer hands us the upload
// as a `std::vector<uint8_t>`, so we write it to a per-job tempfile and
// pass the path to the plugin. Both sinks share the same spool/unspool
// dance — factored out here so the helper is unit-testable independent
// of the sinks and there's a single source of truth for the file mode
// and cleanup semantics.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_UPLOAD_SPOOL_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_UPLOAD_SPOOL_HPP

#include "../server/IUploadSink.hpp"

#include <string>

namespace Slic3r {
namespace bridge {
namespace router {

// Spool the job's bytes to a fresh `${TMPDIR}/bridge-upload-XXXXXX/<name>`
// (mode 0600 — print payloads may carry proprietary G-code). `<name>`
// is the slicer-supplied filename (sanitised — basename only, `.3mf`
// suffix forced if missing) so the plugin sees the same file naming
// the GUI's SendJob would feed it (`MyProject_plate_3.3mf`, …).
// Returns the absolute path on success, empty string on failure.
std::string spool_upload_to_tempfile(const server::UploadJob& job);

// Counterpart cleanup — remove the spooled file AND the per-job
// tempdir spool_upload_to_tempfile() created. Safe to call with an
// empty string (no-op).
void cleanup_upload_tempfile(const std::string& path);

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_UPLOAD_SPOOL_HPP
