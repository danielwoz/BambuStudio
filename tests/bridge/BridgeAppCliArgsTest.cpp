// Bambu Bridge — unit test for the shared BridgeAppCliArgs parser.
//
// The parser backs BambuStudio's `--bridge-only` headless mode. This
// test drives it directly so any flag-handling regression surfaces
// here before the slicer is rebuilt.

#include "../../src/bambu_bridge/headless/BridgeAppCliArgs.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// argv builder for the tests — owns its strings so the char** pointers
// remain valid for the parse_cli_args call.
struct Argv {
    std::vector<std::string> storage;
    std::vector<char*>       argv;
    explicit Argv(std::initializer_list<const char*> args) {
        storage.reserve(args.size());
        argv.reserve(args.size() + 1);
        for (const char* a : args) {
            storage.emplace_back(a);
            argv.push_back(storage.back().data());
        }
        argv.push_back(nullptr);
    }
    int   argc() const { return static_cast<int>(storage.size()); }
    char** ptr()       { return argv.data(); }
};

int test_no_args_uses_defaults() {
    Argv a{"BambuStudio --bridge-only"};
    auto r = Slic3r::bridge::headless::parse_cli_args(
        "BambuStudio --bridge-only", a.argc(), a.ptr());
    if (r.exit_code != 0) {
        std::fprintf(stderr, "FAIL: defaults should parse, got exit=%d msg=%s\n",
                     r.exit_code, r.error_message.c_str());
        return 1;
    }
    // Full-proxy default: every server on. Individual flags can be
    // turned off via --no-{ssdp,mqtt,ftps,rtsp}.
    if (r.config.enable_ssdp != true ||
        r.config.enable_mqtt != true ||
        r.config.enable_ftps != true ||
        r.config.enable_rtsp != true) {
        std::fprintf(stderr, "FAIL: defaults should be all-servers-on "
                             "(got ssdp=%d mqtt=%d ftps=%d rtsp=%d)\n",
                     (int)r.config.enable_ssdp, (int)r.config.enable_mqtt,
                     (int)r.config.enable_ftps, (int)r.config.enable_rtsp);
        return 1;
    }
    if (r.config.lan_iface_bind != "0.0.0.0") {
        std::fprintf(stderr, "FAIL: default bind != 0.0.0.0 (got '%s')\n",
                     r.config.lan_iface_bind.c_str());
        return 1;
    }
    return 0;
}

int test_help_returns_help_text() {
    Argv a{"BambuStudio --bridge-only", "--help"};
    auto r = Slic3r::bridge::headless::parse_cli_args(
        "BambuStudio --bridge-only", a.argc(), a.ptr());
    if (r.exit_code != 1) {
        std::fprintf(stderr, "FAIL: --help should produce exit_code=1, got %d\n",
                     r.exit_code);
        return 1;
    }
    if (r.help_text.find("usage: BambuStudio --bridge-only") == std::string::npos) {
        std::fprintf(stderr, "FAIL: help_text missing usage header\n");
        return 1;
    }
    if (r.help_text.find("--plugin") == std::string::npos ||
        r.help_text.find("--bind")   == std::string::npos ||
        r.help_text.find("--no-ssdp")== std::string::npos) {
        std::fprintf(stderr, "FAIL: help_text missing one of the documented flags\n");
        return 1;
    }
    return 0;
}

int test_short_help() {
    Argv a{"daemon", "-h"};
    auto r = Slic3r::bridge::headless::parse_cli_args("daemon", a.argc(), a.ptr());
    return (r.exit_code == 1) ? 0 : (std::fprintf(stderr, "FAIL: -h not handled\n"), 1);
}

int test_unknown_option_is_error() {
    Argv a{"daemon", "--what-even"};
    auto r = Slic3r::bridge::headless::parse_cli_args("daemon", a.argc(), a.ptr());
    if (r.exit_code != 2) {
        std::fprintf(stderr, "FAIL: unknown option should exit 2, got %d\n", r.exit_code);
        return 1;
    }
    if (r.error_message.find("unknown option") == std::string::npos) {
        std::fprintf(stderr, "FAIL: error message doesn't mention 'unknown option': %s\n",
                     r.error_message.c_str());
        return 1;
    }
    return 0;
}

int test_value_flags_consume_argument() {
    Argv a{"d",
        "--plugin", "/tmp/plug.so",
        "--config-dir", "/etc/bambu",
        "--country-code", "us",
        "--bind", "127.0.0.1",
        "--mqtt-port-base", "12000",
        "--ftps-port-base", "12100",
        "--rtsp-port-base", "12200",
        "--cert-cache-dir", "/tmp/certs",
        "--inventory-poll-seconds", "15"};
    auto r = Slic3r::bridge::headless::parse_cli_args("d", a.argc(), a.ptr());
    if (r.exit_code != 0) {
        std::fprintf(stderr, "FAIL: valid flags should parse, msg=%s\n",
                     r.error_message.c_str());
        return 1;
    }
    auto& c = r.config;
    if (c.plugin_path != "/tmp/plug.so") { std::fprintf(stderr, "FAIL: plugin_path\n"); return 1; }
    if (c.config_dir != "/etc/bambu")    { std::fprintf(stderr, "FAIL: config_dir\n"); return 1; }
    if (c.country_code != "us")          { std::fprintf(stderr, "FAIL: country_code\n"); return 1; }
    if (c.lan_iface_bind != "127.0.0.1") { std::fprintf(stderr, "FAIL: bind\n"); return 1; }
    if (c.mqtt_port_base != 12000)       { std::fprintf(stderr, "FAIL: mqtt port base\n"); return 1; }
    if (c.ftps_port_base != 12100)       { std::fprintf(stderr, "FAIL: ftps port base\n"); return 1; }
    if (c.rtsp_port_base != 12200)       { std::fprintf(stderr, "FAIL: rtsp port base\n"); return 1; }
    if (c.cert_cache_dir != "/tmp/certs"){ std::fprintf(stderr, "FAIL: cert_cache_dir\n"); return 1; }
    if (c.inventory_poll != std::chrono::seconds(15)) {
        std::fprintf(stderr, "FAIL: inventory_poll != 15s\n"); return 1;
    }
    return 0;
}

