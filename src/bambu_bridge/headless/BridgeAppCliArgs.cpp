// Bambu Bridge — shared CLI argument parser implementation.

#include "BridgeAppCliArgs.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>      // getenv, strtol
#include <cstring>
#include <filesystem>
#include <sstream>

namespace Slic3r {
namespace bridge {
namespace headless {

namespace {

bool parse_int(const char* s, int* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    *out = static_cast<int>(v);
    return true;
}

bool parse_uint16(const char* s, uint16_t* out) {
    int v;
    if (!parse_int(s, &v) || v < 0 || v > 65535) return false;
    *out = static_cast<uint16_t>(v);
    return true;
}

} // namespace

std::string render_usage(const std::string& program_name) {
    std::ostringstream o;
    o << "usage: " << program_name << " [options]\n"
        "\n"
        "Walks the cloud inventory of the logged-in Bambu user and stands\n"
        "up SSDP + MQTT + FTPS + RTSP servers for every cloud-bound printer,\n"
        "sharing a single bambu_networking plugin handle. Each device gets\n"
        "an ascending high-port set per role so multiple printers can run\n"
        "on the same host without colliding.\n"
        "\n"
        "Plugin options:\n"
        "  --plugin <path>        Path to libbambu_networking.so. If unset,\n"
        "                         falls back to $BAMBU_BRIDGE_PLUGIN_PATH,\n"
        "                         then the plugin handle's default probe.\n"
        "  --config-dir <path>    Plugin set_config_dir() pass-through.\n"
        "  --country-code <cc>    Plugin set_country_code() pass-through.\n"
        "\n"
        "Server options:\n"
        "  --bind <ip>            Bind IP for all per-device listeners\n"
        "                         (default: 0.0.0.0).\n"
        "  --no-ssdp              Disable the SSDP responder.\n"
        "  --no-mqtt              Disable the per-device MQTT broker.\n"
        "  --no-ftps              Disable the per-device FTPS server.\n"
        "  --no-rtsp              Disable the per-device RTSP server.\n"
        "  --mqtt-port-base N     Per-device MQTT port base (default 38883).\n"
        "  --ftps-port-base N     Per-device FTPS port base (default 39990).\n"
        "  --rtsp-port-base N     Per-device RTSP port base (default 38322).\n"
        "  --cert-cache-dir <p>   Directory for per-device certs (default:\n"
        "                         $XDG_CONFIG_HOME/BambuStudio/bridge/certs).\n"
        "\n"
        "Cadence:\n"
        "  --inventory-poll-seconds N\n"
        "                         How often to refresh the cloud inventory\n"
        "                         and add / remove / re-IP devices.\n"
        "                         Default: 60s.\n"
        "  --lan-ip-stale-after-seconds N\n"
        "                         A device's discovered lan_ip is cleared\n"
        "                         if no SSDP NOTIFY arrived from it in the\n"
        "                         last N seconds. Default: 120s (4x the\n"
        "                         firmware's typical ~30s announce cadence).\n"
        "  --only-dev-id <serial> Restrict the bridge to a single printer\n"
        "                         (or repeat the flag for multiple). Other\n"
        "                         cloud-inventory entries are silently\n"
        "                         dropped. Useful for one-printer\n"
        "                         debugging without the other listeners\n"
        "                         on 8884/8885 confusing slicer discovery.\n"
        "\n"
        "Misc:\n"
        "  -v, --verbose          Verbose logging (currently a no-op; reserved\n"
        "                         for phase 11 wire-diff capture).\n"
        "  -h, --help             Show this help and exit.\n";
    return o.str();
}

ParseResult parse_cli_args(const std::string& program_name,
                           int                argc,
                           char**             argv) {
    ParseResult result;
    BridgeAppConfig& cfg = result.config;

    // Environment fallback for the plugin path.
    if (const char* envv = std::getenv("BAMBU_BRIDGE_PLUGIN_PATH");
        envv && *envv) {
        cfg.plugin_path = envv;
    }

    // `BridgeAppConfig` defaults `host_drives_inventory = true` because
    // that is the safe choice when the bridge is constructed inside
    // BambuStudio's GUI process (the slicer's NetworkAgent owns the
    // plugin). When this parser runs, however, the caller is
    // `BambuStudio --bridge-only` headless — there is no GUI, so the
    // bridge IS the sole plugin consumer and must dlopen the plugin
    // itself.
    cfg.host_drives_inventory = false;

    auto take_value = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            result.exit_code = 2;
            result.error_message =
                program_name + ": " + flag + " requires a value";
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i] ? argv[i] : "";

