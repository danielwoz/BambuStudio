// Bambu Bridge — cloud-fallback uplink implementation (phase 6).
//
// See CloudUplink.hpp for the design discussion. This file holds:
//
//   * `CloudUplink::Impl` — per-dev_id state owner, topic refcount,
//     dispatch between IUplink calls and the shared
//     BambuNetworkingPluginHandle.
//
// Unlike LanUplink, there is no per-device session/thread here: the
// plugin owns the cloud transport for the whole process, so CloudUplink
// is largely a routing table.

#include "CloudUplink.hpp"

#include "../BambuNetworkingPluginHandle.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct CloudUplink::Impl {
    struct DeviceState {
        CloudUplinkConfig                  cfg;
        // Topic → refcount. Cloud-side we still keep this so the bridge
        // doesn't redundantly call add_subscribe when multiple slicers
        // (or one slicer with multiple sessions) share the same dev_id.
        // For the cloud broker the dev_id IS the subscription unit, so
        // any topic ref simply marks "we want reports for this device";
        // the first non-zero count triggers add_subscribe, the last
        // 1→0 triggers del_subscribe.
        std::unordered_map<std::string, int> topic_refs;
        bool subscribed = false;
    };

    std::shared_ptr<BambuNetworkingPluginHandle>                handle;
    mutable std::mutex                                          mu;
    std::unordered_map<std::string, std::unique_ptr<DeviceState>> devices;
    std::unordered_map<std::string,
                       server::IUplink::DownstreamPublisher>     downstreams;

    DeviceState* find_locked(const std::string& dev_id) {
        auto it = devices.find(dev_id);
        return it == devices.end() ? nullptr : it->second.get();
    }
};

// ---------------------------------------------------------------------------
// CloudUplink public API
// ---------------------------------------------------------------------------

CloudUplink::CloudUplink() : m_impl(std::make_unique<Impl>()) {}

CloudUplink::~CloudUplink() {
    // Unregister every receiver so the plugin doesn't fire into a
    // destroyed CloudUplink. Done outside any other lock to avoid
    // ordering issues with the plugin's dispatch mutex.
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    std::vector<std::string> dev_ids;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        h = m_impl->handle;
        for (auto& kv : m_impl->devices) dev_ids.push_back(kv.first);
    }
    if (h) {
        for (const auto& d : dev_ids) h->unregister_receiver(d);
    }
}

void CloudUplink::attach_plugin(std::shared_ptr<BambuNetworkingPluginHandle> handle) {
    // Re-register every existing device's receiver against the new handle.
    std::vector<std::string> dev_ids;
    std::shared_ptr<BambuNetworkingPluginHandle> old;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        old = m_impl->handle;
        m_impl->handle = handle;
        for (auto& kv : m_impl->devices) dev_ids.push_back(kv.first);
    }
    if (old) {
        for (const auto& d : dev_ids) old->unregister_receiver(d);
    }
    if (handle) {
        Impl* impl = m_impl.get();
        for (const auto& dev_id : dev_ids) {
            handle->register_receiver(dev_id,
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

std::shared_ptr<BambuNetworkingPluginHandle> CloudUplink::plugin_handle() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->handle;
}

void CloudUplink::add_device(CloudUplinkConfig cfg) {
    const std::string dev_id = cfg.dev_id;
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto state = std::make_unique<Impl::DeviceState>();
        state->cfg = std::move(cfg);
        m_impl->devices[dev_id] = std::move(state);
        h = m_impl->handle;
    }
    if (h) {
        // Register the receiver up front. The plugin's dispatcher will
        // call us once messages start arriving — even before our first
        // subscribe — and we want them routed properly.
        Impl* impl = m_impl.get();
        h->register_receiver(dev_id,
            [impl, dev_id](std::string topic,
                           std::vector<uint8_t> payload,
                           uint8_t qos) {
            server::IUplink::DownstreamPublisher cb;
            {
                std::lock_guard<std::mutex> lk(impl->mu);
                auto it = impl->downstreams.find(dev_id);
                if (it != impl->downstreams.end()) cb = it->second;
            }
            std::fprintf(stderr,
                "[cloud-uplink] inbound dev_id=%s topic=%s payload_len=%zu "
                "qos=%u downstream=%s\n",
                dev_id.c_str(), topic.c_str(), payload.size(),
                static_cast<unsigned>(qos), cb ? "yes" : "no");
            if (cb) cb(std::move(topic), std::move(payload), qos);
        });
    }
}

void CloudUplink::remove_device(const std::string& dev_id) {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool was_subscribed = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto it = m_impl->devices.find(dev_id);
        if (it == m_impl->devices.end()) return;
        was_subscribed = it->second->subscribed;
        m_impl->devices.erase(it);
        m_impl->downstreams.erase(dev_id);
        h = m_impl->handle;
    }
    if (h) {
        if (was_subscribed) h->unsubscribe_device(dev_id);
        h->unregister_receiver(dev_id);
    }
}

bool CloudUplink::is_connected(const std::string& dev_id) const {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool have_device = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        h = m_impl->handle;
        have_device = m_impl->devices.count(dev_id) > 0;
    }
    if (!h || !have_device) return false;
    if (!h->agent_ready())  return false;
    if (!h->is_user_login()) return false;
    return h->is_server_connected();
}

void CloudUplink::on_subscribe(const std::string& dev_id, std::string topic) {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool first_subscribe = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto* d = m_impl->find_locked(dev_id);
        if (!d) return;
        int& rc = d->topic_refs[topic];
        if (rc == 0 && !d->subscribed) {
            first_subscribe = true;
            d->subscribed = true;
        }
        ++rc;
        h = m_impl->handle;
    }
    if (h && first_subscribe) {
        h->subscribe_device(dev_id);
    }
}

void CloudUplink::on_publish(const std::string& dev_id, std::string topic,
                             std::vector<uint8_t> payload, uint8_t qos) {
    (void)topic; // cloud broker derives topic from dev_id internally
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool have_device = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        have_device = m_impl->devices.count(dev_id) > 0;
        h = m_impl->handle;
    }
    if (!h || !have_device) return;
    // The slicer's payload is raw JSON (phase 4 confirmed). The plugin
    // wraps it in whatever cloud envelope it needs.
    std::string json(payload.begin(), payload.end());
    h->publish_to_device(dev_id, json, static_cast<int>(qos));
}

void CloudUplink::on_unsubscribe(const std::string& dev_id, std::string topic) {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool last_unsubscribe = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto* d = m_impl->find_locked(dev_id);
        if (!d) return;
        auto it = d->topic_refs.find(topic);
        if (it == d->topic_refs.end()) return;
        if (--it->second <= 0) {
            d->topic_refs.erase(it);
            if (d->topic_refs.empty() && d->subscribed) {
                last_unsubscribe = true;
                d->subscribed = false;
            }
        }
        h = m_impl->handle;
    }
    if (h && last_unsubscribe) {
        h->unsubscribe_device(dev_id);
    }
}

void CloudUplink::on_disconnect(const std::string& dev_id) {
    // Slicer detached. Match LanUplink's behaviour: we do NOT tear down
    // our cloud routing — the agent is shared and reports keep flowing
    // for the next slicer that connects. Just drop the per-device
    // downstream publisher.
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->downstreams.erase(dev_id);
}

void CloudUplink::attach_downstream(const std::string& dev_id,
                                    DownstreamPublisher publisher) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (publisher) m_impl->downstreams[dev_id] = std::move(publisher);
    else           m_impl->downstreams.erase(dev_id);
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
