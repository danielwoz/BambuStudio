// Bambu Bridge — shared E2E driver (phase 12).

#include "e2e_common.hpp"

#include "E2EHarness.hpp"
#include "E2EPrinterClient.hpp"
#include "E2EFixtureLoader.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace e2e {

namespace {

void trim(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
}

// Parse "model@ip[:code],model@ip[:code],..." into a list filtered by
// `model_filter`. Falls back to $BAMBU_BRIDGE_E2E_<MODEL>_CODE for the
// access code when not given inline (keeps secrets out of CMake cache).
std::vector<PrinterEndpoint>
parse_endpoints(const std::string& env, const std::string& model_filter) {
    std::vector<PrinterEndpoint> out;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        const auto at = cur.find('@');
        if (at == std::string::npos) { cur.clear(); return; }
        PrinterEndpoint ep;
        ep.model = cur.substr(0, at);
        std::string rest = cur.substr(at + 1);
        const auto colon = rest.find(':');
        if (colon == std::string::npos) {
            ep.ip = rest;
        } else {
            ep.ip          = rest.substr(0, colon);
            ep.access_code = rest.substr(colon + 1);
        }
        trim(ep.model); trim(ep.ip); trim(ep.access_code);
        if (model_filter.empty() || ep.model == model_filter) {
            if (ep.access_code.empty()) {
                std::string var = "BAMBU_BRIDGE_E2E_" + ep.model + "_CODE";
                for (auto& c : var) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (const char* v = std::getenv(var.c_str()); v && *v) {
                    ep.access_code = v;
                }
            }
            out.push_back(std::move(ep));
        }
        cur.clear();
    };
    for (char c : env) {
        if (c == ',') flush();
        else          cur.push_back(c);
    }
    flush();
    return out;
}

bool default_running_pred(const std::string& p) {
    return p.find("RUNNING")  != std::string::npos
        || p.find("PRINTING") != std::string::npos;
}
bool default_paused_pred(const std::string& p) {
    return p.find("PAUSE") != std::string::npos;
}
bool default_idle_pred(const std::string& p) {
    return p.find("IDLE")   != std::string::npos
        || p.find("FINISH") != std::string::npos
        || p.find("FAILED") != std::string::npos;
}

} // namespace

