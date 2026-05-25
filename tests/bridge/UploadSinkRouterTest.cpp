// Bambu Bridge — UploadSinkRouter unit test (phase 9).
//
// Pure state-machine test: no sockets, no real FTPS. We subclass
// LanUploadSink and CloudUploadSink so each deliver() call records the
// invocation and returns a toggleable success/failure response.
//
// Scenarios:
//   1. Both sinks healthy, prefer_lan=true              → goes to LAN.
//   2. LAN sink fails, fallback_to_cloud=true           → falls back to cloud.
//   3. LAN sink fails, fallback_to_cloud=false          → returns LAN failure.
//   4. No sinks attached                                → returns no-sink failure.
//   5. prefer_lan=false, cloud succeeds first           → goes to cloud only.
//   6. Both sinks fail with fallback enabled            → combined error.

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "router/CloudUploadSink.hpp"
#include "router/LanUploadSink.hpp"
#include "router/UploadSinkRouter.hpp"
#include "router/UplinkHealth.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

using Slic3r::bridge::router::CloudUploadSink;
using Slic3r::bridge::router::LanUploadSink;
using Slic3r::bridge::router::UploadSinkRouter;
using Slic3r::bridge::router::UplinkHealthMonitor;
using Slic3r::bridge::server::UploadJob;
using Slic3r::bridge::server::UploadResult;

// Stub LAN sink: records every deliver() and returns success/failure
// based on toggle. We override the base's deliver() to skip the real
// FTPS forwarder.
class StubLanUploadSink : public LanUploadSink {
public:
    mutable std::mutex mu;
    std::atomic<bool>  succeed{true};
    std::vector<std::string> jobs;
    std::string        last_filename;
    std::string        url_prefix = "ftps://test/";

    UploadResult deliver(UploadJob job) override {
        std::lock_guard<std::mutex> lk(mu);
        jobs.push_back(job.dev_id + ":" + job.filename);
        last_filename = job.filename;
        UploadResult r;
        if (succeed.load()) {
            r.ok         = true;
            r.remote_url = url_prefix + job.filename;
        } else {
            r.ok            = false;
            r.error_message = "stub-lan failure";
        }
        return r;
    }
};

class StubCloudUploadSink : public CloudUploadSink {
public:
    mutable std::mutex mu;
    std::atomic<bool>  succeed{true};
    std::vector<std::string> jobs;
    std::string        url_prefix = "https://cloud-stub/";

    UploadResult deliver(UploadJob job) override {
        std::lock_guard<std::mutex> lk(mu);
        jobs.push_back(job.dev_id + ":" + job.filename);
        UploadResult r;
        if (succeed.load()) {
            r.ok         = true;
            r.remote_url = url_prefix + job.filename;
        } else {
            r.ok            = false;
            r.error_message = "stub-cloud failure";
        }
        return r;
    }
};

UploadJob make_job(const std::string& dev_id, const std::string& filename,
                   size_t nbytes) {
    UploadJob j;
    j.dev_id      = dev_id;
    j.filename    = filename;
    j.remote_path = "/model/" + filename;
    j.content.assign(nbytes, 0xCC);
    j.received_at = std::chrono::system_clock::now();
    return j;
}

} // namespace

