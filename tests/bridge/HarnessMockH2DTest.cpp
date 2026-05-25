// Bambu Bridge — H2D printer mock end-to-end test (harness, step 5).
//
// H2D adds dual-extruder + dual-AMS dimensions on top of H2S. Asserts:
//   * The `device.extruder` array carries 2 entries.
//   * The `ams.ams` array carries 2 units (each with 4 trays).
//   * `model_id` == "O1D" (per Bambu Studio profile).
//   * The whole FSM/command vocabulary still works on the dual config.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include "router/CloudUplink.hpp"
#include "harness/ShimRecorder.hpp"
#include "mocks/H2DPrinter.hpp"
#include "mocks/MockPlugin.hpp"
#include "third_party/nlohmann/json.hpp"

using Slic3r::bridge::router::CloudUplink;
using Slic3r::bridge::router::CloudUplinkConfig;
using Slic3r::bridge::mocks::GcodeState;
using Slic3r::bridge::mocks::H2DPrinter;
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
        ("harness_mock_h2d_" + std::to_string(::getpid()) + ".jsonl");
    std::error_code ec;
    fs::remove(trace_path, ec);

    const std::string dev_id = "0951ABCDH2D0042";
    const std::string topic_report  = "device/" + dev_id + "/report";
    const std::string topic_request = "device/" + dev_id + "/request";

    auto h2d  = std::make_shared<H2DPrinter>(dev_id);
    auto mock = std::make_shared<MockPlugin>();
    mock->add_printer(std::make_shared<PrinterAdapter<H2DPrinter>>(h2d));

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

    up.on_subscribe(dev_id, topic_report);
    {
        std::lock_guard<std::mutex> lk(ds_mu);
        check(downstream_msgs.size() == 1, "H2D initial push_status emitted");
        if (!downstream_msgs.empty()) {
            const auto& j = downstream_msgs[0]["print"];
            check(j["gcode_state"] == "IDLE",     "H2D FSM IDLE");
            check(j.contains("chamber_temper"),    "H2D chamber_temper present");
            check(j.contains("device"),            "H2D device key present");
            check(j["device"]["extruder"].is_array(),
                  "device.extruder is array");
            check(j["device"]["extruder"].size() == 2,
                  "H2D reports two extruders");
            check(j["ams"]["ams"].is_array(),
                  "ams.ams is array");
            check(j["ams"]["ams"].size() == 2,
                  "H2D reports two AMS units");
            if (j["ams"]["ams"].size() == 2) {
                check(j["ams"]["ams"][0]["tray"].size() == 4,
                      "AMS unit 0 has 4 trays");
                check(j["ams"]["ams"][1]["tray"].size() == 4,
                      "AMS unit 1 has 4 trays");
                check(j["ams"]["ams"][0].contains("humidity"),
                      "AMS unit 0 reports humidity");
            }
        }
    }

    auto publish_cmd = [&](const std::string& cmd) {
        const json c = {{"print", {{"command", cmd}}}};
        const std::string s = c.dump();
        std::vector<uint8_t> p(s.begin(), s.end());
        up.on_publish(dev_id, topic_request, p, 0);
    };
    publish_cmd("start");
    publish_cmd("start");
    check(h2d->fsm().state() == GcodeState::Running, "H2D reaches RUNNING");
    publish_cmd("stop");
    check(h2d->fsm().state() == GcodeState::Finish,  "H2D stop -> FINISH");

    unsigned int http_code = 0;
    std::string  body;
    mock->get_user_print_info(&http_code, &body);
    {
        json b = json::parse(body, nullptr, false);
        if (!b.is_discarded() && b["devices"].size() == 1) {
            check(b["devices"][0]["dev_model_name"] == "O1D",
                  "H2D model_id == O1D");
        }
    }

    rec.disable();
    fs::remove(trace_path, ec);
    up.remove_device(dev_id);

    if (g_fails) {
        std::fprintf(stderr, "HarnessMockH2DTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("HarnessMockH2DTest: ok\n");
    return 0;
}