        if (a == "--plugin") {
            auto v = take_value(i, "--plugin");      if (!v) return result; cfg.plugin_path  = v;
        } else if (a == "--config-dir") {
            auto v = take_value(i, "--config-dir");  if (!v) return result; cfg.config_dir   = v;
        } else if (a == "--country-code") {
            auto v = take_value(i, "--country-code");if (!v) return result; cfg.country_code = v;
        } else if (a == "--bind") {
            auto v = take_value(i, "--bind");        if (!v) return result; cfg.lan_iface_bind = v;
        } else if (a == "--no-ssdp")   { cfg.enable_ssdp = false;
        } else if (a == "--no-mqtt")   { cfg.enable_mqtt = false;
        } else if (a == "--no-ftps")   { cfg.enable_ftps = false;
        } else if (a == "--no-rtsp")   { cfg.enable_rtsp = false;
        } else if (a == "--mqtt-port-base") {
            auto v = take_value(i, "--mqtt-port-base"); if (!v) return result;
            if (!parse_uint16(v, &cfg.mqtt_port_base)) {
                result.exit_code = 2;
                result.error_message =
                    program_name + ": --mqtt-port-base requires an integer 0..65535, got '" + v + "'";
                return result;
            }
        } else if (a == "--ftps-port-base") {
            auto v = take_value(i, "--ftps-port-base"); if (!v) return result;
            if (!parse_uint16(v, &cfg.ftps_port_base)) {
                result.exit_code = 2;
                result.error_message =
                    program_name + ": --ftps-port-base requires an integer 0..65535, got '" + v + "'";
                return result;
            }
        } else if (a == "--rtsp-port-base") {
            auto v = take_value(i, "--rtsp-port-base"); if (!v) return result;
            if (!parse_uint16(v, &cfg.rtsp_port_base)) {
                result.exit_code = 2;
                result.error_message =
                    program_name + ": --rtsp-port-base requires an integer 0..65535, got '" + v + "'";
                return result;
            }
        } else if (a == "--cert-cache-dir") {
            auto v = take_value(i, "--cert-cache-dir"); if (!v) return result;
            cfg.cert_cache_dir = std::filesystem::path(v);
        } else if (a == "--inventory-poll-seconds") {
            auto v = take_value(i, "--inventory-poll-seconds"); if (!v) return result;
            int secs;
            if (!parse_int(v, &secs) || secs <= 0) {
                result.exit_code = 2;
                result.error_message =
                    program_name + ": --inventory-poll-seconds requires a positive integer, got '" + v + "'";
                return result;
            }
            cfg.inventory_poll = std::chrono::seconds(secs);
        } else if (a == "--only-dev-id") {
            auto v = take_value(i, "--only-dev-id"); if (!v) return result;
            cfg.only_dev_ids.emplace_back(v);
        } else if (a == "--lan-ip-stale-after-seconds") {
            auto v = take_value(i, "--lan-ip-stale-after-seconds"); if (!v) return result;
            int secs;
            if (!parse_int(v, &secs) || secs <= 0) {
                result.exit_code = 2;
                result.error_message =
                    program_name + ": --lan-ip-stale-after-seconds requires a positive integer, got '" + v + "'";
                return result;
            }
            cfg.lan_ip_stale_after = std::chrono::seconds(secs);
        } else if (a == "-v" || a == "--verbose") {
            // Reserved (phase 11 wire-diff capture). No-op today.
        } else if (a == "--bridge-only") {
            // Recognised when this parser is hosted by BambuStudio. It's
            // the dispatch flag the main binary uses to enter this path;
            // by the time we run, the dispatch has already happened.
            // Tolerate it so users can pass `--bridge-only` and `--plugin`
            // in any order.
        } else if (a == "-h" || a == "--help") {
            result.exit_code = 1;
            result.help_text = render_usage(program_name);
            return result;
        } else {
            result.exit_code = 2;
            result.error_message =
                program_name + ": unknown option '" + a + "'";
            return result;
        }
    }

    return result;
}

} // namespace headless
} // namespace bridge
} // namespace Slic3r
