// Bambu Bridge — shared print-dispatch logic.
//
// See PrintDispatcher.hpp for the rationale.
//
// This file is a faithful port of the dispatch tree in `PrintJob::
// process()` (PrintJob.cpp:224-253 + 550-624 in the BambuStudio-bridge
// fork at the time of extraction). Behaviour-preserving: any change
// here should land in both the GUI and bridge call sites simultaneously
// because both call into this.

#include "PrintDispatcher.hpp"

#include "FileTransferUtils.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <memory>

using namespace BBL;

namespace Slic3r {

bool PrintDispatcher::lan_verify_job(
        BBL::PrintParams& params,
        NetworkAgent*     agent,
        const Inputs&     inputs,
        Result&           result_out) {
    // Mirrors PrintJob.cpp:224-253. Only fires when connection is LAN
    // and the print is a normal (slicer-driven) print.
    if (params.connection_type != "lan" || params.print_type != "from_normal")
        return true; // nothing to verify; treat as pass

    bool emmc_ok = false;
    bool ftp_ok  = false;

    if (inputs.could_emmc_print) {
        // Try the printer's eMMC tunnel on port 6000. Same URL shape
        // the GUI uses (PrintJob.cpp:230).
        const std::string url =
            "bambu:///local/" + params.dev_ip +
            "?port=6000&user=bblp&passwd=" + params.password;
        std::unique_ptr<FileTransferTunnel> tunnel =
            std::make_unique<FileTransferTunnel>(::Slic3r::module(), url);
        emmc_ok = tunnel->sync_start_connect();
    }

    {
        // Save the caller's main-print fields so we can restore them
        // for the subsequent real print call.
        const std::string saved_project_name = params.project_name;
        const std::string saved_filename     = params.filename;

        params.project_name = "verify_job";
        params.filename     = inputs.verify_temp_path;

        int result = agent->start_send_gcode_to_sdcard(
            params, /*update_fn=*/nullptr, /*cancel_fn=*/nullptr, /*wait_fn=*/nullptr);
        ftp_ok = (result == 0);

        params.project_name = saved_project_name;
        params.filename     = saved_filename;
    }

    if (!emmc_ok && !ftp_ok) {
        result_out.verify_job_failed = true;
        result_out.rc = BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
        result_out.error_diagnostic =
            "verify_job failed: neither eMMC tunnel nor FTPS probe succeeded";
        BOOST_LOG_TRIVIAL(error)
            << "PrintDispatcher: LAN verify_job failed (emmc_ok=0, ftp_ok=0)";
        return false;
    }

    return true;
}

PrintDispatcher::Result PrintDispatcher::dispatch(
        BBL::PrintParams&     params,
        NetworkAgent*         agent,
        const Inputs&         inputs,
        BBL::OnUpdateStatusFn update_fn,
        BBL::WasCancelledFn   cancel_fn,
        BBL::OnWaitFn         wait_fn,
        PreCallStatus         pre_status) {
    Result out;

    if (!agent) {
        out.rc = -1;
        out.error_diagnostic = "PrintDispatcher: agent is null";
        return out;
    }

    // ---- Step 1: LAN-mode verify-job pre-probe -----------------------
    //
    // Mirrors PrintJob.cpp:224-253. Fires before the main print call
    // when connection_type==lan && print_type==from_normal.
    if (!lan_verify_job(params, agent, inputs, out)) {
        return out; // verify_job_failed already set
    }

    // ---- Step 2: main dispatch tree ----------------------------------
    //
    // Faithful port of PrintJob.cpp:550-624. The four branches map to
    // the four cells in docs/plugin-trace/IMPLEMENTATION-PLAN.md.

    auto announce = [&](const char* l10n_key) {
        if (pre_status) pre_status(l10n_key);
    };

    int result = 0;

    if (params.print_type == "from_sdcard_view") {
        // Reprint a file already on the printer's SD card.
        announce("sending_print_job_cloud");
        result = agent->start_sdcard_print(params, update_fn, cancel_fn);
    }
    else if (params.connection_type != "lan") {
        // ===== CLOUD MODE =============================================

        // Initial `comments` based on capability (cf. PrintJob:555-562).
        // The plugin reads this as a hint for which transport family
        // to use; see docs/plugin-trace/H2D-cloud.yaml +
        // A1-cloud.yaml for observed values.
        if (params.dev_ip.empty())
            params.comments = "no_ip";
        else if (inputs.cloud_print_only)
            params.comments = "low_version"; // A1 lives here
        else if (!inputs.has_sdcard)
            params.comments = "no_sdcard";
        else if (params.password.empty())
            params.comments = "no_password";

        if (inputs.app_lan_mode_only) {
            // Forced LAN-only FTP — no fallback (PrintJob:566-583).
            if (params.password.empty() || params.dev_ip.empty()) {
                out.error_diagnostic =
                    "lan_mode_only requested but access code or dev_ip missing";
                result = BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
            } else {
                announce("sending_print_job_lan");
                out.tried_lan = true;
                result = agent->start_local_print_with_record(
                    params, update_fn, cancel_fn, wait_fn);
            }
        } else {
            // Standard cloud-mode dispatch (PrintJob:585-614). Two
            // sub-branches depending on whether local-print-via-record
            // is even worth trying.
            if (!inputs.cloud_print_only
                && !params.password.empty()
                && !params.dev_ip.empty()
                && inputs.has_sdcard) {
                // H2D-cloud path: try `start_local_print_with_record`
                // first; on -2130 fall back to `start_print` with
                // comments="upload_failed".
                announce("sending_print_job_lan");
                out.tried_lan = true;
                result = agent->start_local_print_with_record(
                    params, update_fn, cancel_fn, wait_fn);
                if (result == 0) {
                    params.comments = "";
                } else if (result == BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED) {
                    params.comments = "upload_failed";
                } else {
                    params.comments =
                        (boost::format("failed(%1%)") % result).str();
                }
                if (result < 0) {
                    out.lan_failed_used_cloud = true;
                    announce("sending_print_job_cloud");
                    result = agent->start_print(
                        params, update_fn, cancel_fn, wait_fn);
                }
            } else {
                // A1-cloud path: go straight to start_print. The
                // `comments` value was set above (most commonly
                // "low_version" for A1 because cloud_print_only=true).
                announce("sending_print_job_cloud");
                result = agent->start_print(
                    params, update_fn, cancel_fn, wait_fn);
            }
        }
    }
    else {
        // ===== LAN MODE ===============================================
        // (PrintJob.cpp:616-624.) Use `start_local_print` only if the
        // printer has a place to put the file. A1 reports has_sdcard=
        // true even though it lacks a physical SD slot (its internal
        // flash is what the slicer considers "sdcard" here).
        if (inputs.has_sdcard || inputs.could_emmc_print) {
            announce("sending_print_job_lan");
            result = agent->start_local_print(
                params, update_fn, cancel_fn);
        } else {
            announce("storage_needs_to_be_inserted");
            out.error_diagnostic =
                "LAN mode requires has_sdcard or could_emmc_print";
            result = BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
        }
    }

    out.rc = result;
    return out;
}

} // namespace Slic3r
