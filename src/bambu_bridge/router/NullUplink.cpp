// Bambu Bridge — default uplink stub (phase 4).

#include "NullUplink.hpp"

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
    std::lock_guard<std::mutex> lk(m_mu);
    m_pub.erase(dev_id);
}

void NullUplink::attach_downstream(const std::string& dev_id,
                                   DownstreamPublisher publisher) {
    std::lock_guard<std::mutex> lk(m_mu);
    if (publisher) m_pub[dev_id] = std::move(publisher);
    else           m_pub.erase(dev_id);
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
