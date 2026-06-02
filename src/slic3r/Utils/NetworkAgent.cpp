#include <stdio.h>
#include <stdlib.h>
#include <cstdarg>
#include <cstdio>
#if defined(_MSC_VER) || defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <atomic>
#include <fstream>
#include <set>
#include <sstream>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/BBLUtil.hpp"
#include "NetworkAgent.hpp"
#include "NetworkAgentBridgeHooks.hpp"
#include "PluginTrace.hpp"
#include "bambu_virtual_client/VirtualFtpsClient.hpp"
#include "bambu_virtual_client/VirtualMqttClient.hpp"
#include "bambu_virtual_client/VirtualSsdpDiscovery.hpp"
#include "bambu_virtual_client/VirtualLanPrinterStore.hpp"

#include "slic3r/Utils/FileTransferUtils.hpp"
#include "slic3r/Utils/CertificateVerify.hpp"

#if defined(BAMBU_BRIDGE_HARNESS_ENABLE)
// Pulled in only when the harness build option is set. The ShimRecorder
// is process-wide; the wrap function defined near the bottom of this
// file rewrites NetworkAgent::*_ptr to point at recording trampolines.
#include "harness/ShimRecorder.hpp"
namespace Slic3r {
// Forward declaration so initialize_network_module can call into the
// wrap helper without dragging the trampolines up the file.
void bb_harness_wrap_network_agent_pointers();
}
#endif

using namespace BBL;

namespace Slic3r {

#define BAMBU_SOURCE_LIBRARY "BambuSource"

// ---- Full plugin-call trace (gated on env vars) ----------------------
//
//   BAMBU_BRIDGE_PLUGIN_TRACE=1     — emit `[plugincall] …` lines
//   BAMBU_BRIDGE_PLUGIN_SNAPSHOT=1  — also hard-link/copy every .3mf
//                                     PrintParams references into
//                                     /tmp/plugin-trace-3mf-snapshots/
//
// Shared infrastructure lives in PluginTrace.hpp. Used by NetworkAgent,
// BambuSourceHandle, PrinterFileSystem.
namespace {

using Slic3r::plugin_trace::log_event;
using Slic3r::plugin_trace::truncate;
using Slic3r::plugin_trace::snapshot_path;
using Slic3r::plugin_trace::dump_stack;

// Always-on snapshot of the .3mf BBS hands to the plugin (or to our own
// virtual_lan_print_), keyed by dev_id. Mirrors the bridge's own
// `/tmp/bridge-last-upload-<dev_id>.3mf` capture so the two files are
// directly comparable:
//
//   /tmp/bbs-sent-<dev_id>.3mf            ← slicer-side original
//   /tmp/bbs-sent-<dev_id>.json           ← print-command fields incl.
//                                           ams_mapping, nozzle_mapping,
//                                           ftp_*, task_*, connection_type
//   /tmp/bridge-last-upload-<dev_id>.3mf  ← bridge post-rewrite (only
//                                            written when bridge is up)
//
// For an FFFF print all three files exist; for a direct-to-printer print
// (bridge down) only the bbs-sent ones exist. Hard-link is free on the
// same filesystem; falls back to copy on EXDEV. Same paths are
// overwritten on each print so the latest is always inspectable.
inline void snapshot_bbs_sent(const std::string& dev_id,
                              const std::string& src_path) {
    if (dev_id.empty() || src_path.empty()) return;
    struct stat st{};
    if (::stat(src_path.c_str(), &st) != 0) return;
    std::string dbg = "/tmp/bbs-sent-" + dev_id + ".3mf";
    ::unlink(dbg.c_str());
    if (::link(src_path.c_str(), dbg.c_str()) != 0) {
        FILE* in  = std::fopen(src_path.c_str(), "rb");
        FILE* out = std::fopen(dbg.c_str(),      "wb");
        if (in && out) {
            char buf[64 * 1024];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
                std::fwrite(buf, 1, n, out);
        }
        if (in)  std::fclose(in);
        if (out) std::fclose(out);
    }
    std::fprintf(stderr,
        "[bbs-sent] snapshot dev=%s bytes=%lld src=%s -> %s\n",
        dev_id.c_str(), (long long) st.st_size, src_path.c_str(), dbg.c_str());
    std::fflush(stderr);
}

// Dump the PrintParams fields that drive the printer's accept/reject
// decision — most importantly `ams_mapping` (and its v2 / info variants),
// `nozzle_mapping`, the task_* flags, FTP target, and the connection
// type. JSON is a wrapper around the raw string fields so a `diff`
// between a cloud print and a LAN print is mechanical: keys are
// stable, only values change.
//
// This is the slicer-side equivalent of capturing the print-command
// MQTT payload — the on-wire JSON is built by the proprietary plugin
// from these same fields, so a diff at this layer is sufficient
// without packet-capturing the MQTT publish.
inline std::string j_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o += c;
            }
        }
    }
    return o;
}

inline void snapshot_print_cmd(const char* call_site,
                               const BBL::PrintParams& p) {
    if (p.dev_id.empty()) return;

    // Write the JSON body into any FILE*. Factored into a lambda so we
    // can emit a fresh copy directly into the per-print capture dir —
    // hard-linking the "latest" file into the dir doesn't work because
    // the next print re-opens that path with fopen("wb") which truncates
    // the inode in place, clobbering every hard-linked snapshot too.
    auto write_json = [&](FILE* f) {
        auto wkv = [&](const char* k, const std::string& v, bool last = false) {
            std::fprintf(f, "  \"%s\": \"%s\"%s\n", k, j_escape(v).c_str(),
                last ? "" : ",");
        };
        auto wki = [&](const char* k, long long v, bool last = false) {
            std::fprintf(f, "  \"%s\": %lld%s\n", k, v, last ? "" : ",");
        };
        auto wkb = [&](const char* k, bool v, bool last = false) {
            std::fprintf(f, "  \"%s\": %s%s\n", k, v ? "true" : "false",
                last ? "" : ",");
        };
        std::fprintf(f, "{\n");
        wkv("_call_site",        call_site);
        wkv("dev_id",            p.dev_id);
        wkv("dev_ip",            p.dev_ip);
        wkv("dev_name",          p.dev_name);
        wkv("username",          p.username);
        wkv("connection_type",   p.connection_type);
        wkv("filename",          p.filename);
        wkv("config_filename",   p.config_filename);
        wkv("project_name",      p.project_name);
        wkv("task_name",         p.task_name);
        wkv("preset_name",       p.preset_name);
        wki("plate_index",       p.plate_index);
        wkb("use_ssl_for_ftp",   p.use_ssl_for_ftp);
        wkb("use_ssl_for_mqtt",  p.use_ssl_for_mqtt);
        wkv("ftp_folder",        p.ftp_folder);
        wkv("ftp_file",          p.ftp_file);
        wkv("ftp_file_md5",      p.ftp_file_md5);
        wkv("nozzle_mapping",    p.nozzle_mapping);
        wkv("ams_mapping",       p.ams_mapping);
        wkv("ams_mapping2",      p.ams_mapping2);
        wkv("ams_mapping_info",  p.ams_mapping_info);
        wkv("nozzles_info",      p.nozzles_info);
        wkv("comments",          p.comments);
        wki("origin_profile_id", p.origin_profile_id);
        wki("stl_design_id",     p.stl_design_id);
        wkv("origin_model_id",   p.origin_model_id);
        wkv("print_type",        p.print_type);
        wkv("dst_file",          p.dst_file);
        wkb("task_bed_leveling",        p.task_bed_leveling);
        wkb("task_flow_cali",           p.task_flow_cali);
        wkb("task_vibration_cali",      p.task_vibration_cali);
        wkb("task_layer_inspect",       p.task_layer_inspect);
        wkb("task_record_timelapse",    p.task_record_timelapse);
        wkb("task_timelapse_use_internal", p.task_timelapse_use_internal);
        wkb("task_use_ams",             p.task_use_ams);
        wkv("task_bed_type",            p.task_bed_type);
        wkv("extra_options",            p.extra_options);
        wki("auto_bed_leveling",        p.auto_bed_leveling);
        wki("auto_flow_cali",           p.auto_flow_cali);
        wki("auto_offset_cali",         p.auto_offset_cali);
        wki("extruder_cali_manual_mode", p.extruder_cali_manual_mode);
        wkb("task_ext_change_assist",   p.task_ext_change_assist);
        wkb("try_emmc_print",           p.try_emmc_print, /*last=*/true);
        std::fprintf(f, "}\n");
    };

    // Legacy "latest" snapshot at a stable path. Overwritten each print —
    // that's intentional; this is the convenience file for `cat` / `jq`.
    std::string out_path = "/tmp/bbs-sent-" + p.dev_id + ".json";
    // Unlink first so we always get a fresh inode — this is what keeps
    // any earlier hard-link in a per-print capture dir from being
    // clobbered when we re-open the latest path for write.
    ::unlink(out_path.c_str());
    if (FILE* f = std::fopen(out_path.c_str(), "wb")) {
        write_json(f);
        std::fclose(f);
    }
    std::fprintf(stderr,
        "[bbs-sent] print-cmd dev=%s call=%s ams_mapping=\"%.200s\" "
        "connection_type=%s -> %s\n",
        p.dev_id.c_str(), call_site,
        p.ams_mapping.c_str(),
        p.connection_type.c_str(), out_path.c_str());
    std::fflush(stderr);

    // ------------------------------------------------------------------
    // Per-print timestamped archive (BAMBU_BRIDGE_GUI_CAPTURE, default-on).
    //
    // The legacy /tmp/bbs-sent-<dev>.json / .3mf above is "latest snapshot"
    // — it's overwritten on every print. For a ground-truth study we want
    // each print preserved so the GUI→real-printer dual-extruder payload
    // can be diff'd against the bridge's own /tmp/bridge-capture/ entry
    // for the same model.
    //
    //   /tmp/slicer-capture/<dev_id>/<UTC-ts>_plate<N>/
    //     ├── <basename>.3mf
    //     ├── <basename>_config.3mf       (if config_filename set)
    //     ├── meta.json                   (== /tmp/bbs-sent-<dev>.json
    //                                       at the moment of capture)
    //     └── stderr.txt                  (mapping bodies, one per line)
    //
    // Set BAMBU_BRIDGE_GUI_CAPTURE=0 to disable.
    // ------------------------------------------------------------------
    {
        const char* cap_env = std::getenv("BAMBU_BRIDGE_GUI_CAPTURE");
        const bool  cap_on  = !cap_env || std::strcmp(cap_env, "0") != 0;
        if (!cap_on) return;

        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm{};
        ::gmtime_r(&ts.tv_sec, &tm);
        char ts_buf[64];
        std::snprintf(ts_buf, sizeof(ts_buf),
            "%04d%02d%02dT%02d%02d%02d.%03ldZ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec,
            long(ts.tv_nsec / 1000000));

        const std::string cap_root = "/tmp/slicer-capture";
        const std::string cap_dev  = cap_root + "/" + p.dev_id;
        char plate_buf[32];
        std::snprintf(plate_buf, sizeof(plate_buf), "_plate%d", p.plate_index);
        const std::string cap_dir = cap_dev + "/" + ts_buf + plate_buf
                                  + "_" + call_site;
        ::mkdir(cap_root.c_str(), 0700);
        ::mkdir(cap_dev.c_str(),  0700);
        ::mkdir(cap_dir.c_str(),  0700);

        auto link_or_copy = [](const std::string& src,
                                const std::string& dst) -> bool {
            if (src.empty()) return false;
            struct stat st{};
            if (::stat(src.c_str(), &st) != 0) return false;
            ::unlink(dst.c_str());
            if (::link(src.c_str(), dst.c_str()) == 0) return true;
            FILE* in_f  = std::fopen(src.c_str(),  "rb");
            FILE* out_f = std::fopen(dst.c_str(), "wb");
            if (!in_f || !out_f) {
                if (in_f)  std::fclose(in_f);
                if (out_f) std::fclose(out_f);
                return false;
            }
            char buf[64 * 1024];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), in_f)) > 0)
                std::fwrite(buf, 1, n, out_f);
            std::fclose(in_f);
            std::fclose(out_f);
            return true;
        };

        // Pick a friendly basename. p.project_name is the model name in
        // the slicer; fall back to filename's basename, then a literal.
        std::string cap_basename = p.project_name;
        if (cap_basename.empty() && !p.filename.empty()) {
            cap_basename = p.filename;
            if (auto pos = cap_basename.find_last_of('/'); pos != std::string::npos)
                cap_basename = cap_basename.substr(pos + 1);
        }
        if (cap_basename.empty()) cap_basename = "print";
        if (cap_basename.size() >= 4 &&
            cap_basename.compare(cap_basename.size() - 4, 4, ".3mf") == 0)
            cap_basename.resize(cap_basename.size() - 4);

        const bool got_3mf = link_or_copy(p.filename,
                                cap_dir + "/" + cap_basename + ".3mf");
        const bool got_cfg = link_or_copy(p.config_filename,
                                cap_dir + "/" + cap_basename + "_config.3mf");

        // meta.json — write a fresh copy directly. We can NOT hard-link
        // from /tmp/bbs-sent-<dev>.json: the next print re-opens that
        // path with fopen("wb"), which truncates the underlying inode
        // and corrupts every prior capture sharing it. write_json gives
        // us a distinct inode per capture dir.
        if (FILE* mf = std::fopen((cap_dir + "/meta.json").c_str(), "wb")) {
            write_json(mf);
            std::fclose(mf);
        }

        // Per-line mapping bodies for easy grep/jq.
        if (FILE* lf = std::fopen((cap_dir + "/stderr.txt").c_str(), "w")) {
            std::fprintf(lf, "call_site=%s\n",        call_site);
            std::fprintf(lf, "dev_id=%s\n",           p.dev_id.c_str());
            std::fprintf(lf, "dev_ip=%s\n",           p.dev_ip.c_str());
            std::fprintf(lf, "connection_type=%s\n",  p.connection_type.c_str());
            std::fprintf(lf, "project_name=%s\n",     p.project_name.c_str());
            std::fprintf(lf, "plate_index=%d\n",      p.plate_index);
            std::fprintf(lf, "task_use_ams=%d\n",     int(p.task_use_ams));
            std::fprintf(lf, "task_bed_type=%s\n",    p.task_bed_type.c_str());
            std::fprintf(lf, "ams_mapping=%s\n",      p.ams_mapping.c_str());
            std::fprintf(lf, "ams_mapping2=%s\n",     p.ams_mapping2.c_str());
            std::fprintf(lf, "ams_mapping_info=%s\n", p.ams_mapping_info.c_str());
            std::fprintf(lf, "nozzles_info=%s\n",     p.nozzles_info.c_str());
            std::fprintf(lf, "nozzle_mapping=%s\n",   p.nozzle_mapping.c_str());
            std::fprintf(lf, "captured_3mf=%d captured_config=%d\n",
                int(got_3mf), int(got_cfg));
            std::fclose(lf);
        }

        std::fprintf(stderr,
            "[bbs-sent] CAPTURE dir=%s 3mf=%d cfg=%d "
            "ams_mapping_info_len=%zu\n",
            cap_dir.c_str(), int(got_3mf), int(got_cfg),
            p.ams_mapping_info.size());
        std::fflush(stderr);
    }
}

