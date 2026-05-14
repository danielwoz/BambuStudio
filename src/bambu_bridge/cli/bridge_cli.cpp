// Bambu Bridge — minimal `bridge-cli` entry point.
//
// Phase 1 only ships `list-devices`. The executable is intentionally tiny:
// it loads the proprietary plugin via CloudInventory, dumps the resulting
// cache as TSV, and exits. Future phases will tack on additional
// subcommands (mirror-up, status, etc.) on the same dispatch table.
//
// Phase 2 adds `mint-cert <dev_id>`: forces a CertFactory cache hit for
// a given serial and prints the resulting paths + SHA-256 fingerprint.
// Useful for ops — pre-mint certs before starting the bridge, or
// confirm/inspect existing ones.
//
// Phase 3 adds `announce`: stands up an SsdpResponder with one synthetic
// device and runs it for `--duration` seconds. Useful for confirming the
// bridge is reachable on a real LAN before wiring up the full proxy.
//
// Plugin location precedence:
//   1. --plugin <path>            (explicit on the command line)
//   2. $BAMBU_BRIDGE_PLUGIN_PATH  (env override)
//   3. CloudInventory's built-in default probe list
//
// Output format on stdout:
//   dev_id\tmodel\tname\tonline\tlan_ip   (header row, always printed)
//   ... one TSV row per cloud-bound printer
//
// Exit code: 0 if list-devices completed (even if the list is empty);
// non-zero only on argument errors or hard plugin-load failure.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../BambuNetworkingPluginHandle.hpp"
#include "../BridgeService.hpp"
#include "../CloudInventory.hpp"
#include "../router/CameraSourceRouter.hpp"
#include "../router/CloudCameraSource.hpp"
#include "../router/CloudUplink.hpp"
#include "../router/CloudUploadSink.hpp"
#include "../router/LanCameraSource.hpp"
#include "../router/LanUplink.hpp"
#include "../router/LanUploadSink.hpp"
#include "../router/NullCameraSource.hpp"
#include "../router/NullUplink.hpp"
#include "../router/NullUploadSink.hpp"
#include "../router/SessionRouter.hpp"
#include "../router/UplinkHealth.hpp"
#include "../router/UploadSinkRouter.hpp"
#include "../server/FtpsServer.hpp"
#include "../server/ICameraSource.hpp"
#include "../server/IUploadSink.hpp"
#include "../server/MqttBroker.hpp"
#include "../server/RtspServer.hpp"
#include "../server/SsdpResponder.hpp"
#include "../tls/CertFactory.hpp"

#include <filesystem>
#include <fstream>

namespace {

void print_usage(std::FILE* out) {
    std::fprintf(out,
        "usage: bridge-cli <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  list-devices [--config-dir <path>] [--plugin <path-to-libbambu_networking.so>]\n"
        "                List cloud-bound printers for the logged-in user.\n"
        "\n"
        "  mint-cert <dev_id> [--cache-dir <path>]\n"
        "                Mint (or load from cache) the self-signed TLS cert for\n"
        "                a printer serial. Prints cert path, key path, and the\n"
        "                cert's SHA-256 fingerprint. Idempotent across runs.\n"
        "\n"
        "  announce --dev-id <id> --name <n> --model <m> --firmware <fw>\n"
        "           --lan-ip <ip> [--http-port <p>] [--duration <secs>]\n"
        "           [--bind-address <addr>]\n"
        "                Stand up an SSDP responder for one synthetic device and\n"
        "                announce it for the given duration (default 60s).\n"
        "                SIGINT / SIGTERM trigger a graceful byebye-and-exit.\n"
        "\n"
        "  broker --dev-id <id> --access-code <code> [--bind 127.0.0.1] [--port 8883]\n"
        "         [--cache-dir <path>]\n"
        "                Mint a self-signed cert for <id>, start a one-device\n"
        "                MQTT broker on the given bind/port, and sleep until\n"
        "                SIGINT / SIGTERM. Useful for hand-driving with\n"
        "                mosquitto_pub etc. Logs every event via NullUplink.\n"
        "\n"
        "  lan-bridge --dev-id <id> --printer-ip <ip> --port <p> --access-code <code>\n"
        "             [--bind 127.0.0.1] [--bind-port 38883] [--cache-dir <path>]\n"
        "             [--name <n>] [--model <m>] [--firmware <fw>]\n"
        "                Stand up the full phase-3+4+5 stack against one device:\n"
        "                SSDP announce + MqttBroker on bind:bind-port + LanUplink\n"
        "                to printer-ip:port. Runs until SIGINT/SIGTERM. Lets you\n"
        "                point a real slicer at bind:bind-port and have it talk\n"
        "                to the real printer through the bridge.\n"
        "\n"
        "  rtsp --dev-id <id> --access-code <code> [--source null|lan|cloud]\n"
        "       [--bind 127.0.0.1] [--port 38322] [--cache-dir <path>]\n"
        "       [--printer-ip <ip>] [--printer-port 322]\n"
        "                Mint a self-signed cert for <id>, start a one-device\n"
        "                RTSPS camera server on the given bind/port, and\n"
        "                serve frames from the chosen source (default null —\n"
        "                emits a 16x16 black H.264 test pattern). Sleeps until\n"
        "                SIGINT / SIGTERM. Point ffplay/VLC at\n"
        "                rtsps://<bind>:<port>/streaming/live/1 to verify.\n"
        "\n"
        "  ftps --dev-id <id> --access-code <code> [--bind 127.0.0.1] [--port 9990]\n"
        "       [--cache-dir <path>] [--upload-dir <path>]\n"
        "                Mint a self-signed cert for <id>, start a one-device\n"
        "                FTPS server on the given bind/port, and log every\n"
        "                received .3mf to <upload-dir> (default\n"
        "                /tmp/bridge_uploads/<dev_id>/). Sleeps until SIGINT/\n"
        "                SIGTERM. Useful for confirming a real slicer can\n"
        "                complete an upload through the bridge.\n"
        "\n"
        "  proxy --dev-id <id> --access-code <code> --printer-ip <ip>\n"
        "        --plugin <path-to-libbambu_networking.so>\n"
        "        [--config-dir <path>] [--country-code <cc>]\n"
        "        [--bind 127.0.0.1] [--mqtt-port 38883] [--ftps-port 39990]\n"
        "        [--rtsp-port 38322] [--cache-dir <path>] [--upload-dir <path>]\n"
        "        [--name <n>] [--model <m>] [--firmware <fw>]\n"
        "                Phase-9 canonical command: stand up the FULL stack\n"
        "                (SSDP + MqttBroker + FtpsServer + RtspServer) against\n"
        "                one device, with SessionRouter / UploadSinkRouter /\n"
        "                CameraSourceRouter routing between LAN and cloud.\n"
        "                Runs until SIGINT / SIGTERM.\n"
        "\n"
        "  cloud-bridge --dev-id <id> --plugin <path-to-libbambu_networking.so>\n"
        "               --config-dir <path> [--country-code <cc>]\n"
        "               [--access-code <code>] [--bind 127.0.0.1] [--bind-port 38883]\n"
        "               [--cache-dir <path>] [--name <n>] [--model <m>] [--firmware <fw>]\n"
        "                Stand up the full phase-3+4+6 stack against one device:\n"
        "                SSDP announce + MqttBroker on bind:bind-port + CloudUplink\n"
        "                through the proprietary plugin. Runs until SIGINT/SIGTERM.\n"
        "                Useful for testing the cloud path without LAN access to\n"
        "                the printer.\n"
        "\n"
        "Environment:\n"
        "  BAMBU_BRIDGE_PLUGIN_PATH    Fallback for --plugin.\n"
        "  XDG_CONFIG_HOME             Used to resolve the default cert cache\n"
        "                              dir ($XDG_CONFIG_HOME/BambuStudio/bridge/certs).\n");
}

int cmd_list_devices(int argc, char** argv) {
    std::string config_dir;
    std::string plugin_path;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--config-dir") {
            const char* v = need_value("--config-dir");
            if (!v) return 2;
            config_dir = v;
        } else if (a == "--plugin") {
            const char* v = need_value("--plugin");
            if (!v) return 2;
            plugin_path = v;
        } else if (a == "-h" || a == "--help") {
            print_usage(stdout);
            return 0;
        } else {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
    }

