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

#include <algorithm>
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

    // Per-session downstream entry. Multiple sessions can subscribe to
    // the same dev_id; printer-side traffic fans out to all of them.
    struct Subscriber {
        uint64_t                            session_id;
        server::IUplink::DownstreamPublisher publisher;
    };

    // Last few inbound messages per dev_id, replayed synchronously to a
    // newly-attached publisher so a fresh slicer can render state
    // without waiting for the printer's next push. Bounded to 2 — a
    // pushall (~25 KB) plus one delta covers the realistic case.
    struct RetainedMsg {
        std::string          topic;
        std::vector<uint8_t> payload;
        uint8_t              qos;
    };
    static constexpr size_t kRetainedCap = 2;

    std::shared_ptr<BambuNetworkingPluginHandle>                handle;
    mutable std::mutex                                          mu;
    std::unordered_map<std::string, std::unique_ptr<DeviceState>> devices;
    std::unordered_map<std::string, std::vector<Subscriber>>    downstreams;
    std::unordered_map<std::string, std::vector<RetainedMsg>>   retained;

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
                std::vector<server::IUplink::DownstreamPublisher> cbs;
                {
                    std::lock_guard<std::mutex> lk(impl->mu);
                    auto it = impl->downstreams.find(dev_id);
                    if (it != impl->downstreams.end()) {
                        cbs.reserve(it->second.size());
                        for (auto& s : it->second) cbs.push_back(s.publisher);
                    }
                    // Cache for replay to future subscribers.
                    auto& ring = impl->retained[dev_id];
                    ring.push_back({topic, payload, qos});
                    if (ring.size() > Impl::kRetainedCap) {
                        ring.erase(ring.begin(),
                                   ring.begin() + (ring.size() - Impl::kRetainedCap));
                    }
                }
                for (auto& cb : cbs) {
                    if (cb) cb(topic, payload, qos);
                }
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
    std::fprintf(stderr, "[cloud-uplink] add_device dev=%s\n", dev_id.c_str());
    std::fflush(stderr);
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto state = std::make_unique<Impl::DeviceState>();
        state->cfg = std::move(cfg);
        m_impl->devices[dev_id] = std::move(state);
        h = m_impl->handle;
    }
    if (h) {
        Impl* impl = m_impl.get();
        h->register_receiver(dev_id,
            [impl, dev_id](std::string topic,
                           std::vector<uint8_t> payload,
                           uint8_t qos) {
            std::vector<server::IUplink::DownstreamPublisher> cbs;
            size_t subscriber_count = 0;
            {
                std::lock_guard<std::mutex> lk(impl->mu);
                auto it = impl->downstreams.find(dev_id);
                if (it != impl->downstreams.end()) {
                    subscriber_count = it->second.size();
                    cbs.reserve(subscriber_count);
                    for (auto& s : it->second) cbs.push_back(s.publisher);
                }
                // Cache for replay to future subscribers.
                auto& ring = impl->retained[dev_id];
                ring.push_back({topic, payload, qos});
                if (ring.size() > Impl::kRetainedCap) {
                    ring.erase(ring.begin(),
                               ring.begin() + (ring.size() - Impl::kRetainedCap));
                }
            }
            std::fprintf(stderr,
                "[cloud-uplink] receiver dev=%s topic=%s bytes=%zu qos=%u subscribers=%zu\n",
                dev_id.c_str(), topic.c_str(), payload.size(), unsigned(qos), subscriber_count);
            std::fflush(stderr);
            for (auto& cb : cbs) {
                if (cb) cb(topic, payload, qos);
            }
        });
        std::fprintf(stderr, "[cloud-uplink] register_receiver dev=%s installed\n", dev_id.c_str());
        std::fflush(stderr);
    } else {
        std::fprintf(stderr, "[cloud-uplink] add_device dev=%s NO HANDLE — receiver not registered\n", dev_id.c_str());
        std::fflush(stderr);
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
        m_impl->retained.erase(dev_id);
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
    if (!h || !have_device) {
        std::fprintf(stderr,
            "[cloud-uplink] on_publish DROP dev=%s handle=%d have_device=%d "
            "bytes=%zu\n",
            dev_id.c_str(), int(bool(h)), int(have_device), payload.size());
        std::fflush(stderr);
        return;
    }
    // The slicer's payload is raw JSON (phase 4 confirmed). The plugin
    // wraps it in whatever cloud envelope it needs.
    std::string json(payload.begin(), payload.end());
    int rc = h->publish_to_device(dev_id, json, static_cast<int>(qos));
    std::fprintf(stderr,
        "[cloud-uplink] on_publish dev=%s bytes=%zu qos=%u "
        "publish_to_device rc=%d\n",
        dev_id.c_str(), payload.size(), unsigned(qos), rc);
    std::fflush(stderr);
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
    // Slicer detached. With multi-subscriber fan-out, on_disconnect from
    // the broker means "no more sessions for this dev_id" — but per-
    // session detach has already been performed by MqttBroker's
    // session-end cleanup. Keep this a no-op for downstreams to avoid
    // wiping concurrent sessions racily.
    (void)dev_id;
}

void CloudUplink::attach_downstream(const std::string& dev_id,
                                    uint64_t            session_id,
                                    DownstreamPublisher publisher) {
    // Add (or replace, if same session_id reattaches) the subscriber.
    // Then snapshot retained[dev_id] under lock; invoke the new
    // publisher with each retained entry OUTSIDE the lock so it can
    // enqueue into a session's send queue without deadlocking on us.
    std::vector<Impl::RetainedMsg> replay;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (!publisher) {
            // No-publisher attach is a detach signal in the new world.
            auto it = m_impl->downstreams.find(dev_id);
            if (it != m_impl->downstreams.end()) {
                auto& vec = it->second;
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                              [&](const Impl::Subscriber& s) {
                                  return s.session_id == session_id;
                              }),
                          vec.end());
                if (vec.empty()) m_impl->downstreams.erase(it);
            }
            return;
        }
        auto& vec = m_impl->downstreams[dev_id];
        bool replaced = false;
        for (auto& s : vec) {
            if (s.session_id == session_id) {
                s.publisher = publisher;
                replaced = true;
                break;
            }
        }
        if (!replaced) vec.push_back({session_id, publisher});
        auto rit = m_impl->retained.find(dev_id);
        if (rit != m_impl->retained.end()) replay = rit->second;
    }
    for (auto& m : replay) {
        publisher(m.topic, m.payload, m.qos);
    }
}

void CloudUplink::detach_downstream(const std::string& dev_id,
                                    uint64_t            session_id) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    auto it = m_impl->downstreams.find(dev_id);
    if (it == m_impl->downstreams.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                  [&](const Impl::Subscriber& s) {
                      return s.session_id == session_id;
                  }),
              vec.end());
    if (vec.empty()) m_impl->downstreams.erase(it);
}

void CloudUplink::attach_downstream(const std::string& dev_id,
                                    DownstreamPublisher publisher) {
    // Deprecated. Synthesises session_id = 0 so legacy callers keep
    // working but only get the single "default" slot. The new world
    // routes per-session via the 3-arg overload.
    std::fprintf(stderr,
        "[cloud-uplink] WARN: deprecated 2-arg attach_downstream(dev=%s) — "
        "caller should use the (dev_id, session_id, publisher) overload\n",
        dev_id.c_str());
    std::fflush(stderr);
    if (publisher) {
        attach_downstream(dev_id, /*session_id=*/0, std::move(publisher));
    } else {
        detach_downstream(dev_id, /*session_id=*/0);
    }
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