// Dumps every field on PrintParams that PrintJob / SendJob is known to
// fill, in a fixed order so cross-scenario diffs are mechanical. Also
// hard-links the .3mf at p.filename and p.config_filename into
// /tmp/plugin-trace-3mf-snapshots/ when BAMBU_BRIDGE_PLUGIN_SNAPSHOT=1.
// Called BEFORE the plugin export so the call args are logged even when
// the plugin then deadlocks / returns -3070 / etc.
void dump_print_params(const char* fn, const BBL::PrintParams& p) {
    if (!Slic3r::plugin_trace::enabled()) return;
    Slic3r::plugin_trace::write_prefix(stderr);
    std::fprintf(stderr,
        "%s "
        "dev_id=%s dev_ip=%s username=%s "
        "filename=%s config_filename=%s "
        "project_name=%s task_name=%s preset_name=%s "
        "plate_index=%d connection_type=%s "
        "use_ssl_for_ftp=%d use_ssl_for_mqtt=%d "
        "ftp_folder=%s ftp_file=%s ftp_file_md5=%s "
        "nozzle_mapping=%s ams_mapping=%s ams_mapping2=%s "
        "ams_mapping_info=%s nozzles_info=%s comments=%s "
        "origin_profile_id=%d stl_design_id=%d "
        "origin_model_id=%s print_type=%s dst_file=%s dev_name=%s "
        "task_bed_leveling=%d task_flow_cali=%d task_vibration_cali=%d "
        "task_layer_inspect=%d task_record_timelapse=%d "
        "task_timelapse_use_internal=%d task_use_ams=%d "
        "task_bed_type=%s extra_options=%s "
        "auto_bed_leveling=%d auto_flow_cali=%d auto_offset_cali=%d "
        "extruder_cali_manual_mode=%d task_ext_change_assist=%d "
        "try_emmc_print=%d\n",
        fn,
        p.dev_id.c_str(), p.dev_ip.c_str(), p.username.c_str(),
        p.filename.c_str(), p.config_filename.c_str(),
        p.project_name.c_str(), p.task_name.c_str(), p.preset_name.c_str(),
        p.plate_index, p.connection_type.c_str(),
        int(p.use_ssl_for_ftp), int(p.use_ssl_for_mqtt),
        p.ftp_folder.c_str(), p.ftp_file.c_str(), p.ftp_file_md5.c_str(),
        p.nozzle_mapping.c_str(), p.ams_mapping.c_str(), p.ams_mapping2.c_str(),
        p.ams_mapping_info.c_str(), p.nozzles_info.c_str(), p.comments.c_str(),
        p.origin_profile_id, p.stl_design_id,
        p.origin_model_id.c_str(), p.print_type.c_str(),
        p.dst_file.c_str(), p.dev_name.c_str(),
        int(p.task_bed_leveling), int(p.task_flow_cali),
        int(p.task_vibration_cali), int(p.task_layer_inspect),
        int(p.task_record_timelapse), int(p.task_timelapse_use_internal),
        int(p.task_use_ams), p.task_bed_type.c_str(),
        p.extra_options.c_str(),
        p.auto_bed_leveling, p.auto_flow_cali, p.auto_offset_cali,
        p.extruder_cali_manual_mode, int(p.task_ext_change_assist),
        int(p.try_emmc_print));
    std::fflush(stderr);

    // Snapshot the referenced .3mf files so we can inspect their
    // structure offline (zip listing, compare filename vs config_
    // filename byte-for-byte etc.). Caller is responsible for not
    // overlapping snapshot calls for the same path within 1 ms.
    snapshot_path(fn, "filename",        p.filename);
    snapshot_path(fn, "config_filename", p.config_filename);

    // Call-tree dump (gated separately on BAMBU_BRIDGE_PLUGIN_STACK=1
    // because backtrace + demangle is allocator-heavy). Tag with `fn`
    // so the lines can be associated with this PrintParams dump.
    Slic3r::plugin_trace::dump_stack(fn);
}

} // namespace


#if defined(_MSC_VER) || defined(_WIN32)
static HMODULE networking_module = NULL;
static HMODULE source_module = NULL;
#else
static void* networking_module = NULL;
static void* source_module = NULL;
#endif


func_check_debug_consistent         NetworkAgent::check_debug_consistent_ptr = nullptr;
func_get_version                    NetworkAgent::get_version_ptr = nullptr;
func_create_agent                   NetworkAgent::create_agent_ptr = nullptr;
func_destroy_agent                  NetworkAgent::destroy_agent_ptr = nullptr;
func_init_log                       NetworkAgent::init_log_ptr = nullptr;
func_set_config_dir                 NetworkAgent::set_config_dir_ptr = nullptr;
func_set_cert_file                  NetworkAgent::set_cert_file_ptr = nullptr;
func_set_country_code               NetworkAgent::set_country_code_ptr = nullptr;
func_start                          NetworkAgent::start_ptr = nullptr;
func_set_on_ssdp_msg_fn             NetworkAgent::set_on_ssdp_msg_fn_ptr = nullptr;
func_set_on_user_login_fn           NetworkAgent::set_on_user_login_fn_ptr = nullptr;
func_set_on_printer_connected_fn    NetworkAgent::set_on_printer_connected_fn_ptr = nullptr;
func_set_on_server_connected_fn     NetworkAgent::set_on_server_connected_fn_ptr = nullptr;
func_set_on_http_error_fn           NetworkAgent::set_on_http_error_fn_ptr = nullptr;
func_set_get_country_code_fn        NetworkAgent::set_get_country_code_fn_ptr = nullptr;
func_set_on_subscribe_failure_fn    NetworkAgent::set_on_subscribe_failure_fn_ptr = nullptr;
func_set_on_message_fn              NetworkAgent::set_on_message_fn_ptr = nullptr;
func_set_on_user_message_fn         NetworkAgent::set_on_user_message_fn_ptr = nullptr;
func_set_on_local_connect_fn        NetworkAgent::set_on_local_connect_fn_ptr = nullptr;
func_set_on_local_message_fn        NetworkAgent::set_on_local_message_fn_ptr = nullptr;
func_set_queue_on_main_fn           NetworkAgent::set_queue_on_main_fn_ptr = nullptr;
func_connect_server                 NetworkAgent::connect_server_ptr = nullptr;
func_is_server_connected            NetworkAgent::is_server_connected_ptr = nullptr;
func_refresh_connection             NetworkAgent::refresh_connection_ptr = nullptr;
func_start_subscribe                NetworkAgent::start_subscribe_ptr = nullptr;
func_stop_subscribe                 NetworkAgent::stop_subscribe_ptr = nullptr;
func_add_subscribe                  NetworkAgent::add_subscribe_ptr = nullptr;
func_del_subscribe                  NetworkAgent::del_subscribe_ptr = nullptr;
func_enable_multi_machine           NetworkAgent::enable_multi_machine_ptr = nullptr;
func_send_message                   NetworkAgent::send_message_ptr = nullptr;
func_connect_printer                NetworkAgent::connect_printer_ptr = nullptr;
func_disconnect_printer             NetworkAgent::disconnect_printer_ptr = nullptr;
func_send_message_to_printer        NetworkAgent::send_message_to_printer_ptr = nullptr;
func_check_cert                     NetworkAgent::check_cert_ptr = nullptr;
func_install_device_cert            NetworkAgent::install_device_cert_ptr = nullptr;
func_start_discovery                NetworkAgent::start_discovery_ptr = nullptr;
func_change_user                    NetworkAgent::change_user_ptr = nullptr;
func_is_user_login                  NetworkAgent::is_user_login_ptr = nullptr;
func_user_logout                    NetworkAgent::user_logout_ptr = nullptr;
func_get_user_id                    NetworkAgent::get_user_id_ptr = nullptr;
func_get_user_name                  NetworkAgent::get_user_name_ptr = nullptr;
func_get_user_avatar                NetworkAgent::get_user_avatar_ptr = nullptr;
func_get_user_nickanme              NetworkAgent::get_user_nickanme_ptr = nullptr;
func_build_login_cmd                NetworkAgent::build_login_cmd_ptr = nullptr;
func_build_logout_cmd               NetworkAgent::build_logout_cmd_ptr = nullptr;
func_build_login_info               NetworkAgent::build_login_info_ptr = nullptr;
func_ping_bind                      NetworkAgent::ping_bind_ptr = nullptr;
func_bind_detect                    NetworkAgent::bind_detect_ptr = nullptr;
func_report_consent                 NetworkAgent::report_consent_ptr = nullptr;
func_set_server_callback            NetworkAgent::set_server_callback_ptr = nullptr;
func_bind                           NetworkAgent::bind_ptr = nullptr;
func_unbind                         NetworkAgent::unbind_ptr = nullptr;
func_get_bambulab_host              NetworkAgent::get_bambulab_host_ptr = nullptr;
func_get_user_selected_machine      NetworkAgent::get_user_selected_machine_ptr = nullptr;
func_set_user_selected_machine      NetworkAgent::set_user_selected_machine_ptr = nullptr;
func_start_print                    NetworkAgent::start_print_ptr = nullptr;
func_start_local_print_with_record  NetworkAgent::start_local_print_with_record_ptr = nullptr;
func_start_send_gcode_to_sdcard     NetworkAgent::start_send_gcode_to_sdcard_ptr = nullptr;
func_start_local_print              NetworkAgent::start_local_print_ptr = nullptr;
func_start_sdcard_print             NetworkAgent::start_sdcard_print_ptr = nullptr;
func_get_user_presets               NetworkAgent::get_user_presets_ptr = nullptr;
func_request_setting_id             NetworkAgent::request_setting_id_ptr = nullptr;
func_put_setting                    NetworkAgent::put_setting_ptr = nullptr;
func_get_setting_list               NetworkAgent::get_setting_list_ptr = nullptr;
func_get_setting_list2              NetworkAgent::get_setting_list2_ptr = nullptr;
func_delete_setting                 NetworkAgent::delete_setting_ptr = nullptr;
func_get_studio_info_url            NetworkAgent::get_studio_info_url_ptr = nullptr;
func_set_extra_http_header          NetworkAgent::set_extra_http_header_ptr = nullptr;
func_get_my_message                 NetworkAgent::get_my_message_ptr = nullptr;
func_check_user_task_report         NetworkAgent::check_user_task_report_ptr = nullptr;
func_get_user_print_info            NetworkAgent::get_user_print_info_ptr = nullptr;
func_get_user_tasks                 NetworkAgent::get_user_tasks_ptr = nullptr;
func_get_filament_spools            NetworkAgent::get_filament_spools_ptr = nullptr;
func_create_filament_spool          NetworkAgent::create_filament_spool_ptr = nullptr;
func_update_filament_spool          NetworkAgent::update_filament_spool_ptr = nullptr;
func_delete_filament_spools         NetworkAgent::delete_filament_spools_ptr = nullptr;
func_get_filament_config            NetworkAgent::get_filament_config_ptr = nullptr;
func_get_printer_firmware           NetworkAgent::get_printer_firmware_ptr = nullptr;
func_get_task_plate_index           NetworkAgent::get_task_plate_index_ptr = nullptr;
func_get_user_info                  NetworkAgent::get_user_info_ptr = nullptr;
func_request_bind_ticket            NetworkAgent::request_bind_ticket_ptr = nullptr;
func_get_subtask_info               NetworkAgent::get_subtask_info_ptr = nullptr;
func_get_slice_info                 NetworkAgent::get_slice_info_ptr = nullptr;
func_query_bind_status              NetworkAgent::query_bind_status_ptr = nullptr;
func_modify_printer_name            NetworkAgent::modify_printer_name_ptr = nullptr;
func_get_camera_url                 NetworkAgent::get_camera_url_ptr = nullptr;
func_get_camera_url_for_golive      NetworkAgent::get_camera_url_for_golive_ptr = nullptr;
func_get_design_staffpick           NetworkAgent::get_design_staffpick_ptr = nullptr;
func_start_pubilsh                  NetworkAgent::start_publish_ptr = nullptr;
func_get_model_publish_url          NetworkAgent::get_model_publish_url_ptr = nullptr;
func_get_model_mall_home_url        NetworkAgent::get_model_mall_home_url_ptr = nullptr;
func_get_model_mall_detail_url      NetworkAgent::get_model_mall_detail_url_ptr = nullptr;
func_get_subtask                    NetworkAgent::get_subtask_ptr = nullptr;
func_get_my_profile                 NetworkAgent::get_my_profile_ptr = nullptr;
func_get_my_token                   NetworkAgent::get_my_token_ptr = nullptr;
func_track_enable                   NetworkAgent::track_enable_ptr = nullptr;
func_track_remove_files             NetworkAgent::track_remove_files_ptr = nullptr;
func_track_event                    NetworkAgent::track_event_ptr = nullptr;
func_track_header                   NetworkAgent::track_header_ptr = nullptr;
func_track_update_property          NetworkAgent::track_update_property_ptr = nullptr;
func_track_get_property             NetworkAgent::track_get_property_ptr = nullptr;
func_put_model_mall_rating_url      NetworkAgent::put_model_mall_rating_url_ptr = nullptr;
func_get_oss_config                 NetworkAgent::get_oss_config_ptr = nullptr;
func_put_rating_picture_oss         NetworkAgent::put_rating_picture_oss_ptr = nullptr;
func_get_model_mall_rating_result   NetworkAgent::get_model_mall_rating_result_ptr  = nullptr;

