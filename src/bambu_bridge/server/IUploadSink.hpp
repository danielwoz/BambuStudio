// Bambu Bridge — upload sink interface (phase 7).
//
// `FtpsServer` terminates downstream-slicer FTPS uploads and hands the
// resulting `.3mf` file off to an `IUploadSink`. The sink decides where
// the bytes actually go:
//
//   - phase 7: `LanUploadSink`   -- forwards via FTPS to the real printer
//   - phase 7: `CloudUploadSink` -- pushes via the proprietary cloud plugin
//   - phase 7: `NullUploadSink`  -- drops with a documented log message
//   - phase 9: `SessionRouter`   -- picks LAN vs cloud per upload
//
// This is the analog of `IUplink` (MQTT messages) but for file transfers.
// The lifecycle is different: an upload is a single discrete unit — the
// file is fully received by FtpsServer (or rejected at the size cap)
// BEFORE `deliver` is called. There's no streaming back-pressure here.
// `deliver` runs synchronously on the FTPS connection thread, so the
// 226/551 reply can reflect the sink's actual success/failure.
//
// Threading: the sink is invoked from the per-connection FTPS thread.
// Concurrent uploads for different dev_ids may invoke `deliver` from
// different threads simultaneously; implementations must be thread-safe.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_IUPLOAD_SINK_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_IUPLOAD_SINK_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace server {

struct UploadJob {
    std::string  dev_id;
    std::string  filename;     // e.g. "demo.3mf"
    std::string  remote_path;  // e.g. "/model/demo.3mf"
    std::vector<uint8_t> content;
    std::chrono::system_clock::time_point received_at;
};

struct UploadResult {
    bool        ok = false;
    // On success, an opaque URL the bridge can hand back to other
    // components (e.g. the MQTT broker uses this in a follow-up
    // `gcode_file` PUBLISH). Forms in use:
    //   "ftps://<printer_ip>/model/<file>"
    //   "https://<cloud-oss-url>/..."
    //   "bambu:///local/<dev_id>?file=..."
    std::string remote_url;
    // On failure, a short human-readable explanation. Empty on success.
    std::string error_message;
};

class IUploadSink {
public:
    virtual ~IUploadSink() = default;

    // Synchronous delivery. Called from the FtpsServer's per-connection
    // thread after STOR completes. Returns ok=true on successful
    // forwarding (or staging, for sinks that defer); ok=false otherwise
    // with error_message populated.
    virtual UploadResult deliver(UploadJob job) = 0;
};

} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_IUPLOAD_SINK_HPP
