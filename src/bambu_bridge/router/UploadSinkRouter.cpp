// Bambu Bridge — upload sink router implementation (phase 9).

#include "UploadSinkRouter.hpp"

#include "LanUploadSink.hpp"
#include "CloudUploadSink.hpp"
#include "UplinkHealth.hpp"

#include <cstdio>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

UploadSinkRouter::UploadSinkRouter()  = default;
UploadSinkRouter::~UploadSinkRouter() = default;

void UploadSinkRouter::set_lan_sink(std::shared_ptr<LanUploadSink> lan) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_lan = std::move(lan);
}

void UploadSinkRouter::set_cloud_sink(std::shared_ptr<CloudUploadSink> cloud) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_cloud = std::move(cloud);
}

void UploadSinkRouter::set_health_monitor(std::shared_ptr<UplinkHealthMonitor> monitor) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_health = std::move(monitor);
}

void UploadSinkRouter::set_policy(Policy p) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_policy = p;
}

server::UploadResult UploadSinkRouter::deliver(server::UploadJob job) {
    std::shared_ptr<LanUploadSink>       lan;
    std::shared_ptr<CloudUploadSink>     cloud;
    std::shared_ptr<UplinkHealthMonitor> health;
    Policy                               policy;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        lan    = m_lan;
        cloud  = m_cloud;
        health = m_health;
        policy = m_policy;
    }

    bool lan_ok   = false;
    bool cloud_ok = false;
    if (health) {
        auto snap = health->snapshot(job.dev_id);
        lan_ok    = snap.lan_connected;
        cloud_ok  = snap.cloud_connected;
    } else {
        // If we don't have a health monitor, assume "available" iff the
        // sink shared_ptr is present. The sink itself will fail loudly if
        // the device isn't actually reachable; we'll honour fallback.
        lan_ok   = static_cast<bool>(lan);
        cloud_ok = static_cast<bool>(cloud);
    }

    // Pick primary + secondary by policy.
    server::IUploadSink* primary   = nullptr;
    const char*          primary_name = "";
    server::IUploadSink* secondary = nullptr;
    const char*          secondary_name = "";

    if (policy.prefer_lan) {
        if (lan_ok   && lan)   { primary   = lan.get();   primary_name   = "lan"; }
        if (cloud_ok && cloud) {
            if (!primary) { primary = cloud.get(); primary_name = "cloud"; }
            else          { secondary = cloud.get(); secondary_name = "cloud"; }
        }
        // If LAN was chosen primary but cloud isn't healthy, still allow
        // fallback to cloud sink if `fallback_to_cloud` and we have one.
        if (primary == lan.get() && !secondary && cloud && policy.fallback_to_cloud) {
            secondary = cloud.get();
            secondary_name = "cloud";
        }
    } else {
        if (cloud_ok && cloud) { primary   = cloud.get(); primary_name   = "cloud"; }
        if (lan_ok   && lan)   {
            if (!primary) { primary = lan.get(); primary_name = "lan"; }
            else          { secondary = lan.get(); secondary_name = "lan"; }
        }
        if (primary == cloud.get() && !secondary && lan && policy.fallback_to_cloud) {
            // fallback_to_cloud is misnamed when prefer_lan=false (it
            // really means "allow fallback at all"). Reuse the same flag.
            secondary = lan.get();
            secondary_name = "lan";
        }
    }

    if (!primary) {
        server::UploadResult r;
        r.ok            = false;
        r.error_message = "UploadSinkRouter: no healthy sink available for " + job.dev_id;
        std::fprintf(stderr,
            "[upload-router] FAIL dev=%s no healthy sink (lan_ok=%d cloud_ok=%d)\n",
            job.dev_id.c_str(), static_cast<int>(lan_ok), static_cast<int>(cloud_ok));
        return r;
    }

    // Try primary.
    std::fprintf(stderr,
        "[upload-router] dev=%s file=%s -> %s\n",
        job.dev_id.c_str(), job.filename.c_str(), primary_name);
    server::UploadJob job_copy = job; // sub-sinks consume by value
    auto result = primary->deliver(std::move(job_copy));
    if (result.ok) return result;

    // Primary failed. Aggregate the error and try secondary if allowed.
    std::string primary_err = result.error_message;
    if (secondary && policy.fallback_to_cloud) {
        std::fprintf(stderr,
            "[upload-router] dev=%s primary (%s) failed: %s — falling back to %s\n",
            job.dev_id.c_str(), primary_name, primary_err.c_str(), secondary_name);
        auto fb = secondary->deliver(std::move(job));
        if (fb.ok) {
            fb.error_message.clear();
            return fb;
        }
        // Both failed; surface a combined error message.
        server::UploadResult combined;
        combined.ok            = false;
        combined.error_message =
            std::string("primary (") + primary_name + ") failed: " + primary_err +
            "; secondary (" + secondary_name + ") failed: " + fb.error_message;
        return combined;
    }

    return result;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
