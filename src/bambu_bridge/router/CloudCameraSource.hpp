// Bambu Bridge — cloud-relay camera source.
//
// Same shape as `LanCameraSource`, but the URL isn't built locally — it
// comes from the proprietary `bambu_networking` plugin via
// `BambuNetworkingPluginHandle::get_camera_url` (which wraps the upstream
// `bambu_network_get_camera_url` callback-style export). Whatever URL the
// plugin returns is handed verbatim to `BambuSourceHandle::bambu_create`;
// BambuSource dispatches by scheme internally:
//
//   - `bambu:///rtsps___...`  -> live555 RTSPS (LAN-direct)
//   - `bambu:///agora/...`    -> Agora P2P (proprietary)
//   - `bambu:///tutk/...`     -> TUTK P2P  (proprietary)
//   - `bambu:///local/...`    -> port-6000 BambuTunnel
//
// We don't have to know which transport the URL is for — that's the whole
// point of routing through BambuSource. Cloud-side requests always go via
// this source because the URL retrieval requires the plugin's cloud session.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_CAMERA_SOURCE_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_CAMERA_SOURCE_HPP

#include "../server/ICameraSource.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {

class BambuNetworkingPluginHandle;
class BambuSourceHandle;

namespace router {

struct CloudCameraSourceConfig {
    std::string          dev_id;
    std::chrono::seconds connect_timeout{10};

    // Slicer-identity fields the GUI embeds in the cloud-camera URL
    // (`~/BambuStudio/src/slic3r/GUI/MediaPlayCtrl.cpp:374-385`). The
    // first three are concatenated as `<dev_id>|<dev_ver>|<protocols>`
    // and passed to `bambu_network_get_camera_url`; the rest are
    // appended as query params to the URL the plugin returns. Mirror
    // the GUI exactly — the plugin fingerprints them.
    std::string          dev_ver;      // printer firmware version
    std::string          net_ver;      // NetworkAgent::get_version()
    std::string          cli_id;       // app_config slicer_uuid
    std::string          cli_ver;      // SLIC3R_VERSION

    // Optional pre-resolved camera URL — set by the GUI host via the
    // shared `Slic3r::GUI::build_media_live_url` helper. When non-empty
    // the source skips agent->get_camera_url and uses this URL
    // directly. Same plumbing as LanCameraSourceConfig::url_override.
    std::string          url_override;
};

class CloudCameraSource : public server::ICameraSource {
public:
    explicit CloudCameraSource(CloudCameraSourceConfig cfg);

    // Convenience ctor for code that has both ready at construction.
    CloudCameraSource(CloudCameraSourceConfig                       cfg,
                      std::shared_ptr<BambuNetworkingPluginHandle>  plugin,
                      std::shared_ptr<BambuSourceHandle>            source);

    ~CloudCameraSource() override;

    // Attach the shared plugin handle. May be called before or after
    // open(); nullptr detaches. Caller keeps the shared_ptr alive.
    void attach_plugin(std::shared_ptr<BambuNetworkingPluginHandle> handle);

    // Attach the shared BambuSourceHandle. Same lifecycle rules as
    // attach_plugin.
    void attach_source_handle(std::shared_ptr<BambuSourceHandle> handle);

    // ICameraSource:
    bool open()           override;
    void close()          override;
    bool is_open() const  override;

    std::optional<server::VideoFrame> next_frame(int timeout_ms) override;
    server::ICameraSource::StreamInfo info() const override;

    // For inspection / tests: the URL the plugin returned (or "" before
    // open() succeeds / on failure).
    std::string last_url() const;

protected:
    CloudCameraSourceConfig                       m_cfg;
    std::atomic<bool>                             m_open{false};

    mutable std::mutex                            m_mu;
    std::shared_ptr<BambuNetworkingPluginHandle>  m_handle;
    std::shared_ptr<BambuSourceHandle>            m_source;
    std::string                                   m_last_url;
    void*                                         m_tunnel = nullptr;
    server::ICameraSource::StreamInfo             m_info;
    std::vector<uint8_t>                          m_scratch;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_CLOUD_CAMERA_SOURCE_HPP
