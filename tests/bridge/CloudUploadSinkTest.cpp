// Bambu Bridge — unit test for the real (no-longer-stubbed) CloudUpload
// Sink. Uses a MockPluginHandle that records every `upload_gcode_to_sdcard`
// call without ever loading the proprietary plugin.

#include "../../src/bambu_bridge/BambuNetworkingPluginHandle.hpp"
#include "../../src/bambu_bridge/router/CloudUploadSink.hpp"
#include "../../src/bambu_bridge/server/IUploadSink.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

class MockPluginHandle : public Slic3r::bridge::BambuNetworkingPluginHandle {
public:
    struct UploadCall {
        std::string dev_id;
        std::string local_file_path;
        std::string project_name;
        std::string connection_type;
        std::vector<uint8_t> bytes_seen;   // spooled file contents we read back
    };

    mutable std::mutex      mu;
    std::vector<UploadCall> uploads;
    int                     return_code = 0;
    bool                    have_export = true;

    MockPluginHandle()
        : Slic3r::bridge::BambuNetworkingPluginHandle({}) {
        this->set_agent_ready_for_test(true);
    }

    bool init() override         { return true; }
    bool agent_ready() const override { return true; }

    int upload_gcode_to_sdcard(const CloudUploadParams& p) override {
        if (!have_export) return -2;

        UploadCall c;
        c.dev_id          = p.dev_id;
        c.local_file_path = p.local_file_path;
        c.project_name    = p.project_name;
        c.connection_type = p.connection_type;

        // Read back the spooled tempfile so the test can assert the
        // bytes the sink wrote match what was handed to deliver().
        std::ifstream f(p.local_file_path, std::ios::binary);
        if (f) {
            c.bytes_seen.assign(
                std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>());
        }

        std::lock_guard<std::mutex> lk(mu);
        uploads.push_back(std::move(c));
        return return_code;
    }
};

int test_no_plugin_attached() {
    Slic3r::bridge::router::CloudUploadSink sink;
    Slic3r::bridge::server::UploadJob job;
    job.dev_id   = "TESTDEV1";
    job.filename = "demo.3mf";
    job.content  = {0x42, 0x42, 0x42};

    auto r = sink.deliver(std::move(job));
    if (r.ok) {
        std::fprintf(stderr, "FAIL: no-plugin sink reported ok=true\n");
        return 1;
    }
    if (r.error_message.find("no plugin handle") == std::string::npos) {
        std::fprintf(stderr, "FAIL: error message missing 'no plugin handle': %s\n",
                     r.error_message.c_str());
        return 1;
    }
    return 0;
}

int test_happy_path_through_plugin() {
    auto plugin = std::make_shared<MockPluginHandle>();
    Slic3r::bridge::router::CloudUploadSink sink;
    sink.attach_plugin(plugin);

    Slic3r::bridge::server::UploadJob job;
    job.dev_id      = "0938BC582502312";    // H2S serial-shape
    job.filename    = "demo.3mf";
    job.remote_path = "/model/demo.3mf";
    job.content     = std::vector<uint8_t>(2048);
    for (size_t i = 0; i < job.content.size(); ++i) {
        job.content[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto r = sink.deliver(job);
    if (!r.ok) {
        std::fprintf(stderr, "FAIL: deliver returned ok=false: %s\n",
                     r.error_message.c_str());
        return 1;
    }
    if (r.remote_url.find("bambu-cloud:///") != 0) {
        std::fprintf(stderr, "FAIL: remote_url shape wrong: %s\n",
                     r.remote_url.c_str());
        return 1;
    }

    std::lock_guard<std::mutex> lk(plugin->mu);
    if (plugin->uploads.size() != 1) {
        std::fprintf(stderr, "FAIL: expected 1 upload, got %zu\n",
                     plugin->uploads.size());
        return 1;
    }
    auto& u = plugin->uploads[0];
    if (u.dev_id != "0938BC582502312") {
        std::fprintf(stderr, "FAIL: dev_id wrong: %s\n", u.dev_id.c_str()); return 1;
    }
    if (u.project_name != "demo.3mf") {
        std::fprintf(stderr, "FAIL: project_name wrong: %s\n", u.project_name.c_str()); return 1;
    }
    if (u.connection_type != "cloud") {
        std::fprintf(stderr, "FAIL: connection_type wrong: %s\n", u.connection_type.c_str()); return 1;
    }
    if (u.bytes_seen.size() != job.content.size()) {
        std::fprintf(stderr, "FAIL: byte count wrong: got %zu expected %zu\n",
                     u.bytes_seen.size(), job.content.size());
        return 1;
    }
    if (std::memcmp(u.bytes_seen.data(), job.content.data(), job.content.size()) != 0) {
        std::fprintf(stderr, "FAIL: spooled bytes don't match input\n");
        return 1;
    }
    // Temp file should be removed by the sink after the upload returned.
    if (std::ifstream(u.local_file_path).good()) {
        std::fprintf(stderr,
            "FAIL: tempfile %s still exists after deliver()\n",
            u.local_file_path.c_str());
        return 1;
    }
    return 0;
}

int test_plugin_missing_export() {
    auto plugin = std::make_shared<MockPluginHandle>();
    plugin->have_export = false;
    Slic3r::bridge::router::CloudUploadSink sink;
    sink.attach_plugin(plugin);

    Slic3r::bridge::server::UploadJob job;
    job.dev_id   = "X";
    job.filename = "y.3mf";
    job.content  = {1, 2, 3};

    auto r = sink.deliver(std::move(job));
    if (r.ok) {
        std::fprintf(stderr, "FAIL: missing-export sink reported ok=true\n");
        return 1;
    }
    if (r.error_message.find("rc=-2") == std::string::npos) {
        std::fprintf(stderr, "FAIL: error message missing rc=-2: %s\n",
                     r.error_message.c_str());
        return 1;
    }
    return 0;
}

int test_plugin_upload_failure_propagates() {
    auto plugin = std::make_shared<MockPluginHandle>();
    plugin->return_code = -99;          // simulate cloud upload failure
    Slic3r::bridge::router::CloudUploadSink sink;
    sink.attach_plugin(plugin);

    Slic3r::bridge::server::UploadJob job;
    job.dev_id  = "X";
    job.filename = "y.3mf";
    job.content  = std::vector<uint8_t>(16, 0xAB);

    auto r = sink.deliver(std::move(job));
    if (r.ok) {
        std::fprintf(stderr, "FAIL: rc=-99 sink reported ok=true\n");
        return 1;
    }
    if (r.error_message.find("rc=-99") == std::string::npos) {
        std::fprintf(stderr, "FAIL: error message missing rc=-99: %s\n",
                     r.error_message.c_str());
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    int rc = 0;
    rc |= test_no_plugin_attached();
    rc |= test_happy_path_through_plugin();
    rc |= test_plugin_missing_export();
    rc |= test_plugin_upload_failure_propagates();
    if (rc == 0) std::printf("OK\n");
    return rc;
}
