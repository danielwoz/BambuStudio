// Bambu Bridge — default uplink stub (phase 4).

#include "NullUplink.hpp"

#include <algorithm>
#include <cstdio>

namespace Slic3r {
namespace bridge {
namespace router {

void NullUplink::on_subscribe(const std::string& dev_id, std::string topic) {
}

void NullUplink::on_publish(const std::string& dev_id, std::string topic,
                            std::vector<uint8_t> payload, uint8_t qos) {
}

void NullUplink::on_unsubscribe(const std::string& dev_id, std::string topic) {
}

void NullUplink::on_disconnect(const std::string& dev_id) {
    // Per-session detach is the new API; on_disconnect from the broker
    // can race with concurrent reconnect, so leave entries alone.
    (void)dev_id;
}

void NullUplink::attach_downstream(const std::string& dev_id,
                                   uint64_t            session_id,
                                   DownstreamPublisher publisher) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (!publisher) {
        auto it = m_pub.find(dev_id);
        if (it == m_pub.end()) return;
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                      [&](const Subscriber& s) {
                          return s.session_id == session_id;
                      }),
                  vec.end());
        if (vec.empty()) m_pub.erase(it);
        return;
    }
    auto& vec = m_pub[dev_id];
    for (auto& s : vec) {
        if (s.session_id == session_id) {
            s.publisher = std::move(publisher);
            return;
        }
    }
    vec.push_back({session_id, std::move(publisher)});
}

void NullUplink::detach_downstream(const std::string& dev_id,
                                   uint64_t            session_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_pub.find(dev_id);
    if (it == m_pub.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                  [&](const Subscriber& s) {
                      return s.session_id == session_id;
                  }),
              vec.end());
    if (vec.empty()) m_pub.erase(it);
}

void NullUplink::attach_downstream(const std::string& dev_id,
                                   DownstreamPublisher publisher) {
    std::fprintf(stderr,
        "[null-uplink] WARN: deprecated 2-arg attach_downstream(dev=%s)\n",
        dev_id.c_str());
    std::fflush(stderr);
    if (publisher) attach_downstream(dev_id, /*session_id=*/0, std::move(publisher));
    else           detach_downstream(dev_id, /*session_id=*/0);
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
