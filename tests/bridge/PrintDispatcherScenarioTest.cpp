// Bridge — print-dispatcher scenario tests.
//
// Each test is one (printer model × connection mode) cell from the
// reference captures at docs/plugin-trace/{H2D,A1}-{cloud,lan}.yaml.
//
// The mock fakes `IPluginPrintEntry` — it records every plugin call
// (start_print / start_local_print_with_record / start_send_gcode_to_
// sdcard / start_local_print / start_sdcard_print) with a full
// PrintParams snapshot, and returns canned rcs configured per-test.
// The test asserts:
//   * the dispatcher fired the right call kinds in the right order
//   * params at each call match what the reference YAML observed
//
// Validation is intentionally NETWORK-FREE — these tests cover the
// dispatcher's tree, not the plugin's transport. Future work (Phase
// E1-network) would add a real MockBambuPrinter that exercises the
// plugin too; this file is the cheap, fast layer underneath.

#define CATCH_CONFIG_MAIN
#include "../catch2/catch.hpp"

#include "slic3r/Utils/PrintDispatcher.hpp"
#include "bambu_networking.hpp"

#include <string>
#include <vector>

namespace {

// One recorded plugin call.
struct Call {
    enum Kind {
        StartPrint,
        StartLocalPrintWithRecord,
        StartSendGcodeToSdcard,
        StartLocalPrint,
        StartSdcardPrint,
    };
    Kind             kind;
    BBL::PrintParams params; // snapshot at call time
};

const char* kind_name(Call::Kind k) {
    switch (k) {
        case Call::StartPrint:                 return "start_print";
        case Call::StartLocalPrintWithRecord:  return "start_local_print_with_record";
        case Call::StartSendGcodeToSdcard:     return "start_send_gcode_to_sdcard";
        case Call::StartLocalPrint:            return "start_local_print";
        case Call::StartSdcardPrint:           return "start_sdcard_print";
    }
    return "?";
}

// MockPluginEntry — records every dispatcher call. Per-call return
// codes are configurable so we can simulate the GUI's -2130 fallback
// path without an actual plugin / network.
class MockPluginEntry : public Slic3r::IPluginPrintEntry {
public:
    std::vector<Call> calls;

    // Default rcs: success on all calls. Tests override by kind.
    int rc_start_print                    = 0;
    int rc_start_local_print_with_record  = 0;
    int rc_start_send_gcode_to_sdcard     = 0;
    int rc_start_local_print              = 0;
    int rc_start_sdcard_print             = 0;

    int start_print(BBL::PrintParams& p, BBL::OnUpdateStatusFn,
                    BBL::WasCancelledFn, BBL::OnWaitFn) override {
        calls.push_back({Call::StartPrint, p});
        return rc_start_print;
    }
    int start_local_print_with_record(BBL::PrintParams& p, BBL::OnUpdateStatusFn,
                                      BBL::WasCancelledFn, BBL::OnWaitFn) override {
        calls.push_back({Call::StartLocalPrintWithRecord, p});
        return rc_start_local_print_with_record;
    }
    int start_send_gcode_to_sdcard(BBL::PrintParams& p, BBL::OnUpdateStatusFn,
                                   BBL::WasCancelledFn, BBL::OnWaitFn) override {
        calls.push_back({Call::StartSendGcodeToSdcard, p});
        return rc_start_send_gcode_to_sdcard;
    }
    int start_local_print(BBL::PrintParams& p, BBL::OnUpdateStatusFn,
                          BBL::WasCancelledFn) override {
        calls.push_back({Call::StartLocalPrint, p});
        return rc_start_local_print;
    }
    int start_sdcard_print(BBL::PrintParams& p, BBL::OnUpdateStatusFn,
                           BBL::WasCancelledFn) override {
        calls.push_back({Call::StartSdcardPrint, p});
        return rc_start_sdcard_print;
    }
};

// Synthetic PrintParams populated with values close to what the GUI
// produced for a real BMCU-C print in the capture runs. Most fields
// don't influence the dispatcher's tree; the ones that do
// (dev_ip / password / connection_type / print_type) are set per-test.
BBL::PrintParams make_baseline_params() {
    BBL::PrintParams p;
    p.dev_id          = "0948DB561601642";
    p.dev_ip          = "192.168.1.47";
    p.username        = "bblp";
    p.password        = "5c673ee4";
    p.filename        = "/tmp/synthetic/plate_1.3mf";
    p.config_filename = "/tmp/synthetic/plate_1_config.3mf";
    p.project_name    = "Synthetic";
    p.plate_index     = 1;
    p.print_type      = "from_normal";
    p.ams_mapping     = "[0,-1,-1,-1,-1,-1,-1,-1]";
    p.task_bed_type   = "hot_plate";
    p.try_emmc_print  = true;
    p.use_ssl_for_ftp  = true;
    p.use_ssl_for_mqtt = true;
    return p;
}

} // namespace

// ---------------------------------------------------------------------
// H2D — Cloud mode
// ---------------------------------------------------------------------

