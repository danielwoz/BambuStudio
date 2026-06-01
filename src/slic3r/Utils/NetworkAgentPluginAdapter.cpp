#include "NetworkAgentPluginAdapter.hpp"

#include "NetworkAgent.hpp"
#include "PrintDispatcher.hpp"
#include "../../bambu_bridge/router/RawMqttPublisher.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>


namespace Slic3r {

void NetworkAgentPluginAdapter::set_dispatcher_inputs_resolver(
        DispatcherInputsResolver r) {
    std::lock_guard<std::mutex> lk(m_resolver_mu);
    m_inputs_resolver = std::move(r);
}

void NetworkAgentPluginAdapter::set_mtls_resolver(MtlsResolver r) {
    std::lock_guard<std::mutex> lk(m_resolver_mu);
    m_mtls_resolver = std::move(r);
}

namespace {

// Copy every GUI-equivalent field from the bridge's adapter-side
// param struct (LocalPrintParams or CloudUploadParams — they share the
// same set of GUI-mirror fields, see BambuNetworkingPluginHandle.hpp)
// into the upstream slicer-side PrintParams struct that NetworkAgent's
// start_print / start_local_print / start_send_gcode_to_sdcard expect.
//
// Templated on SrcT so one helper handles both bridge-side structs
// without dragging their private definitions into NetworkAgent's
// header. Defaults match what each call site was doing inline before
// this refactor, so behaviour is preserved when callers leave the new
// fields at their default values.
template <typename SrcT>
void fill_print_params(const SrcT&        src,
                       PrintParams&       dst,
                       const std::string& default_connection,
                       const std::string& filename_override = {}) {
    dst.dev_id           = src.dev_id;
    dst.dev_ip           = src.dev_ip;
    dst.username         = "bblp";
    dst.password         = src.access_code;
    dst.filename         = filename_override.empty()
                           ? src.local_file_path : filename_override;
    dst.project_name     = src.project_name.empty()
                           ? src.local_file_path : src.project_name;
    dst.connection_type  = src.connection_type.empty()
                           ? default_connection : src.connection_type;
    dst.use_ssl_for_ftp  = src.use_ssl_for_ftp;
    dst.use_ssl_for_mqtt = src.use_ssl_for_mqtt;

    // GUI-equivalent PrintParams fields. The bridge mirrors the GUI's
    // PrintJob exactly when these are plumbed through; when callers
    // leave them at their defaults the resulting PrintParams looks
    // the same as the pre-refactor inline copy did.
    dst.task_name                  = src.task_name;
    dst.preset_name                = src.preset_name;
    dst.config_filename            = src.config_filename;
    dst.plate_index                = src.plate_index;
    dst.nozzle_mapping             = src.nozzle_mapping;
    dst.ams_mapping                = src.ams_mapping;
    dst.ams_mapping2               = src.ams_mapping2;
    dst.ams_mapping_info           = src.ams_mapping_info;
    dst.nozzles_info               = src.nozzles_info;
    dst.comments                   = src.comments;
    dst.origin_profile_id          = src.origin_profile_id;
    dst.stl_design_id              = src.stl_design_id;
    dst.origin_model_id            = src.origin_model_id;
    dst.print_type                 = src.print_type;
    dst.dst_file                   = src.dst_file;
    dst.dev_name                   = src.dev_name;
    dst.task_bed_leveling          = src.task_bed_leveling;
    dst.task_flow_cali             = src.task_flow_cali;
    dst.task_vibration_cali        = src.task_vibration_cali;
    dst.task_layer_inspect         = src.task_layer_inspect;
    dst.task_record_timelapse      = src.task_record_timelapse;
    dst.task_timelapse_use_internal= src.task_timelapse_use_internal;
    dst.task_use_ams               = src.task_use_ams;
    dst.task_bed_type              = src.task_bed_type;
    dst.extra_options              = src.extra_options;
    dst.auto_bed_leveling          = src.auto_bed_leveling;
    dst.auto_flow_cali             = src.auto_flow_cali;
    dst.auto_offset_cali           = src.auto_offset_cali;
    dst.extruder_cali_manual_mode  = src.extruder_cali_manual_mode;
    dst.task_ext_change_assist     = src.task_ext_change_assist;
    dst.try_emmc_print             = src.try_emmc_print;
}

// Plugin callback stubs for the four PrintParams-taking plugin calls
// the bridge issues from headless / --bridge-only mode. The proprietary
// plugin (libbambu_networking 02.06.01.55) does NOT guard against null
// std::function callbacks — passing nullptr makes its state machine
// either skip critical phases or abort outright with errors that look
// nothing like the underlying cause (observed: -3070 PRINT_SP_FILE_NOT_
// EXIST returned even when the .3mf file is on disk and readable, because
// the wait phase couldn't run).
//
// Mirror what the GUI's PrintJob would pass:
//   - update_fn: progress reporter, fold to a bridge-side log line so we
//     see the plugin's stage transitions.
//   - cancel_fn: "user cancelled?" — always false in headless. The GUI
//     wires this to ctl.was_canceled().
//   - wait_fn:   "the printer acked the job, ok to proceed?" — always
//     true in headless. The GUI does a 60s wait-for-job_id-match loop
//     here, but in --bridge-only we have no UI to time-out and the
//     downstream slicer (Orca) is already showing its own progress, so
//     a synchronous true is correct.
static BBL::OnUpdateStatusFn make_update_fn(const char*       tag,
                                            const std::string& dev_id) {
    return [tag, dev_id](int stage, int code, std::string info) {
        std::fprintf(stderr,
            "[adapter:%s] dev=%s stage=%d code=%d info=%s\n",
            tag, dev_id.c_str(), stage, code, info.c_str());
        std::fflush(stderr);
    };
}

static BBL::WasCancelledFn make_cancel_fn() {
    return []() -> bool { return false; };
}

static BBL::OnWaitFn make_wait_fn(const char*        tag,
                                   const std::string& dev_id) {
    return [tag, dev_id](int state, std::string job_info) -> bool {
        std::fprintf(stderr,
            "[adapter:%s] dev=%s wait state=%d job_info=%s -> ack\n",
            tag, dev_id.c_str(), state, job_info.c_str());
        std::fflush(stderr);
        return true;
    };
}

} // namespace

NetworkAgentPluginAdapter::NetworkAgentPluginAdapter(NetworkAgent* agent)
    : bridge::BambuNetworkingPluginHandle(bridge::PluginHandleConfig{}),
      m_agent(agent)
{
    if (!m_agent) return;

    // The tap fires on a plugin worker thread for every non-virtual
    // dev_id message. Forward into the base class's dispatch routines
    // so per-dev_id receivers registered via register_receiver /
    // register_local_message_receiver actually fire.
    m_agent->set_bridge_message_tap(
        [this](const std::string& dev_id,
               const std::string& payload,
               bool               is_local) {
            if (is_local) this->dispatch_local_message(dev_id, payload);
            else          this->dispatch_message      (dev_id, payload);
        });
}

NetworkAgentPluginAdapter::~NetworkAgentPluginAdapter()
{
    if (m_agent) m_agent->set_bridge_message_tap(nullptr);
}

bool NetworkAgentPluginAdapter::init() {
    // No-op. NetworkAgent has already dlopened the plugin and called
    // create_agent / start; constructing a second handle would corrupt
    // the plugin's global state. Returning true keeps every bridge-side
    // "is the handle good?" check happy.
    return m_agent != nullptr;
}

bool NetworkAgentPluginAdapter::agent_ready() const {
    return m_agent != nullptr && m_agent->get_network_agent() != nullptr;
}

bool NetworkAgentPluginAdapter::is_user_login() const {
    return m_agent && m_agent->is_user_login();
}

bool NetworkAgentPluginAdapter::is_server_connected() const {
    return m_agent && m_agent->is_server_connected();
}

bool NetworkAgentPluginAdapter::is_local_connected() const {
    // The plugin has no public is_local_connected() export. We mirror
    // the LAN connection state ourselves: set on a successful
    // connect_printer, cleared on disconnect_printer. Good enough for
    // LanUplink::is_connected — if the plugin drops the LAN session
    // independently, the next publish will surface the error.
    return m_local_connected.load();
}

bool NetworkAgentPluginAdapter::get_user_print_info(
        unsigned int* http_code, std::string* http_body) const {
    if (!m_agent || !http_code || !http_body) return false;
    return m_agent->get_user_print_info(http_code, http_body) == 0;
}

int NetworkAgentPluginAdapter::subscribe_device(const std::string& dev_id) {
    if (!m_agent) return -1;
    return m_agent->add_subscribe(std::vector<std::string>{dev_id});
}

int NetworkAgentPluginAdapter::unsubscribe_device(const std::string& dev_id) {
    if (!m_agent) return -1;
    return m_agent->del_subscribe(std::vector<std::string>{dev_id});
}

int NetworkAgentPluginAdapter::publish_to_device(
        const std::string& dev_id, const std::string& json_payload, int qos) {
    if (!m_agent) return -1;
    // NetworkAgent::send_message hits the plugin's cloud `send_message`
    // export (bambu_network_send_message). `flag` is reserved upstream;
    // pass 0 to match what GUI_App's own publishers do.
    return m_agent->send_message(dev_id, json_payload, qos, 0);
}

int NetworkAgentPluginAdapter::upload_gcode_to_sdcard(
        const CloudUploadParams& params) {
    // Mirror what the GUI's PrintJob does: try the LAN-FTPS-to-printer
    // path first (start_send_gcode_to_sdcard), and on failure fall back
    // to the cloud-relay path (start_print) — the same one PrintJob
    // uses for cloud-bound printers and for printers like A1 that have
    // no LAN FTPS server.
    //
    // Why both: not all real Bambu printers run an FTPS endpoint. H2S,
    // H2D, X1, P1 do; A1/N2S does NOT (per printers.yaml — port 6000
    // on A1 is MJPEG, no file tunnel, no SD-card FTPS). For A1 the
    // start_send_gcode_to_sdcard call returns -5010
    // (BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED) because the
    // plugin can't open the FTPS connection to 192.168.1.6:990. The
    // GUI's PrintJob handles this by routing through start_print
    // (cloud-relay); the bridge needs to do the same so the FFFP
    // FTPS-into-bridge flow remains transparent end-to-end.
    if (!m_agent) return -1;
    PrintParams pp{};
    fill_print_params(params, pp, /*default_connection=*/"cloud");
    int rc = m_agent->start_send_gcode_to_sdcard(
        pp,
        make_update_fn("upload_gcode_to_sdcard.primary", pp.dev_id),
        make_cancel_fn(),
        make_wait_fn("upload_gcode_to_sdcard.primary", pp.dev_id));
    std::fprintf(stderr,
        "[adapter] upload_gcode_to_sdcard primary "
        "(start_send_gcode_to_sdcard) dev=%s ip=%s rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(), rc);
    std::fflush(stderr);
    if (rc == 0) return 0;
    // Cloud-relay fallback. PrintJob for cloud-bound + FTPS-less
    // printers does this same call.
    int rc2 = m_agent->start_print(
        pp,
        make_update_fn("upload_gcode_to_sdcard.fallback", pp.dev_id),
        make_cancel_fn(),
        make_wait_fn("upload_gcode_to_sdcard.fallback", pp.dev_id));
    std::fprintf(stderr,
        "[adapter] upload_gcode_to_sdcard fallback "
        "(start_print / cloud-relay) dev=%s ip=%s rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(), rc2);
    std::fflush(stderr);
    return rc2;
}

int NetworkAgentPluginAdapter::connect_printer(
        const std::string& dev_id, const std::string& dev_ip,
        const std::string& username, const std::string& password,
        bool use_ssl) {
    if (!m_agent) return -1;
    int rc = m_agent->connect_printer(dev_id, dev_ip, username, password, use_ssl);
    if (rc == 0) m_local_connected.store(true);
    return rc;
}

int NetworkAgentPluginAdapter::set_user_selected_machine(const std::string& dev_id) {
    if (!m_agent) return -1;
    return m_agent->set_user_selected_machine(dev_id);
}

void NetworkAgentPluginAdapter::install_device_cert(const std::string& dev_id, bool lan_only) {
    if (!m_agent) return;
    m_agent->install_device_cert(dev_id, lan_only);
}

int NetworkAgentPluginAdapter::disconnect_printer() {
    if (!m_agent) return -1;
    int rc = m_agent->disconnect_printer();
    m_local_connected.store(false);
    return rc;
}

int NetworkAgentPluginAdapter::send_message_to_printer(
        const std::string& dev_id, const std::string& json_payload, int qos) {
    if (!m_agent) {
        std::fprintf(stderr,
            "[adapter] send_message_to_printer dev=%s NO AGENT\n",
            dev_id.c_str());
        std::fflush(stderr);
        return -1;
    }
    // Mirror MachineObject::publish_json (DeviceManager.cpp:2354): for
    // cloud-bound printers it routes "print" commands via cloud
    // (`cloud_publish_json` → `send_message`); only LAN-only printers
    // use `send_message_to_printer`. The bridge is cloud-authenticated
    // and owns these dev_ids in the user's account, so cloud is the
    // right path here. Try cloud first; fall back to LAN MQTT if the
    // cloud route fails (which happens for LAN-only printers that
    // have no cloud relay).
    // Try cloud route first (matches MachineObject::cloud_publish_json
    // for cloud-bound printers); LAN fallback for LAN-only ones. The
    // plugin's send_message rejects `print.command=*` (control) payloads
    // with -2/-4 regardless of every prep we've tried; that's a hard
    // limit of the proprietary plugin from outside the GUI's
    // click-driven publish path.
    int rc_cloud = m_agent->send_message(dev_id, json_payload, qos, 0);
    if (rc_cloud == 0) {
        std::fprintf(stderr,
            "[adapter] send_message(CLOUD) dev=%s qos=%d bytes=%zu rc=0\n",
            dev_id.c_str(), qos, json_payload.size());
        std::fflush(stderr);
        return 0;
    }
    int rc_lan = m_agent->send_message_to_printer(dev_id, json_payload, qos, 0);
    std::fprintf(stderr,
        "[adapter] send_message_to_printer dev=%s qos=%d bytes=%zu "
        "rc_cloud=%d rc_lan=%d login=%d server=%d payload_head=%.60s\n",
        dev_id.c_str(), qos, json_payload.size(), rc_cloud, rc_lan,
        int(m_agent->is_user_login()),
        int(m_agent->is_server_connected()),
        json_payload.c_str());
    std::fflush(stderr);
    // No raw mTLS fallback: the GUI does not have one, so neither does
    // the bridge in "be identical to the GUI" mode. If the plugin's
    // primary cloud+LAN send_message returned an error, surface it —
    // do not synthesise a publish through a path the GUI never uses.
    // See experiment/identical-to-gui branch rationale + removed
    // mTLS resolver / RawMqttPublisher one-shot path that used to live
    // here.
    return rc_lan;
}

int NetworkAgentPluginAdapter::start_local_print_with_record(
        const LocalPrintParams& params) {
    // Delegates to the shared PrintDispatcher (Slic3r/Utils/PrintDispatcher.cpp)
    // which is the exact same decision tree the GUI's PrintJob walks.
    // Behaviour observed across all four captured (model × mode)
    // scenarios — see docs/plugin-trace/{H2D,A1}-{cloud,lan}.yaml — is
    // produced by that single dispatcher, so the bridge stops needing
    // its own fallback chain.
    //
    // What this method used to do inline (start_local_print_with_record
    // primary → on -2130 fall back to start_print) is now case 2 of the
    // dispatcher's cloud branch. The dispatcher additionally handles
    // the A1-cloud (start_print direct with comments="low_version"),
    // LAN-mode verify_job, and LAN-mode start_local_print branches —
    // none of which were correctly covered by the previous inline
    // logic.
    if (!m_agent) return -1;

    PrintParams pp{};
    fill_print_params(params, pp, /*default_connection=*/"lan");

    // Resolve per-printer capability info + ftp_folder via the
    // installed resolver. The resolver reads from MachineObject and
    // the per-model JSON in resources/printers/ — see
    // PrintDispatcherInputs.hpp for the bridge-from-GUI mapping.
    PrintDispatcher::Inputs in;
    std::string             ftp_folder;
    {
        std::lock_guard<std::mutex> lk(m_resolver_mu);
        if (m_inputs_resolver) m_inputs_resolver(pp.dev_id, in, ftp_folder);
    }
    if (!ftp_folder.empty()) pp.ftp_folder = ftp_folder;

    auto rr = PrintDispatcher{}.dispatch(
        pp, m_agent, in,
        make_update_fn("start_local_print_with_record", pp.dev_id),
        make_cancel_fn(),
        make_wait_fn("start_local_print_with_record", pp.dev_id),
        /*pre_status=*/nullptr);

    std::fprintf(stderr,
        "[adapter] dispatch start_local_print_with_record dev=%s ip=%s "
        "connection_type=%s comments=%s tried_lan=%d "
        "lan_failed_used_cloud=%d verify_job_failed=%d rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(),
        pp.connection_type.c_str(), pp.comments.c_str(),
        int(rr.tried_lan), int(rr.lan_failed_used_cloud),
        int(rr.verify_job_failed), rr.rc);
    std::fflush(stderr);
    return rr.rc;
}

// LAN print (no slicer-side record). The wider PrintParams that ths
// GUI fills (AMS mapping, task flags, etc.) flow through here when
// they're plumbed end-to-end; for now we fill what we have.
int NetworkAgentPluginAdapter::start_local_print(const LocalPrintParams& params) {
    if (!m_agent) return -1;
    PrintParams pp{};
    fill_print_params(params, pp, /*default_connection=*/"lan");
    int rc = m_agent->start_local_print(
        pp,
        make_update_fn("start_local_print", pp.dev_id),
        make_cancel_fn());
    std::fprintf(stderr,
        "[adapter] start_local_print dev=%s ip=%s rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(), rc);
    std::fflush(stderr);
    return rc;
}

// Re-print a 3MF that's already resident on the printer's SD card.
// `local_file_path` carries the on-printer path here, not a local
// upload — same convention as the upstream plugin's start_sdcard_print.
int NetworkAgentPluginAdapter::start_sdcard_print(const LocalPrintParams& params) {
    if (!m_agent) return -1;
    PrintParams pp{};
    // local_file_path here carries the on-printer path (e.g.
    // "/sdcard/Metadata/plate_1.3mf") — the helper preserves it.
    fill_print_params(params, pp, /*default_connection=*/"lan");
    int rc = m_agent->start_sdcard_print(
        pp,
        make_update_fn("start_sdcard_print", pp.dev_id),
        make_cancel_fn());
    std::fprintf(stderr,
        "[adapter] start_sdcard_print dev=%s ip=%s on_printer_path=%s rc=%d\n",
        pp.dev_id.c_str(), pp.dev_ip.c_str(), pp.filename.c_str(), rc);
    std::fflush(stderr);
    return rc;
}

int NetworkAgentPluginAdapter::get_camera_url(
        const std::string& dev_id, std::string* url_out, int timeout_ms) {
    if (!m_agent || !url_out) return -1;

    std::mutex              mu;
    std::condition_variable cv;
    bool                    done = false;
    std::string             url;
    auto cb = [&](std::string u) {
        std::lock_guard<std::mutex> lk(mu);
        url  = std::move(u);
        done = true;
        cv.notify_one();
    };
    int rc = m_agent->get_camera_url(dev_id, cb);
    if (rc != 0) return -3;

    std::unique_lock<std::mutex> lk(mu);
    if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                     [&]{ return done; }))
        return -4;
    *url_out = std::move(url);
    return 0;
}

} // namespace Slic3r
