// Bambu Bridge — shared print-dispatch logic.
//
// Extracted from `PrintJob::process()` (the cloud-vs-LAN decision tree
// at PrintJob.cpp:550-624 plus the LAN-mode verify-job pre-probe at
// PrintJob.cpp:224-253). Both the slicer GUI's PrintJob and the
// bridge's LanUploadSink call into this — so when BambuStudio updates
// the decision tree, the bridge benefits automatically.
//
// The dispatcher is pure business logic: no wxJob, no Plater, no GUI
// state. Callers fill a `BBL::PrintParams` and an `Inputs` capability
// struct, then call `dispatch()`. The dispatcher walks the same tree
// the GUI walks, mutating `params.comments` along the way (matching
// PrintJob's existing pattern) and firing the correct sequence of
// plugin calls.
//
// See docs/plugin-trace/IMPLEMENTATION-PLAN.md for the four-cell
// matrix this dispatcher is responsible for matching.

#ifndef SLIC3R_PRINT_DISPATCHER_HPP
#define SLIC3R_PRINT_DISPATCHER_HPP

#include <functional>
#include <string>

#include "NetworkAgent.hpp"
#include "bambu_networking.hpp"

namespace Slic3r {

// Abstract plugin-entry interface used by `PrintDispatcher`. Production
// path wraps the slicer's NetworkAgent (see `NetworkAgentPrintEntry`
// below); tests can implement this directly to record calls without
// dragging in the real plugin.
//
// Each method maps 1:1 to the equivalent NetworkAgent function. The
// dispatcher only ever needs these five — it never reaches back into
// NetworkAgent for anything else.
class IPluginPrintEntry {
public:
    virtual ~IPluginPrintEntry() = default;

    virtual int start_print                 (BBL::PrintParams& params,
                                             BBL::OnUpdateStatusFn update_fn,
                                             BBL::WasCancelledFn   cancel_fn,
                                             BBL::OnWaitFn         wait_fn) = 0;

    virtual int start_local_print_with_record(BBL::PrintParams& params,
                                              BBL::OnUpdateStatusFn update_fn,
                                              BBL::WasCancelledFn   cancel_fn,
                                              BBL::OnWaitFn         wait_fn) = 0;

    virtual int start_send_gcode_to_sdcard  (BBL::PrintParams& params,
                                             BBL::OnUpdateStatusFn update_fn,
                                             BBL::WasCancelledFn   cancel_fn,
                                             BBL::OnWaitFn         wait_fn) = 0;

    virtual int start_local_print           (BBL::PrintParams& params,
                                             BBL::OnUpdateStatusFn update_fn,
                                             BBL::WasCancelledFn   cancel_fn) = 0;

    virtual int start_sdcard_print          (BBL::PrintParams& params,
                                             BBL::OnUpdateStatusFn update_fn,
                                             BBL::WasCancelledFn   cancel_fn) = 0;
};

// Production adapter — wraps a NetworkAgent so production callers
// (PrintJob, NetworkAgentPluginAdapter) get a thin pass-through.
class NetworkAgentPrintEntry : public IPluginPrintEntry {
public:
    explicit NetworkAgentPrintEntry(NetworkAgent* agent) : m_agent(agent) {}

    int start_print(BBL::PrintParams& p, BBL::OnUpdateStatusFn u,
                    BBL::WasCancelledFn c, BBL::OnWaitFn w) override {
        return m_agent ? m_agent->start_print(p, u, c, w) : -1;
    }
    int start_local_print_with_record(BBL::PrintParams& p, BBL::OnUpdateStatusFn u,
                                       BBL::WasCancelledFn c, BBL::OnWaitFn w) override {
        return m_agent ? m_agent->start_local_print_with_record(p, u, c, w) : -1;
    }
    int start_send_gcode_to_sdcard(BBL::PrintParams& p, BBL::OnUpdateStatusFn u,
                                    BBL::WasCancelledFn c, BBL::OnWaitFn w) override {
        return m_agent ? m_agent->start_send_gcode_to_sdcard(p, u, c, w) : -1;
    }
    int start_local_print(BBL::PrintParams& p, BBL::OnUpdateStatusFn u,
                          BBL::WasCancelledFn c) override {
        return m_agent ? m_agent->start_local_print(p, u, c) : -1;
    }
    int start_sdcard_print(BBL::PrintParams& p, BBL::OnUpdateStatusFn u,
                           BBL::WasCancelledFn c) override {
        return m_agent ? m_agent->start_sdcard_print(p, u, c) : -1;
    }

private:
    NetworkAgent* m_agent;
};

class PrintDispatcher {
public:
    // Capabilities the GUI's PrintJob reads from `MachineObject` /
    // `app_config`. The bridge fills these from its per-model
    // capability table (see PrinterCapability.hpp).
    struct Inputs {
        // From MachineObject:
        //   cloud_print_only      = obj->is_support_cloud_print_only
        //   has_sdcard            = obj->GetStorage()->get_sdcard_state() == HAS_SDCARD_NORMAL
        //   could_emmc_print      = obj->is_support_print_with_emmc
        bool cloud_print_only  = false; // true for A1 — triggers "low_version"
        bool has_sdcard        = false;
        bool could_emmc_print  = false;

