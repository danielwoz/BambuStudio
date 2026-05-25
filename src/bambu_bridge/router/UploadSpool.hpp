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

// Spool the job's bytes to a fresh `${TMPDIR}/bridge-upload-XXXXXX`
// tempfile (mode 0600 — print payloads may carry proprietary G-code).
// Returns the absolute path on success, empty string on failure.
std::string spool_upload_to_tempfile(const server::UploadJob& job);

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_UPLOAD_SPOOL_HPP