int test_disable_flags() {
    Argv a{"d", "--no-ssdp", "--no-mqtt", "--no-ftps", "--no-rtsp"};
    auto r = Slic3r::bridge::headless::parse_cli_args("d", a.argc(), a.ptr());
    if (r.exit_code != 0) { std::fprintf(stderr, "FAIL: --no-* failed\n"); return 1; }
    auto& c = r.config;
    if (c.enable_ssdp || c.enable_mqtt || c.enable_ftps || c.enable_rtsp) {
        std::fprintf(stderr, "FAIL: not all servers disabled\n");
        return 1;
    }
    return 0;
}

int test_missing_value_is_error() {
    Argv a{"d", "--plugin"};   // no value after --plugin
    auto r = Slic3r::bridge::headless::parse_cli_args("d", a.argc(), a.ptr());
    if (r.exit_code != 2 || r.error_message.find("--plugin") == std::string::npos) {
        std::fprintf(stderr, "FAIL: missing-value should error mentioning --plugin\n");
        return 1;
    }
    return 0;
}

int test_bad_integer_is_error() {
    Argv a{"d", "--mqtt-port-base", "abc"};
    auto r = Slic3r::bridge::headless::parse_cli_args("d", a.argc(), a.ptr());
    if (r.exit_code != 2) {
        std::fprintf(stderr, "FAIL: non-integer port-base should error\n");
        return 1;
    }
    return 0;
}

int test_port_out_of_range_is_error() {
    Argv a{"d", "--mqtt-port-base", "70000"};
    auto r = Slic3r::bridge::headless::parse_cli_args("d", a.argc(), a.ptr());
    if (r.exit_code != 2) {
        std::fprintf(stderr, "FAIL: out-of-range port should error\n");
        return 1;
    }
    return 0;
}

int test_bridge_only_token_tolerated() {
    // BambuStudio uses --bridge-only as its dispatch flag, then hands
    // the rest to this parser. The parser should accept --bridge-only
    // as a no-op so the same argv works for both entry points.
    Argv a{"BambuStudio", "--bridge-only", "--plugin", "/x.so"};
    auto r = Slic3r::bridge::headless::parse_cli_args(
        "BambuStudio --bridge-only", a.argc(), a.ptr());
    if (r.exit_code != 0) {
        std::fprintf(stderr, "FAIL: --bridge-only should be tolerated, msg=%s\n",
                     r.error_message.c_str());
        return 1;
    }
    if (r.config.plugin_path != "/x.so") {
        std::fprintf(stderr, "FAIL: --plugin after --bridge-only didn't take\n");
        return 1;
    }
    return 0;
}

int test_env_fallback_for_plugin_path() {
    ::setenv("BAMBU_BRIDGE_PLUGIN_PATH", "/from/env.so", /*overwrite*/1);
    Argv a{"d"};
    auto r = Slic3r::bridge::headless::parse_cli_args("d", a.argc(), a.ptr());
    int rc = 0;
    if (r.config.plugin_path != "/from/env.so") {
        std::fprintf(stderr,
            "FAIL: env fallback for plugin_path didn't apply (got '%s')\n",
            r.config.plugin_path.c_str());
        rc = 1;
    }
    // --plugin should override env
    Argv b{"d", "--plugin", "/cli/wins.so"};
    auto r2 = Slic3r::bridge::headless::parse_cli_args("d", b.argc(), b.ptr());
    if (r2.config.plugin_path != "/cli/wins.so") {
        std::fprintf(stderr,
            "FAIL: --plugin didn't override env (got '%s')\n",
            r2.config.plugin_path.c_str());
        rc = 1;
    }
    ::unsetenv("BAMBU_BRIDGE_PLUGIN_PATH");
    return rc;
}

int test_verbose_is_noop() {
    Argv a{"d", "-v", "--verbose"};
    auto r = Slic3r::bridge::headless::parse_cli_args("d", a.argc(), a.ptr());
    return (r.exit_code == 0) ? 0 : (std::fprintf(stderr, "FAIL: -v/--verbose\n"), 1);
}

int test_program_name_in_errors() {
    Argv a{"daemon", "--what"};
    auto r = Slic3r::bridge::headless::parse_cli_args(
        "BambuStudio --bridge-only", a.argc(), a.ptr());
    if (r.error_message.find("BambuStudio --bridge-only") == std::string::npos) {
        std::fprintf(stderr, "FAIL: program_name not surfaced in error: %s\n",
                     r.error_message.c_str());
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    int rc = 0;
    rc |= test_no_args_uses_defaults();
    rc |= test_help_returns_help_text();
    rc |= test_short_help();
    rc |= test_unknown_option_is_error();
    rc |= test_value_flags_consume_argument();
    rc |= test_disable_flags();
    rc |= test_missing_value_is_error();
    rc |= test_bad_integer_is_error();
    rc |= test_port_out_of_range_is_error();
    rc |= test_bridge_only_token_tolerated();
    rc |= test_env_fallback_for_plugin_path();
    rc |= test_verbose_is_noop();
    rc |= test_program_name_in_errors();
    if (rc == 0) std::printf("OK\n");
    return rc;
}
