// Bambu Bridge — default upload sink stub (phase 7).
//
// Drops every upload after writing a one-line breadcrumb to stderr.
// Useful for:
//   - integration tests that want to confirm an FTPS round-trip without
//     forwarding anywhere (RecordingUploadSink is the assertion variant);
//   - bridge-cli's `ftps` subcommand, so a developer can run a slicer
//     against the bridge and watch uploads land;
//   - the BridgeService default before phase 9 wires a real LanUploadSink
//     or CloudUploadSink.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_UPLOAD_SINK_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_UPLOAD_SINK_HPP

#include "../server/IUploadSink.hpp"

namespace Slic3r {
namespace bridge {
namespace router {

class NullUploadSink final : public server::IUploadSink {
public:
    NullUploadSink() = default;
    ~NullUploadSink() override = default;

    server::UploadResult deliver(server::UploadJob job) override;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_NULL_UPLOAD_SINK_HPP
