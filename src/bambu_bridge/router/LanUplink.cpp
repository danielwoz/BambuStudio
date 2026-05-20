// Bambu Bridge — direct-LAN uplink implementation.
//
// See LanUplink.hpp for the design discussion. Per-device state is
// largely a routing table: the proprietary plugin owns the LAN TLS+MQTT
// transport, so this file is just plumbing between IUplink callbacks
// and the `BambuNetworkingPluginHandle` LAN-side wrappers.

#include "LanUplink.hpp"

#include "../BambuNetworkingPluginHandle.hpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace router {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct LanUplink::Impl {
    struct DeviceState {
        LanUplinkConfig                                cfg;
        // Topic → refcount. Cloud / LAN both use the same convention:
        // first add → SUBSCRIBE on the wire; last drop → UNSUBSCRIBE on
        // the wire. The plugin's LAN broker, like real Bambu LAN MQTT,
        // auto-pushes `device/<dev_id>/report` after connect_printer
        // succeeds — but we still refcount slicer-side subscriptions so
        // attach_downstream cleanup stays hygienic.
        std::unordered_map<std::string, int> topic_refs;
    };

    std::shared_ptr<BambuNetworkingPluginHandle>                  handle;
    mutable std::mutex                                            mu;
    std::unordered_map<std::string, std::unique_ptr<DeviceState>> devices;
    std::unordered_map<std::string,
                       server::IUplink::DownstreamPublisher>      downstreams;

    // The plugin only supports ONE active LAN connection at a time. Track
    // which dev_id currently owns it; is_connected(dev_id) returns true
    // only when the plugin reports up AND `current_connected_dev_id`
    // matches the caller's dev_id.
    std::string                                                   current_connected_dev_id;

    DeviceState* find_locked(const std::string& dev_id) {
        auto it = devices.find(dev_id);
        return it == devices.end() ? nullptr : it->second.get();
    }
};

// ---------------------------------------------------------------------------
// LanUplink public API
// ---------------------------------------------------------------------------

LanUplink::LanUplink()  : m_impl(std::make_unique<Impl>()) {}

LanUplink::~LanUplink() {
    // Drop every registered local-message receiver and disconnect the
    // plugin's LAN session if we hold it. Snapshot under lock first so
    // plugin callbacks acquiring the handle's own mutex don't deadlock
    // against ours.
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    std::vector<std::string> dev_ids;
    bool we_were_connected = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        h = m_impl->handle;
        for (auto& kv : m_impl->devices) dev_ids.push_back(kv.first);
        we_were_connected = !m_impl->current_connected_dev_id.empty();
    }
    if (h) {
        for (const auto& d : dev_ids) h->unregister_local_message_receiver(d);
        if (we_were_connected) h->disconnect_printer();
    }
}

void LanUplink::attach_plugin(std::shared_ptr<BambuNetworkingPluginHandle> handle) {
    // Same pattern as CloudUplink::attach_plugin: re-register every
    // existing device's local-message receiver against the new handle.
    std::vector<std::string> dev_ids;
    std::shared_ptr<BambuNetworkingPluginHandle> old;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        old = m_impl->handle;
        m_impl->handle = handle;
        for (auto& kv : m_impl->devices) dev_ids.push_back(kv.first);
    }
    if (old) {
        for (const auto& d : dev_ids) old->unregister_local_message_receiver(d);
    }
    if (handle) {
        Impl* impl = m_impl.get();
        for (const auto& dev_id : dev_ids) {
            handle->register_local_message_receiver(dev_id,
                [impl, dev_id](std::string topic,
                               std::vector<uint8_t> payload,
                               uint8_t qos) {
                server::IUplink::DownstreamPublisher cb;
                {
                    std::lock_guard<std::mutex> lk(impl->mu);
                    auto it = impl->downstreams.find(dev_id);
                    if (it != impl->downstreams.end()) cb = it->second;
                }
                if (cb) cb(std::move(topic), std::move(payload), qos);
            });
        }
    }
}

std::shared_ptr<BambuNetworkingPluginHandle> LanUplink::plugin_handle() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->handle;
}

void LanUplink::add_device(LanUplinkConfig cfg) {
    const std::string dev_id     = cfg.dev_id;
    const std::string dev_ip     = cfg.printer_ip;
    const std::string access     = cfg.access_code;
    const bool        use_ssl    = cfg.use_ssl;

    std::shared_ptr<BambuNetworkingPluginHandle> h;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto state = std::make_unique<Impl::DeviceState>();
        state->cfg = std::move(cfg);
        m_impl->devices[dev_id] = std::move(state);
        h = m_impl->handle;
    }

    if (!h) return;

    // Register the local-message receiver up front. The plugin may
    // start firing reports as soon as connect_printer lands.
    Impl* impl = m_impl.get();
    h->register_local_message_receiver(dev_id,
        [impl, dev_id](std::string topic,
                       std::vector<uint8_t> payload,
                       uint8_t qos) {
        server::IUplink::DownstreamPublisher cb;
        {
            std::lock_guard<std::mutex> lk(impl->mu);
            auto it = impl->downstreams.find(dev_id);
            if (it != impl->downstreams.end()) cb = it->second;
        }
        if (cb) cb(std::move(topic), std::move(payload), qos);
    });

    // Establish the LAN connection. The plugin only holds one LAN
    // connection at a time — if a different dev_id was previously
    // connected the plugin's own re-entry semantics handle the swap.
    int rc = h->connect_printer(dev_id, dev_ip, "bblp", access, use_ssl);
    if (rc == 0) {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        m_impl->current_connected_dev_id = dev_id;
    }
}

