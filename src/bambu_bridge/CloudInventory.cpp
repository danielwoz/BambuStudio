// Bambu Bridge — CloudInventory implementation.
//
// As of phase 6 the dlopen / symbol-resolve / agent-lifecycle logic has
// moved to `BambuNetworkingPluginHandle`. CloudInventory is now a thin
// wrapper that drives a single handle (shared with CloudUplink) and
// parses the JSON device list the plugin returns. The contracts the
// phase-1 tests rely on — refresh() returns false on missing plugin,
// snapshot() empty when no refresh has succeeded — are preserved
// verbatim.

#include "CloudInventory.hpp"

#include "BambuNetworkingPluginHandle.hpp"
#include "Verbose.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

namespace Slic3r {
namespace bridge {

CloudInventory::CloudInventory(CloudInventoryConfig config) {
    PluginHandleConfig hc;
    hc.plugin_path  = std::move(config.plugin_path);
    hc.config_dir   = std::move(config.config_dir);
    hc.country_code = std::move(config.country_code);
    m_handle = std::make_shared<BambuNetworkingPluginHandle>(std::move(hc));
}

CloudInventory::CloudInventory(std::shared_ptr<BambuNetworkingPluginHandle> handle)
    : m_handle(std::move(handle)) {}

CloudInventory::~CloudInventory() = default;

bool CloudInventory::plugin_loaded() const {
    return m_handle && m_handle->agent_ready();
}

std::vector<CloudDevice> CloudInventory::snapshot() const {
    std::lock_guard<std::mutex> g(m_cache_mutex);
    return m_cache;
}

std::shared_ptr<BambuNetworkingPluginHandle> CloudInventory::handle() const {
    return m_handle;
}

bool CloudInventory::refresh() {
    if (!m_handle) return false;
    if (!m_handle->init()) {
        // Plugin not loaded / agent not created. Leave cache untouched
        // (so we don't lose a previously-good list to a transient outage)
        // and report failure. Matches the phase-1 contract.
        return false;
    }
    if (!m_handle->is_user_login()) return false;

    unsigned int http_code = 0;
    std::string  body;
    const bool ok = m_handle->get_user_print_info(&http_code, &body);
    if (Slic3r::bridge::verbose()) {
        std::fprintf(stderr,
            "[cloud-inv] get_user_print_info ok=%d http_code=%u body_len=%zu "
            "body_preview='%.120s'\n",
            ok ? 1 : 0, http_code, body.size(), body.c_str());
    }
    if (!ok) return false;
    if (body.empty()) return false;

    std::vector<CloudDevice> parsed;
    try {
        auto j = nlohmann::json::parse(body);
        if (j.contains("devices") && j["devices"].is_array()) {
            for (const auto& elem : j["devices"]) {
                CloudDevice d;
                if (elem.contains("dev_id") && !elem["dev_id"].is_null())
                    d.dev_id = elem["dev_id"].get<std::string>();
                if (elem.contains("dev_name") && !elem["dev_name"].is_null())
                    d.name = elem["dev_name"].get<std::string>();
                if (elem.contains("dev_model_name") && !elem["dev_model_name"].is_null())
                    d.model = elem["dev_model_name"].get<std::string>();
                if (elem.contains("dev_online") && !elem["dev_online"].is_null())
                    d.online = elem["dev_online"].get<bool>();
                if (elem.contains("dev_access_code") && !elem["dev_access_code"].is_null()) {
                    auto acc = elem["dev_access_code"].get<std::string>();
                    acc.erase(std::remove(acc.begin(), acc.end(), '\n'), acc.end());
                    d.access_code = std::move(acc);
                }
                if (elem.contains("dev_ip") && !elem["dev_ip"].is_null())
                    d.lan_ip = elem["dev_ip"].get<std::string>();
                if (!d.dev_id.empty()) parsed.push_back(std::move(d));
            }
        }
    } catch (const std::exception&) {
        return false;
    }

    {
        std::lock_guard<std::mutex> g(m_cache_mutex);
        m_cache = std::move(parsed);
    }
    return true;
}

void CloudInventory::probe_lan_reachability() {
    std::lock_guard<std::mutex> g(m_cache_mutex);
    for (auto& d : m_cache) {
        d.lan_reachable = !d.lan_ip.empty();
    }
}

} // namespace bridge
} // namespace Slic3r