func_get_mw_user_preference         NetworkAgent::get_mw_user_preference_ptr = nullptr;
func_get_mw_user_4ulist             NetworkAgent::get_mw_user_4ulist_ptr     = nullptr;
func_get_hms_snapshot               NetworkAgent::get_hms_snapshot_ptr       = nullptr;

NetworkAgent::NetworkAgent(std::string log_dir)
{
    if (create_agent_ptr) {
        network_agent = create_agent_ptr(log_dir);
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", line %1%, network_agent=%2%, create_agent_ptr=%3%")%__LINE__ %network_agent %create_agent_ptr;
}

NetworkAgent::~NetworkAgent()
{
    int ret = 0;
    if (network_agent && destroy_agent_ptr) {
        ret = destroy_agent_ptr(network_agent);
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", line %1%, network_agent=%2%, destroy_agent_ptr=%3%, ret %4%")%__LINE__ %network_agent %destroy_agent_ptr %ret;
}

std::string NetworkAgent::get_libpath_in_current_directory(std::string library_name)
{
    std::string lib_path;
#if defined(_MSC_VER) || defined(_WIN32)
    wchar_t file_name[512];
    DWORD ret = GetModuleFileNameW(NULL, file_name, 512);
    if (!ret) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", GetModuleFileNameW return error, can not Load Library for %1%") %library_name;
        return lib_path;
    }
    int size_needed = ::WideCharToMultiByte(0, 0, file_name, wcslen(file_name), nullptr, 0, nullptr, nullptr);
    std::string file_name_string(size_needed, 0);
    ::WideCharToMultiByte(0, 0, file_name, wcslen(file_name), file_name_string.data(), size_needed, nullptr, nullptr);

    std::size_t found = file_name_string.find("bambu-studio.exe");
    if (found == (file_name_string.size() - 16)) {
        lib_path = library_name + ".dll";
        lib_path = file_name_string.replace(found, 16, lib_path);
    }
#else
#endif
    return lib_path;
}


int NetworkAgent::initialize_network_module(bool using_backup, bool validate_cert)
{
    //int ret = -1;
    std::string library;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

    if (using_backup) {
        plugin_folder = plugin_folder/"backup";
    }
    std::optional<SignerSummary> self_cert_summary, module_cert_summary;
    if (validate_cert)
        self_cert_summary = SummarizeSelf();
    else
        BOOST_LOG_TRIVIAL(info) << "wouldn't validate networking dll cert";
    if (!self_cert_summary)
        BOOST_LOG_TRIVIAL(info) << "self cert not exist";

    //first load the library
#if defined(_MSC_VER) || defined(_WIN32)
    library = plugin_folder.string() + "\\" + std::string(BAMBU_NETWORK_LIBRARY) + ".dll";
    wchar_t lib_wstr[128];
    memset(lib_wstr, 0, sizeof(lib_wstr));
    ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
    if (self_cert_summary) {
        module_cert_summary = SummarizeModule(library);
        if (module_cert_summary) {
            if (IsSamePublisher(*self_cert_summary, *module_cert_summary))
                networking_module = LoadLibrary(lib_wstr);
            else
                BOOST_LOG_TRIVIAL(info) << "module is from another publisher:" << module_cert_summary->as_print();
        }
        else
            BOOST_LOG_TRIVIAL(info) << "module_cert is null";
    } else
        networking_module = LoadLibrary(lib_wstr);
    if (!networking_module) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", try load library directly from current directory");

        std::string library_path = get_libpath_in_current_directory(std::string(BAMBU_NETWORK_LIBRARY));
        if (library_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", can not get path in current directory for %1%") % BAMBU_NETWORK_LIBRARY;
            return -1;
        }
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library_path.c_str(), strlen(library_path.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        if (self_cert_summary) {
            module_cert_summary = SummarizeModule(library_path);
            if (module_cert_summary) {
                if (IsSamePublisher(*self_cert_summary, *module_cert_summary))
                    networking_module = LoadLibrary(lib_wstr);
                else
                    BOOST_LOG_TRIVIAL(info) << "module is from another publisher:" << module_cert_summary->as_print();
            }
            else
                BOOST_LOG_TRIVIAL(info) << "module_cert is null";
        }
        else
            networking_module = LoadLibrary(lib_wstr);
    }
#else
    #if defined(__WXMAC__)
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".dylib";
    #else
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".so";
    #endif
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", line %1%, loading network module, using_backup %2%\n")%__LINE__ %using_backup;
    module_cert_summary = SummarizeModule(library);
    if (self_cert_summary) {
        module_cert_summary = SummarizeModule(library);
        if (module_cert_summary) {
            if (IsSamePublisher(*self_cert_summary, *module_cert_summary))
                networking_module = dlopen(library.c_str(), RTLD_LAZY);
            else
                BOOST_LOG_TRIVIAL(info) << "module is from another publisher:" << module_cert_summary->as_print();
        }
        else
            BOOST_LOG_TRIVIAL(info) << "module_cert is null";
    }
    else
        networking_module = dlopen( library.c_str(), RTLD_LAZY);
    if (!networking_module) {
        char* dll_error = dlerror();
        std::string err       = dll_error ? std::string(dll_error) : std::string("(null)");
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", error, dlerror is %1%") % err;
    }
    BOOST_LOG_TRIVIAL(info) << boost::format("after dlopen, network_module is %1%") % networking_module;
#endif

    if (!networking_module) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", line %1%, can not Load Library, using_backup %2%\n")%__LINE__ %using_backup;
        return -1;
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", line %1%,  successfully loaded library, using_backup %2%, module %3%")%__LINE__ %using_backup %networking_module;

    // load file transfer interface
    InitFTModule(networking_module);

    //load the functions
    check_debug_consistent_ptr        =  reinterpret_cast<func_check_debug_consistent>(get_network_function("bambu_network_check_debug_consistent"));
    get_version_ptr                   =  reinterpret_cast<func_get_version>(get_network_function("bambu_network_get_version"));
    create_agent_ptr                  =  reinterpret_cast<func_create_agent>(get_network_function("bambu_network_create_agent"));
    destroy_agent_ptr                 =  reinterpret_cast<func_destroy_agent>(get_network_function("bambu_network_destroy_agent"));
    init_log_ptr                      =  reinterpret_cast<func_init_log>(get_network_function("bambu_network_init_log"));
    set_config_dir_ptr                =  reinterpret_cast<func_set_config_dir>(get_network_function("bambu_network_set_config_dir"));
    set_cert_file_ptr                 =  reinterpret_cast<func_set_cert_file>(get_network_function("bambu_network_set_cert_file"));
    set_country_code_ptr              =  reinterpret_cast<func_set_country_code>(get_network_function("bambu_network_set_country_code"));
    start_ptr                         =  reinterpret_cast<func_start>(get_network_function("bambu_network_start"));
    set_on_ssdp_msg_fn_ptr            =  reinterpret_cast<func_set_on_ssdp_msg_fn>(get_network_function("bambu_network_set_on_ssdp_msg_fn"));
    set_on_user_login_fn_ptr          =  reinterpret_cast<func_set_on_user_login_fn>(get_network_function("bambu_network_set_on_user_login_fn"));
    set_on_printer_connected_fn_ptr   =  reinterpret_cast<func_set_on_printer_connected_fn>(get_network_function("bambu_network_set_on_printer_connected_fn"));
    set_on_server_connected_fn_ptr    =  reinterpret_cast<func_set_on_server_connected_fn>(get_network_function("bambu_network_set_on_server_connected_fn"));
    set_on_http_error_fn_ptr          =  reinterpret_cast<func_set_on_http_error_fn>(get_network_function("bambu_network_set_on_http_error_fn"));
    set_get_country_code_fn_ptr       =  reinterpret_cast<func_set_get_country_code_fn>(get_network_function("bambu_network_set_get_country_code_fn"));
    set_on_subscribe_failure_fn_ptr   =  reinterpret_cast<func_set_on_subscribe_failure_fn>(get_network_function("bambu_network_set_on_subscribe_failure_fn"));
    set_on_message_fn_ptr             =  reinterpret_cast<func_set_on_message_fn>(get_network_function("bambu_network_set_on_message_fn"));
    set_on_user_message_fn_ptr        =  reinterpret_cast<func_set_on_user_message_fn>(get_network_function("bambu_network_set_on_user_message_fn"));
    set_on_local_connect_fn_ptr       =  reinterpret_cast<func_set_on_local_connect_fn>(get_network_function("bambu_network_set_on_local_connect_fn"));
    set_on_local_message_fn_ptr       =  reinterpret_cast<func_set_on_local_message_fn>(get_network_function("bambu_network_set_on_local_message_fn"));
    set_queue_on_main_fn_ptr          = reinterpret_cast<func_set_queue_on_main_fn>(get_network_function("bambu_network_set_queue_on_main_fn"));
    connect_server_ptr                =  reinterpret_cast<func_connect_server>(get_network_function("bambu_network_connect_server"));
    is_server_connected_ptr           =  reinterpret_cast<func_is_server_connected>(get_network_function("bambu_network_is_server_connected"));
    refresh_connection_ptr            =  reinterpret_cast<func_refresh_connection>(get_network_function("bambu_network_refresh_connection"));
    start_subscribe_ptr               =  reinterpret_cast<func_start_subscribe>(get_network_function("bambu_network_start_subscribe"));
    stop_subscribe_ptr                =  reinterpret_cast<func_stop_subscribe>(get_network_function("bambu_network_stop_subscribe"));
    add_subscribe_ptr                 =  reinterpret_cast<func_add_subscribe>(get_network_function("bambu_network_add_subscribe"));
    del_subscribe_ptr                 =  reinterpret_cast<func_del_subscribe>(get_network_function("bambu_network_del_subscribe"));
    enable_multi_machine_ptr          =  reinterpret_cast<func_enable_multi_machine>(get_network_function("bambu_network_enable_multi_machine"));
    send_message_ptr                  =  reinterpret_cast<func_send_message>(get_network_function("bambu_network_send_message"));
    connect_printer_ptr               =  reinterpret_cast<func_connect_printer>(get_network_function("bambu_network_connect_printer"));
    disconnect_printer_ptr            =  reinterpret_cast<func_disconnect_printer>(get_network_function("bambu_network_disconnect_printer"));
    send_message_to_printer_ptr       =  reinterpret_cast<func_send_message_to_printer>(get_network_function("bambu_network_send_message_to_printer"));
    check_cert_ptr                    =  reinterpret_cast<func_check_cert>(get_network_function("bambu_network_update_cert"));
    install_device_cert_ptr           =  reinterpret_cast<func_install_device_cert>(get_network_function("bambu_network_install_device_cert"));
    start_discovery_ptr               =  reinterpret_cast<func_start_discovery>(get_network_function("bambu_network_start_discovery"));
    change_user_ptr                   =  reinterpret_cast<func_change_user>(get_network_function("bambu_network_change_user"));
    is_user_login_ptr                 =  reinterpret_cast<func_is_user_login>(get_network_function("bambu_network_is_user_login"));
    user_logout_ptr                   =  reinterpret_cast<func_user_logout>(get_network_function("bambu_network_user_logout"));
    get_user_id_ptr                   =  reinterpret_cast<func_get_user_id>(get_network_function("bambu_network_get_user_id"));
    get_user_name_ptr                 =  reinterpret_cast<func_get_user_name>(get_network_function("bambu_network_get_user_name"));
    get_user_avatar_ptr               =  reinterpret_cast<func_get_user_avatar>(get_network_function("bambu_network_get_user_avatar"));
    get_user_nickanme_ptr             =  reinterpret_cast<func_get_user_nickanme>(get_network_function("bambu_network_get_user_nickanme"));
    build_login_cmd_ptr               =  reinterpret_cast<func_build_login_cmd>(get_network_function("bambu_network_build_login_cmd"));
    build_logout_cmd_ptr              =  reinterpret_cast<func_build_logout_cmd>(get_network_function("bambu_network_build_logout_cmd"));
    build_login_info_ptr              =  reinterpret_cast<func_build_login_info>(get_network_function("bambu_network_build_login_info"));
    ping_bind_ptr                     =  reinterpret_cast<func_ping_bind>(get_network_function("bambu_network_ping_bind"));
    bind_detect_ptr                   =  reinterpret_cast<func_bind_detect>(get_network_function("bambu_network_bind_detect"));
    report_consent_ptr                =  reinterpret_cast<func_report_consent>(get_network_function("bambu_network_report_consent"));
    set_server_callback_ptr           =  reinterpret_cast<func_set_server_callback>(get_network_function("bambu_network_set_server_callback"));
    bind_ptr                          =  reinterpret_cast<func_bind>(get_network_function("bambu_network_bind"));
    unbind_ptr                        =  reinterpret_cast<func_unbind>(get_network_function("bambu_network_unbind"));
    get_bambulab_host_ptr             =  reinterpret_cast<func_get_bambulab_host>(get_network_function("bambu_network_get_bambulab_host"));
    get_user_selected_machine_ptr     =  reinterpret_cast<func_get_user_selected_machine>(get_network_function("bambu_network_get_user_selected_machine"));
    set_user_selected_machine_ptr     =  reinterpret_cast<func_set_user_selected_machine>(get_network_function("bambu_network_set_user_selected_machine"));
    start_print_ptr                   =  reinterpret_cast<func_start_print>(get_network_function("bambu_network_start_print"));
    start_local_print_with_record_ptr =  reinterpret_cast<func_start_local_print_with_record>(get_network_function("bambu_network_start_local_print_with_record"));
    start_send_gcode_to_sdcard_ptr    =  reinterpret_cast<func_start_send_gcode_to_sdcard>(get_network_function("bambu_network_start_send_gcode_to_sdcard"));
    start_local_print_ptr             =  reinterpret_cast<func_start_local_print>(get_network_function("bambu_network_start_local_print"));
    start_sdcard_print_ptr            =  reinterpret_cast<func_start_sdcard_print>(get_network_function("bambu_network_start_sdcard_print"));
    get_user_presets_ptr              =  reinterpret_cast<func_get_user_presets>(get_network_function("bambu_network_get_user_presets"));
    request_setting_id_ptr            =  reinterpret_cast<func_request_setting_id>(get_network_function("bambu_network_request_setting_id"));
    put_setting_ptr                   =  reinterpret_cast<func_put_setting>(get_network_function("bambu_network_put_setting"));
    get_setting_list_ptr              = reinterpret_cast<func_get_setting_list>(get_network_function("bambu_network_get_setting_list"));
    get_setting_list2_ptr             = reinterpret_cast<func_get_setting_list2>(get_network_function("bambu_network_get_setting_list2"));
    delete_setting_ptr                =  reinterpret_cast<func_delete_setting>(get_network_function("bambu_network_delete_setting"));
    get_studio_info_url_ptr           =  reinterpret_cast<func_get_studio_info_url>(get_network_function("bambu_network_get_studio_info_url"));
    set_extra_http_header_ptr         =  reinterpret_cast<func_set_extra_http_header>(get_network_function("bambu_network_set_extra_http_header"));
    get_my_message_ptr                =  reinterpret_cast<func_get_my_message>(get_network_function("bambu_network_get_my_message"));
    check_user_task_report_ptr        =  reinterpret_cast<func_check_user_task_report>(get_network_function("bambu_network_check_user_task_report"));
    get_user_print_info_ptr           =  reinterpret_cast<func_get_user_print_info>(get_network_function("bambu_network_get_user_print_info"));
    get_user_tasks_ptr                =  reinterpret_cast<func_get_user_tasks>(get_network_function("bambu_network_get_user_tasks"));
    get_filament_spools_ptr           =  reinterpret_cast<func_get_filament_spools>(get_network_function("bambu_network_get_filament_spools"));
    create_filament_spool_ptr         =  reinterpret_cast<func_create_filament_spool>(get_network_function("bambu_network_create_filament_spool"));
    update_filament_spool_ptr         =  reinterpret_cast<func_update_filament_spool>(get_network_function("bambu_network_update_filament_spool"));
    delete_filament_spools_ptr        =  reinterpret_cast<func_delete_filament_spools>(get_network_function("bambu_network_delete_filament_spools"));
    get_filament_config_ptr           =  reinterpret_cast<func_get_filament_config>(get_network_function("bambu_network_get_filament_config"));
    get_printer_firmware_ptr          =  reinterpret_cast<func_get_printer_firmware>(get_network_function("bambu_network_get_printer_firmware"));
    get_task_plate_index_ptr          =  reinterpret_cast<func_get_task_plate_index>(get_network_function("bambu_network_get_task_plate_index"));
    get_user_info_ptr                 =  reinterpret_cast<func_get_user_info>(get_network_function("bambu_network_get_user_info"));
    request_bind_ticket_ptr           =  reinterpret_cast<func_request_bind_ticket>(get_network_function("bambu_network_request_bind_ticket"));
    get_subtask_info_ptr              =  reinterpret_cast<func_get_subtask_info>(get_network_function("bambu_network_get_subtask_info"));
    get_slice_info_ptr                =  reinterpret_cast<func_get_slice_info>(get_network_function("bambu_network_get_slice_info"));
    query_bind_status_ptr             =  reinterpret_cast<func_query_bind_status>(get_network_function("bambu_network_query_bind_status"));
    modify_printer_name_ptr           =  reinterpret_cast<func_modify_printer_name>(get_network_function("bambu_network_modify_printer_name"));
    get_camera_url_ptr                =  reinterpret_cast<func_get_camera_url>(get_network_function("bambu_network_get_camera_url"));
    get_camera_url_for_golive_ptr     =  reinterpret_cast<func_get_camera_url_for_golive>(get_network_function("bambu_network_get_camera_url_for_golive"));
    get_design_staffpick_ptr          =  reinterpret_cast<func_get_design_staffpick>(get_network_function("bambu_network_get_design_staffpick"));
    start_publish_ptr                 =  reinterpret_cast<func_start_pubilsh>(get_network_function("bambu_network_start_publish"));
    get_model_publish_url_ptr         =  reinterpret_cast<func_get_model_publish_url>(get_network_function("bambu_network_get_model_publish_url"));
    get_subtask_ptr                   =  reinterpret_cast<func_get_subtask>(get_network_function("bambu_network_get_subtask"));
    get_model_mall_home_url_ptr       =  reinterpret_cast<func_get_model_mall_home_url>(get_network_function("bambu_network_get_model_mall_home_url"));
    get_model_mall_detail_url_ptr     =  reinterpret_cast<func_get_model_mall_detail_url>(get_network_function("bambu_network_get_model_mall_detail_url"));
    get_my_profile_ptr                =  reinterpret_cast<func_get_my_profile>(get_network_function("bambu_network_get_my_profile"));
    get_my_token_ptr                  =  reinterpret_cast<func_get_my_profile>(get_network_function("bambu_network_get_my_token"));
    track_enable_ptr                  =  reinterpret_cast<func_track_enable>(get_network_function("bambu_network_track_enable"));
    track_remove_files_ptr            =  reinterpret_cast<func_track_remove_files>(get_network_function("bambu_network_track_remove_files"));
    track_event_ptr                   =  reinterpret_cast<func_track_event>(get_network_function("bambu_network_track_event"));
    track_header_ptr                  =  reinterpret_cast<func_track_header>(get_network_function("bambu_network_track_header"));
    track_update_property_ptr         = reinterpret_cast<func_track_update_property>(get_network_function("bambu_network_track_update_property"));
    track_get_property_ptr            = reinterpret_cast<func_track_get_property>(get_network_function("bambu_network_track_get_property"));
    put_model_mall_rating_url_ptr     = reinterpret_cast<func_put_model_mall_rating_url>(get_network_function("bambu_network_put_model_mall_rating"));
    get_oss_config_ptr                = reinterpret_cast<func_get_oss_config>(get_network_function("bambu_network_get_oss_config"));
    put_rating_picture_oss_ptr        = reinterpret_cast<func_put_rating_picture_oss>(get_network_function("bambu_network_put_rating_picture_oss"));
    get_model_mall_rating_result_ptr  = reinterpret_cast<func_get_model_mall_rating_result>(get_network_function("bambu_network_get_model_mall_rating"));

    get_mw_user_preference_ptr = reinterpret_cast<func_get_mw_user_preference>(get_network_function("bambu_network_get_mw_user_preference"));
    get_mw_user_4ulist_ptr     = reinterpret_cast<func_get_mw_user_4ulist>(get_network_function("bambu_network_get_mw_user_4ulist"));
    get_hms_snapshot_ptr              = reinterpret_cast<func_get_hms_snapshot>(get_network_function("bambu_network_get_hms_snapshot"));

