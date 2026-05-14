// Bambu Bridge — H2S printer mock end-to-end test (harness, step 5).
//
// Same shape as HarnessMockA1Test, parameterised on H2SPrinter. The
// extra assertions cover the H2S-specific capability fields:
// `chamber_temper`, AMS `humidity`, and the "series_o" / "O1S"
// printer_type / model_id codes.

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include "router/CloudUplink.hpp"
#include "harness/ShimRecorder.hpp"
#include "mocks/H2SPrinter.hpp"
#include "mocks/MockPlugin.hpp"
#include "third_party/nlohmann/json.hpp"

using Slic3r::bridge::router::CloudUplink;
using Slic3r::bridge::router::CloudUplinkConfig;
using Slic3r::bridge::mocks::GcodeState;
using Slic3r::bridge::mocks::H2SPrinter;
using Slic3r::bridge::mocks::MockPlugin;
using Slic3r::bridge::mocks::PrinterAdapter;
using Slic3r::bridge::harness::ShimRecorder;
using nlohmann::json;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path trace_path = fs::temp_directory_path() /
        ("harness_mock_h2s_" + std::to_string(::getpid()) + ".jsonl");
    std::error_code ec;
    fs::remove(trace_path, ec);

    const std::string dev_id = "0950ABCDH2S0042";
    const std::string topic_report  = "device/" + dev_id + "/report";
    const std::string topic_request = "device/" + dev_id + "/request";

    auto h2s  = std::make_shared<H2SPrinter>(dev_id);
    auto mock = std::make_shared<MockPlugin>();
    mock->add_printer(std::make_shared<PrinterAdapter<H2SPrinter>>(h2s));

    auto& rec = ShimRecorder::instance();
    rec.disable();
    rec.enable(trace_path.string());

    CloudUplink up;
    up.attach_plugin(mock);

    CloudUplinkConfig cfg;
    cfg.dev_id      = dev_id;
    cfg.access_code = "BBLP";
    up.add_device(cfg);

    std::mutex ds_mu;
    std::vector<json> downstream_msgs;
    up.attach_downstream(dev_id,
        [&](std::string /*topic*/, std::vector<uint8_t> p, uint8_t /*q*/) {
            std::string s(p.begin(), p.end());
            json j = json::parse(s, nullptr, false);
            if (!j.is_discarded()) {
                std::lock_guard<std::mutex> lk(ds_mu);
                downstream_msgs.push_back(std::move(j));
            }
        });

    // Initial subscribe + push_status.
    up.on_subscribe(dev_id, topic_report);
    {
        std::lock_guard<std::mutex> lk(ds_mu);
        check(downstream_msgs.size() == 1,
              "H2S subscribe emits one initial push_status");
        if (!downstream_msgs.empty()) {
            const auto& j = downstream_msgs[0];
            check(j["print"]["gcode_state"] == "IDLE",
                  "H2S FSM starts IDLE");
            check(j["print"].contains("chamber_temper"),
                  "H2S push_status carries chamber_temper");
            check(j["print"]["ams"]["ams"][0].contains("humidity"),
                  "H2S AMS unit reports humidity");
        }
    }

    // Drive through start -> running -> stop.
    auto publish_cmd = [&](const std::string& cmd) {
        const json c = {{"print", {{"command", cmd}}}};
        const std::string s = c.dump();
        std::vector<uint8_t> p(s.begin(), s.end());
        up.on_publish(dev_id, topic_request, p, 0);
    };
    publish_cmd("start"); // IDLE -> PREPARE
    publish_cmd("start"); // PREPARE -> RUNNING
    check(h2s->fsm().state() == GcodeState::Running, "H2S reaches RUNNING");
    publish_cmd("pause");
    check(h2s->fsm().state() == GcodeState::Pause,   "H2S pause -> PAUSE");
    publish_cmd("resume");
    check(h2s->fsm().state() == GcodeState::Running, "H2S resume -> RUNNING");
    publish_cmd("stop");
    check(h2s->fsm().state() == GcodeState::Finish,  "H2S stop -> FINISH");

    // Inventory: matching model_id.
    unsigned int http_code = 0;
    std::string  body;
    mock->get_user_print_info(&http_code, &body);
    check(http_code == 200, "H2S inventory http_code 200");
    {
        json b = json::parse(body, nullptr, false);
        if (!b.is_discarded() && b["devices"].size() == 1) {
            check(b["devices"][0]["dev_model_name"] == "O1S",
                  "H2S model_id == O1S");
        }
    }

    rec.disable();
    fs::remove(trace_path, ec);
    up.remove_device(dev_id);

    if (g_fails) {
        std::fprintf(stderr, "HarnessMockH2STest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("HarnessMockH2STest: ok\n");
    return 0;
}