void LanUplink::remove_device(const std::string& dev_id) {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool was_current = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto it = m_impl->devices.find(dev_id);
        if (it == m_impl->devices.end()) return;
        m_impl->devices.erase(it);
        m_impl->downstreams.erase(dev_id);
        if (m_impl->current_connected_dev_id == dev_id) {
            m_impl->current_connected_dev_id.clear();
            was_current = true;
        }
        h = m_impl->handle;
    }
    if (h) {
        h->unregister_local_message_receiver(dev_id);
        // The plugin only has one LAN connection slot — disconnecting it
        // when the device that owned it goes away. If a different
        // dev_id was the active one, leave the plugin alone.
        if (was_current) h->disconnect_printer();
    }
}

bool LanUplink::is_connected(const std::string& dev_id) const {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool current_match = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->devices.count(dev_id) == 0) return false;
        h = m_impl->handle;
        current_match = (m_impl->current_connected_dev_id == dev_id);
    }
    if (!h || !current_match) return false;
    return h->is_local_connected();
}

void LanUplink::on_subscribe(const std::string& dev_id, std::string topic) {
    // The plugin only holds ONE active LAN connection at a time. When a
    // slicer subscribes to a device that isn't currently the plugin's
    // active dev_id, we have to swap — otherwise the plugin's
    // local-message receiver only fires for the previous device and
    // the slicer sees no push_status.
    //
    // We still refcount slicer-side topic subscribes for cleanup
    // hygiene (attach_downstream + on_disconnect coordinate against it).
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool need_swap = false;
    bool found = false;
    std::string dev_ip;
    std::string access_code;
    bool use_ssl = true;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto* d = m_impl->find_locked(dev_id);
        found = (d != nullptr);
        if (d) {
            ++d->topic_refs[topic];
            if (m_impl->current_connected_dev_id != dev_id) {
                need_swap   = true;
                dev_ip      = d->cfg.printer_ip;
                access_code = d->cfg.access_code;
                use_ssl     = d->cfg.use_ssl;
                h           = m_impl->handle;
            }
        }
    }
    if (!found) return;
    if (need_swap && h) {
        std::fprintf(stderr,
            "[lan-uplink] SWAP active dev → %s (ip=%s) on slicer subscribe %s\n",
            dev_id.c_str(), dev_ip.c_str(), topic.c_str());
        std::fflush(stderr);
        int rc = h->connect_printer(dev_id, dev_ip, "bblp", access_code, use_ssl);
        if (rc == 0) {
            std::lock_guard<std::mutex> lk(m_impl->mu);
            m_impl->current_connected_dev_id = dev_id;
        } else {
            std::fprintf(stderr,
                "[lan-uplink] SWAP failed rc=%d for dev=%s\n", rc, dev_id.c_str());
            std::fflush(stderr);
        }
    }
}

void LanUplink::on_publish(const std::string& dev_id, std::string topic,
                           std::vector<uint8_t> payload, uint8_t qos) {
    (void)topic; // plugin routes by dev_id, not by topic
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool have_device = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        have_device = m_impl->devices.count(dev_id) > 0;
        h = m_impl->handle;
    }
    if (!h || !have_device) return;
    // The slicer's payload is raw JSON. The plugin's
    // send_message_to_printer wraps it in the LAN MQTT PUBLISH envelope
    // when there's a live `connect_printer` session for this dev_id.
    std::string json(payload.begin(), payload.end());
    h->send_message_to_printer(dev_id, json, static_cast<int>(qos));
}

void LanUplink::on_unsubscribe(const std::string& dev_id, std::string topic) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    auto* d = m_impl->find_locked(dev_id);
    if (!d) return;
    auto it = d->topic_refs.find(topic);
    if (it == d->topic_refs.end()) return;
    if (--it->second <= 0) {
        d->topic_refs.erase(it);
    }
}

void LanUplink::on_disconnect(const std::string& dev_id) {
    // Slicer detached. Match CloudUplink behaviour: we do NOT tear down
    // the plugin's LAN session — the bridge keeps it hot so reports keep
    // flowing for the next slicer that connects. Just drop the
    // per-device downstream publisher.
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->downstreams.erase(dev_id);
}

void LanUplink::attach_downstream(const std::string& dev_id,
                                  DownstreamPublisher publisher) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (publisher) m_impl->downstreams[dev_id] = std::move(publisher);
    else           m_impl->downstreams.erase(dev_id);
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