#if defined(BAMBU_BRIDGE_HARNESS_ENABLE)
    // Harness ShimRecorder: replace the 10 function pointers listed in
    // test_harness_plan.md §7 with recording trampolines. The wrap is
    // a no-op until BAMBU_BRIDGE_SHIM env var is set + ShimRecorder
    // is enabled — see bb_harness_wrap_network_agent_pointers below.
    bb_harness_wrap_network_agent_pointers();
#endif

    return 0;
}

int NetworkAgent::unload_network_module()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", network module %1%")%networking_module;
    UnloadFTModule();
#if defined(_MSC_VER) || defined(_WIN32)
    if (networking_module) {
        FreeLibrary(networking_module);
        networking_module = NULL;
    }
    if (source_module) {
        FreeLibrary(source_module);
        source_module = NULL;
    }
#else
    if (networking_module) {
        dlclose(networking_module);
        networking_module = NULL;
    }
    if (source_module) {
        dlclose(source_module);
        source_module = NULL;
    }
#endif

    check_debug_consistent_ptr        =  nullptr;
    get_version_ptr                   =  nullptr;
    create_agent_ptr                  =  nullptr;
    destroy_agent_ptr                 =  nullptr;
    init_log_ptr                      =  nullptr;
    set_config_dir_ptr                =  nullptr;
    set_cert_file_ptr                 =  nullptr;
    set_country_code_ptr              =  nullptr;
    start_ptr                         =  nullptr;
    set_on_ssdp_msg_fn_ptr            =  nullptr;
    set_on_user_login_fn_ptr          =  nullptr;
    set_on_printer_connected_fn_ptr   =  nullptr;
    set_on_server_connected_fn_ptr    =  nullptr;
    set_on_http_error_fn_ptr          =  nullptr;
    set_get_country_code_fn_ptr       =  nullptr;
    set_on_subscribe_failure_fn_ptr   =  nullptr;
    set_on_message_fn_ptr             =  nullptr;
    set_on_user_message_fn_ptr        =  nullptr;
    set_on_local_connect_fn_ptr       =  nullptr;
    set_on_local_message_fn_ptr       =  nullptr;
    set_queue_on_main_fn_ptr          = nullptr;
    connect_server_ptr                =  nullptr;
    is_server_connected_ptr           =  nullptr;
    refresh_connection_ptr            =  nullptr;
    start_subscribe_ptr               =  nullptr;
    stop_subscribe_ptr                =  nullptr;
    send_message_ptr                  =  nullptr;
    connect_printer_ptr               =  nullptr;
    disconnect_printer_ptr            =  nullptr;
    send_message_to_printer_ptr       =  nullptr;
    check_cert_ptr                    =  nullptr;
    start_discovery_ptr               =  nullptr;
    change_user_ptr                   =  nullptr;
    is_user_login_ptr                 =  nullptr;
    user_logout_ptr                   =  nullptr;
    get_user_id_ptr                   =  nullptr;
    get_user_name_ptr                 =  nullptr;
    get_user_avatar_ptr               =  nullptr;
    get_user_nickanme_ptr             =  nullptr;
    build_login_cmd_ptr               =  nullptr;
    build_logout_cmd_ptr              =  nullptr;
    build_login_info_ptr              =  nullptr;
    ping_bind_ptr                     =  nullptr;
    bind_ptr                          =  nullptr;
    unbind_ptr                        =  nullptr;
    get_bambulab_host_ptr             =  nullptr;
    get_user_selected_machine_ptr     =  nullptr;
    set_user_selected_machine_ptr     =  nullptr;
    start_print_ptr                   =  nullptr;
    start_local_print_with_record_ptr =  nullptr;
    start_send_gcode_to_sdcard_ptr    =  nullptr;
    start_local_print_ptr             =  nullptr;
    start_sdcard_print_ptr             =  nullptr;
    get_user_presets_ptr              =  nullptr;
    request_setting_id_ptr            =  nullptr;
    put_setting_ptr                   =  nullptr;
    get_setting_list_ptr              =  nullptr;
    get_setting_list2_ptr             =  nullptr;
    delete_setting_ptr                =  nullptr;
    get_studio_info_url_ptr           =  nullptr;
    set_extra_http_header_ptr         =  nullptr;
    get_my_message_ptr                =  nullptr;
    check_user_task_report_ptr        =  nullptr;
    get_user_print_info_ptr           =  nullptr;
    get_user_tasks_ptr                =  nullptr;
    get_filament_spools_ptr           =  nullptr;
    create_filament_spool_ptr         =  nullptr;
    update_filament_spool_ptr         =  nullptr;
    delete_filament_spools_ptr        =  nullptr;
    get_filament_config_ptr           =  nullptr;
    get_printer_firmware_ptr          =  nullptr;
    get_task_plate_index_ptr          =  nullptr;
    get_user_info_ptr                 =  nullptr;
    get_subtask_info_ptr              =  nullptr;
    get_slice_info_ptr                =  nullptr;
    query_bind_status_ptr             =  nullptr;
    modify_printer_name_ptr           =  nullptr;
    get_camera_url_ptr                =  nullptr;
    get_camera_url_for_golive_ptr     =  nullptr;
    get_design_staffpick_ptr          =  nullptr;
    start_publish_ptr                 =  nullptr;
    get_model_publish_url_ptr         =  nullptr;
    get_subtask_ptr                   =  nullptr;
    get_model_mall_home_url_ptr       =  nullptr;
    get_model_mall_detail_url_ptr     =  nullptr;
    get_my_profile_ptr                =  nullptr;
    get_my_token_ptr                  =  nullptr;
    track_enable_ptr                  =  nullptr;
    track_remove_files_ptr            =  nullptr;
    track_event_ptr                   =  nullptr;
    track_header_ptr                  =  nullptr;
    track_update_property_ptr         =  nullptr;
    track_get_property_ptr            =  nullptr;
    get_oss_config_ptr                =  nullptr;
    put_rating_picture_oss_ptr        =  nullptr;
    put_model_mall_rating_url_ptr     =  nullptr;
    get_model_mall_rating_result_ptr  = nullptr;

    get_mw_user_preference_ptr        = nullptr;
    get_mw_user_4ulist_ptr            = nullptr;

    return 0;
}

#if defined(_MSC_VER) || defined(_WIN32)
HMODULE NetworkAgent::get_bambu_source_entry()
#else
void* NetworkAgent::get_bambu_source_entry()
#endif
{
    if ((source_module) || (!networking_module))
        return source_module;

    //int ret = -1;
    std::string library;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";
#if defined(_MSC_VER) || defined(_WIN32)
    wchar_t lib_wstr[128];

    //goto load bambu source
    library = plugin_folder.string() + "/" + std::string(BAMBU_SOURCE_LIBRARY) + ".dll";
    memset(lib_wstr, 0, sizeof(lib_wstr));
    ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
    source_module = LoadLibrary(lib_wstr);
    if (!source_module) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", try load BambuSource directly from current directory");
        std::string library_path = get_libpath_in_current_directory(std::string(BAMBU_SOURCE_LIBRARY));
        if (library_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", can not get path in current directory for %1%") % BAMBU_SOURCE_LIBRARY;
            return source_module;
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", line %1%")%__LINE__;
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library_path.c_str(), strlen(library_path.c_str()) + 1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        source_module = LoadLibrary(lib_wstr);
    }
