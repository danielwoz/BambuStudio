// Bambu Bridge — LAN-direct camera source.
//
// `LanCameraSource` forwards the printer's known LAN credentials to the
// proprietary `libBambuSource.so` library and pumps the resulting H.264
// stream to `RtspServer`. The slicer-side analogue is `MediaPlayCtrl.cpp`
// in `~/BambuStudio/src/slic3r/GUI/`:
//
//   url = "bambu:///rtsps___" + user + ":" + pw + "@" + ip +
//         "/streaming/live/1?proto=rtsps"
//
// (note: TRIPLE underscore between `rtsps` and the credentials — that's
// how BambuSource recognises the scheme. `?proto=rtsps` is a hint to its
// internal dispatcher; the LAN client uses live555.)
//
// We don't reimplement RTSPS — BambuSource bundles a live555 client
// internally. We just build the URL, hand it to `Bambu_Create`, drive
// `Bambu_Open` → `Bambu_StartStream`, and pull frames via
// `Bambu_ReadSample` until end-of-stream.
//
// References:
//   * `BambuTunnel.h` for the C-ABI.
//   * `~/BambuStudio/src/slic3r/GUI/MediaPlayCtrl.cpp` for the URL recipe.

#ifndef SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_CAMERA_SOURCE_HPP
#define SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_CAMERA_SOURCE_HPP

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

class BambuSourceHandle;

namespace router {

struct LanCameraSourceConfig {
    std::string dev_id;
    std::string printer_ip;
    // Kept for backwards compat with BridgeApp wiring; ignored — the URL
    // format BambuSource expects bakes the standard /streaming/live/1
    // path in. Real printers always listen on 322 for RTSPS; we don't
    // probe other ports.
    uint16_t    printer_port = 322;
    std::string access_code;
    std::chrono::seconds connect_timeout{5};
    std::chrono::seconds io_timeout{30};
    // MediaPlayCtrl.cpp always uses "bblp"; same default here.
    std::string username = "bblp";

    // Slicer-identity query params the GUI appends to every camera URL
    // (`~/BambuStudio/src/slic3r/GUI/MediaPlayCtrl.cpp:322-329`). The
    // proprietary plugin fingerprints the URL — passing only the bare
    // `bambu:///rtsps___user:pw@ip/...` gets `bambu_start_stream rc=-107`
    // even though `bambu_create` and `bambu_open` succeed. Mirror all
    // five params byte-for-byte to keep the call shape identical to the
    // GUI path. `dev_id` above doubles as `&device=`.
    std::string slicer_net_ver;   // &net_ver= — plugin's get_version()
    std::string slicer_dev_ver;   // &dev_ver= — printer firmware version
    std::string slicer_cli_id;    // &cli_id=  — slicer_uuid
    std::string slicer_cli_ver;   // &cli_ver= — SLIC3R_VERSION

    // Optional pre-resolved camera URL. When non-empty, `build_url`
    // returns this verbatim instead of constructing a
    // `bambu:///rtsps___user:pass@ip/...` URL from the fields above.
    // GUI-driven mode fills this via the slicer's
    // `Slic3r::GUI::build_media_live_url` helper (the same ladder
    // MediaPlayCtrl picks from), keeping one URL-construction path
    // across MediaPlayCtrl, MediaFilePanel, and the bridge's camera
    // sources. Headless mode leaves it empty and the source falls
    // back to its own rtsps:// builder.
    std::string url_override;
};

class LanCameraSource : public server::ICameraSource {
public:
    // Config-only constructor (kept for test stubs that subclass and
    // override every virtual). `BambuSourceHandle` defaults to nullptr;
    // open() then fails fast. Production code calls
    // `attach_source_handle(...)` after construction (mirrors how
    // CloudCameraSource takes its `BambuNetworkingPluginHandle`).
    explicit LanCameraSource(LanCameraSourceConfig cfg);

    // Convenience ctor for code that has both ready at construction.
    LanCameraSource(LanCameraSourceConfig                cfg,
                    std::shared_ptr<BambuSourceHandle>  source);

    ~LanCameraSource() override;

    // Attach / replace the BambuSourceHandle. Safe to call before or
    // after open(); nullptr detaches. Caller keeps the shared_ptr alive.
    void attach_source_handle(std::shared_ptr<BambuSourceHandle> handle);

    // The URL `open()` will hand to `Bambu_Create`. Exposed for tests so
    // they can pin the format without scraping log output.
    std::string url() const;

    // ICameraSource:
    bool open()           override;
    void close()          override;
    bool is_open() const  override;

    std::optional<server::VideoFrame> next_frame(int timeout_ms) override;
    server::ICameraSource::StreamInfo info() const override;

    const LanCameraSourceConfig& config() const { return m_cfg; }

protected:
    LanCameraSourceConfig                 m_cfg;
    std::atomic<bool>                     m_open{false};

    mutable std::mutex                    m_mu;
    std::shared_ptr<BambuSourceHandle>    m_source;
    void*                                 m_tunnel = nullptr;
    server::ICameraSource::StreamInfo     m_info;

    // Scratch buffer for next_frame; allocated once at open() based on
    // the stream's max_frame_size hint so the per-frame path doesn't
    // allocate (BambuSource's `buffer` is library-owned and the next
    // ReadSample invalidates it; we copy out into nal_data).
    std::vector<uint8_t>                  m_scratch;

    // Build the URL from the config. Called by open(); also exposed via
    // `url()` for tests.
    std::string build_url() const;
};

} // namespace router
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_ROUTER_LAN_CAMERA_SOURCE_HPP