    // CloudInventory has its own env/default probe, but we still try to
    // honour the documented CLI fallback list here so a user running with
    // neither flag *nor* env gets predictable behaviour.
    if (plugin_path.empty()) {
        if (const char* envv = std::getenv("BAMBU_BRIDGE_PLUGIN_PATH"); envv && *envv) {
            plugin_path = envv;
        }
    }

    Slic3r::bridge::CloudInventoryConfig cfg;
    cfg.plugin_path = plugin_path;
    cfg.config_dir  = config_dir;

    Slic3r::bridge::CloudInventory inventory(cfg);
    const bool ok = inventory.refresh();
    inventory.probe_lan_reachability();

    // TSV header always printed (so downstream parsers don't have to special-
    // case empty output).
    std::cout << "dev_id\tmodel\tname\tonline\tlan_ip\n";

    for (const auto& d : inventory.snapshot()) {
        std::cout
            << d.dev_id    << '\t'
            << d.model     << '\t'
            << d.name      << '\t'
            << (d.online ? "true" : "false") << '\t'
            << d.lan_ip
            << '\n';
    }

    if (!ok) {
        // Non-fatal: keep exit 0 so that "plugin absent / not logged in" is
        // discoverable from the TSV (empty body) without a noisy error code.
        // We do log a stderr breadcrumb though, so interactive users
        // understand why the list is empty.
        std::fprintf(stderr,
            "bridge-cli: no devices listed (plugin missing, not logged in, or cloud unreachable)\n");
    }
    return 0;
}

int cmd_mint_cert(int argc, char** argv) {
    std::string dev_id;
    std::string cache_dir;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--cache-dir") {
            const char* v = need_value("--cache-dir");
            if (!v) return 2;
            cache_dir = v;
        } else if (a == "-h" || a == "--help") {
            print_usage(stdout);
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        } else if (dev_id.empty()) {
            dev_id = a;
        } else {
            std::fprintf(stderr, "bridge-cli: unexpected positional arg '%s'\n", a.c_str());
            return 2;
        }
    }

    if (dev_id.empty()) {
        std::fprintf(stderr, "bridge-cli: mint-cert requires <dev_id>\n");
        print_usage(stderr);
        return 2;
    }

    Slic3r::bridge::tls::CertFactoryConfig cfg;
    if (!cache_dir.empty()) cfg.cache_dir = cache_dir;

    try {
        Slic3r::bridge::tls::CertFactory factory(cfg);
        const auto mat = factory.get_or_create(dev_id);
        std::cout
            << "dev_id\t"      << dev_id << '\n'
            << "cert_path\t"   << factory.cert_path(dev_id).string() << '\n'
            << "key_path\t"    << factory.key_path (dev_id).string() << '\n'
            << "fingerprint\t" << mat.fingerprint_sha256 << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: mint-cert failed: %s\n", ex.what());
        return 1;
    }
}

// Module-scope flag flipped from the SIGINT/SIGTERM handler so the announce
// loop can shut down cleanly (and emit ssdp:byebye) on Ctrl-C.
volatile std::sig_atomic_t g_announce_stop = 0;

extern "C" void on_announce_signal(int) { g_announce_stop = 1; }