int main() {
    const std::string dev_id = "EXAMPLESERIAL01";
    auto lan   = std::make_shared<StubLanUploadSink>();
    auto cloud = std::make_shared<StubCloudUploadSink>();

    // ---- Scenario 1: both healthy, prefer_lan=true → LAN ----
    {
        UploadSinkRouter router;
        router.set_lan_sink(lan);
        router.set_cloud_sink(cloud);
        UploadSinkRouter::Policy pol;
        pol.prefer_lan        = true;
        pol.fallback_to_cloud = true;
        router.set_policy(pol);

        auto r = router.deliver(make_job(dev_id, "a.3mf", 128));
        check(r.ok,                "S1: deliver succeeded");
        check(r.remote_url.find("ftps://") == 0,
              "S1: remote_url is the LAN URL");
        {
            std::lock_guard<std::mutex> lk(lan->mu);
            check(lan->jobs.size() == 1, "S1: LAN saw the upload");
        }
        {
            std::lock_guard<std::mutex> lk(cloud->mu);
            check(cloud->jobs.empty(),    "S1: cloud did NOT see the upload");
        }
        lan->jobs.clear();
        cloud->jobs.clear();
    }

    // ---- Scenario 2: LAN fails → fallback to cloud ----
    {
        lan->succeed.store(false);
        cloud->succeed.store(true);
        UploadSinkRouter router;
        router.set_lan_sink(lan);
        router.set_cloud_sink(cloud);
        UploadSinkRouter::Policy pol;
        pol.prefer_lan        = true;
        pol.fallback_to_cloud = true;
        router.set_policy(pol);

        auto r = router.deliver(make_job(dev_id, "b.3mf", 256));
        check(r.ok,
              "S2: deliver overall succeeded via fallback");
        check(r.remote_url.find("https://cloud-stub/") == 0,
              "S2: remote_url is the cloud URL");
        {
            std::lock_guard<std::mutex> lk(lan->mu);
            check(lan->jobs.size() == 1, "S2: LAN was tried first");
        }
        {
            std::lock_guard<std::mutex> lk(cloud->mu);
            check(cloud->jobs.size() == 1, "S2: cloud picked up the fallback");
        }
        lan->jobs.clear();
        cloud->jobs.clear();
    }

    // ---- Scenario 3: LAN fails, fallback disabled → surface LAN error ----
    {
        lan->succeed.store(false);
        cloud->succeed.store(true);
        UploadSinkRouter router;
        router.set_lan_sink(lan);
        router.set_cloud_sink(cloud);
        UploadSinkRouter::Policy pol;
        pol.prefer_lan        = true;
        pol.fallback_to_cloud = false;
        router.set_policy(pol);

        auto r = router.deliver(make_job(dev_id, "c.3mf", 64));
        check(!r.ok, "S3: deliver failed (fallback disabled)");
        check(r.error_message.find("stub-lan") != std::string::npos,
              "S3: error mentions LAN sink failure");
        {
            std::lock_guard<std::mutex> lk(cloud->mu);
            check(cloud->jobs.empty(),
                  "S3: cloud was NOT tried because fallback disabled");
        }
        lan->jobs.clear();
        cloud->jobs.clear();
    }

    // ---- Scenario 4: no sinks attached → no-sink failure ----
    {
        UploadSinkRouter router;
        auto r = router.deliver(make_job(dev_id, "d.3mf", 1));
        check(!r.ok, "S4: deliver fails when no sinks are attached");
        check(r.error_message.find("no healthy sink") != std::string::npos,
              "S4: error mentions no-healthy-sink");
    }

    // ---- Scenario 5: prefer_lan=false, cloud healthy → cloud only ----
    {
        lan->succeed.store(true);
        cloud->succeed.store(true);
        UploadSinkRouter router;
        router.set_lan_sink(lan);
        router.set_cloud_sink(cloud);
        UploadSinkRouter::Policy pol;
        pol.prefer_lan        = false;
        pol.fallback_to_cloud = true;
        router.set_policy(pol);

        auto r = router.deliver(make_job(dev_id, "e.3mf", 16));
        check(r.ok, "S5: deliver succeeded");
        check(r.remote_url.find("https://cloud-stub/") == 0,
              "S5: prefer_lan=false routed to cloud");
        {
            std::lock_guard<std::mutex> lk(lan->mu);
            check(lan->jobs.empty(),
                  "S5: LAN was not consulted when cloud is primary");
        }
        lan->jobs.clear();
        cloud->jobs.clear();
    }

    // ---- Scenario 6: both sinks fail with fallback → combined error ----
    {
        lan->succeed.store(false);
        cloud->succeed.store(false);
        UploadSinkRouter router;
        router.set_lan_sink(lan);
        router.set_cloud_sink(cloud);
        UploadSinkRouter::Policy pol;
        pol.prefer_lan        = true;
        pol.fallback_to_cloud = true;
        router.set_policy(pol);

        auto r = router.deliver(make_job(dev_id, "f.3mf", 32));
        check(!r.ok, "S6: both sinks failing → overall failure");
        check(r.error_message.find("stub-lan") != std::string::npos &&
              r.error_message.find("stub-cloud") != std::string::npos,
              "S6: error message aggregates both sink failures");
        {
            std::lock_guard<std::mutex> lk(lan->mu);
            check(lan->jobs.size() == 1,   "S6: LAN was tried");
        }
        {
            std::lock_guard<std::mutex> lk(cloud->mu);
            check(cloud->jobs.size() == 1, "S6: cloud was tried as fallback");
        }
    }

    if (g_fails) {
        std::fprintf(stderr, "UploadSinkRouterTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("UploadSinkRouterTest: ok\n");
    return 0;
}