#else
#if defined(__WXMAC__)
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_SOURCE_LIBRARY) + ".dylib";
#else
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_SOURCE_LIBRARY) + ".so";
#endif
    source_module = dlopen( library.c_str(), RTLD_LAZY);
    /*if (!source_module) {
#if defined(__WXMAC__)
        library = std::string("lib") + BAMBU_SOURCE_LIBRARY + ".dylib";
#else
        library = std::string("lib") + BAMBU_SOURCE_LIBRARY + ".so";
#endif
        source_module = dlopen( library.c_str(), RTLD_LAZY);
    }*/
#endif

    return source_module;
}

void* NetworkAgent::get_network_function(const char* name)
{
    void* function = nullptr;

    if (!networking_module)
        return function;

#if defined(_MSC_VER) || defined(_WIN32)
    function = GetProcAddress(networking_module, name);
#else
    function = dlsym(networking_module, name);
#endif

    if (!function) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", can not find function %1%")%name;
    }
    return function;
}

std::string NetworkAgent::get_version()
{
    bool consistent = true;
    //check the debug consistent first
    if (check_debug_consistent_ptr) {
#if defined(NDEBUG)
        consistent = check_debug_consistent_ptr(false);
#else
        consistent = check_debug_consistent_ptr(true);
#endif
    }
    if (!consistent) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", inconsistent library,return 00.00.00.00!");
        return "00.00.00.00";
    }
    if (get_version_ptr) {
        return get_version_ptr();
    }
    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", get_version not supported,return 00.00.00.00!");
    return "00.00.00.00";
}