TEST_CASE("dispatcher: H2D-cloud (primary succeeds)", "[dispatcher][H2D][cloud]") {
    // Reference: docs/plugin-trace/H2D-cloud.yaml v2 — primary call
    // start_local_print_with_record returns 0 directly. comments
    // remains "" because !cloud_print_only && has_sdcard && password
    // non-empty.
    MockPluginEntry mock;
    mock.rc_start_local_print_with_record = 0;

    BBL::PrintParams params = make_baseline_params();
    params.connection_type = "cloud";

    Slic3r::PrintDispatcher::Inputs in;
    in.cloud_print_only = false; // H2D
    in.has_sdcard       = true;
    in.could_emmc_print = true;

    auto r = Slic3r::PrintDispatcher{}.dispatch(
        params, &mock, in, nullptr, nullptr, nullptr, nullptr);

    REQUIRE(r.rc == 0);
    REQUIRE(r.tried_lan == true);
    REQUIRE(r.lan_failed_used_cloud == false);

    REQUIRE(mock.calls.size() == 1);
    REQUIRE(mock.calls[0].kind == Call::StartLocalPrintWithRecord);
    REQUIRE(mock.calls[0].params.connection_type == "cloud");
    REQUIRE(mock.calls[0].params.comments == "");
}

TEST_CASE("dispatcher: H2D-cloud (primary fails -2130, fallback succeeds)",
          "[dispatcher][H2D][cloud][fallback]") {
    // Reference: docs/plugin-trace/H2D-cloud.yaml v1 — primary returns
    // -2130, the GUI sets comments="upload_failed" and falls back to
    // start_print, which succeeds.
    MockPluginEntry mock;
    mock.rc_start_local_print_with_record =
        BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED;
    mock.rc_start_print = 0;

    BBL::PrintParams params = make_baseline_params();
    params.connection_type = "cloud";

    Slic3r::PrintDispatcher::Inputs in;
    in.cloud_print_only = false;
    in.has_sdcard       = true;
    in.could_emmc_print = true;

    auto r = Slic3r::PrintDispatcher{}.dispatch(
        params, &mock, in, nullptr, nullptr, nullptr, nullptr);

    REQUIRE(r.rc == 0);
    REQUIRE(r.tried_lan == true);
    REQUIRE(r.lan_failed_used_cloud == true);

    REQUIRE(mock.calls.size() == 2);
    REQUIRE(mock.calls[0].kind == Call::StartLocalPrintWithRecord);
    REQUIRE(mock.calls[1].kind == Call::StartPrint);
    // The big finding from H2D-cloud v1: comments flipped to
    // "upload_failed" before the fallback call.
    REQUIRE(mock.calls[1].params.comments == "upload_failed");
    REQUIRE(mock.calls[1].params.connection_type == "cloud");
}

// ---------------------------------------------------------------------
// A1 — Cloud mode
// ---------------------------------------------------------------------

TEST_CASE("dispatcher: A1-cloud (start_print direct with low_version)",
          "[dispatcher][A1][cloud]") {
    // Reference: docs/plugin-trace/A1-cloud.yaml — cloud_print_only=true
    // for A1 skips start_local_print_with_record entirely; the GUI
    // goes straight to start_print with comments="low_version".
    MockPluginEntry mock;
    mock.rc_start_print = 0;

    BBL::PrintParams params = make_baseline_params();
    params.dev_id          = "03900D610219434";
    params.dev_ip          = "192.168.1.6";
    params.password        = "18b1a572";
    params.connection_type = "cloud";
    params.try_emmc_print  = false;          // A1 has no eMMC

    Slic3r::PrintDispatcher::Inputs in;
    in.cloud_print_only = true; // ★ key flag
    in.has_sdcard       = true; // A1 reports has_sdcard=true (internal flash)
    in.could_emmc_print = false;

    auto r = Slic3r::PrintDispatcher{}.dispatch(
        params, &mock, in, nullptr, nullptr, nullptr, nullptr);

    REQUIRE(r.rc == 0);
    REQUIRE(r.tried_lan == false);            // skipped start_local_print_with_record
    REQUIRE(r.lan_failed_used_cloud == false);

    REQUIRE(mock.calls.size() == 1);
    REQUIRE(mock.calls[0].kind == Call::StartPrint);
    REQUIRE(mock.calls[0].params.comments == "low_version");
    REQUIRE(mock.calls[0].params.connection_type == "cloud");
}

// ---------------------------------------------------------------------
// H2D — LAN mode
// ---------------------------------------------------------------------

