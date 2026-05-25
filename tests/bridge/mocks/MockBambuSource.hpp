// Bambu Bridge — MockBambuSource (harness).
//
// Subclass of BambuSourceHandle that returns a fixed MJPG sample from
// an embedded byte buffer (no fixture file dependency — keeps the test
// in-process and deterministic). Mirrors LanCameraSourcePluginTest's
// existing mock, but slimmed to the §7 subset.

#ifndef SLIC3R_BAMBU_BRIDGE_MOCKS_MOCK_BAMBU_SOURCE_HPP
#define SLIC3R_BAMBU_BRIDGE_MOCKS_MOCK_BAMBU_SOURCE_HPP

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "BambuSourceHandle.hpp"

namespace Slic3r {
namespace bridge {
namespace mocks {

class MockBambuSource : public BambuSourceHandle {
public:
    MockBambuSource();
    ~MockBambuSource() override = default;

    // ---- BambuSourceHandle overrides -------------------------------------
    bool init() override                  { return true; }
    bool library_ready() const override   { return true; }

    int  bambu_create(void** out_tunnel, const std::string& url) override;
    void bambu_destroy(void* tunnel) override;
    int  bambu_open(void* tunnel) override;
    void bambu_close(void* tunnel) override;
    int  bambu_start_stream(void* tunnel, bool video) override;
    int  bambu_get_stream_count(void* tunnel) override;
    int  bambu_get_stream_info(void* tunnel, int index, void* info_out) override;
    int  bambu_read_sample(void* tunnel, void* sample_out) override;

    // Test introspection.
    int  open_count()  const { return m_open_count.load(); }
    int  read_count()  const { return m_read_count.load(); }
    int  close_count() const { return m_close_count.load(); }

private:
    std::atomic<int> m_open_count{0};
    std::atomic<int> m_read_count{0};
    std::atomic<int> m_close_count{0};

    // Single static MJPG-like fixture (one frame, ~64 bytes). The
    // bytes themselves don't decode — the harness only cares that
    // the read/start_stream/close lifecycle fires correctly.
    static const std::vector<unsigned char>& mjpg_sample();

    // Tracks the active tunnel + per-tunnel cursor for ReadSample.
    struct TunnelState {
        std::string url;
        bool        open    = false;
        bool        started = false;
        size_t      read_cursor = 0;
    };
    mutable std::mutex                              m_mu;
    std::vector<std::unique_ptr<TunnelState>>      m_tunnels;
};

} // namespace mocks
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_MOCKS_MOCK_BAMBU_SOURCE_HPP
