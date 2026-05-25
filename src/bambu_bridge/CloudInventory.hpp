// Bambu Bridge — CloudInventory.
//
// Wraps the proprietary `bambu_networking` plugin so the rest of the
// bridge can ask "which cloud-bound printers does this user own?" without
// compile-time linking against a closed-source dependency. When the
// plugin is absent or fails to load, refresh() simply returns false and
// the snapshot stays empty, leaving the bridge safely degraded.
//
// As of phase 6 the dlopen + symbol-resolution + agent-lifecycle logic
// lives in `BambuNetworkingPluginHandle` so CloudUplink (phase 6) and
// CloudInventory share a single agent (the plugin only allows one per
// process). CloudInventory is now a thin wrapper that holds a
// shared_ptr<BambuNetworkingPluginHandle> and parses the device list.
//
// Two construction paths:
//
//   - CloudInventory(CloudInventoryConfig)
//        Phase-1 compatibility: constructs and owns an internal handle.
//        Useful for `bridge-cli list-devices` and the missing-plugin
//        unit test.
//   - CloudInventory(std::shared_ptr<BambuNetworkingPluginHandle>)
//        Phase-6 path: BridgeService creates the handle once and shares
//        it across CloudInventory and CloudUplink.

#ifndef SLIC3R_BAMBU_BRIDGE_CLOUD_INVENTORY_HPP
#define SLIC3R_BAMBU_BRIDGE_CLOUD_INVENTORY_HPP

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {

class BambuNetworkingPluginHandle;

// Per-printer record published by the cloud `get_user_print_info` endpoint
// plus a couple of LAN-side fields the bridge fills in itself.
struct CloudDevice {
    std::string dev_id;          // serial — primary key
    std::string name;            // friendly name ("dev_name")
    std::string model;           // dev_model_name, e.g. "H2S", "A1M", "H2"
    std::string access_code;     // LAN MQTT/FTPS auth string
    std::string lan_ip;          // last known LAN IP, empty if unknown
    bool        online        = false; // cloud-reachable
    bool        lan_reachable = false; // bridge-host can speak to it on LAN
};

struct CloudInventoryConfig {
    // Absolute path to the proprietary plugin. Empty = let the inventory
    // probe its default locations.
    std::string plugin_path;
    // Pass-through to `bambu_network_set_config_dir` once the agent is
    // created.
    std::string config_dir;
    // Optional ISO country code for `bambu_network_set_country_code`.
    std::string country_code;
};

class CloudInventory {
public:
    // Owns an internal plugin handle constructed from `config`. Phase-1
    // compatibility path.
    explicit CloudInventory(CloudInventoryConfig config);

    // Phase-6 path: share an externally-owned plugin handle.
    explicit CloudInventory(std::shared_ptr<BambuNetworkingPluginHandle> handle);

    ~CloudInventory();

    CloudInventory(const CloudInventory&) = delete;
    CloudInventory& operator=(const CloudInventory&) = delete;

    // Returns a copy of the cached device list. Thread-safe.
    std::vector<CloudDevice> snapshot() const;

    // Talks to the cloud (synchronous) and rebuilds the cache. Returns
    // true on success, false if the plugin couldn't be loaded, the agent
    // couldn't be created, the user isn't logged in, or the HTTP call
    // failed. Always safe to call: in failure paths the cache is left
    // empty and the bridge can keep running without cloud visibility.
    bool refresh();

    // Best-effort, non-blocking probe of each cached device's LAN
    // reachability. Phase 1's simple heuristic remains: trust dev_ip.
    void probe_lan_reachability();

    // True iff the proprietary plugin has been loaded successfully.
    bool plugin_loaded() const;

    // Accessor for the underlying handle. May be null if construction
    // failed. Phase-6 BridgeService uses this to share the handle with
    // CloudUplink.
    std::shared_ptr<BambuNetworkingPluginHandle> handle() const;

private:
    std::shared_ptr<BambuNetworkingPluginHandle> m_handle;
    mutable std::mutex                           m_cache_mutex;
    std::vector<CloudDevice>                     m_cache;
};

} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_CLOUD_INVENTORY_HPP