TEST_CASE("dispatcher: H2D-lan (verify_job then start_local_print)",
          "[dispatcher][H2D][lan]") {
    // Reference: docs/plugin-trace/H2D-lan.yaml — verify_job FTPS probe
    // first (start_send_gcode_to_sdcard returning 0), then the real
    // print via start_local_print.
    MockPluginEntry mock;
    mock.rc_start_send_gcode_to_sdcard = 0;
    mock.rc_start_local_print          = 0;

    BBL::PrintParams params = make_baseline_params();
    params.connection_type = "lan";

    Slic3r::PrintDispatcher::Inputs in;
    in.has_sdcard       = true;
    in.could_emmc_print = true;
    in.verify_temp_path = "/path/to/check_access_code.txt";

    auto r = Slic3r::PrintDispatcher{}.dispatch(
        params, &mock, in, nullptr, nullptr, nullptr, nullptr);

    REQUIRE(r.rc == 0);

    REQUIRE(mock.calls.size() == 2);
    REQUIRE(mock.calls[0].kind == Call::StartSendGcodeToSdcard);
    REQUIRE(mock.calls[0].params.project_name == "verify_job");
    REQUIRE(mock.calls[0].params.filename     == "/path/to/check_access_code.txt");
    REQUIRE(mock.calls[1].kind == Call::StartLocalPrint);
    // verify_job temporary fields should have been restored before the
    // main call.
    REQUIRE(mock.calls[1].params.project_name == "Synthetic");
}

TEST_CASE("dispatcher: H2D-lan verify_job fails (signals access-code dialog)",
          "[dispatcher][H2D][lan][verify_fail]") {
    // If both the eMMC tunnel and the FTPS probe fail, the GUI's
    // m_enter_ip_address_fun_fail() fires and PrintJob bails. The
    // dispatcher signals this via Result.verify_job_failed = true so
    // the caller (PrintJob, bridge adapter) can react.
    MockPluginEntry mock;
    mock.rc_start_send_gcode_to_sdcard =
        BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;

    BBL::PrintParams params = make_baseline_params();
    params.connection_type = "lan";

    Slic3r::PrintDispatcher::Inputs in;
    in.has_sdcard       = true;
    in.could_emmc_print = false; // skip eMMC handshake — forces ftp_ok to drive
    in.verify_temp_path = "/path/to/check_access_code.txt";

    auto r = Slic3r::PrintDispatcher{}.dispatch(
        params, &mock, in, nullptr, nullptr, nullptr, nullptr);

    REQUIRE(r.verify_job_failed == true);
    REQUIRE(r.rc != 0);

    // Only one call (the verify probe); main print was skipped.
    REQUIRE(mock.calls.size() == 1);
    REQUIRE(mock.calls[0].kind == Call::StartSendGcodeToSdcard);
    REQUIRE(mock.calls[0].params.project_name == "verify_job");
}

// ---------------------------------------------------------------------
// A1 — LAN mode
// ---------------------------------------------------------------------

TEST_CASE("dispatcher: A1-lan (same shape as H2D-lan)",
          "[dispatcher][A1][lan]") {
    // Reference: docs/plugin-trace/A1-lan.yaml — A1 in LAN mode uses
    // the same plugin export sequence as H2D-lan (verify_job then
    // start_local_print). Per-printer deltas (ftp_folder=sdcard/,
    // try_emmc_print=0, nozzles_info=[]) are properties of the
    // PrintParams the caller built — the dispatcher passes them
    // through unchanged. This test pins that the call sequence
    // generalises across the model dimension.
    MockPluginEntry mock;
    mock.rc_start_send_gcode_to_sdcard = 0;
    mock.rc_start_local_print          = 0;

    BBL::PrintParams params = make_baseline_params();
    params.dev_id          = "03900D610219434";
    params.dev_ip          = "192.168.1.6";
    params.password        = "18b1a572";
    params.connection_type = "lan";
    params.try_emmc_print  = false;
    params.ftp_folder      = "sdcard/";

    Slic3r::PrintDispatcher::Inputs in;
    in.has_sdcard       = true;
    in.could_emmc_print = false; // A1 has no eMMC
    in.verify_temp_path = "/path/to/check_access_code.txt";

    auto r = Slic3r::PrintDispatcher{}.dispatch(
        params, &mock, in, nullptr, nullptr, nullptr, nullptr);

    REQUIRE(r.rc == 0);

    REQUIRE(mock.calls.size() == 2);
    REQUIRE(mock.calls[0].kind == Call::StartSendGcodeToSdcard);
    REQUIRE(mock.calls[0].params.project_name == "verify_job");
    REQUIRE(mock.calls[1].kind == Call::StartLocalPrint);
    REQUIRE(mock.calls[1].params.ftp_folder == "sdcard/");
    REQUIRE(mock.calls[1].params.try_emmc_print == false);
}

// ---------------------------------------------------------------------
// Sanity: from_sdcard_view (reprint from printer's SD)
// ---------------------------------------------------------------------

TEST_CASE("dispatcher: from_sdcard_view (reprint)", "[dispatcher][reprint]") {
    // PrintJob.cpp:550-552 — for sdcard-view reprint the dispatcher
    // calls start_sdcard_print directly.
    MockPluginEntry mock;
    mock.rc_start_sdcard_print = 0;

    BBL::PrintParams params = make_baseline_params();
    params.print_type      = "from_sdcard_view";
    params.connection_type = "cloud";

    Slic3r::PrintDispatcher::Inputs in;
    in.has_sdcard = true;

    auto r = Slic3r::PrintDispatcher{}.dispatch(
        params, &mock, in, nullptr, nullptr, nullptr, nullptr);

    REQUIRE(r.rc == 0);
    REQUIRE(mock.calls.size() == 1);
    REQUIRE(mock.calls[0].kind == Call::StartSdcardPrint);
}