int cmd_announce(int argc, char** argv) {
    std::string dev_id, name, model, firmware, lan_ip, bind_address;
    int         http_port = 80;
    int         duration  = 60;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--dev-id"       ) { auto v = need_value("--dev-id");        if (!v) return 2; dev_id     = v; }
        else if (a == "--name"         ) { auto v = need_value("--name");          if (!v) return 2; name       = v; }
        else if (a == "--model"        ) { auto v = need_value("--model");         if (!v) return 2; model      = v; }
        else if (a == "--firmware"     ) { auto v = need_value("--firmware");      if (!v) return 2; firmware   = v; }
        else if (a == "--lan-ip"       ) { auto v = need_value("--lan-ip");        if (!v) return 2; lan_ip     = v; }
        else if (a == "--bind-address" ) { auto v = need_value("--bind-address");  if (!v) return 2; bind_address = v; }
        else if (a == "--http-port"    ) { auto v = need_value("--http-port");     if (!v) return 2; http_port  = std::atoi(v); }
        else if (a == "--duration"     ) { auto v = need_value("--duration");      if (!v) return 2; duration   = std::atoi(v); }
        else if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        else {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
    }

    if (dev_id.empty() || name.empty() || model.empty() || firmware.empty() || lan_ip.empty()) {
        std::fprintf(stderr,
            "bridge-cli: announce requires --dev-id, --name, --model, --firmware, --lan-ip\n");
        print_usage(stderr);
        return 2;
    }

    using namespace Slic3r::bridge::server;
    SsdpResponderConfig cfg;
    if (!bind_address.empty()) cfg.bind_address = bind_address;
    SsdpResponder responder(cfg);

    SsdpVirtualDevice dev;
    dev.dev_id    = dev_id;
    dev.name      = name;
    dev.model     = model;
    dev.firmware  = firmware;
    dev.lan_ip    = lan_ip;
    dev.http_port = static_cast<uint16_t>(http_port);
    dev.bound     = true;
    dev.secure    = true;
    responder.add_device(dev);

    std::signal(SIGINT,  on_announce_signal);
    std::signal(SIGTERM, on_announce_signal);

    responder.start();
    if (!responder.running()) {
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
    while (!g_announce_stop && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    responder.stop();
    return 0;
}

// Reused for the broker subcommand below.
volatile std::sig_atomic_t g_broker_stop = 0;
extern "C" void on_broker_signal(int) { g_broker_stop = 1; }

int cmd_broker(int argc, char** argv) {
    std::string dev_id, access_code, bind = "127.0.0.1", cache_dir;
    int         port = 8883;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--dev-id")       { auto v = need_value("--dev-id");       if (!v) return 2; dev_id = v; }
        else if (a == "--access-code")  { auto v = need_value("--access-code");  if (!v) return 2; access_code = v; }
        else if (a == "--bind")         { auto v = need_value("--bind");         if (!v) return 2; bind = v; }
        else if (a == "--port")         { auto v = need_value("--port");         if (!v) return 2; port = std::atoi(v); }
        else if (a == "--cache-dir")    { auto v = need_value("--cache-dir");    if (!v) return 2; cache_dir = v; }
        else if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        else {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
    }
    if (dev_id.empty() || access_code.empty()) {
        std::fprintf(stderr,
            "bridge-cli: broker requires --dev-id and --access-code\n");
        print_usage(stderr);
        return 2;
    }

    using namespace Slic3r::bridge;

    tls::CertFactoryConfig cfg;
    if (!cache_dir.empty()) cfg.cache_dir = cache_dir;
    tls::CertMaterial cert;
    try {
        tls::CertFactory factory(cfg);
        cert = factory.get_or_create(dev_id);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: cert mint failed: %s\n", ex.what());
        return 1;
    }

    auto uplink = std::make_shared<router::NullUplink>();
    server::MqttBrokerConfig brkcfg;
    brkcfg.uplink                 = uplink;
    brkcfg.max_clients_per_device = 1;
    auto broker = std::make_unique<server::MqttBroker>(brkcfg);

    server::MqttBrokerVirtualDevice dev;
    dev.dev_id      = dev_id;
    dev.lan_ip      = bind;
    dev.port        = static_cast<uint16_t>(port);
    dev.access_code = access_code;
    dev.cert        = cert;

    try {
        broker->add_device(dev);
        broker->start();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: broker start failed: %s\n", ex.what());
        return 1;
    }
    const uint16_t bound = broker->bound_port(dev_id);

    std::signal(SIGINT,  on_broker_signal);
    std::signal(SIGTERM, on_broker_signal);
    while (!g_broker_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    broker->stop();
    return 0;
}

// ---- lan-bridge --------------------------------------------------------

volatile std::sig_atomic_t g_lanbridge_stop = 0;
extern "C" void on_lanbridge_signal(int) { g_lanbridge_stop = 1; }

int cmd_lan_bridge(int argc, char** argv) {
    std::string dev_id, printer_ip, access_code;
    std::string bind = "127.0.0.1";
    std::string cache_dir;
    std::string name     = "Bambu Bridge";
    std::string model    = "H2S";
    std::string firmware = "01.02.00.00";
    int         port      = 8883;
    int         bind_port = 38883;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--dev-id")       { auto v = need_value("--dev-id");       if (!v) return 2; dev_id = v; }
        else if (a == "--printer-ip")   { auto v = need_value("--printer-ip");   if (!v) return 2; printer_ip = v; }
        else if (a == "--port")         { auto v = need_value("--port");         if (!v) return 2; port = std::atoi(v); }
        else if (a == "--access-code")  { auto v = need_value("--access-code");  if (!v) return 2; access_code = v; }
        else if (a == "--bind")         { auto v = need_value("--bind");         if (!v) return 2; bind = v; }
        else if (a == "--bind-port")    { auto v = need_value("--bind-port");    if (!v) return 2; bind_port = std::atoi(v); }
        else if (a == "--cache-dir")    { auto v = need_value("--cache-dir");    if (!v) return 2; cache_dir = v; }
        else if (a == "--name")         { auto v = need_value("--name");         if (!v) return 2; name = v; }
        else if (a == "--model")        { auto v = need_value("--model");        if (!v) return 2; model = v; }
        else if (a == "--firmware")     { auto v = need_value("--firmware");     if (!v) return 2; firmware = v; }
        else if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        else {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
    }

    if (dev_id.empty() || printer_ip.empty() || access_code.empty()) {
        std::fprintf(stderr,
            "bridge-cli: lan-bridge requires --dev-id, --printer-ip, --access-code\n");
        print_usage(stderr);
        return 2;
    }

    using namespace Slic3r::bridge;

    // 1) Mint a self-signed cert for the bridge's MQTT broker (CN = dev_id).
    tls::CertFactoryConfig cfg;
    if (!cache_dir.empty()) cfg.cache_dir = cache_dir;
    tls::CertMaterial cert;
    try {
        tls::CertFactory factory(cfg);
        cert = factory.get_or_create(dev_id);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: cert mint failed: %s\n", ex.what());
        return 1;
    }

    // 2) LanUplink first. Without a proprietary plugin handle this is a
    //    no-op — LAN now requires the plugin. The `lan-bridge` subcommand
    //    is mostly a relic of the pre-plugin LAN path; warn loudly.
    auto lan_uplink = std::make_shared<router::LanUplink>();
    router::LanUplinkConfig lcfg;
    lcfg.dev_id       = dev_id;
    lcfg.printer_ip   = printer_ip;
    lcfg.printer_port = static_cast<uint16_t>(port);
    lcfg.access_code  = access_code;
    lan_uplink->add_device(lcfg);

    // 3) MqttBroker pointing at the LanUplink.
    server::MqttBrokerConfig brkcfg;
    brkcfg.uplink                 = lan_uplink;
    brkcfg.max_clients_per_device = 1;
    auto broker = std::make_unique<server::MqttBroker>(brkcfg);

    server::MqttBrokerVirtualDevice dev;
    dev.dev_id      = dev_id;
    dev.lan_ip      = bind;
    dev.port        = static_cast<uint16_t>(bind_port);
    dev.access_code = access_code;
    dev.cert        = cert;
    try {
        broker->add_device(dev);
        broker->start();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: broker start failed: %s\n", ex.what());
        return 1;
    }
    const uint16_t bound = broker->bound_port(dev_id);

    // 4) SSDP responder announcing the bridge as the virtual printer.
    server::SsdpResponderConfig sscfg;
    auto responder = std::make_unique<server::SsdpResponder>(sscfg);
    server::SsdpVirtualDevice sdev;
    sdev.dev_id    = dev_id;
    sdev.name      = name;
    sdev.model     = model;
    sdev.firmware  = firmware;
    sdev.lan_ip    = bind;
    sdev.http_port = bound;
    sdev.bound     = true;
    sdev.secure    = true;
    responder->add_device(sdev);
    responder->start();
    if (!responder->running()) {
    }

    std::signal(SIGINT,  on_lanbridge_signal);
    std::signal(SIGTERM, on_lanbridge_signal);
    while (!g_lanbridge_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (responder) responder->stop();
    broker->stop();
    lan_uplink->remove_device(dev_id);
    return 0;
}

// ---- cloud-bridge ------------------------------------------------------

volatile std::sig_atomic_t g_cloudbridge_stop = 0;
extern "C" void on_cloudbridge_signal(int) { g_cloudbridge_stop = 1; }

int cmd_cloud_bridge(int argc, char** argv) {
    std::string dev_id, plugin_path, config_dir, country_code, access_code;
    std::string bind = "127.0.0.1";
    std::string cache_dir;
    std::string name     = "Bambu Bridge";
    std::string model    = "H2S";
    std::string firmware = "01.02.00.00";
    int         bind_port = 38883;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--dev-id")       { auto v = need_value("--dev-id");       if (!v) return 2; dev_id = v; }
        else if (a == "--plugin")       { auto v = need_value("--plugin");       if (!v) return 2; plugin_path = v; }
        else if (a == "--config-dir")   { auto v = need_value("--config-dir");   if (!v) return 2; config_dir = v; }
        else if (a == "--country-code") { auto v = need_value("--country-code"); if (!v) return 2; country_code = v; }
        else if (a == "--access-code")  { auto v = need_value("--access-code");  if (!v) return 2; access_code = v; }
        else if (a == "--bind")         { auto v = need_value("--bind");         if (!v) return 2; bind = v; }
        else if (a == "--bind-port")    { auto v = need_value("--bind-port");    if (!v) return 2; bind_port = std::atoi(v); }
        else if (a == "--cache-dir")    { auto v = need_value("--cache-dir");    if (!v) return 2; cache_dir = v; }
        else if (a == "--name")         { auto v = need_value("--name");         if (!v) return 2; name = v; }
        else if (a == "--model")        { auto v = need_value("--model");        if (!v) return 2; model = v; }
        else if (a == "--firmware")     { auto v = need_value("--firmware");     if (!v) return 2; firmware = v; }
        else if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        else {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
    }

    if (dev_id.empty() || plugin_path.empty() || config_dir.empty()) {
        std::fprintf(stderr,
            "bridge-cli: cloud-bridge requires --dev-id, --plugin, --config-dir\n");
        print_usage(stderr);
        return 2;
    }

    using namespace Slic3r::bridge;

    // 1) Bring up the shared plugin handle.
    PluginHandleConfig hcfg;
    hcfg.plugin_path  = plugin_path;
    hcfg.config_dir   = config_dir;
    hcfg.country_code = country_code;
    auto handle = std::make_shared<BambuNetworkingPluginHandle>(hcfg);
    if (!handle->init()) {
        std::fprintf(stderr,
            "bridge-cli: failed to init plugin '%s' (missing or wrong arch?)\n",
            plugin_path.c_str());
        return 1;
    }

    // 2) Mint a self-signed cert for the bridge's MQTT broker.
    tls::CertFactoryConfig ccfg;
    if (!cache_dir.empty()) ccfg.cache_dir = cache_dir;
    tls::CertMaterial cert;
    try {
        tls::CertFactory factory(ccfg);
        cert = factory.get_or_create(dev_id);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: cert mint failed: %s\n", ex.what());
        return 1;
    }

    // 3) CloudUplink wired against the shared handle.
    auto cloud_uplink = std::make_shared<router::CloudUplink>();
    cloud_uplink->attach_plugin(handle);
    router::CloudUplinkConfig ucfg;
    ucfg.dev_id      = dev_id;
    ucfg.access_code = access_code;
    cloud_uplink->add_device(ucfg);

    // 4) MqttBroker pointing at the CloudUplink (phase-6 single-uplink
    //    mode; phase-9 SessionRouter will replace this with a fan-out).
    server::MqttBrokerConfig brkcfg;
    brkcfg.uplink                 = cloud_uplink;
    brkcfg.max_clients_per_device = 1;
    auto broker = std::make_unique<server::MqttBroker>(brkcfg);

    server::MqttBrokerVirtualDevice dev;
    dev.dev_id      = dev_id;
    dev.lan_ip      = bind;
    dev.port        = static_cast<uint16_t>(bind_port);
    dev.access_code = access_code;
    dev.cert        = cert;
    try {
        broker->add_device(dev);
        broker->start();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: broker start failed: %s\n", ex.what());
        return 1;
    }
    const uint16_t bound = broker->bound_port(dev_id);

    // 5) SSDP responder announcing the bridge as the virtual printer.
    server::SsdpResponderConfig sscfg;
    auto responder = std::make_unique<server::SsdpResponder>(sscfg);
    server::SsdpVirtualDevice sdev;
    sdev.dev_id    = dev_id;
    sdev.name      = name;
    sdev.model     = model;
    sdev.firmware  = firmware;
    sdev.lan_ip    = bind;
    sdev.http_port = bound;
    sdev.bound     = true;
    sdev.secure    = true;
    responder->add_device(sdev);
    responder->start();
    if (!responder->running()) {
    }

    std::signal(SIGINT,  on_cloudbridge_signal);
    std::signal(SIGTERM, on_cloudbridge_signal);
    while (!g_cloudbridge_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (responder) responder->stop();
    broker->stop();
    cloud_uplink->remove_device(dev_id);
    return 0;
}

// ---- ftps --------------------------------------------------------------
//
// Sink that writes every received UploadJob to <upload-dir>/<filename>
// (one breadcrumb to stderr per upload). Used only by `bridge-cli ftps`.

class DiskLoggingUploadSink final : public Slic3r::bridge::server::IUploadSink {
public:
    explicit DiskLoggingUploadSink(std::filesystem::path dir)
        : m_dir(std::move(dir)) {
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
    }
    Slic3r::bridge::server::UploadResult deliver(
        Slic3r::bridge::server::UploadJob job) override {
        Slic3r::bridge::server::UploadResult r;
        auto dest = m_dir / job.filename;
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            r.ok            = false;
            r.error_message = "could not open " + dest.string() + " for write";
            return r;
        }
        if (!job.content.empty()) {
            out.write(reinterpret_cast<const char*>(job.content.data()),
                      static_cast<std::streamsize>(job.content.size()));
        }
        out.close();
        r.ok         = true;
        r.remote_url = "file://" + dest.string();
        return r;
    }
private:
    std::filesystem::path m_dir;
};

volatile std::sig_atomic_t g_ftps_stop = 0;
extern "C" void on_ftps_signal(int) { g_ftps_stop = 1; }

int cmd_ftps(int argc, char** argv) {
    std::string dev_id, access_code, bind = "127.0.0.1", cache_dir, upload_dir;
    int         port = 9990;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--dev-id")      { auto v = need_value("--dev-id");      if (!v) return 2; dev_id = v; }
        else if (a == "--access-code") { auto v = need_value("--access-code"); if (!v) return 2; access_code = v; }
        else if (a == "--bind")        { auto v = need_value("--bind");        if (!v) return 2; bind = v; }
        else if (a == "--port")        { auto v = need_value("--port");        if (!v) return 2; port = std::atoi(v); }
        else if (a == "--cache-dir")   { auto v = need_value("--cache-dir");   if (!v) return 2; cache_dir = v; }
        else if (a == "--upload-dir")  { auto v = need_value("--upload-dir");  if (!v) return 2; upload_dir = v; }
        else if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        else {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
    }
    if (dev_id.empty() || access_code.empty()) {
        std::fprintf(stderr,
            "bridge-cli: ftps requires --dev-id and --access-code\n");
        print_usage(stderr);
        return 2;
    }

    using namespace Slic3r::bridge;

    tls::CertFactoryConfig cfg;
    if (!cache_dir.empty()) cfg.cache_dir = cache_dir;
    tls::CertMaterial cert;
    try {
        tls::CertFactory factory(cfg);
        cert = factory.get_or_create(dev_id);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: cert mint failed: %s\n", ex.what());
        return 1;
    }

    std::filesystem::path udir = upload_dir.empty()
        ? std::filesystem::path("/tmp/bridge_uploads") / dev_id
        : std::filesystem::path(upload_dir);
    auto sink = std::make_shared<DiskLoggingUploadSink>(udir);

    server::FtpsServerConfig fcfg;
    fcfg.sink              = sink;
    fcfg.pasv_advertise_ip = bind;
    auto srv = std::make_unique<server::FtpsServer>(fcfg);

    server::FtpsVirtualDevice fdev;
    fdev.dev_id      = dev_id;
    fdev.lan_ip      = bind;
    fdev.port        = static_cast<uint16_t>(port);
    fdev.access_code = access_code;
    fdev.cert        = cert;

    try {
        srv->add_device(fdev);
        srv->start();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: ftps start failed: %s\n", ex.what());
        return 1;
    }
    const uint16_t bound = srv->bound_port(dev_id);

    std::signal(SIGINT,  on_ftps_signal);
    std::signal(SIGTERM, on_ftps_signal);
    while (!g_ftps_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    srv->stop();
    return 0;
}

// ---- rtsp ---------------------------------------------------------------

volatile std::sig_atomic_t g_rtsp_stop = 0;
extern "C" void on_rtsp_signal(int) { g_rtsp_stop = 1; }

int cmd_rtsp(int argc, char** argv) {
    std::string dev_id, access_code, bind = "127.0.0.1", cache_dir;
    std::string source_kind = "null";
    std::string printer_ip;
    int         port         = 38322;
    int         printer_port = 322;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--dev-id")       { auto v = need_value("--dev-id");       if (!v) return 2; dev_id = v; }
        else if (a == "--access-code")  { auto v = need_value("--access-code");  if (!v) return 2; access_code = v; }
        else if (a == "--bind")         { auto v = need_value("--bind");         if (!v) return 2; bind = v; }
        else if (a == "--port")         { auto v = need_value("--port");         if (!v) return 2; port = std::atoi(v); }
        else if (a == "--cache-dir")    { auto v = need_value("--cache-dir");    if (!v) return 2; cache_dir = v; }
        else if (a == "--source")       { auto v = need_value("--source");       if (!v) return 2; source_kind = v; }
        else if (a == "--printer-ip")   { auto v = need_value("--printer-ip");   if (!v) return 2; printer_ip = v; }
        else if (a == "--printer-port") { auto v = need_value("--printer-port"); if (!v) return 2; printer_port = std::atoi(v); }
        else if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        else {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
    }
    if (dev_id.empty() || access_code.empty()) {
        std::fprintf(stderr,
            "bridge-cli: rtsp requires --dev-id and --access-code\n");
        print_usage(stderr);
        return 2;
    }

    using namespace Slic3r::bridge;

    tls::CertFactoryConfig cfg;
    if (!cache_dir.empty()) cfg.cache_dir = cache_dir;
    tls::CertMaterial cert;
    try {
        tls::CertFactory factory(cfg);
        cert = factory.get_or_create(dev_id);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: cert mint failed: %s\n", ex.what());
        return 1;
    }

    std::shared_ptr<server::ICameraSource> source;
    if (source_kind == "null") {
        source = std::make_shared<router::NullCameraSource>();
    } else if (source_kind == "lan") {
        if (printer_ip.empty()) {
            std::fprintf(stderr,
                "bridge-cli: rtsp --source lan requires --printer-ip\n");
            return 2;
        }
        router::LanCameraSourceConfig lcfg;
        lcfg.dev_id       = dev_id;
        lcfg.printer_ip   = printer_ip;
        lcfg.printer_port = static_cast<uint16_t>(printer_port);
        lcfg.access_code  = access_code;
        source = std::make_shared<router::LanCameraSource>(lcfg);
    } else if (source_kind == "cloud") {
        router::CloudCameraSourceConfig ccfg;
        ccfg.dev_id = dev_id;
        source = std::make_shared<router::CloudCameraSource>(ccfg);
    } else {
        std::fprintf(stderr,
            "bridge-cli: rtsp --source must be null|lan|cloud (got %s)\n",
            source_kind.c_str());
        return 2;
    }

    server::RtspServerConfig rcfg;
    auto srv = std::make_unique<server::RtspServer>(rcfg);

    server::RtspVirtualDevice rdev;
    rdev.dev_id      = dev_id;
    rdev.lan_ip      = bind;
    rdev.port        = static_cast<uint16_t>(port);
    rdev.access_code = access_code;
    rdev.cert        = cert;
    rdev.source      = source;

    try {
        srv->add_device(rdev);
        srv->start();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: rtsp start failed: %s\n", ex.what());
        return 1;
    }
    const uint16_t bound = srv->bound_port(dev_id);

    std::signal(SIGINT,  on_rtsp_signal);
    std::signal(SIGTERM, on_rtsp_signal);
    while (!g_rtsp_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    srv->stop();
    return 0;
}

// ---- proxy --------------------------------------------------------------
//
// Canonical "run the bridge against ONE printer" command. Phase 9's full
// stack: SSDP + MqttBroker + FtpsServer + RtspServer, all routed through
// SessionRouter / UploadSinkRouter / CameraSourceRouter with both
// LanUplink and CloudUplink (via the proprietary plugin) attached.
//
// Reuses BridgeService for lifecycle. The cloud uplink/sink/source are
// only wired if --plugin is provided AND the plugin loads; otherwise the
// proxy is LAN-only (the routers degrade to LAN-only by themselves).

volatile std::sig_atomic_t g_proxy_stop = 0;
extern "C" void on_proxy_signal(int) { g_proxy_stop = 1; }

class DiskLoggingUploadSinkProxy final : public Slic3r::bridge::server::IUploadSink {
public:
    explicit DiskLoggingUploadSinkProxy(std::filesystem::path dir)
        : m_dir(std::move(dir)) {
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
    }
    Slic3r::bridge::server::UploadResult deliver(
        Slic3r::bridge::server::UploadJob job) override {
        Slic3r::bridge::server::UploadResult r;
        auto dest = m_dir / job.filename;
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            r.ok            = false;
            r.error_message = "could not open " + dest.string() + " for write";
            return r;
        }
        if (!job.content.empty()) {
            out.write(reinterpret_cast<const char*>(job.content.data()),
                      static_cast<std::streamsize>(job.content.size()));
        }
        out.close();
        r.ok         = true;
        r.remote_url = "file://" + dest.string();
        return r;
    }
private:
    std::filesystem::path m_dir;
};

int cmd_proxy(int argc, char** argv) {
    std::string dev_id, access_code, printer_ip;
    std::string plugin_path, config_dir, country_code;
    std::string bind = "127.0.0.1";
    std::string cache_dir, upload_dir;
    std::string name     = "Bambu Bridge";
    std::string model    = "H2S";
    std::string firmware = "01.02.00.00";
    int         mqtt_port = 38883;
    int         ftps_port = 39990;
    int         rtsp_port = 38322;
    int         printer_mqtt_port = 8883;
    int         printer_ftps_port = 990;
    int         printer_rtsp_port = 322;

    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "bridge-cli: %s requires a value\n", flag);
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--dev-id")       { auto v = need_value("--dev-id");       if (!v) return 2; dev_id = v; }
        else if (a == "--access-code")  { auto v = need_value("--access-code");  if (!v) return 2; access_code = v; }
        else if (a == "--printer-ip")   { auto v = need_value("--printer-ip");   if (!v) return 2; printer_ip = v; }
        else if (a == "--plugin")       { auto v = need_value("--plugin");       if (!v) return 2; plugin_path = v; }
        else if (a == "--config-dir")   { auto v = need_value("--config-dir");   if (!v) return 2; config_dir = v; }
        else if (a == "--country-code") { auto v = need_value("--country-code"); if (!v) return 2; country_code = v; }
        else if (a == "--bind")         { auto v = need_value("--bind");         if (!v) return 2; bind = v; }
        else if (a == "--mqtt-port")    { auto v = need_value("--mqtt-port");    if (!v) return 2; mqtt_port = std::atoi(v); }
        else if (a == "--ftps-port")    { auto v = need_value("--ftps-port");    if (!v) return 2; ftps_port = std::atoi(v); }
        else if (a == "--rtsp-port")    { auto v = need_value("--rtsp-port");    if (!v) return 2; rtsp_port = std::atoi(v); }
        else if (a == "--cache-dir")    { auto v = need_value("--cache-dir");    if (!v) return 2; cache_dir = v; }
        else if (a == "--upload-dir")   { auto v = need_value("--upload-dir");   if (!v) return 2; upload_dir = v; }
        else if (a == "--name")         { auto v = need_value("--name");         if (!v) return 2; name = v; }
        else if (a == "--model")        { auto v = need_value("--model");        if (!v) return 2; model = v; }
        else if (a == "--firmware")     { auto v = need_value("--firmware");     if (!v) return 2; firmware = v; }
        else if (a == "-h" || a == "--help") { print_usage(stdout); return 0; }
        else {
            std::fprintf(stderr, "bridge-cli: unknown option '%s'\n", a.c_str());
            print_usage(stderr);
            return 2;
        }
    }
    if (dev_id.empty() || access_code.empty() || printer_ip.empty()) {
        std::fprintf(stderr,
            "bridge-cli: proxy requires --dev-id, --access-code, --printer-ip\n");
        print_usage(stderr);
        return 2;
    }

    using namespace Slic3r::bridge;

    // 1) Self-signed cert.
    tls::CertFactoryConfig ccfg;
    if (!cache_dir.empty()) ccfg.cache_dir = cache_dir;
    tls::CertMaterial cert;
    try {
        tls::CertFactory factory(ccfg);
        cert = factory.get_or_create(dev_id);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: cert mint failed: %s\n", ex.what());
        return 1;
    }

    // 2) Optional plugin handle (powers CloudUplink + CloudUploadSink + CloudCameraSource).
    std::shared_ptr<BambuNetworkingPluginHandle> plugin;
    if (!plugin_path.empty()) {
        PluginHandleConfig hcfg;
        hcfg.plugin_path  = plugin_path;
        hcfg.config_dir   = config_dir;
        hcfg.country_code = country_code;
        plugin = std::make_shared<BambuNetworkingPluginHandle>(hcfg);
        if (!plugin->init()) {
            plugin.reset();
        } else {
        }
    } else {
    }

    // 3) LanUplink to the real printer. The LAN side now also routes
    //    through the plugin (connect_printer + send_message_to_printer);
    //    without a plugin, LanUplink degrades to a no-op.
    auto lan_uplink = std::make_shared<router::LanUplink>();
    if (plugin) lan_uplink->attach_plugin(plugin);
    router::LanUplinkConfig lucfg;
    lucfg.dev_id       = dev_id;
    lucfg.printer_ip   = printer_ip;
    lucfg.printer_port = static_cast<uint16_t>(printer_mqtt_port);
    lucfg.access_code  = access_code;
    lan_uplink->add_device(lucfg);

    // 4) CloudUplink (if plugin loaded).
    std::shared_ptr<router::CloudUplink> cloud_uplink;
    if (plugin) {
        cloud_uplink = std::make_shared<router::CloudUplink>();
        cloud_uplink->attach_plugin(plugin);
        router::CloudUplinkConfig cucfg;
        cucfg.dev_id      = dev_id;
        cucfg.access_code = access_code;
        cloud_uplink->add_device(cucfg);
    }

    // 5) UplinkHealthMonitor + SessionRouter.
    auto health = std::make_shared<router::UplinkHealthMonitor>();
    health->set_lan_uplink(lan_uplink);
    if (cloud_uplink) health->set_cloud_uplink(cloud_uplink);

    auto session_router = std::make_shared<router::SessionRouter>();
    session_router->set_lan_uplink(lan_uplink);
    if (cloud_uplink) session_router->set_cloud_uplink(cloud_uplink);
    session_router->set_health_monitor(health);

    // 6) Upload sinks + UploadSinkRouter.
    auto lan_sink = std::make_shared<router::LanUploadSink>();
    if (plugin) lan_sink->attach_plugin(plugin);
    router::LanUploadSinkDevice lsd;
    lsd.dev_id       = dev_id;
    lsd.printer_ip   = printer_ip;
    lsd.printer_port = static_cast<uint16_t>(printer_ftps_port);
    lsd.access_code  = access_code;
    lan_sink->add_device(lsd);

    std::shared_ptr<router::CloudUploadSink> cloud_sink;
    if (plugin) {
        cloud_sink = std::make_shared<router::CloudUploadSink>();
        cloud_sink->attach_plugin(plugin);
    }

    auto upload_router = std::make_shared<router::UploadSinkRouter>();
    upload_router->set_lan_sink(lan_sink);
    if (cloud_sink) upload_router->set_cloud_sink(cloud_sink);
    upload_router->set_health_monitor(health);

    // Local disk archive of everything that comes through.
    std::filesystem::path udir = upload_dir.empty()
        ? std::filesystem::path("/tmp/bridge_uploads") / dev_id
        : std::filesystem::path(upload_dir);
    auto disk_sink = std::make_shared<DiskLoggingUploadSinkProxy>(udir);

    // 7) BridgeService + servers.
    BridgeService service;

    // MQTT broker.
    server::MqttBrokerConfig brkcfg;
    brkcfg.uplink                 = nullptr; // SessionRouter installed via start().
    brkcfg.max_clients_per_device = 1;
    auto broker = std::make_unique<server::MqttBroker>(brkcfg);
    server::MqttBrokerVirtualDevice mdev;
    mdev.dev_id      = dev_id;
    mdev.lan_ip      = bind;
    mdev.port        = static_cast<uint16_t>(mqtt_port);
    mdev.access_code = access_code;
    mdev.cert        = cert;
    broker->add_device(mdev);

    // FTPS server. Its initial sink is the disk logger; BridgeService::start()
    // replaces it with the UploadSinkRouter before listeners come up.
    server::FtpsServerConfig fcfg;
    fcfg.sink              = disk_sink;
    fcfg.pasv_advertise_ip = bind;
    auto ftps = std::make_unique<server::FtpsServer>(fcfg);
    server::FtpsVirtualDevice fdev;
    fdev.dev_id      = dev_id;
    fdev.lan_ip      = bind;
    fdev.port        = static_cast<uint16_t>(ftps_port);
    fdev.access_code = access_code;
    fdev.cert        = cert;
    ftps->add_device(fdev);

    // RTSP server with a CameraSourceRouter per device.
    server::RtspServerConfig rcfg;
    auto rtsp = std::make_unique<server::RtspServer>(rcfg);

    auto cam_router = std::make_shared<router::CameraSourceRouter>(dev_id);
    auto lan_cam = std::make_shared<router::LanCameraSource>(
        router::LanCameraSourceConfig{dev_id, printer_ip,
            static_cast<uint16_t>(printer_rtsp_port), access_code,
            std::chrono::seconds(5), std::chrono::seconds(30)});
    cam_router->set_lan_source(lan_cam);
    if (plugin) {
        auto cloud_cam = std::make_shared<router::CloudCameraSource>(
            router::CloudCameraSourceConfig{dev_id, std::chrono::seconds(10)});
        cloud_cam->attach_plugin(plugin);
        cam_router->set_cloud_source(cloud_cam);
    }
    auto null_cam = std::make_shared<router::NullCameraSource>();
    cam_router->set_null_source(null_cam);
    cam_router->set_health_monitor(health);
    router::CameraSourceRouter::Policy campol;
    campol.prefer_lan          = true;
    campol.allow_null_fallback = true; // demos / tests want a test pattern fallback.
    cam_router->set_policy(campol);

    server::RtspVirtualDevice rdev;
    rdev.dev_id      = dev_id;
    rdev.lan_ip      = bind;
    rdev.port        = static_cast<uint16_t>(rtsp_port);
    rdev.access_code = access_code;
    rdev.cert        = cert;
    rdev.source      = cam_router;
    rtsp->add_device(rdev);

    // SSDP responder.
    server::SsdpResponderConfig sscfg;
    auto responder = std::make_unique<server::SsdpResponder>(sscfg);
    server::SsdpVirtualDevice sdev;
    sdev.dev_id    = dev_id;
    sdev.name      = name;
    sdev.model     = model;
    sdev.firmware  = firmware;
    sdev.lan_ip    = bind;
    sdev.http_port = static_cast<uint16_t>(mqtt_port);
    sdev.bound     = true;
    sdev.secure    = true;
    responder->add_device(sdev);

    // 8) Install everything on the service. Order matters here: routers
    //    AND uplinks must be set BEFORE start() because start() wires
    //    them into the broker/ftps server.
    service.set_lan_uplink(lan_uplink);
    if (cloud_uplink) service.set_cloud_uplink(cloud_uplink);
    service.set_uplink_health_monitor(health);
    service.set_session_router(session_router);
    service.set_upload_sink_router(upload_router);
    service.set_camera_source_router(cam_router);
    service.set_ssdp_responder(std::move(responder));
    service.set_mqtt_broker(std::move(broker));
    service.set_ftps_server(std::move(ftps));
    service.set_rtsp_server(std::move(rtsp));

    try {
        service.start();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "bridge-cli: service start failed: %s\n", ex.what());
        return 1;
    }

    const uint16_t bound_mqtt = service.mqtt_broker() ? service.mqtt_broker()->bound_port(dev_id) : 0;
    const uint16_t bound_ftps = service.ftps_server() ? service.ftps_server()->bound_port(dev_id) : 0;
    const uint16_t bound_rtsp = service.rtsp_server() ? service.rtsp_server()->bound_port(dev_id) : 0;

    std::signal(SIGINT,  on_proxy_signal);
    std::signal(SIGTERM, on_proxy_signal);
    while (!g_proxy_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    service.stop();
    lan_uplink->remove_device(dev_id);
    if (cloud_uplink) cloud_uplink->remove_device(dev_id);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }
    const std::string sub = argv[1];
    if (sub == "list-devices") {
        return cmd_list_devices(argc - 2, argv + 2);
    }
    if (sub == "mint-cert") {
        return cmd_mint_cert(argc - 2, argv + 2);
    }
    if (sub == "announce") {
        return cmd_announce(argc - 2, argv + 2);
    }
    if (sub == "broker") {
        return cmd_broker(argc - 2, argv + 2);
    }
    if (sub == "lan-bridge") {
        return cmd_lan_bridge(argc - 2, argv + 2);
    }
    if (sub == "cloud-bridge") {
        return cmd_cloud_bridge(argc - 2, argv + 2);
    }
    if (sub == "ftps") {
        return cmd_ftps(argc - 2, argv + 2);
    }
    if (sub == "rtsp") {
        return cmd_rtsp(argc - 2, argv + 2);
    }
    if (sub == "proxy") {
        return cmd_proxy(argc - 2, argv + 2);
    }
    if (sub == "-h" || sub == "--help" || sub == "help") {
        print_usage(stdout);
        return 0;
    }
    std::fprintf(stderr, "bridge-cli: unknown subcommand '%s'\n", sub.c_str());
    print_usage(stderr);
    return 2;
}