        // From app_config: "lan_mode_only" == "1"
        bool app_lan_mode_only = false;

        // For the LAN-mode verify-job pre-probe (only fires when
        // connection_type=="lan" && print_type=="from_normal"):
        //   verify_temp_path = job_data._temp_path.string() in the GUI;
        //                      bridge supplies a small file path with
        //                      the same role.
        std::string verify_temp_path;

        // Caller-performed eMMC tunnel handshake result. The GUI's
        // PrintJob.cpp:227-233 opens a `bambu:///local/<ip>?port=6000`
        // FileTransferTunnel and records whether it landed (true if
        // sync_start_connect returned true). The dispatcher uses this
        // OR the start_send_gcode_to_sdcard FTPS probe to decide
        // whether the access code is valid.
        //
        // Caller is responsible for doing the handshake (it touches
        // network; keeping it out of the dispatcher makes the
        // dispatcher testable without a real network stack). Pass
        // `false` if you don't want to attempt it — the dispatcher
        // will fall back to the FTPS probe only.
        bool emmc_handshake_ok = false;
    };

    // What the dispatcher did, for the caller's UI / log.
    struct Result {
        int rc = 0;                       // final plugin return code (0 = success)
        bool tried_lan = false;           // attempted LAN path (start_local_print_with_record)
        bool lan_failed_used_cloud = false; // LAN attempt failed and we fell back to start_print
        bool verify_job_failed = false;   // both emmc_ok and ftp_ok were false in LAN verify
        std::string error_diagnostic;     // human-readable error context if rc<0
    };

    // Pre-call status hook — invoked just before each plugin call so
    // the caller (GUI) can update its progress text. Bridge can pass
    // an empty function. Argument is a stable l10n key, e.g.
    //   "sending_print_job_lan"
    //   "sending_print_job_cloud"
    //   "storage_needs_to_be_inserted"
    using PreCallStatus = std::function<void(const std::string& l10n_key)>;

    PrintDispatcher() = default;

    // Walks the print-dispatch decision tree. May mutate `params.comments`,
    // `params.project_name`, and `params.filename` (around the LAN verify
    // step). The dispatcher does NOT take ownership of the entry; the
    // caller must keep it alive for the duration of dispatch.
    Result dispatch(BBL::PrintParams&       params,
                    IPluginPrintEntry*      entry,
                    const Inputs&           inputs,
                    BBL::OnUpdateStatusFn   update_fn,
                    BBL::WasCancelledFn     cancel_fn,
                    BBL::OnWaitFn           wait_fn,
                    PreCallStatus           pre_status = nullptr);

    // Back-compat overload — wraps the agent in a NetworkAgentPrintEntry
    // and forwards. Lets existing callers keep their `agent->dispatch(...)`
    // shape until they're updated to use IPluginPrintEntry directly.
    Result dispatch(BBL::PrintParams&       params,
                    NetworkAgent*           agent,
                    const Inputs&           inputs,
                    BBL::OnUpdateStatusFn   update_fn,
                    BBL::WasCancelledFn     cancel_fn,
                    BBL::OnWaitFn           wait_fn,
                    PreCallStatus           pre_status = nullptr) {
        NetworkAgentPrintEntry e(agent);
        return dispatch(params, &e, inputs, update_fn, cancel_fn, wait_fn,
                        pre_status);
    }

private:
    // The LAN verify-job sub-step. Returns true iff verification passed
    // (either eMMC tunnel handshake succeeded or the FTPS probe got
    // ret=0). Resets params.filename + params.project_name to "" on
    // success so the caller's subsequent main-print params take effect.
    // Sets `result_out.verify_job_failed = true` on failure.
    bool lan_verify_job(BBL::PrintParams&  params,
                        IPluginPrintEntry* entry,
                        const Inputs&      inputs,
                        Result&            result_out);
};

} // namespace Slic3r

#endif // SLIC3R_PRINT_DISPATCHER_HPP