int run_print_test(const std::string& model, ModelFamily /*family*/) {
    // Note on `family`: both H2Like and A1Like use the same set of
    // keywords for the high-level state transitions we test here
    // (RUNNING/PAUSE/IDLE). The family parameter is reserved for
    // future AMS-aware sub-checks; today it's a tag for the log line.

    const char* env = std::getenv("BAMBU_BRIDGE_E2E_PRINTERS");
    if (!env || !*env) {
        std::fprintf(stderr,
            "e2e_%s_print: BAMBU_BRIDGE_E2E_PRINTERS not set; skipping\n",
            model.c_str());
        return 77;
    }
    auto endpoints = parse_endpoints(env, model);
    if (endpoints.empty()) {
        std::fprintf(stderr,
            "e2e_%s_print: no '%s' endpoint in BAMBU_BRIDGE_E2E_PRINTERS; "
            "skipping\n", model.c_str(), model.c_str());
        return 77;
    }
    auto& ep = endpoints[0];
    if (ep.access_code.empty()) {
        std::string var = "BAMBU_BRIDGE_E2E_" + model + "_CODE";
        for (auto& c : var) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::fprintf(stderr,
            "e2e_%s_print: no access code for '%s' "
            "(use 'model@ip:code' inline OR set %s); skipping\n",
            model.c_str(), model.c_str(), var.c_str());
        return 77;
    }

    // 1) Fixture guard. Check this BEFORE spawning the daemon: a stub
    //    .3mf can't drive a real print, and the harness would otherwise
    //    block its full startup timeout waiting for the daemon to learn
    //    a device that the test will never actually drive.
    auto fix = load_cube_5mm_fixture();
    if (fix.is_stub) {
        std::fprintf(stderr,
            "e2e_%s_print: fixture cube_5mm.3mf is a STUB (%s).\n"
            "  Drop a real BambuStudio-sliced cube into %s and re-run.\n"
            "  See docs/bambu_bridge_e2e_guide.md.\n",
            model.c_str(), fix.notes.c_str(), fix.fixture_path.c_str());
        return 77;
    }

    // 2) Start the bridge daemon.
    E2EHarness harness({ep});
    if (!harness.start(std::chrono::seconds(45))) {
        std::fprintf(stderr,
            "e2e_%s_print: harness.start() failed:\n--- stderr ---\n%s\n--- end ---\n",
            model.c_str(), harness.captured_stderr().c_str());
        return 1;
    }
    auto ports = harness.ports_for(model);
    if (!ports) {
        std::fprintf(stderr,
            "e2e_%s_print: harness started but no ports for '%s'\n",
            model.c_str(), model.c_str());
        return 1;
    }
    std::fprintf(stderr,
        "e2e_%s_print: bridge bound mqtt=%u ftps=%u rtsp=%u dev_id=%s\n",
        model.c_str(),
        unsigned(ports->mqtt), unsigned(ports->ftps), unsigned(ports->rtsp),
        ports->dev_id.c_str());

    E2EPrinterClient client;

    // 3) MQTT.
    if (!client.connect_mqtt(ports->bind_ip, ports->mqtt,
                             ports->dev_id, ep.access_code)) {
        std::fprintf(stderr, "e2e_%s_print: connect_mqtt: %s\n",
            model.c_str(), client.last_error().c_str());
        return 1;
    }
    if (!client.subscribe_report(ports->dev_id)) {
        std::fprintf(stderr, "e2e_%s_print: subscribe_report: %s\n",
            model.c_str(), client.last_error().c_str());
        return 1;
    }

    // 4) Upload.
    std::string uploaded;
    if (!client.upload_3mf(ports->bind_ip, ports->ftps,
                           ports->dev_id, ep.access_code,
                           fix.fixture_path, uploaded)) {
        std::fprintf(stderr, "e2e_%s_print: upload_3mf: %s\n",
            model.c_str(), client.last_error().c_str());
        return 1;
    }

    // 5) Start.
    if (!client.start_print(ports->dev_id, uploaded, fix.plate_idx)) {
        std::fprintf(stderr, "e2e_%s_print: start_print: %s\n",
            model.c_str(), client.last_error().c_str());
        return 1;
    }
    if (client.wait_for_status(default_running_pred,
                               std::chrono::seconds(60)).empty()) {
        std::fprintf(stderr, "e2e_%s_print: never saw RUNNING\n",
            model.c_str());
        return 1;
    }
    std::fprintf(stderr, "e2e_%s_print: RUNNING\n", model.c_str());

    // 6) Pause.
    if (!client.pause_print(ports->dev_id)) {
        std::fprintf(stderr, "e2e_%s_print: pause_print: %s\n",
            model.c_str(), client.last_error().c_str());
        return 1;
    }
    if (client.wait_for_status(default_paused_pred,
                               std::chrono::seconds(20)).empty()) {
        std::fprintf(stderr, "e2e_%s_print: never saw PAUSED\n",
            model.c_str());
        return 1;
    }
    std::fprintf(stderr, "e2e_%s_print: PAUSED\n", model.c_str());

    // 7) Cancel.
    if (!client.cancel_print(ports->dev_id)) {
        std::fprintf(stderr, "e2e_%s_print: cancel_print: %s\n",
            model.c_str(), client.last_error().c_str());
        return 1;
    }
    if (client.wait_for_status(default_idle_pred,
                               std::chrono::seconds(60)).empty()) {
        std::fprintf(stderr, "e2e_%s_print: never saw IDLE/FINISH\n",
            model.c_str());
        return 1;
    }
    std::fprintf(stderr, "e2e_%s_print: IDLE/FINISH\n", model.c_str());

    client.disconnect();
    harness.stop();

    std::fprintf(stderr,
        "e2e_%s_print: PASS — bridge round-tripped upload+print+pause+cancel\n",
        model.c_str());
    return 0;
}

} // namespace e2e
} // namespace bridge
} // namespace Slic3r