int NetworkAgent::init_log()
{
    int ret = 0;
    if (network_agent && init_log_ptr) {
        ret = init_log_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_config_dir(std::string config_dir)
{
    int ret = 0;
    if (network_agent && set_config_dir_ptr) {
        ret = set_config_dir_ptr(network_agent, config_dir);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, config_dir=%3%")%network_agent %ret %config_dir ;
    }
    return ret;
}

int NetworkAgent::set_cert_file(std::string folder, std::string filename)
{
    int ret = 0;
    if (network_agent && set_cert_file_ptr) {
        ret = set_cert_file_ptr(network_agent, folder, filename);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, folder=%3%, filename=%4%")%network_agent %ret %folder %filename;
    }
    return ret;
}

int NetworkAgent::set_country_code(std::string country_code)
{
    int ret = 0;
    if (network_agent && set_country_code_ptr) {
        ret = set_country_code_ptr(network_agent, country_code);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, country_code=%3%")%network_agent %ret %country_code ;
    }
    return ret;
}

int NetworkAgent::start()
{
    int ret = 0;
    if (network_agent && start_ptr) {
        ret = start_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    int ret = 0;
    if (network_agent && set_on_ssdp_msg_fn_ptr) {
        log_event("set_on_ssdp_msg_fn register");
        dump_stack("set_on_ssdp_msg_fn");
        ret = set_on_ssdp_msg_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_user_login_fn(OnUserLoginFn fn)
{
    int ret = 0;
    if (network_agent && set_on_user_login_fn_ptr) {
        log_event("set_on_user_login_fn register");
        dump_stack("set_on_user_login_fn");
        ret = set_on_user_login_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    int ret = 0;
    if (network_agent && set_on_printer_connected_fn_ptr) {
        log_event("set_on_printer_connected_fn register");
        dump_stack("set_on_printer_connected_fn");
        ret = set_on_printer_connected_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_server_connected_fn(OnServerConnectedFn fn)
{
    int ret = 0;
    if (network_agent && set_on_server_connected_fn_ptr) {
        log_event("set_on_server_connected_fn register");
        dump_stack("set_on_server_connected_fn");
        ret = set_on_server_connected_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_http_error_fn(OnHttpErrorFn fn)
{
    int ret = 0;
    if (network_agent && set_on_http_error_fn_ptr) {
        log_event("set_on_http_error_fn register");
        dump_stack("set_on_http_error_fn");
        ret = set_on_http_error_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    int ret = 0;
    if (network_agent && set_get_country_code_fn_ptr) {
        log_event("set_get_country_code_fn register");
        dump_stack("set_get_country_code_fn");
        ret = set_get_country_code_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    int ret = 0;
    if (network_agent && set_on_subscribe_failure_fn_ptr) {
        log_event("set_on_subscribe_failure_fn register");
        dump_stack("set_on_subscribe_failure_fn");
        ret = set_on_subscribe_failure_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::set_on_message_fn(OnMessageFn fn)
{
    int ret = 0;
    if (network_agent && set_on_message_fn_ptr) {
        log_event("set_on_message_fn register (cloud-side OnMessageFn; wrapped via bridge_hooks)");
        dump_stack("set_on_message_fn");
        ret = set_on_message_fn_ptr(network_agent,
            bridge_hooks::Dispatcher::make_on_message_wrapper(this, fn));
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_user_message_fn(OnMessageFn fn)
{
    int ret = 0;
    if (network_agent && set_on_user_message_fn_ptr) {
        log_event("set_on_user_message_fn register");
        dump_stack("set_on_user_message_fn");
        ret = set_on_user_message_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    bridge_hooks::Dispatcher::capture_local_connect_cb(this, fn);
    int ret = 0;
    if (network_agent && set_on_local_connect_fn_ptr) {
        log_event("set_on_local_connect_fn register (LAN session up/down; wrapped via bridge_hooks)");
        dump_stack("set_on_local_connect_fn");
        ret = set_on_local_connect_fn_ptr(network_agent,
            bridge_hooks::Dispatcher::make_on_local_connect_wrapper(fn));
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_local_message_fn(OnMessageFn fn)
{
    bridge_hooks::Dispatcher::capture_local_message_cb(this, fn);
    int ret = 0;
    if (network_agent && set_on_local_message_fn_ptr) {
        log_event("set_on_local_message_fn register (LAN-side OnMessageFn; wrapped via bridge_hooks)");
        dump_stack("set_on_local_message_fn");
        ret = set_on_local_message_fn_ptr(network_agent,
            bridge_hooks::Dispatcher::make_on_local_message_wrapper(this, fn));
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

void NetworkAgent::set_bridge_message_tap(BridgeMessageTap tap)
{
    bridge_hooks::Dispatcher::set_bridge_message_tap(this, std::move(tap));
}

int NetworkAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    int ret = 0;
    if (network_agent && set_queue_on_main_fn_ptr) {
        ret = set_queue_on_main_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::connect_server()
{
    int ret = 0;
    if (network_agent && connect_server_ptr) {
        log_event("connect_server");
        ret = connect_server_ptr(network_agent);
        log_event("connect_server ret=%d", ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

bool NetworkAgent::is_server_connected()
{
    bool ret = false;
    if (network_agent && is_server_connected_ptr) {
        ret = is_server_connected_ptr(network_agent);
        //BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::refresh_connection()
{
    int ret = 0;
    if (network_agent && refresh_connection_ptr) {
        log_event("refresh_connection");
        ret = refresh_connection_ptr(network_agent);
        log_event("refresh_connection ret=%d", ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::start_subscribe(std::string module)
{
    int ret = 0;
    if (network_agent && start_subscribe_ptr) {
        log_event("start_subscribe module=%s", module.c_str());
        ret = start_subscribe_ptr(network_agent, module);
        log_event("start_subscribe module=%s ret=%d", module.c_str(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, module=%3%")%network_agent %ret %module ;
    }
    return ret;
}

int NetworkAgent::stop_subscribe(std::string module)
{
    int ret = 0;
    if (network_agent && stop_subscribe_ptr) {
        log_event("stop_subscribe module=%s", module.c_str());
        ret = stop_subscribe_ptr(network_agent, module);
        log_event("stop_subscribe module=%s ret=%d", module.c_str(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, module=%3%")%network_agent %ret %module ;
    }
    return ret;
}

int NetworkAgent::add_subscribe(std::vector<std::string> dev_list)
{
    int ret = 0;
    if (network_agent && add_subscribe_ptr) {
        if (Slic3r::plugin_trace::enabled()) {
            std::string ids; for (auto& d : dev_list) { if (!ids.empty()) ids += ","; ids += d; }
            log_event("add_subscribe n=%zu ids=%s", dev_list.size(), ids.c_str());
        }
        ret = add_subscribe_ptr(network_agent, dev_list);
        log_event("add_subscribe n=%zu ret=%d", dev_list.size(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") %network_agent %ret;
    }
    return ret;
}

int NetworkAgent::del_subscribe(std::vector<std::string> dev_list)
{
    int ret = 0;
    if (network_agent && del_subscribe_ptr) {
        if (Slic3r::plugin_trace::enabled()) {
            std::string ids; for (auto& d : dev_list) { if (!ids.empty()) ids += ","; ids += d; }
            log_event("del_subscribe n=%zu ids=%s", dev_list.size(), ids.c_str());
        }
        ret = del_subscribe_ptr(network_agent, dev_list);
        log_event("del_subscribe n=%zu ret=%d", dev_list.size(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") %network_agent %ret;
    }
    return ret;
}

void NetworkAgent::enable_multi_machine(bool enable)
{
    if (network_agent && enable_multi_machine_ptr) {
        enable_multi_machine_ptr(network_agent, enable);
    }
}

int NetworkAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    int ret = 0;
    if (network_agent && send_message_ptr) {
        log_event("send_message CLOUD dev_id=%s qos=%d flag=%d bytes=%zu payload=%s",
            dev_id.c_str(), qos, flag, json_str.size(), truncate(json_str).c_str());
        ret = send_message_ptr(network_agent, dev_id, json_str, qos, flag);
        log_event("send_message CLOUD dev_id=%s ret=%d", dev_id.c_str(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ <<
            boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%, json_str=%4%, qos=%5%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(dev_id) %json_str %qos;
    }
    return ret;
}

int NetworkAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    int rc = 0;
    if (bridge_hooks::Dispatcher::try_connect_printer(
            this, dev_id, dev_ip, username, password, &rc))
        return rc;
    int ret = 0;
    if (network_agent && connect_printer_ptr) {
        log_event("connect_printer dev_id=%s dev_ip=%s username=%s use_ssl=%d",
            dev_id.c_str(), dev_ip.c_str(), username.c_str(), int(use_ssl));
        ret = connect_printer_ptr(network_agent, dev_id, dev_ip, username, password, use_ssl);
        log_event("connect_printer dev_id=%s ret=%d", dev_id.c_str(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ <<
            (boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%, dev_ip=%4%, username=%5%, password=%6%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(dev_id) %BBLCrossTalk::Crosstalk_DevIP(dev_ip) %username %password).str();
        else
            bridge_hooks::Dispatcher::note_plugin_connect_success(this, dev_id);
    }
    return ret;
}

int NetworkAgent::disconnect_printer()
{
    int rc = 0;
    if (bridge_hooks::Dispatcher::try_disconnect_printer(this, &rc))
        return rc;
    int ret = 0;
    if (network_agent && disconnect_printer_ptr) {
        log_event("disconnect_printer");
        ret = disconnect_printer_ptr(network_agent);
        log_event("disconnect_printer ret=%d", ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") %network_agent %ret;
        else
            bridge_hooks::Dispatcher::note_plugin_disconnect_success(this);
    }
    return ret;
}

int NetworkAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    int rc = 0;
    if (bridge_hooks::Dispatcher::try_send_message_to_printer(
            dev_id, json_str, qos, &rc))
        return rc;
    int ret = 0;
    if (network_agent && send_message_to_printer_ptr) {
        log_event("send_message_to_printer LAN dev_id=%s qos=%d flag=%d bytes=%zu payload=%s",
            dev_id.c_str(), qos, flag, json_str.size(), truncate(json_str).c_str());
        ret = send_message_to_printer_ptr(network_agent, dev_id, json_str, qos, flag);
        log_event("send_message_to_printer LAN dev_id=%s ret=%d", dev_id.c_str(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%, json_str=%4%, qos=%5%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(dev_id) %json_str %qos;
    }
    return ret;
}

int NetworkAgent::check_cert()
{
    int ret = 0;
    if (network_agent && check_cert_ptr) {
        ret = check_cert_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") %network_agent %ret;
    }
    return ret;
}

void NetworkAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    if (network_agent && install_device_cert_ptr) {
        log_event("install_device_cert dev_id=%s lan_only=%d",
            dev_id.c_str(), int(lan_only));
        install_device_cert_ptr(network_agent, dev_id, lan_only);
    }
}

bool NetworkAgent::start_discovery(bool start, bool sending)
{
    bool ret = false;
    if (network_agent && start_discovery_ptr) {
        ret = start_discovery_ptr(network_agent, start, sending);
        //BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, start=%3%, sending=%4%")%network_agent %ret %start %sending;
    }
    return ret;
}

int  NetworkAgent::change_user(std::string user_info)
{
    int ret = 0;
    if (network_agent && change_user_ptr) {
        ret = change_user_ptr(network_agent, user_info);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") %network_agent %ret ;
    }
    return ret;
}

bool NetworkAgent::is_user_login()
{
    bool ret = false;
    if (network_agent && is_user_login_ptr) {
        ret = is_user_login_ptr(network_agent);
    }
    return ret;
}

int  NetworkAgent::user_logout(bool request)
{
    int ret = 0;
    if (network_agent && user_logout_ptr) {
        ret = user_logout_ptr(network_agent, request);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") %network_agent %ret;
    }
    return ret;
}

std::string NetworkAgent::get_user_id()
{
    std::string ret;
    if (network_agent && get_user_id_ptr) {
        ret = get_user_id_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::get_user_name()
{
    std::string ret;
    if (network_agent && get_user_name_ptr) {
        ret = get_user_name_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::get_user_avatar()
{
    std::string ret;
    if (network_agent && get_user_avatar_ptr) {
        ret = get_user_avatar_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::get_user_nickanme()
{
    std::string ret;
    if (network_agent && get_user_nickanme_ptr) {
        ret = get_user_nickanme_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::build_login_cmd()
{
    std::string ret;
    if (network_agent && build_login_cmd_ptr) {
        ret = build_login_cmd_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::build_logout_cmd()
{
    std::string ret;
    if (network_agent && build_logout_cmd_ptr) {
        ret = build_logout_cmd_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::build_login_info()
{
    std::string ret;
    if (network_agent && build_login_info_ptr) {
        ret = build_login_info_ptr(network_agent);
    }
    return ret;
}

int NetworkAgent::ping_bind(std::string ping_code)
{
    int ret = 0;
    if (network_agent && ping_bind_ptr) {
        ret = ping_bind_ptr(network_agent, ping_code);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")
            % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    int ret = 0;
    if (network_agent && bind_detect_ptr) {
        ret = bind_detect_ptr(network_agent, dev_ip, sec_link, detect);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_ip=%3%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevIP(dev_ip);
    }
    return ret;
}

int NetworkAgent::report_consent(std::string expand)
{
    int ret = 0;
    if (network_agent && report_consent_ptr) {
        ret = report_consent_ptr(network_agent, expand);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::set_server_callback(OnServerErrFn fn)
{
    int ret = 0;
    if (network_agent && set_server_callback_ptr) {
        ret = set_server_callback_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")
            % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone,  bool improved, OnUpdateStatusFn update_fn)
{
    int ret = 0;
    if (network_agent && bind_ptr) {
        ret = bind_ptr(network_agent, dev_ip, dev_id, sec_link, timezone, improved, update_fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_ip=%3%, timezone=%4%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevIP(dev_ip) %timezone;
    }
    return ret;
}

int NetworkAgent::unbind(std::string dev_id)
{
    int ret = 0;
    if (network_agent && unbind_ptr) {
        ret = unbind_ptr(network_agent, dev_id);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") %network_agent %ret ;
    }
    return ret;
}

std::string NetworkAgent::get_bambulab_host()
{
    std::string ret;
    if (network_agent && get_bambulab_host_ptr) {
        ret = get_bambulab_host_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::get_user_selected_machine()
{
    std::string ret;
    if (network_agent && get_user_selected_machine_ptr) {
        ret = get_user_selected_machine_ptr(network_agent);
    }
    return ret;
}

int NetworkAgent::set_user_selected_machine(std::string dev_id)
{
    int ret = 0;
    if (network_agent && set_user_selected_machine_ptr) {
        log_event("set_user_selected_machine dev_id=%s", dev_id.c_str());
        ret = set_user_selected_machine_ptr(network_agent, dev_id);
        log_event("set_user_selected_machine dev_id=%s ret=%d", dev_id.c_str(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, user_info=%3%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(dev_id);
    }
    return ret;
}

// ============================================================================
// Virtual-LAN print path — FFFF dev_id detection short-circuit.
//
// Ported from OrcaSlicer-bridge's NetworkAgent.cpp (the same helpers Orca
// uses to route FFFF prints through VirtualFtpsClient/VirtualMqttClient
// instead of through the plugin's printer-IP:990 path). Without these,
// BambuStudio-bridge sends 3mf uploads to <printer_ip>:990 — but for FFFF
// dev_ids that's the BRIDGE's IP, and the bridge listens on the offset
// port 39990 + N. The user sees "Failed to connect to 192.168.1.151:990".
//
// See docs/plugin-trace/H2D-{cloud,lan}.yaml § threemf_payload for the
// canonical gcode_file MQTT payload shape these helpers reproduce.
// ============================================================================


// Mirror of BambuStudio OSS
// `bambu_net_oss/core/LocalPrintOrchestrator.cpp::compose_remote_path`.
// Used as the SHARED source of truth for both:
//   - the FTPS STOR remote name (what bytes go where on the printer's
//     SD card / the bridge's spool)
//   - the `print.param` field of the slicer's `gcode_file` MQTT command
//     (which is how the printer finds the file on disk to start the
//     print).
// These two MUST be identical character-for-character — the bridge's
// MqttBroker print interceptor keys its spool lookup on
// `print.param`'s basename. Any divergence and the interceptor falls
// back to verbatim-forwarding a command the printer can't resolve.
//
// We mirror the OSS chain exactly:
//   ftp_folder (default "/")
//   + ( dst_file
//       | ftp_file
//       | basename(filename)
//       | "lan_print.3mf" )
// The slicer's `project_name` is intentionally NOT included here — it
// rides separately on the MQTT command as `print.project_name`, and
// that's what the printer uses for its UI "currently printing" label.
// The filesystem path stays under the slicer's own naming convention
// (the dotted temp `.NNNNNN.0.3mf` BambuStudio writes), matching what
// stock direct prints do.
static std::string compose_remote_path(const BBL::PrintParams& p) {
    std::string folder = p.ftp_folder.empty() ? std::string("/") : p.ftp_folder;
    if (folder.empty() || folder.back() != '/') folder += '/';

    std::string fname = p.dst_file;
    if (fname.empty()) fname = p.ftp_file;
    if (fname.empty()) {
        try {
            fname = boost::filesystem::path(p.filename).filename().string();
        } catch (...) {}
    }
    if (fname.empty()) fname = "lan_print.3mf";
    if (!fname.empty() && fname.front() == '/') fname.erase(0, 1);
    return folder + fname;
}

int virtual_ftps_upload_(const PrintParams& params,
                         OnUpdateStatusFn  update_fn,
                         WasCancelledFn    cancel_fn) {
    BOOST_LOG_TRIVIAL(info)
        << "[ORCA-TRACE] virtual_ftps_upload_ entered "
        << " dev_id=" << params.dev_id
        << " dev_ip=" << params.dev_ip
        << " ftp_folder=" << params.ftp_folder
        << " ftp_file=" << params.ftp_file
        << " dst_file=" << params.dst_file
        << " filename=" << params.filename
        << " username=" << params.username
        << " pass_len=" << params.password.size();
    Slic3r::virtual_ftps::UploadParams up;
    up.host        = params.dev_ip;
    up.port        = Slic3r::VirtualSsdpDiscovery::port_for(params.dev_id, 39990, params.dev_ip);
    BOOST_LOG_TRIVIAL(info)
        << "[ORCA-TRACE] virtual_ftps_upload_ resolved "
        << " host=" << up.host << " port=" << up.port;
    up.user        = params.username.empty() ? std::string("bblp") : params.username;
    up.pass        = params.password;
    up.local_path  = params.filename;
    // Compose the FTPS STOR remote name via the shared helper. MUST
    // exactly match the `remote_path` virtual_lan_print_ embeds in the
    // slicer's MQTT `print.param` — see compose_remote_path's
    // doc-comment for the why.
    {
        std::string composed = compose_remote_path(params);
        // up.remote_name expects a basename (the FTPS protocol takes
        // the folder via CWD and the file via STOR <fname>); strip
        // the leading folder prefix.
        auto slash = composed.find_last_of('/');
        up.remote_name = (slash == std::string::npos)
                         ? composed : composed.substr(slash + 1);
    }
    Slic3r::virtual_ftps::ProgressFn  prog = nullptr;
    Slic3r::virtual_ftps::CancelledFn canc = nullptr;
    if (update_fn) prog = [update_fn](int pct, std::string msg){ update_fn(pct, 0, msg); };
    if (cancel_fn) canc = [cancel_fn]() -> bool { return cancel_fn(); };
    BOOST_LOG_TRIVIAL(info) << "virtual_ftps_upload: dev=" << params.dev_id
                            << " ftps_port=" << up.port
                            << " remote=" << up.remote_name;
    return Slic3r::virtual_ftps::upload(up, prog, canc);
}

int virtual_lan_print_(const PrintParams& params,
                       OnUpdateStatusFn  update_fn,
                       WasCancelledFn    cancel_fn) {
    // (Plates start at 1: slicer writes plate_1.gcode, slice_info.config
    // index=1, bridge passes through, printer opens plate_1.gcode. An
    // earlier `virtual_print_normalise_plate_to_zero` renamed
    // plate_<N>.* → plate_0.* but did NOT update slice_info.config,
    // creating a mismatch the printer rejected. Removed in both slicer
    // and bridge; see memory `project_orca_3mf_filament_settings`.)

    // Mirror the real-printer LAN flow: FTPS upload first, then publish
    // the `print.command=gcode_file` MQTT command. The MQTT command is
    // what tells the printer "open this file off your SD card and start
    // printing"; the bridge forwards it verbatim (FFFF→0948 payload
    // rewrite) and the real printer does the rest. The bridge does NOT
    // call any print-starting plugin function — it's a pass-through for
    // both the file (via its FTPS server → onward to the real printer)
    // and the command.
    int rc = virtual_ftps_upload_(params, update_fn, cancel_fn);
    if (rc != 0) {
        BOOST_LOG_TRIVIAL(warning) << "virtual_lan_print: FTPS upload failed rc=" << rc;
        return rc;
    }
    // Mirror BambuStudio OSS LocalPrintOrchestrator::compose_remote_path so
    // the path here is identical to the one virtual_ftps_upload_ just sent
    // as STOR remote_name. Both ends must agree or the printer can't find
    // the file the bridge stored.
    // Use the same shared helper as the FTPS leg — see compose_remote_path
    // for why it MUST be bit-identical on both legs (bridge MqttBroker
    // print interceptor keys its spool lookup on this).
    const std::string remote_path = compose_remote_path(params);
    static std::atomic<uint64_t> s_seq{1};
    const std::string seq = std::to_string(s_seq.fetch_add(1));

    // Build the gcode_file payload to mirror what BambuStudio's
    // proprietary plugin's `start_local_print` produces — captured in
    // docs/plugin-trace/H2D-lan.yaml (2026-05-30, the successful BBS LAN
    // print). The bare 3-field payload (BBS OSS reference) works for
    // some calibration files but not for AMS-equipped object prints,
    // because the printer has no way to know which AMS slot to draw
    // from. Adding the AMS map + bed-type + cali toggles brings the
    // wire shape into line with what the H2D firmware sees in cloud-
    // relay project_file ACKs.
    nlohmann::json j;
    j["print"]["command"]     = "gcode_file";
    j["print"]["param"]       = remote_path;
    j["print"]["sequence_id"] = seq;
    // AMS routing. Source-of-truth example: "[3,-1,-1,-1,-1,-1,-1,-1]"
    // (8-entry int array, -1 = unmapped). When PrintParams holds the
    // stringified form, embed verbatim; the printer parses the same
    // shape from project_file too.
    auto emit_json_or_string = [&](const char* key, const std::string& s) {
        if (s.empty()) return;
        try { j["print"][key] = nlohmann::json::parse(s); }
        catch (...) { j["print"][key] = s; }
    };
    emit_json_or_string("ams_mapping",      params.ams_mapping);
    emit_json_or_string("ams_mapping2",     params.ams_mapping2);
    emit_json_or_string("ams_mapping_info", params.ams_mapping_info);
    emit_json_or_string("nozzles_info",     params.nozzles_info);
    emit_json_or_string("nozzle_mapping",   params.nozzle_mapping);
    if (!params.task_bed_type.empty())
        j["print"]["task_bed_type"]    = params.task_bed_type;
    j["print"]["use_ams"]              = params.task_use_ams;
    j["print"]["task_use_ams"]         = params.task_use_ams;
    j["print"]["bed_leveling"]         = params.task_bed_leveling;
    j["print"]["flow_cali"]            = params.task_flow_cali;
    j["print"]["vibration_cali"]       = params.task_vibration_cali;
    j["print"]["layer_inspect"]        = params.task_layer_inspect;
    j["print"]["timelapse"]            = params.task_record_timelapse;
    j["print"]["auto_bed_leveling"]    = params.auto_bed_leveling;
    j["print"]["auto_flow_cali"]       = params.auto_flow_cali;
    j["print"]["auto_offset_cali"]     = params.auto_offset_cali;
    if (params.plate_index > 0)
        j["print"]["plate_idx"]        = std::to_string(params.plate_index);
    if (params.origin_profile_id > 0)
        j["print"]["profile_id"]       = std::to_string(params.origin_profile_id);
    if (!params.origin_model_id.empty())
        j["print"]["model_id"]         = params.origin_model_id;
    if (!params.project_name.empty())
        j["print"]["project_name"]     = params.project_name;
    if (!params.task_name.empty())
        j["print"]["task_name"]        = params.task_name;
    // try_emmc_print isn't observed as an MQTT field in any trace; it's a
    // slicer-side flag the plugin uses to pick the upload transport. Keep
    // out of the wire payload.

    const std::string cmd = j.dump();
    BOOST_LOG_TRIVIAL(info) << "virtual_lan_print: dev=" << params.dev_id
                            << " gcode_file " << remote_path
                            << " bytes=" << cmd.size()
                            << " ams_mapping=" << params.ams_mapping
                            << " use_ams=" << int(params.task_use_ams);
    int pubrc = Slic3r::VirtualMqttClient::instance().send_message(params.dev_id, cmd, /*qos=*/0);
    if (pubrc != 0) {
        BOOST_LOG_TRIVIAL(warning) << "virtual_lan_print: MQTT send rc=" << pubrc;
        return -1;
    }

    // SendJob (BambuStudio) and PrintJob (OrcaSlicer) BOTH fire a
    // leading `start_send_gcode_to_sdcard` with `project_name="verify_job"`
    // and a 16-byte probe body purely to test that the printer's FTPS
    // endpoint accepts the access code. NetworkAgent dispatches that
    // probe through this very same virtual_lan_print_ for FFFF dev_ids.
    // The bridge absorbs the probe at the FTPS layer (LanUploadSink
    // detects the 16-byte body and never spools it), so there's no
    // matching dispatch and no progress file will ever appear — if we
    // entered the wait loop we'd block the slicer for the full 120 s
    // timeout before it can even start the REAL upload.
    //
    // Skip the wait for the probe (project_name=="verify_job"); return
    // success immediately so SendJob/PrintJob proceeds to the real
    // upload, where the wait below DOES apply.
    if (params.project_name == "verify_job") {
        BOOST_LOG_TRIVIAL(info)
            << "virtual_lan_print: probe (project_name=verify_job) — "
               "skipping bridge-dispatch wait";
        return 0;
    }

    // Wait for the bridge to finish dispatching this print to the real
    // printer before returning. Without this, BBS's PrintJob considers
    // the print "sent" the moment the local-FTPS-to-bridge upload
    // finishes (a fraction of a second) and immediately redirects the
    // user to the Device tab while the bridge is still uploading to
    // the printer and waiting for the print to actually start
    // (10–20 s after).
    //
    // The bridge writes a JSON status file at
    //   /tmp/bridge-progress/<FFFF dev_id>.json
    // and updates it on every plugin update_fn callback. We poll the
    // file every 250 ms, mirror the (stage, code, info) into the
    // caller's update_fn so the slicer's "Sending…" dialog shows
    // progress, and return only when the bridge marks the upload
    // `done` (rc=0) or `failed`. Bounded by a 120-second timeout so
    // a totally hung bridge doesn't lock up BBS's print dialog.
    {
        const std::string progress_path =
            "/tmp/bridge-progress/" + params.dev_id + ".json";
        using namespace std::chrono;
        const auto t0       = steady_clock::now();
        const auto deadline = t0 + seconds(120);
        std::string last_phase;
        int         last_stage = -1, last_code = -1;
        std::string last_info;
        while (true) {
            if (cancel_fn && cancel_fn()) {
                BOOST_LOG_TRIVIAL(info)
                    << "virtual_lan_print: cancelled while waiting for "
                       "bridge dispatch";
                return -2;
            }
            if (steady_clock::now() > deadline) {
                BOOST_LOG_TRIVIAL(warning)
                    << "virtual_lan_print: bridge dispatch wait timed out "
                       "after 120s — returning success anyway so BBS "
                       "doesn't strand the user";
                return 0;
            }
            // Read + parse the progress file. Bridge writes atomically
            // (tmp + rename) so a partial read won't happen.
            std::ifstream f(progress_path);
            if (f) {
                std::stringstream ss; ss << f.rdbuf();
                try {
                    auto j2 = nlohmann::json::parse(ss.str());
                    int stage = j2.value("stage", -1);
                    int code  = j2.value("code",  -1);
                    std::string info  = j2.value("info",  std::string{});
                    std::string phase = j2.value("phase", std::string{});
                    if (stage != last_stage || code != last_code
                        || info != last_info || phase != last_phase) {
                        if (update_fn) update_fn(stage, code, info);
                        BOOST_LOG_TRIVIAL(info)
                            << "virtual_lan_print: bridge progress phase="
                            << phase << " stage=" << stage
                            << " code=" << code << " info=" << info;
                        last_stage = stage;
                        last_code  = code;
                        last_info  = info;
                        last_phase = phase;
                    }
                    if (phase == "done") {
                        BOOST_LOG_TRIVIAL(info)
                            << "virtual_lan_print: bridge dispatch done; "
                               "returning success";
                        return 0;
                    }
                    if (phase == "failed") {
                        BOOST_LOG_TRIVIAL(warning)
                            << "virtual_lan_print: bridge dispatch failed; "
                               "info=" << info;
                        return code != 0 ? code : -1;
                    }
                } catch (...) {
                    // Half-written file or stale; just retry.
                }
            }
            std::this_thread::sleep_for(milliseconds(250));
        }
    }
}



int NetworkAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    snapshot_bbs_sent(params.dev_id, params.filename);
    snapshot_print_cmd("start_print", params);
    if (is_virtual_dev_id(params.dev_id)) {
        BOOST_LOG_TRIVIAL(info) << "[bbs-virtual] start_print FFFF dev_id=" << params.dev_id;
        return virtual_lan_print_(params, update_fn, cancel_fn);
    }
    int ret = 0;
    if (network_agent && start_print_ptr) {
        dump_print_params("start_print(pre)", params);
        ret = start_print_ptr(network_agent, params, update_fn, cancel_fn, wait_fn);
        log_event("start_print dev_id=%s ret=%d", params.dev_id.c_str(), ret);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__
                                << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(params.dev_id) %params.task_name %params.project_name;
    }
    return ret;
}

int NetworkAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    snapshot_bbs_sent(params.dev_id, params.filename);
    snapshot_print_cmd("start_local_print_with_record", params);
    if (is_virtual_dev_id(params.dev_id)) {
        BOOST_LOG_TRIVIAL(info) << "[bbs-virtual] start_local_print_with_record FFFF dev_id=" << params.dev_id;
        return virtual_lan_print_(params, update_fn, cancel_fn);
    }
    int ret = 0;
    if (network_agent && start_local_print_with_record_ptr) {
        dump_print_params("start_local_print_with_record(pre)", params);
        ret = start_local_print_with_record_ptr(network_agent, params, update_fn, cancel_fn, wait_fn);
        log_event("start_local_print_with_record dev_id=%s ret=%d", params.dev_id.c_str(), ret);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(params.dev_id) %params.task_name %params.project_name;
    }
    return ret;
}

int NetworkAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    snapshot_bbs_sent(params.dev_id, params.filename);
    snapshot_print_cmd("start_send_gcode_to_sdcard", params);
    if (is_virtual_dev_id(params.dev_id)) {
        BOOST_LOG_TRIVIAL(info) << "[bbs-virtual] start_send_gcode_to_sdcard FFFF dev_id=" << params.dev_id;
        return virtual_lan_print_(params, update_fn, cancel_fn);
    }
    int rc = 0;
    if (bridge_hooks::Dispatcher::try_start_send_gcode_to_sdcard(
            params, update_fn, cancel_fn, &rc))
        return rc;
    int ret = 0;
    if (network_agent && start_send_gcode_to_sdcard_ptr) {
        dump_print_params("start_send_gcode_to_sdcard(pre)", params);
        ret = start_send_gcode_to_sdcard_ptr(network_agent, params, update_fn, cancel_fn, wait_fn);
        log_event("start_send_gcode_to_sdcard dev_id=%s ret=%d", params.dev_id.c_str(), ret);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(params.dev_id) %params.task_name %params.project_name;
    }
    return ret;
}

int NetworkAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    snapshot_bbs_sent(params.dev_id, params.filename);
    snapshot_print_cmd("start_local_print", params);
    if (is_virtual_dev_id(params.dev_id)) {
        BOOST_LOG_TRIVIAL(info) << "[bbs-virtual] start_local_print FFFF dev_id=" << params.dev_id;
        return virtual_lan_print_(params, update_fn, cancel_fn);
    }
    int ret = 0;
    if (network_agent && start_local_print_ptr) {
        dump_print_params("start_local_print(pre)", params);
        ret = start_local_print_ptr(network_agent, params, update_fn, cancel_fn);
        log_event("start_local_print dev_id=%s ret=%d", params.dev_id.c_str(), ret);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(params.dev_id) %params.task_name %params.project_name;
    }
    return ret;
}

int NetworkAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    int ret = 0;
    if (network_agent && start_sdcard_print_ptr) {
        dump_print_params("start_sdcard_print(pre)", params);
        ret = start_sdcard_print_ptr(network_agent, params, update_fn, cancel_fn);
        log_event("start_sdcard_print dev_id=%s ret=%d", params.dev_id.c_str(), ret);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(params.dev_id) %params.task_name %params.project_name;
    }
    return ret;
}

int NetworkAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    int ret = 0;
    if (network_agent && get_user_presets_ptr) {
        ret = get_user_presets_ptr(network_agent, user_presets);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, setting_id count=%3%")%network_agent %ret %user_presets->size() ;
    }
    return ret;
}

std::string NetworkAgent::request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    std::string ret;
    if (network_agent && request_setting_id_ptr) {
        ret = request_setting_id_ptr(network_agent, name, values_map, http_code);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, name=%2%, http_code=%3%, ret.setting_id=%4%")
                %network_agent %name %(*http_code) %ret;
    }
    return ret;
}

int NetworkAgent::put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    int ret;
    if (network_agent && put_setting_ptr) {
        ret = put_setting_ptr(network_agent, setting_id, name, values_map, http_code);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, setting_id=%2%, name=%3%, http_code=%4%, ret=%5%")
                %network_agent %setting_id %name %(*http_code) %ret;
    }
    return ret;
}

int NetworkAgent::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    int ret = 0;
    if (network_agent && get_setting_list_ptr) {
        ret = get_setting_list_ptr(network_agent, bundle_version, pro_fn, cancel_fn);
        if (ret) BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, bundle_version=%3%") % network_agent % ret % bundle_version;
    }
    return ret;
}

int NetworkAgent::get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    int ret = 0;
    if (network_agent && get_setting_list2_ptr) {
        ret = get_setting_list2_ptr(network_agent, bundle_version, chk_fn, pro_fn, cancel_fn);
        if (ret) BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, bundle_version=%3%") % network_agent % ret % bundle_version;
    } else {
        ret = get_setting_list(bundle_version, pro_fn, cancel_fn);
    }
    return ret;
}

int NetworkAgent::delete_setting(std::string setting_id)
{
    int ret = 0;
    if (network_agent && delete_setting_ptr) {
        ret = delete_setting_ptr(network_agent, setting_id);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, setting_id=%3%")%network_agent %ret %setting_id ;
    }
    return ret;
}

std::string NetworkAgent::get_studio_info_url()
{
    std::string ret;
    if (network_agent && get_studio_info_url_ptr) {
        ret = get_studio_info_url_ptr(network_agent);
    }
    return ret;
}

int NetworkAgent::set_extra_http_header(std::map<std::string, std::string> extra_headers)
{
    int ret = 0;
    if (network_agent && set_extra_http_header_ptr) {
        ret = set_extra_http_header_ptr(network_agent, extra_headers);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, extra_headers count=%3%")%network_agent %ret %extra_headers.size() ;
    }
    return ret;
}

int NetworkAgent::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_my_message_ptr) {
        ret = get_my_message_ptr(network_agent, type, after, limit, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::check_user_task_report(int* task_id, bool* printable)
{
    int ret = 0;
    if (network_agent && check_user_task_report_ptr) {
        ret = check_user_task_report_ptr(network_agent, task_id, printable);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, task_id=%3%, printable=%4%")%network_agent %ret %(*task_id) %(*printable);
    }
    return ret;
}

int NetworkAgent::get_user_print_info(unsigned int* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_user_print_info_ptr) {
        log_event("get_user_print_info");
        ret = get_user_print_info_ptr(network_agent, http_code, http_body);
        log_event("get_user_print_info ret=%d http_code=%u body_bytes=%zu",
            ret, http_code ? *http_code : 0, http_body ? http_body->size() : 0);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, http_code=%3%")%network_agent %ret %(*http_code);
    }
    return ret;
}

int NetworkAgent::get_user_tasks(TaskQueryParams params, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_user_tasks_ptr) {
        ret = get_user_tasks_ptr(network_agent, params, http_body);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") %network_agent %ret;
    }
    return ret;
}

int NetworkAgent::get_filament_spools(FilamentQueryParams params, std::string* http_body)
{
    if (!network_agent || !get_filament_spools_ptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unavailable (network_agent="
            << network_agent << " func_ptr=" << (void*)get_filament_spools_ptr << ")";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int ret = get_filament_spools_ptr(network_agent, params, http_body);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%") %network_agent %ret;
    return ret;
}

int NetworkAgent::create_filament_spool(std::string request_body, std::string* http_body)
{
    if (!network_agent || !create_filament_spool_ptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unavailable (network_agent="
            << network_agent << " func_ptr=" << (void*)create_filament_spool_ptr << ")";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int ret = create_filament_spool_ptr(network_agent, request_body, http_body);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%") %network_agent %ret;
    return ret;
}

int NetworkAgent::update_filament_spool(std::string spool_id, std::string request_body, std::string* http_body)
{
    if (!network_agent || !update_filament_spool_ptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unavailable (network_agent="
            << network_agent << " func_ptr=" << (void*)update_filament_spool_ptr << ")";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int ret = update_filament_spool_ptr(network_agent, spool_id, request_body, http_body);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, spool_id=%3%") %network_agent %ret %spool_id;
    return ret;
}

int NetworkAgent::delete_filament_spools(FilamentDeleteParams params, std::string* http_body)
{
    if (!network_agent || !delete_filament_spools_ptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unavailable (network_agent="
            << network_agent << " func_ptr=" << (void*)delete_filament_spools_ptr << ")";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int ret = delete_filament_spools_ptr(network_agent, params, http_body);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%") %network_agent %ret;
    return ret;
}

int NetworkAgent::get_filament_config(std::string* http_body)
{
    if (!network_agent || !get_filament_config_ptr) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unavailable (network_agent="
            << network_agent << " func_ptr=" << (void*)get_filament_config_ptr << ")";
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int ret = get_filament_config_ptr(network_agent, http_body);
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%") %network_agent %ret;
    return ret;
}

int NetworkAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_printer_firmware_ptr) {
        ret = get_printer_firmware_ptr(network_agent, dev_id, http_code, http_body);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(dev_id);
    }
    return ret;
}

int NetworkAgent::get_task_plate_index(std::string task_id, int* plate_index)
{
    int ret = 0;
    if (network_agent && get_task_plate_index_ptr) {
        ret = get_task_plate_index_ptr(network_agent, task_id, plate_index);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, task_id=%3%")%network_agent %ret %task_id;
    }
    return ret;
}

int NetworkAgent::get_user_info(int* identifier)
{
    int ret = 0;
    if (network_agent && get_user_info_ptr) {
        ret = get_user_info_ptr(network_agent, identifier);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::request_bind_ticket(std::string* ticket)
{
    int ret = 0;
    if (network_agent && request_bind_ticket_ptr) {
        ret = request_bind_ticket_ptr(network_agent, ticket);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_subtask_info_ptr) {
        ret = get_subtask_info_ptr(network_agent, subtask_id, task_json, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json)
{
    int ret;
    if (network_agent && get_slice_info_ptr) {
        ret = get_slice_info_ptr(network_agent, project_id, profile_id, plate_index, slice_json);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, project_id=%2%, profile_id=%3%, plate_index=%4%, slice_json=%5%")
                %network_agent %project_id %profile_id %plate_index %(*slice_json);
    }
    return ret;
}

int NetworkAgent::query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body)
{
    int ret;
    if (network_agent && query_bind_status_ptr) {
        ret = query_bind_status_ptr(network_agent, query_list, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, http_code=%3%") %network_agent %ret %(*http_code);
    }
    return ret;
}

int NetworkAgent::modify_printer_name(std::string dev_id, std::string dev_name)
{
    int ret = 0;
    if (network_agent && modify_printer_name_ptr) {
        ret = modify_printer_name_ptr(network_agent, dev_id, dev_name);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, dev_name=%4%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(dev_id) %dev_name;
    }
    return ret;
}

int NetworkAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_camera_url_ptr) {
        log_event("get_camera_url dev_id=%s", dev_id.c_str());
        // Wrap the callback so we can log the resolved URL too — that's
        // where the bambu:/// scheme + LAN/cloud routing decision shows
        // up (essential for the camera/video scenario).
        std::string did = dev_id;
        auto wrapped = [cb = std::move(callback), did](std::string url) {
            log_event("get_camera_url dev_id=%s -> url=%s",
                did.c_str(), url.c_str());
            cb(std::move(url));
        };
        ret = get_camera_url_ptr(network_agent, dev_id, std::move(wrapped));
        log_event("get_camera_url dev_id=%s ret=%d", dev_id.c_str(), ret);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(dev_id);
    }
    return ret;
}

int NetworkAgent::get_camera_url_for_golive(std::string dev_id, std::string sdev_id, std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_camera_url_for_golive_ptr) {
        ret = get_camera_url_for_golive_ptr(network_agent, dev_id, sdev_id, callback);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%") %network_agent %ret %BBLCrossTalk::Crosstalk_DevId(dev_id);
    }
    return ret;
}

int NetworkAgent::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_design_staffpick_ptr) {
        ret = get_design_staffpick_ptr(network_agent, offset, limit, callback);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::get_mw_user_preference(std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_mw_user_preference_ptr) {
        ret = get_mw_user_preference_ptr(network_agent,callback);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}


int NetworkAgent::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_mw_user_4ulist_ptr) {
        ret = get_mw_user_4ulist_ptr(network_agent,seed, limit, callback);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback)
{
    int ret = -1;
    if (network_agent && get_hms_snapshot_ptr) {
        ret = get_hms_snapshot_ptr(network_agent, dev_id, file_name, callback);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string *out)
{
    int ret = 0;
    if (network_agent && start_publish_ptr) {
        log_event("start_publish");
        Slic3r::plugin_trace::dump_stack("start_publish");
        ret = start_publish_ptr(network_agent, params, update_fn, cancel_fn, out);
        log_event("start_publish ret=%d out_bytes=%zu",
            ret, out ? out->size() : 0);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_model_publish_url(std::string* url)
{
    int ret = 0;
    if (network_agent && get_model_publish_url_ptr) {
        ret = get_model_publish_url_ptr(network_agent, url);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn)
{
    int ret = 0;
    if (network_agent && get_subtask_ptr) {
        ret = get_subtask_ptr(network_agent, task, getsub_fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }

    return ret;
}

int NetworkAgent::get_model_mall_home_url(std::string* url)
{
    int ret = 0;
    if (network_agent && get_model_publish_url_ptr) {
        ret = get_model_mall_home_url_ptr(network_agent, url);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_model_mall_detail_url(std::string* url, std::string id)
{
    int ret = 0;
    if (network_agent && get_model_publish_url_ptr) {
        ret = get_model_mall_detail_url_ptr(network_agent, url, id);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_my_profile(std::string token, unsigned int *http_code, std::string *http_body)
{
    int ret = 0;
    if (network_agent && get_my_profile_ptr) {
        ret = get_my_profile_ptr(network_agent, token, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_my_token(std::string ticket, unsigned int* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_my_token_ptr) {
        ret = get_my_token_ptr(network_agent, ticket, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_enable(bool enable)
{
    enable_track = enable;
    int ret = 0;
    if (network_agent && track_enable_ptr) {
        ret = track_enable_ptr(network_agent, enable);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_remove_files()
{
    int ret = 0;
    if (network_agent && track_remove_files_ptr) {
        ret = track_remove_files_ptr(network_agent);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_event(std::string evt_key, std::string content)
{
    if (!this->enable_track)
        return 0;

    if (!this->is_user_login())
        return 0;

    int ret = 0;
    if (network_agent && track_event_ptr) {
        ret = track_event_ptr(network_agent, evt_key, content);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_header(std::string header)
{
    if (!this->enable_track)
        return 0;
    int ret = 0;
    if (network_agent && track_header_ptr) {
        ret = track_header_ptr(network_agent, header);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_update_property(std::string name, std::string value, std::string type)
{
    if (!this->enable_track)
        return 0;

    int ret = 0;
    if (network_agent && track_update_property_ptr) {
        ret = track_update_property_ptr(network_agent, name, value, type);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_get_property(std::string name, std::string& value, std::string type)
{
    if (!this->enable_track)
        return 0;

    int ret = 0;
    if (network_agent && track_get_property_ptr) {
        ret = track_get_property_ptr(network_agent, name, value, type);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::put_model_mall_rating(int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error)
{
    int ret = 0;
    if (network_agent && get_model_publish_url_ptr) {
        ret = put_model_mall_rating_url_ptr(network_agent, rating_id, score, content, images, http_code, http_error);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_oss_config(std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error)
{
    int ret = 0;
    if (network_agent && get_oss_config_ptr) {
        ret = get_oss_config_ptr(network_agent, config, country_code, http_code, http_error);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::put_rating_picture_oss(std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error)
{
    int ret = 0;
    if (network_agent && put_rating_picture_oss_ptr) {
        ret = put_rating_picture_oss_ptr(network_agent, config, pic_oss_path, model_id, profile_id, http_code, http_error);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_model_mall_rating_result(int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error)
{
    int ret = 0;
    if (network_agent && get_model_mall_rating_result_ptr) {
        ret = get_model_mall_rating_result_ptr(network_agent, job_id, rating_result, http_code, http_error);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

#if defined(BAMBU_BRIDGE_HARNESS_ENABLE)
// ---- Harness ShimRecorder wrap -------------------------------------------
//
// Replace the 10 NetworkAgent::*_ptr statics listed in
// test_harness_plan.md §7 with recording trampolines. The replacement
// is unconditional at link time; the trampoline itself short-circuits
// when ShimRecorder is inactive, so the runtime cost is one atomic-bool
// load + one indirect call when the harness is OFF.
//
// File-scope statics hold the captured real pointers. Plain free
// functions stay ABI-compatible with the `func_*` typedefs in
// NetworkAgent.hpp (the proprietary plugin invokes via these slots).
namespace {

using bridge::harness::ShimRecorder;
using bridge::harness::TraceLib;

// Captured originals. Initialised once inside the wrap function below.
static func_connect_server               s_real_connect_server               = nullptr;
static func_is_server_connected          s_real_is_server_connected          = nullptr;
static func_set_on_message_fn            s_real_set_on_message_fn            = nullptr;
static func_set_on_server_connected_fn   s_real_set_on_server_connected_fn   = nullptr;
static func_start_subscribe              s_real_start_subscribe              = nullptr;
static func_stop_subscribe               s_real_stop_subscribe               = nullptr;
static func_add_subscribe                s_real_add_subscribe                = nullptr;
static func_del_subscribe                s_real_del_subscribe                = nullptr;
static func_send_message_to_printer      s_real_send_message_to_printer      = nullptr;
static func_get_user_print_info          s_real_get_user_print_info          = nullptr;
static func_get_camera_url               s_real_get_camera_url               = nullptr;
static bool                              s_wrapped                           = false;

// Wrapping trampolines. Each records a TraceLine, forwards to the real
// pointer (if non-null; the slicer tolerates missing exports), then
// records the return value.

static int bb_connect_server(void* agent) {
    int rc = s_real_connect_server ? s_real_connect_server(agent) : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "connect_server", nlohmann::json::object(), rc);
    return rc;
}

static bool bb_is_server_connected(void* agent) {
    bool ok = s_real_is_server_connected ? s_real_is_server_connected(agent) : false;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "is_server_connected", nlohmann::json::object(), ok);
    return ok;
}

static int bb_set_on_message_fn(void* agent, OnMessageFn fn) {
    // Use the std::function's target_type() as a poor-man's identity key
    // (std::function objects are not addressable, but the wrapped target
    // typically is). Falls back to a fresh id per call when target() is
    // null, which is fine — every distinct registration gets a distinct id.
    const void* key = nullptr;
    if (fn) key = fn.target<void(*)(std::string, std::string)>();
    const std::int64_t cb_id = ShimRecorder::instance().callback_id_for(key);
    int rc = s_real_set_on_message_fn ? s_real_set_on_message_fn(agent, fn) : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "set_on_message_fn",
        nlohmann::json{{"_callback", "OnMessageFn"}, {"id", cb_id}},
        rc);
    return rc;
}

static int bb_set_on_server_connected_fn(void* agent, OnServerConnectedFn fn) {
    const void* key = nullptr;
    if (fn) key = fn.target<void(*)(int, int)>();
    const std::int64_t cb_id = ShimRecorder::instance().callback_id_for(key);
    int rc = s_real_set_on_server_connected_fn ? s_real_set_on_server_connected_fn(agent, fn) : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "set_on_server_connected_fn",
        nlohmann::json{{"_callback", "OnServerConnectedFn"}, {"id", cb_id}},
        rc);
    return rc;
}

static int bb_start_subscribe(void* agent, std::string module) {
    int rc = s_real_start_subscribe ? s_real_start_subscribe(agent, module) : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "start_subscribe", nlohmann::json{{"module", module}}, rc);
    return rc;
}

static int bb_stop_subscribe(void* agent, std::string module) {
    int rc = s_real_stop_subscribe ? s_real_stop_subscribe(agent, module) : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "stop_subscribe", nlohmann::json{{"module", module}}, rc);
    return rc;
}

static int bb_add_subscribe(void* agent, std::vector<std::string> dev_list) {
    int rc = s_real_add_subscribe ? s_real_add_subscribe(agent, dev_list) : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "add_subscribe", nlohmann::json{{"dev_list", dev_list}}, rc);
    return rc;
}

static int bb_del_subscribe(void* agent, std::vector<std::string> dev_list) {
    int rc = s_real_del_subscribe ? s_real_del_subscribe(agent, dev_list) : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "del_subscribe", nlohmann::json{{"dev_list", dev_list}}, rc);
    return rc;
}

static int bb_send_message_to_printer(void* agent, std::string dev_id,
                                      std::string json_str, int qos, int flag) {
    int rc = s_real_send_message_to_printer
        ? s_real_send_message_to_printer(agent, dev_id, json_str, qos, flag)
        : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "send_message_to_printer",
        nlohmann::json{
            {"dev_id", dev_id},
            {"json_str", json_str},
            {"qos", qos},
            {"flag", flag}},
        rc);
    return rc;
}

static int bb_get_user_print_info(void* agent, unsigned int* http_code,
                                  std::string* http_body) {
    int rc = s_real_get_user_print_info
        ? s_real_get_user_print_info(agent, http_code, http_body)
        : -1;
    nlohmann::json out = nlohmann::json::object();
    if (http_code) out["http_code"] = *http_code;
    if (http_body) out["http_body"] = *http_body;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "get_user_print_info", nlohmann::json::object(), rc, out);
    return rc;
}

static int bb_get_camera_url(void* agent, std::string dev_id,
                             std::function<void(std::string)> callback) {
    const std::int64_t cb_id =
        ShimRecorder::instance().callback_id_for(
            callback ? callback.target<void(*)(std::string)>() : nullptr);
    // Wrap the user's callback so the URL it gets is also recorded.
    auto wrapped_cb = [cb_id, callback](std::string url) {
        ShimRecorder::instance().record(TraceLib::BambuNetworking,
            "get_camera_url.callback",
            nlohmann::json{{"url", url}},
            nlohmann::json(),
            nlohmann::json::object(), 0, cb_id);
        if (callback) callback(std::move(url));
    };
    int rc = s_real_get_camera_url
        ? s_real_get_camera_url(agent, dev_id, wrapped_cb)
        : -1;
    ShimRecorder::instance().record(TraceLib::BambuNetworking,
        "get_camera_url",
        nlohmann::json{{"dev_id", dev_id},
                       {"_callback", "GetCameraUrlCb"},
                       {"id", cb_id}},
        rc);
    return rc;
}

} // anonymous namespace

void bb_harness_wrap_network_agent_pointers() {
    if (s_wrapped) return;
    s_wrapped = true;

    // Capture originals (any may legitimately be nullptr; the trampoline
    // tolerates that). Then re-point the static slots at the trampolines.
    s_real_connect_server             = NetworkAgent::connect_server_ptr;
    s_real_is_server_connected        = NetworkAgent::is_server_connected_ptr;
    s_real_set_on_message_fn          = NetworkAgent::set_on_message_fn_ptr;
    s_real_set_on_server_connected_fn = NetworkAgent::set_on_server_connected_fn_ptr;
    s_real_start_subscribe            = NetworkAgent::start_subscribe_ptr;
    s_real_stop_subscribe             = NetworkAgent::stop_subscribe_ptr;
    s_real_add_subscribe              = NetworkAgent::add_subscribe_ptr;
    s_real_del_subscribe              = NetworkAgent::del_subscribe_ptr;
    s_real_send_message_to_printer    = NetworkAgent::send_message_to_printer_ptr;
    s_real_get_user_print_info        = NetworkAgent::get_user_print_info_ptr;
    s_real_get_camera_url             = NetworkAgent::get_camera_url_ptr;

    NetworkAgent::connect_server_ptr             = &bb_connect_server;
    NetworkAgent::is_server_connected_ptr        = &bb_is_server_connected;
    NetworkAgent::set_on_message_fn_ptr          = &bb_set_on_message_fn;
    NetworkAgent::set_on_server_connected_fn_ptr = &bb_set_on_server_connected_fn;
    NetworkAgent::start_subscribe_ptr            = &bb_start_subscribe;
    NetworkAgent::stop_subscribe_ptr             = &bb_stop_subscribe;
    NetworkAgent::add_subscribe_ptr              = &bb_add_subscribe;
    NetworkAgent::del_subscribe_ptr              = &bb_del_subscribe;
    NetworkAgent::send_message_to_printer_ptr    = &bb_send_message_to_printer;
    NetworkAgent::get_user_print_info_ptr        = &bb_get_user_print_info;
    NetworkAgent::get_camera_url_ptr             = &bb_get_camera_url;

    // Honour the env var: if BAMBU_BRIDGE_SHIM=1 is set and a path is
    // not already opened, open one now. Tests that drive ShimRecorder
    // directly can also call enable() first — this is idempotent.
    ShimRecorder::instance().enable_from_env();
}
#endif // BAMBU_BRIDGE_HARNESS_ENABLE

} //namespace
