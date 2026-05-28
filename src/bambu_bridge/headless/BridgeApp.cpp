// Bambu Bridge — headless multi-device orchestrator (phase 10).
//
// See BridgeApp.hpp for the high-level design / contract.

#include "BridgeApp.hpp"

#include "../BambuNetworkingPluginHandle.hpp"
#include "../BambuSourceHandle.hpp"
#include "../CloudInventory.hpp"
#include "../tls/CertFactory.hpp"
#include "../server/SsdpResponder.hpp"
#include "../server/SsdpListener.hpp"
#include "../server/MqttBroker.hpp"
#include "../server/FtpsServer.hpp"
#include "../server/RtspServer.hpp"
#include "../server/TranscodingCameraSource.hpp"
#include "../server/VirtualTunnelServer.hpp"
#include "../server/IUplink.hpp"
#include "../server/IUploadSink.hpp"
#include "../server/ICameraSource.hpp"
#include "../router/LanUplink.hpp"
#include "../router/CloudUplink.hpp"
#include "../router/LanUploadSink.hpp"
#include "../router/CloudUploadSink.hpp"
#include "../router/LanCameraSource.hpp"
#include "../router/CloudCameraSource.hpp"
#include "../router/JpegCameraSource.hpp"
#include "../router/NullCameraSource.hpp"
#include "../router/SessionRouter.hpp"
#include "../router/UploadSinkRouter.hpp"
#include "../router/CameraSourceRouter.hpp"
#include "../router/UplinkHealth.hpp"
#include "../router/NativeStorageDelegate.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>

namespace Slic3r {
namespace bridge {
namespace headless {

// Per-printer mTLS cert+key resolution for the cert-bypass control-
// command publish path. `install_device_cert()` extracts the leaf cert
// and matching RSA key from the slicer's plugin heap; we cache them
// on disk in a well-known directory and look them up by dev_id suffix.
//
// Layout (well-known across all bridge installs as of 2026-05-22):
//   /tmp/bbl_capture/mtls.fresh/paired/<TAG>_<dev_id>_chain.pem
//   /tmp/bbl_capture/mtls.fresh/paired/<TAG>_<dev_id>_key.pem
// where <TAG> is the human model name ("H2S", "A1", "H2D"). Files are
// matched on the `_<dev_id>_chain.pem` / `_<dev_id>_key.pem` suffix so
// the bridge doesn't need a model→tag map.
//
// Override via BBL_BRIDGE_MTLS_DIR=/path. Empty = use default.
//
// Returns {cert_path, key_path}; both empty if no files found. Empty
// values cause `LanUplink::on_publish` to fall back to the plugin path
// for print.* publishes (which silently drops them — but at least the
// non-control path stays operational).
static std::pair<std::string, std::string>
resolve_mtls_paths(const std::string& dev_id) {
    std::string dir = "/tmp/bbl_capture/mtls.fresh/paired";
    if (const char* env = std::getenv("BBL_BRIDGE_MTLS_DIR");
        env && *env) {
        dir = env;
    }
    // Allow explicit per-dev override:
    //   BBL_BRIDGE_MTLS_CERT_<dev_id>=/abs/path/chain.pem
    //   BBL_BRIDGE_MTLS_KEY_<dev_id>=/abs/path/key.pem
    std::string cert_env_key = "BBL_BRIDGE_MTLS_CERT_" + dev_id;
    std::string key_env_key  = "BBL_BRIDGE_MTLS_KEY_"  + dev_id;
    const char* ec = std::getenv(cert_env_key.c_str());
    const char* ek = std::getenv(key_env_key.c_str());
    if (ec && *ec && ek && *ek) {
        struct stat st;
        if (::stat(ec, &st) == 0 && ::stat(ek, &st) == 0) {
            return {ec, ek};
        }
    }
    // Scan the directory for files ending in
    // `_<dev_id>_chain.pem` / `_<dev_id>_key.pem`.
    DIR* d = ::opendir(dir.c_str());
    if (!d) return {{}, {}};
    std::string cert_path, key_path;
    const std::string chain_suffix = "_" + dev_id + "_chain.pem";
    const std::string key_suffix   = "_" + dev_id + "_key.pem";
    while (struct dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        auto ends_with = [&](const std::string& suf) {
            return name.size() >= suf.size() &&
                   name.compare(name.size() - suf.size(),
                                suf.size(), suf) == 0;
        };
        if (cert_path.empty() && ends_with(chain_suffix)) {
            cert_path = dir + "/" + name;
        } else if (key_path.empty() && ends_with(key_suffix)) {
            key_path = dir + "/" + name;
        }
        if (!cert_path.empty() && !key_path.empty()) break;
    }
    ::closedir(d);
    return {cert_path, key_path};
}

// Walks the host's network interfaces and returns the first non-loopback
// IPv4 address as a dotted string. Used to fill the SSDP NOTIFY's
// LOCATION header so we don't advertise `http://0.0.0.0:...` (which a
// strict slicer may reject).
static std::string detect_primary_lan_ip() {
    struct ifaddrs* ifa_list = nullptr;
    if (::getifaddrs(&ifa_list) != 0) return {};
    std::string best;
    for (auto* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if ((ifa->ifa_flags & IFF_UP) == 0)        continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)  continue;
        auto* in = reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {0};
        if (!::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf))) continue;
        std::string ip = buf;
        // Skip docker/podman/libvirt/virbr bridges; we want the user's
        // real LAN interface.
        const std::string ifname = ifa->ifa_name ? ifa->ifa_name : "";
        if (ifname.rfind("docker", 0) == 0)  continue;
        if (ifname.rfind("br-",    0) == 0)  continue;
        if (ifname.rfind("virbr",  0) == 0)  continue;
        if (ifname.rfind("veth",   0) == 0)  continue;
        best = ip;
        break;
    }
    ::freeifaddrs(ifa_list);
    return best;
}

// Build the virtual serial advertised in SSDP from the real serial.
// The slicer's NetworkAgent matches on this prefix to decide that the
// connection is a virtual-bridge target and to route LAN MQTT through
// the open-source VirtualMqttClient instead of the proprietary plugin
// (see src/slic3r/Utils/NetworkAgent.cpp). Keeping the rule simple — a
// hardcoded "FFFF" replacing the first 4 chars — means we don't need a
// runtime registry on the slicer side; a string prefix test is enough.
//
//   03900D610219434 -> FFFF0D610219434
//   0938BC582502312 -> FFFF8C582502312
//
// "FFFF" is improbable as the start of any real Bambu serial (every
// observed device starts with "0"), it's still hex so it passes the
// slicer's serial-format regex, and it keeps the total length at the
// expected 15 chars when the real serial is 15+ chars.
static const char* const kVirtualSerialPrefix = "FFFF";

static std::string mangle_serial(const std::string& real_sn) {
    constexpr std::size_t kPrefixLen = 4;
    if (real_sn.size() <= kPrefixLen) {
        // Shorter than the prefix — degrade gracefully by just
        // pre-pending. Slicer regex may then reject, but at least the
        // value is recognisable as virtual.
        return std::string(kVirtualSerialPrefix) + real_sn;
    }
    return std::string(kVirtualSerialPrefix) +
           real_sn.substr(kPrefixLen);
}

BridgeApp::BridgeApp(BridgeAppConfig cfg) : m_cfg(std::move(cfg)) {}

BridgeApp::~BridgeApp() {
    shutdown();
    if (m_poll_thread.joinable()) m_poll_thread.join();
    teardown();
}

void BridgeApp::set_plugin_handle_for_test(
    std::shared_ptr<BambuNetworkingPluginHandle> handle) {
    m_plugin          = std::move(handle);
    m_plugin_injected = (m_plugin != nullptr);
}

void BridgeApp::set_bambu_source_handle_for_test(
    std::shared_ptr<BambuSourceHandle> handle) {
    m_bambu_source          = std::move(handle);
    m_bambu_source_injected = (m_bambu_source != nullptr);
}

void BridgeApp::attach_storage_delegate(
    StorageDelegate                         delegate,
    std::function<void(const std::string&)> release_cb) {
    m_storage_delegate   = std::move(delegate);
    m_storage_release_cb = std::move(release_cb);

    // The GUI's delegate is the *fallback* path inside the native router.
    // Keep the native router's fallback in sync whenever the delegate
    // changes (so re-attach() updates wiring).
    if (m_native_storage) {
        m_native_storage->set_fallback(m_storage_delegate);
    }

    // If the vtun server already exists (initialise() ran), wire it
    // through immediately. Otherwise initialise() will pick it up.
    // We attach the native router's wrapping delegate (NOT
    // m_storage_delegate directly) so the model-gate runs first.
    if (m_vtun && m_native_storage) {
        m_vtun->attach_storage_delegate(m_native_storage->make_delegate());
    } else if (m_vtun && m_storage_delegate) {
        // Native router not yet constructed (e.g. test that calls
        // attach_storage_delegate before initialise()) — fall back to
        // the GUI's delegate unwrapped.
        m_vtun->attach_storage_delegate(m_storage_delegate);
    }
}

void BridgeApp::set_camera_url_resolver(CameraUrlResolver fn) {
    // Stash it; new cloud_cams (built in add_device_locked) pick it up.
    // Also propagate to any cloud_cam that's already been constructed.
    m_camera_url_resolver = fn;
    std::lock_guard<std::mutex> lk(m_devices_mu);
    for (auto& kv : m_devices) {
        if (kv.second.cloud_cam) {
            kv.second.cloud_cam->set_camera_url_resolver(fn);
        }
    }
}

uint16_t BridgeApp::mqtt_port_for_dev_id(const std::string& dev_id) const {
    std::lock_guard<std::mutex> lk(m_devices_mu);
    // Fast path: real dev_id.
    auto it = m_devices.find(dev_id);
    if (it != m_devices.end()) return it->second.mqtt_port;
    // Slow path: FFFF-mangled — match on shared suffix (see
    // lookup_real_device for the rationale).
    if (dev_id.size() > 4 &&
        dev_id.compare(0, 4, kVirtualSerialPrefix) == 0) {
        const std::string tail = dev_id.substr(4);
        for (const auto& kv : m_devices) {
            const std::string& real = kv.first;
            if (real.size() >= tail.size() &&
                real.compare(real.size() - tail.size(),
                             tail.size(), tail) == 0) {
                return kv.second.mqtt_port;
            }
        }
    }
    return 0;
}

BridgeApp::RealDeviceInfo
BridgeApp::lookup_real_device(const std::string& dev_id) const {
    RealDeviceInfo out;
    std::lock_guard<std::mutex> lk(m_devices_mu);
    // Fast path: caller passed the real dev_id.
    auto it = m_devices.find(dev_id);
    if (it != m_devices.end()) {
        out.real_dev_id = it->first;
        out.lan_ip      = it->second.lan_ip;
        return out;
    }
    // Slow path: caller passed the FFFF-mangled form. The mangling
    // replaces the first 4 chars, so the trailing portion is shared
    // between virtual and real serials. Match on suffix.
    if (dev_id.size() > 4 &&
        dev_id.compare(0, 4, kVirtualSerialPrefix) == 0) {
        const std::string tail = dev_id.substr(4);
        for (const auto& kv : m_devices) {
            const std::string& real = kv.first;
            if (real.size() >= tail.size() &&
                real.compare(real.size() - tail.size(),
                             tail.size(), tail) == 0) {
                out.real_dev_id = real;
                out.lan_ip      = kv.second.lan_ip;
                return out;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// initialise()
// ---------------------------------------------------------------------------

bool BridgeApp::initialise() {
    if (m_initialised.load()) return true;

    // 1) Plugin handle.
    //
    // CRITICAL: the proprietary `bambu_networking` plugin only allows ONE
    // agent per process. If the host is BambuStudio (GUI mode), the
    // slicer's NetworkAgent is already that one agent — and constructing
    // another one here corrupts plugin state and kills the GUI's existing
    // printer / cloud session (the symptom is "[lan-uplink dev=...] tcp
    // connect failed: Invalid argument" + the GUI losing its connection).
    //
    // So when `host_drives_inventory` is true (the default for the GUI
    // worker thread), we DO NOT construct a plugin handle here. Instead
    // the host pushes its DeviceManager snapshot via
    // `set_virtual_printers(...)` and the bridge stays purely an SSDP
    // advertiser. `--bridge-only` headless mode flips the flag to false
    // because there's no GUI competing for the plugin.
    if (!m_plugin_injected && !m_cfg.host_drives_inventory) {
        PluginHandleConfig hcfg;
        hcfg.plugin_path        = m_cfg.plugin_path;
        hcfg.config_dir         = m_cfg.config_dir;
        hcfg.country_code       = m_cfg.country_code;
        hcfg.extra_http_headers = m_cfg.http_extra_headers;
        hcfg.cert_dir           = m_cfg.cert_dir;
        hcfg.cert_file          = m_cfg.cert_file;
        m_plugin = std::make_shared<BambuNetworkingPluginHandle>(hcfg);
        if (!m_plugin->init()) {
            m_plugin.reset();
            return false;
        }
    } else if (m_plugin_injected) {
    } else {
    }

    // 1b) BambuSource handle for the camera path. Soft-failure: a missing
    //     libBambuSource.so doesn't stop the daemon — LAN/Cloud camera
    //     sources just refuse to open() and CameraSourceRouter falls
    //     back to NullCameraSource (or refuses outright if its policy
    //     disallows that). Everything else (MQTT/FTPS/SSDP) is unaffected.
    if (!m_bambu_source_injected) {
        BambuSourceConfig scfg;
        scfg.library_path = m_cfg.bambu_source_path;
        m_bambu_source = std::make_shared<BambuSourceHandle>(scfg);
        if (!m_bambu_source->init()) {
        } else {
        }
    } else {
    }

    // 2) CloudInventory — only when we own a plugin handle. In host-
    //    driven mode the inventory comes in via set_virtual_printers().
    if (m_plugin) {
        m_inventory = std::make_unique<CloudInventory>(m_plugin);
    }

    // 3) CertFactory.
    tls::CertFactoryConfig ccfg;
    if (!m_cfg.cert_cache_dir.empty()) ccfg.cache_dir = m_cfg.cert_cache_dir;
    try {
        m_cert_factory = std::make_unique<tls::CertFactory>(ccfg);
    } catch (const std::exception& ex) {
        return false;
    }

    // Proxy mode is everything except SSDP. When ON (the default
    // headless posture), we construct LanUplink/CloudUplink so MQTT
    // and FTPS sessions reaching the bridge can be forwarded. In the
    // GUI-embedded case (host_drives_inventory=true) m_plugin is null,
    // attach_plugin(nullptr) is a no-op, and reconcile_once() never
    // calls add_device — the uplinks exist but stay idle, which is the
    // safe shape until we share the GUI's NetworkAgent plugin pointer.
    const bool proxy_mode = m_cfg.enable_mqtt || m_cfg.enable_ftps ||
                            m_cfg.enable_rtsp;

    if (proxy_mode) {
        // 4) Bottom-tier uplinks / sinks / sources. ONE of each,
        //    shared across every device. Per-dev add_device() calls
        //    later. Both LAN and cloud sides go through the single
        //    shared plugin handle.
        m_lan_uplink   = std::make_shared<router::LanUplink>();
        m_lan_uplink->attach_plugin(m_plugin);
        m_cloud_uplink = std::make_shared<router::CloudUplink>();
        m_cloud_uplink->attach_plugin(m_plugin);

        m_lan_sink   = std::make_shared<router::LanUploadSink>();
        m_lan_sink->attach_plugin(m_plugin);
        m_cloud_sink = std::make_shared<router::CloudUploadSink>();
        m_cloud_sink->attach_plugin(m_plugin);

        m_null_camera = std::make_shared<router::NullCameraSource>();

        // 5) Health monitor + routers.
        m_health = std::make_shared<router::UplinkHealthMonitor>();
        m_health->set_lan_uplink(m_lan_uplink);
        m_health->set_cloud_uplink(m_cloud_uplink);

        m_session_router = std::make_shared<router::SessionRouter>();
        m_session_router->set_lan_uplink(m_lan_uplink);
        m_session_router->set_cloud_uplink(m_cloud_uplink);
        m_session_router->set_health_monitor(m_health);

        m_upload_router = std::make_shared<router::UploadSinkRouter>();
        m_upload_router->set_lan_sink(m_lan_sink);
        m_upload_router->set_cloud_sink(m_cloud_sink);
        m_upload_router->set_health_monitor(m_health);
    }

    // 6) Servers — only if enabled. Each can host many devices.
    if (m_cfg.enable_ssdp) {
        server::SsdpResponderConfig sscfg;
        sscfg.bind_address = m_cfg.lan_iface_bind;
        m_ssdp = std::make_unique<server::SsdpResponder>(sscfg);

        // Passive listener on the same UDP/2021 broadcast channel so we
        // can learn the LAN IP of cloud-bound printers when they
        // announce themselves on the LAN. The cloud REST endpoint only
        // returns dev_ip if the printer recently phoned home from
        // inside the user's LAN, so this is how we discover the IPs
        // we'd otherwise be missing. The callback runs on the
        // listener's own thread and grabs m_devices_mu briefly to
        // apply the lan_ip update via update_lan_ip_locked.
        server::SsdpListener::Config lcfg;
        // Always bind 0.0.0.0 for the SSDP listener, even when the
        // operator passes --bind <specific_ip>. UDP sockets bound to a
        // specific unicast IP on Linux don't receive packets destined
        // for the limited broadcast address (255.255.255.255) — and
        // Bambu printers send their NOTIFYs there. Binding INADDR_ANY
        // is the only way to hear them. SO_REUSEPORT in the listener
        // ensures we coexist with anything else on UDP/2021.
        (void) m_cfg.lan_iface_bind;
        lcfg.bind_address = "0.0.0.0";
        // 2021 is hard-coded — that's the Bambu-firmware-side port.
        m_ssdp_listener = std::make_unique<server::SsdpListener>(
            std::move(lcfg),
            [this](const server::SsdpHeardDevice& heard) {
                std::lock_guard<std::mutex> lk(m_devices_mu);

                // Skip our own broadcasts. SsdpResponder emits the
                // virtual clones with the FFFF-prefix mangle. A heard
                // dev_id starting with that prefix is definitionally
                // ours; we don't need to compare against every tracked
                // real serial.
                if (heard.dev_id.rfind(kVirtualSerialPrefix, 0) == 0) return;

                auto it = m_devices.find(heard.dev_id);
                if (it == m_devices.end()) {
                    // Heard a printer we don't track yet. Cloud
                    // inventory may catch up next tick. Useful log
                    // line for debugging "why isn't $printer in my
                    // virtual list?"
                    return;
                }
                if (it->second.lan_ip == heard.lan_ip) return; // no-op

                // Wire LanUplink + LanUploadSink. update_lan_ip_locked
                // calls LanUplink::add_device which fires the plugin's
                // single-session connect_printer; with --only-dev-id
                // there's no thrash. Without this, SD-card uploads via
                // CloudUploadSink fail because the plugin's cloud
                // start_send_gcode_to_sdcard needs a populated dev_ip
                // it can't derive from cloud-only state alone.
                it->second.lan_ip_last_seen =
                    std::chrono::steady_clock::now();
                update_lan_ip_locked(it->second, heard.lan_ip);
            });
    }
    if (m_cfg.enable_mqtt) {
        server::MqttBrokerConfig bcfg;
        bcfg.uplink                 = m_session_router;
        // Real Bambu printers allow 1 LAN client; we bump it during
        // dev so the CLI testbed and the slicer GUI can coexist
        // against the same bridge for diagnostics. Down to 1 for
        // release-shaped behaviour.
        bcfg.max_clients_per_device = 4;
        m_mqtt = std::make_unique<server::MqttBroker>(bcfg);
    }
    if (m_cfg.enable_ftps) {
        server::FtpsServerConfig fcfg;
        fcfg.sink              = m_upload_router;
        fcfg.pasv_advertise_ip = m_cfg.lan_iface_bind;
        m_ftps = std::make_unique<server::FtpsServer>(fcfg);
    }
    if (m_cfg.enable_rtsp) {
        server::RtspServerConfig rcfg;
        m_rtsp = std::make_unique<server::RtspServer>(rcfg);
    }
    if (m_cfg.enable_vtun) {
        server::VirtualTunnelServerConfig vcfg;
        vcfg.slicer_net_ver = m_cfg.slicer_net_ver;
        vcfg.slicer_cli_id  = m_cfg.slicer_cli_id;
        vcfg.slicer_cli_ver = m_cfg.slicer_cli_ver;
        m_vtun = std::make_unique<server::VirtualTunnelServer>(vcfg);

        // Model-aware storage routing: the bridge owns a
        // NativeStorageDelegate that talks port 6000 directly for
        // X1C / P1S / P1P / X1E / X1 printers (via LocalControlTunnel),
        // and falls back to the GUI's PrinterFileSystem-via-plugin
        // delegate for H2S / H2D / A1 / unknown. The model registry is
        // populated lazily in add_device_locked as devices arrive.
        m_native_storage =
            std::make_shared<router::NativeStorageDelegate>();
        if (m_storage_delegate) {
            m_native_storage->set_fallback(m_storage_delegate);
        }
        m_vtun->attach_storage_delegate(m_native_storage->make_delegate());
    }

    // 7) Start each server. Order: SSDP first (it broadcasts), then the
    //    listeners. add_device() calls will follow during the first
    //    reconcile.
    if (m_ssdp)          m_ssdp->start();
    if (m_ssdp_listener) m_ssdp_listener->start();
    if (m_mqtt)          m_mqtt->start();
    if (m_ftps)          m_ftps->start();
    if (m_rtsp)          m_rtsp->start();
    if (m_vtun)          m_vtun->start();

    m_initialised.store(true);
    return true;
}

// ---------------------------------------------------------------------------
// teardown() — order is reverse of initialise(): RTSP→FTPS→MQTT→SSDP.
// ---------------------------------------------------------------------------

void BridgeApp::teardown() {
    if (!m_initialised.load() && !m_plugin) return;

    // Remove every device from every server so listener threads close
    // cleanly. Snapshot under lock first to avoid holding m_devices_mu
    // across remove_device's own lock acquisitions.
    std::vector<std::string> dev_ids;
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        dev_ids.reserve(m_devices.size());
        for (const auto& kv : m_devices) dev_ids.push_back(kv.first);
    }
    for (const auto& d : dev_ids) {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        remove_device_locked(d);
    }

    // Stop servers RTSP → FTPS → MQTT → SSDP (reverse of start order).
    // RTSP has the most heavyweight per-session threads so we tear it
    // down first to free resources before everything else closes.
    if (m_vtun)          { m_vtun->stop();          m_vtun.reset();          }
    // Drop the native router AFTER vtun is stopped — any in-flight
    // session threads were joined inside m_vtun->stop().
    if (m_native_storage) m_native_storage.reset();
    if (m_rtsp)          { m_rtsp->stop();          m_rtsp.reset();          }
    if (m_ftps)          { m_ftps->stop();          m_ftps.reset();          }
    if (m_mqtt)          { m_mqtt->stop();          m_mqtt.reset();          }
    if (m_ssdp_listener) { m_ssdp_listener->stop(); m_ssdp_listener.reset(); }
    if (m_ssdp)          { m_ssdp->stop();          m_ssdp.reset();          }

    // Sinks / uplinks / routers drop their references. Order is not
    // significant; their destructors are independent.
    m_session_router.reset();
    m_upload_router.reset();
    m_health.reset();
    m_lan_uplink.reset();
    m_cloud_uplink.reset();
    m_lan_sink.reset();
    m_cloud_sink.reset();
    m_null_camera.reset();

    m_inventory.reset();
    m_cert_factory.reset();

    // Plugin handle LAST. Injected handles (tests) outlive us so we just
    // drop our shared_ptr ref. Real handle's dtor calls the plugin's
    // stop/destroy_agent.
    m_plugin.reset();
    m_bambu_source.reset();

    m_initialised.store(false);
}

// ---------------------------------------------------------------------------
// Device-table reconcile (called from the poll thread, and from
// poll_inventory_once() in tests).
// ---------------------------------------------------------------------------

void BridgeApp::reconcile_once() {
    // Expire stale lan_ip entries before sourcing this tick's snapshot.
    // The SsdpListener stamps DeviceState::lan_ip_last_seen on each
    // heard NOTIFY; if we haven't heard from a printer in
    // m_cfg.lan_ip_stale_after we assume it dropped off the LAN and
    // clear the field. A subsequent broadcast will repopulate it.
    {
        std::lock_guard<std::mutex> lk(m_devices_mu);
        const auto now = std::chrono::steady_clock::now();
        for (auto& kv : m_devices) {
            auto& s = kv.second;
            if (s.lan_ip.empty()) continue;
            // Devices added with a non-empty cloud-supplied lan_ip get
            // their timestamp seeded at add time, so this check is
            // safe even before the first listener NOTIFY arrives.
            if (s.lan_ip_last_seen.time_since_epoch().count() == 0) continue;
            if (now - s.lan_ip_last_seen <= m_cfg.lan_ip_stale_after)
                continue;
            s.lan_ip.clear();
            s.lan_ip_last_seen = {};
        }
    }

    // Host-driven path: GUI hands us its DeviceManager snapshot via the
    // `printer_source` callback. Preferred when running inside
    // BambuStudio's GUI worker thread so the bridge never touches the
    // plugin or NetworkAgent state directly.
    if (m_cfg.printer_source) {
        std::vector<VirtualPrinter> printers;
        try {
            printers = m_cfg.printer_source();
        } catch (const std::exception& ex) {
            return;
        }
        set_virtual_printers(std::move(printers));
        return;
    }

    // Standalone (`--bridge-only`) path: bridge owns the plugin and
    // polls cloud inventory directly. Only runs when host_drives_inventory
    // was off, which is what gates m_inventory's construction.
    if (!m_inventory) {
        return;
    }

    const bool ok = m_inventory->refresh();
    m_inventory->probe_lan_reachability();
    auto snap = m_inventory->snapshot();
    // Lift CloudDevice -> VirtualPrinter and route through the common
    // host-driven path so we don't have two reconcile implementations.
    std::vector<VirtualPrinter> printers;
    printers.reserve(snap.size());
    for (const auto& d : snap) {
        if (d.dev_id.empty()) continue;
        // --only-dev-id filter (CLI debug knob).
        if (!m_cfg.only_dev_ids.empty()) {
            if (std::find(m_cfg.only_dev_ids.begin(),
                          m_cfg.only_dev_ids.end(),
                          d.dev_id) == m_cfg.only_dev_ids.end())
                continue;
        }
        VirtualPrinter p;
        p.dev_id      = d.dev_id;
        p.dev_name    = d.name;
        p.lan_ip      = d.lan_ip;
        p.access_code = d.access_code;
        p.model       = d.model;
        // CloudDevice has no firmware string; SSDP falls back to default.
        printers.push_back(std::move(p));
    }
    set_virtual_printers(std::move(printers));
}

void BridgeApp::set_virtual_printers(std::vector<VirtualPrinter> printers) {
    // BAMBU_BRIDGE_PRINTER_ORDER (comma-separated dev_ids) forces the
    // index assignment order, which in turn fixes the per-printer MQTT
    // / FTPS / RTSP / vtun port assignments (port = base + index).
    // Useful when a slicer-side client hardcodes the legacy port_base
    // and you want a specific printer served there. Devices not named
    // in the list keep their relative order and are appended after.
    if (const char* env = std::getenv("BAMBU_BRIDGE_PRINTER_ORDER"); env && *env) {
        std::vector<std::string> want;
        const std::string s = env;
        size_t pos = 0;
        while (pos <= s.size()) {
            const size_t comma = s.find(',', pos);
            const size_t end = (comma == std::string::npos) ? s.size() : comma;
            if (end > pos) want.emplace_back(s.substr(pos, end - pos));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        std::stable_sort(printers.begin(), printers.end(),
            [&want](const VirtualPrinter& a, const VirtualPrinter& b) {
                const auto ia = std::find(want.begin(), want.end(), a.dev_id);
                const auto ib = std::find(want.begin(), want.end(), b.dev_id);
                return ia < ib;
            });
    }

    // BAMBU_BRIDGE_TARGET_DEV (comma-separated dev_ids) restricts the
    // snapshot to just those printers. Pins a --bridge-only child
    // process to one (or a few) device(s) — the basis for the
    // multi-process launcher where each child owns a single real
    // printer (and therefore its own plugin LAN slot, since the
    // proprietary plugin only supports one active LAN connection per
    // process). Empty / unset = keep all (default behaviour).
    // Built when TARGET_DEV is set; used both for filtering printers
    // and as the authoritative dev_id→offset map (env position = index)
    // so slicers' persisted per-dev_id mqtt_port stays valid across
    // bridge reboots / cloud-snapshot reorderings.
    std::vector<std::string> env_keep_order;
    if (const char* env = std::getenv("BAMBU_BRIDGE_TARGET_DEV"); env && *env) {
        const std::string s = env;
        size_t pos = 0;
        while (pos <= s.size()) {
            const size_t comma = s.find(',', pos);
            const size_t end = (comma == std::string::npos) ? s.size() : comma;
            if (end > pos) env_keep_order.emplace_back(s.substr(pos, end - pos));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        printers.erase(
            std::remove_if(printers.begin(), printers.end(),
                [&env_keep_order](const VirtualPrinter& p) {
                    return std::find(env_keep_order.begin(), env_keep_order.end(), p.dev_id) == env_keep_order.end();
                }),
            printers.end());
    }

    std::lock_guard<std::mutex> lk(m_devices_mu);

    // First-time setup: pin each TARGET_DEV dev_id to its position in
    // the env list. add_device_locked reads this map when assigning
    // state.index. Idempotent across set_virtual_printers calls so
    // re-parsing the same env doesn't churn the map.
    if (m_pinned_offset.empty() && !env_keep_order.empty()) {
        for (std::size_t i = 0; i < env_keep_order.size(); ++i) {
            m_pinned_offset.emplace(env_keep_order[i], i);
        }
        // Bump the running counter past the pinned range so any
        // not-in-env devices that show up later don't collide with a
        // pinned slot.
        m_next_index = env_keep_order.size();
    }

    // Snapshot is the source of truth for device EXISTENCE; lan_ip is
    // a sticky field. Cloud REST (get_user_print_info) only reports
    // dev_ip when the printer recently phoned home from inside the
    // user's LAN, so most polls come back with lan_ip empty even
    // though the device is reachable. The SsdpListener writes the
    // authoritative IP in here on each broadcast NOTIFY — if we let an
    // empty cloud value clobber that, the table would oscillate every
    // poll. So:
    //   - source is new in m_devices              → add_device_locked
    //   - source supplies a non-empty lan_ip that
    //     differs from what we have                → update_lan_ip_locked
    //   - source supplies empty lan_ip             → keep what we have
    for (const auto& p : printers) {
        if (p.dev_id.empty()) continue;
        auto it = m_devices.find(p.dev_id);
        if (it == m_devices.end()) {
            add_device_locked(p);
        } else {
            if (!p.lan_ip.empty() && it->second.lan_ip != p.lan_ip)
                update_lan_ip_locked(it->second, p.lan_ip);
            // Firmware version arrives later than the device add
            // (push_status from cloud has to land first). Keep the
            // tracked state and the vtun spec in sync on every push.
            if (!p.firmware.empty() &&
                it->second.firmware_ver != p.firmware) {
                it->second.firmware_ver = p.firmware;
                if (m_vtun)
                    m_vtun->update_printer_firmware_ver(p.dev_id, p.firmware);
            }
            // Camera URL changes when the GUI's MediaUrlBuilder picks a
            // different scheme (e.g. TUTK expired and a new agora URL
            // was fetched). Propagate to the camera sources so the next
            // open() picks up the fresh URL.
            if (it->second.camera_url != p.camera_url) {
                it->second.camera_url = p.camera_url;
                if (it->second.lan_cam) {
                    auto cfg = it->second.lan_cam->config();
                    cfg.url_override = p.camera_url;
                    it->second.lan_cam = std::make_shared<router::LanCameraSource>(cfg);
                    it->second.lan_cam->attach_source_handle(m_bambu_source);
                    if (it->second.cam_router)
                        it->second.cam_router->set_lan_source(it->second.lan_cam);
                }
            }
        }
    }
    std::vector<std::string> doomed;
    for (const auto& kv : m_devices) {
        const auto& tracked = kv.first;
        auto hit = std::find_if(printers.begin(), printers.end(),
            [&](const VirtualPrinter& p) { return p.dev_id == tracked; });
        if (hit == printers.end()) doomed.push_back(tracked);
    }
    for (const auto& d : doomed) remove_device_locked(d);
}

void BridgeApp::add_device_locked(const VirtualPrinter& vp) {
    const std::string& dev_id      = vp.dev_id;
    const std::string& lan_ip      = vp.lan_ip;
    const std::string& access_code = vp.access_code;

    DeviceState state;
    state.dev_id       = dev_id;
    state.lan_ip       = lan_ip;
    state.firmware_ver = vp.firmware;
    state.camera_url   = vp.camera_url;
    state.model        = vp.model;
    // If the cloud snapshot supplied a non-empty lan_ip, treat it as
    // freshly-seen so the staleness pass doesn't immediately expire it
    // before the listener has heard the printer's own NOTIFY.
    if (!lan_ip.empty())
        state.lan_ip_last_seen = std::chrono::steady_clock::now();
    state.access_code = access_code;
    // Prefer the env-pinned offset if this dev_id was listed in
    // BAMBU_BRIDGE_TARGET_DEV; fall through to the running counter for
    // any other dev_id (unfiltered mode / late arrivals).
    if (auto it = m_pinned_offset.find(dev_id); it != m_pinned_offset.end()) {
        state.index = it->second;
    } else {
        state.index = m_next_index++;
    }
    state.mqtt_port   = static_cast<uint16_t>(m_cfg.mqtt_port_base + state.index);
    state.ftps_port   = static_cast<uint16_t>(m_cfg.ftps_port_base + state.index);
    state.rtsp_port   = static_cast<uint16_t>(m_cfg.rtsp_port_base + state.index);
    state.vtun_port   = static_cast<uint16_t>(m_cfg.vtun_port_base + state.index);

    // Mint cert. Failure here is fatal for THIS device but the daemon
    // keeps running for the others.
    tls::CertMaterial cert;
    try {
        cert = m_cert_factory->get_or_create(dev_id);
    } catch (const std::exception& ex) {
        return;
    }

    // Virtual serial used in SSDP USN and in MQTT topic strings the
    // slicer sees. See mangle_serial() / kVirtualSerialPrefix for the
    // choice of mangle. Internal state stays keyed on the real serial
    // (`dev_id`).
    const std::string virtual_dev_id = mangle_serial(dev_id);

    // What we advertise in the SSDP LOCATION header. If we just emit
    // `m_cfg.lan_iface_bind` and that's 0.0.0.0, slicers parsing
    // LOCATION as a URL may reject the entry. Auto-detect the host's
    // primary LAN IPv4 the first time we need it and cache it.
    if (m_ssdp_advertise_ip.empty()) {
        m_ssdp_advertise_ip = m_cfg.lan_iface_bind;
        if (m_ssdp_advertise_ip.empty() ||
            m_ssdp_advertise_ip == "0.0.0.0") {
            std::string lan = detect_primary_lan_ip();
            if (!lan.empty()) m_ssdp_advertise_ip = lan;
        }
    }

    // SSDP: announce the virtual device with a MANGLED serial and a
    // "${printer-name}-virtual" friendly name. The serial is the real
    // serial with its last hex character incremented (e.g.
    // 03900D610219434 → 03900D610219435). Earlier we used a "BR-"
    // prefix, but Bambu slicers gate SSDP entries on the USN matching
    // the Bambu serial regex (effectively ^[0-9A-F]{14,15}$) — a "BR-"
    // prefix made the entry silently ignored on the consumer side.
    // Incrementing the last character keeps the SN shape valid and
    // collision-resistant: every one of the user's real printers
    // already has its real SN registered, so SN+1 only collides with
    // a hypothetical fourth printer that happens to be the exact
    // increment of an existing one. Statistically negligible.
    //
    // Translation note: the slicer will publish/subscribe to
    // device/<virtual_sn>/... topics. Upstream (the bambu_networking
    // plugin) keys on the real_sn. We carry both in DeviceState +
    // MqttBrokerVirtualDevice so MqttBroker can swap virtual <-> real
    // on the wire and the plugin sees the correct dev_id.
    if (m_ssdp) {
        const std::string base_name =
            vp.dev_name.empty() ? m_cfg.ssdp_default_name : vp.dev_name;
        server::SsdpVirtualDevice sdev;
        sdev.dev_id    = virtual_dev_id;
        sdev.name      = base_name + "-virtual";
        sdev.model     = vp.model.empty()    ? m_cfg.ssdp_default_model    : vp.model;
        sdev.firmware  = vp.firmware.empty() ? m_cfg.ssdp_default_firmware : vp.firmware;
        sdev.lan_ip    = m_ssdp_advertise_ip;
        sdev.http_port = state.mqtt_port;
        // Must advertise DevBind.bambu.com=free, not occupied. The slicer's
        // MachineObject::is_avaliable() returns true only when bind_state
        // == "free" — and DevManager::get_my_machine_list() filters anything
        // failing is_avaliable() OUT of the user-visible LAN list. Setting
        // bound=true here caused our virtual printers to appear briefly via
        // localMachineList but vanish from the user's "Devices" list on
        // every refresh / tab switch.
        sdev.bound     = false;
        sdev.secure    = true;
        m_ssdp->add_device(std::move(sdev));
    }

    if (m_mqtt) {
        server::MqttBrokerVirtualDevice mdev;
        mdev.dev_id         = dev_id;
        mdev.virtual_dev_id = virtual_dev_id;
        mdev.lan_ip         = m_cfg.lan_iface_bind;
        mdev.port           = state.mqtt_port;
        mdev.access_code    = access_code;
        mdev.cert           = cert;
        try { m_mqtt->add_device(std::move(mdev)); }
        catch (const std::exception& ex) {
        }
    }

    if (m_ftps) {
        server::FtpsVirtualDevice fdev;
        fdev.dev_id      = dev_id;
        fdev.lan_ip      = m_cfg.lan_iface_bind;
        fdev.port        = state.ftps_port;
        fdev.access_code = access_code;
        fdev.cert        = cert;
        try { m_ftps->add_device(std::move(fdev)); }
        catch (const std::exception& ex) {
        }
    }

    if (m_vtun) {
        server::VirtualTunnelVirtualDevice vdev;
        vdev.dev_id               = dev_id;
        vdev.lan_ip               = m_cfg.lan_iface_bind;
        vdev.port                 = state.vtun_port;
        vdev.access_code          = access_code;
        vdev.printer_lan_ip       = lan_ip;
        vdev.printer_firmware_ver = state.firmware_ver;
        vdev.cert                 = cert;
        try { m_vtun->add_device(std::move(vdev)); }
        catch (const std::exception& ex) {
        }
    }

    // Register the device with the native storage router. This is what
    // emits the "[bridge-app] dev=... model=... -> using ... storage
    // delegate" log line and picks the native-vs-plugin path for every
    // subsequent storage JSON-RPC frame from the slicer.
    if (m_native_storage) {
        m_native_storage->register_device(dev_id, state.model);
    }

    // Proxy-mode-only: camera router + LAN/cloud uplinks + sinks. In
    // passive advertise-only mode (the default) these stay null and we
    // never call `connect_printer` on the shared plugin — which is what
    // would otherwise tear down the GUI app's printer session.
    if (m_lan_uplink) {
        state.cam_router = std::make_shared<router::CameraSourceRouter>(dev_id);
        router::LanCameraSourceConfig lc;
        lc.dev_id         = dev_id;
        lc.printer_ip     = lan_ip;
        lc.access_code    = access_code;
        lc.url_override   = vp.camera_url;
        lc.slicer_net_ver = m_cfg.slicer_net_ver;
        // dev_ver: the GUI's `m_dev_ver` reflects the `ota` module's
        // `sw_ver` from the printer's get_version reply. We don't have
        // per-printer MQTT tracking wired up yet, so default to the
        // bridge's `ssdp_default_firmware` which already matches the
        // real H2S `ota` sw_ver (`01.02.00.00`). The plugin appears to
        // fingerprint the URL — an empty `&dev_ver=` triggers
        // `bambu_start_stream rc=-107`.
        lc.slicer_dev_ver = m_cfg.ssdp_default_firmware;
        lc.slicer_cli_id  = m_cfg.slicer_cli_id;
        lc.slicer_cli_ver = m_cfg.slicer_cli_ver;
        // LAN ladder: if the printer has LAN RTSPS disabled in firmware
        // (`ipcam.rtsp_url == "disable"`, the default on shipped H2S/
        // H2D firmware 01.02.00.00) port 322 is closed and the live555
        // client returns -107. Port 6000 stays open and serves the
        // same video over the `bambu:///local/...?port=6000` form.
        // We build that URL once here so LanCameraSource can retry the
        // primary rtsps:// attempt against it without re-discovering
        // the credentials. Same query-param recipe as MediaPlayCtrl
        // uses for storage on port 6000 — kept aligned so the plugin
        // fingerprints the call shape as legitimate.
        if (!lan_ip.empty() && !access_code.empty()) {
            std::string lf = "bambu:///local/" + lan_ip
                             + ".?port=6000&user=bblp&passwd=" + access_code;
            lf += "&device="  + dev_id;
            lf += "&net_ver=" + m_cfg.slicer_net_ver;
            lf += "&dev_ver=" + m_cfg.ssdp_default_firmware;
            lf += "&cli_id="  + m_cfg.slicer_cli_id;
            lf += "&cli_ver=" + m_cfg.slicer_cli_ver;
            lc.local_fallback_url = std::move(lf);
        }
        state.lan_cam  = std::make_shared<router::LanCameraSource>(lc);
        state.lan_cam->attach_source_handle(m_bambu_source);
        router::CloudCameraSourceConfig cc;
        cc.dev_id        = dev_id;
        cc.url_override  = vp.camera_url;
        cc.dev_ver       = m_cfg.ssdp_default_firmware;
        cc.net_ver       = m_cfg.slicer_net_ver;
        cc.cli_id        = m_cfg.slicer_cli_id;
        cc.cli_ver       = m_cfg.slicer_cli_ver;
        state.cloud_cam = std::make_shared<router::CloudCameraSource>(cc);
        state.cloud_cam->attach_plugin(m_plugin);
        state.cloud_cam->attach_source_handle(m_bambu_source);
        // Invisible-GUI / GUI-host mode: route URL resolution through the
        // slicer's live NetworkAgent so the bridge picks up the host's
        // existing cloud session + TUTK token.
        if (m_camera_url_resolver) {
            state.cloud_cam->set_camera_url_resolver(m_camera_url_resolver);
        }
        state.cam_router->set_lan_source(state.lan_cam);
        state.cam_router->set_cloud_source(state.cloud_cam);
        state.cam_router->set_null_source(m_null_camera);
        state.cam_router->set_health_monitor(m_health);

        // Camera-source selection REUSES the native decision rather than
        // re-implementing it: vp.camera_url was produced by the GUI's
        // Slic3r::GUI::build_media_live_url — the single helper that encodes
        // BambuStudio's liveview_local/remote/lan_mode/lan_ip ladder (the
        // same one MediaPlayCtrl follows). We map its chosen scheme to a
        // source, but in EVERY local/LAN case we lead with the libBambuSource
        // LanCameraSource (lc.url_override = vp.camera_url, set above) — the
        // exact same lib + URL native BambuStudio drives through gstbambusrc,
        // for A1's bambu:///local port-6000 too. The hand-rolled
        // JpegCameraSource (which native has no equivalent of) is demoted to a
        // trailing fallback for the local case, in case the lib's headless
        // local path can't open.
        //   bambu:///local...   (LVL_Local) -> LAN (lib, local URL) + JPEG fb
        //   ...rtsps___/rtsp___ (LVL_Rtsp*) -> LAN (lib, RTSP(S))
        //   remote URL                       -> cloud/TUTK
        //   empty (Disable/None, or not-yet-reported) -> prefer cloud; for the
        //     transient-unknown case wire the JPEG fallback for A1/P1.
        const std::string& cu = vp.camera_url;
        router::CameraSourceRouter::Policy pol;
        pol.allow_null_fallback = false;
        bool        wire_jpeg = false;  // create JpegCameraSource (primary or fallback)
        const char* why       = "";
        if (cu.rfind("bambu:///local", 0) == 0) {
            // A1/P1 port-6000: the hand-rolled JpegCameraSource speaks the
            // actual JPEG protocol and WORKS headless (verified: port-6000
            // serves JPEG-SOI frames). libBambuSource's bambu:///local path
            // connects but would-blocks with frame_count=0 (it drives the
            // tunnel protocol, not the A1 camera), so prefer JPEG and keep
            // LAN(lib) only as the fallback.
            pol.prefer_jpeg = true; pol.prefer_lan = true; wire_jpeg = true;
            why = "camera_url=local -> JPEG-6000 (LAN-lib fallback)";
        } else if (cu.find("rtsps___") != std::string::npos ||
                   cu.find("rtsp___")  != std::string::npos) {
            pol.prefer_lan = true;          // RTSP(S) via LanCameraSource
            why = "camera_url=rtsp(s) -> LAN RTSPS";
        } else if (!cu.empty()) {
            pol.prefer_lan = false;         // resolved to a remote/cloud URL
            why = "camera_url=remote -> cloud/TUTK";
        } else {
            // camera_url unresolved (local disabled / TUTK-async / not yet
            // reported). For jpeg models (A1/P1) prefer the JPEG-6000 source —
            // the one that actually delivers frames headless; otherwise cloud.
            wire_jpeg       = router::is_jpeg_camera_model(state.model);
            pol.prefer_jpeg = wire_jpeg;    // jpeg models: JPEG-6000 first
            pol.prefer_lan  = wire_jpeg;    // then lan(lib), then cloud
            why = wire_jpeg ? "camera_url=empty -> JPEG-6000 (LAN-lib fallback)"
                            : "camera_url=empty -> cloud/TUTK";
        }
        if (wire_jpeg) {
            router::JpegCameraSourceConfig jc;
            jc.dev_id      = dev_id;
            jc.printer_ip  = lan_ip;
            jc.access_code = access_code;
            state.jpeg_cam = std::make_shared<router::JpegCameraSource>(jc);
            state.cam_router->set_jpeg_source(state.jpeg_cam);
        }
        state.cam_router->set_policy(pol);
        std::fprintf(stderr,
            "[bridge-app] dev=%s model=%s camera_url=%.48s -> %s\n",
            dev_id.c_str(), state.model.c_str(),
            cu.empty() ? "(none)" : cu.c_str(), why);
        std::fflush(stderr);

        if (m_rtsp) {
            server::RtspVirtualDevice rdev;
            rdev.dev_id      = dev_id;
            rdev.lan_ip      = m_cfg.lan_iface_bind;
            rdev.port        = state.rtsp_port;
            rdev.access_code = access_code;
            rdev.cert        = cert;
            // Wrap the router in the MJPEG->H.264 transcoder so EVERY virtual
            // printer republishes as uniform standard H.264 (the A1/P1 JPEG
            // cameras get transcoded; H.264 LAN/cloud sources pass through
            // untouched). Players that can't decode MJPEG-over-RTSP (e.g.
            // Windows Media Foundation) then work the same as on Linux.
            rdev.source      = std::make_shared<server::TranscodingCameraSource>(
                                   state.cam_router);
            // Camera RTSP transport. Default PLAIN RTSP: standard clients
            // (slicer GStreamer, VLC, ffmpeg) connect directly without
            // tripping on our self-signed TLS. Set BAMBU_BRIDGE_RTSP_TLS=1
            // to serve RTSPS instead (TLS, real-printer-camera mimicry) —
            // the slicer side must match (its virtual_camera_rtsps flag).
            rdev.tls = false;
            if (const char* e = std::getenv("BAMBU_BRIDGE_RTSP_TLS");
                e && (*e == '1' || *e == 't' || *e == 'T' || *e == 'y' || *e == 'Y')) {
                rdev.tls = true;
            }
            try { m_rtsp->add_device(std::move(rdev)); }
            catch (const std::exception& ex) {
            }
        }

        // Per-device LanUplink / LanUploadSink / CloudUplink wiring. Note
        // we ONLY register a LAN device if we have a non-empty lan_ip —
        // calling `connect_printer` with an empty hostname is what triggers
        // the EINVAL retry storm the bug report flagged.
        if (!lan_ip.empty()) {
            router::LanUplinkConfig u;
            u.dev_id      = dev_id;
            u.printer_ip  = lan_ip;
            u.access_code = access_code;
            // Per-printer mTLS material so LanUplink can bypass the
            // plugin gate for `print.command=*` payloads. See
            // resolve_mtls_paths() comments above.
            auto mtls = resolve_mtls_paths(dev_id);
            u.mtls_cert_path = mtls.first;
            u.mtls_key_path  = mtls.second;
            if (!u.mtls_cert_path.empty()) {
                std::fprintf(stderr,
                    "[bridge-app] dev=%s mtls cert=%s key=%s\n",
                    dev_id.c_str(),
                    u.mtls_cert_path.c_str(),
                    u.mtls_key_path.c_str());
                std::fflush(stderr);
            } else {
                std::fprintf(stderr,
                    "[bridge-app] dev=%s NO mtls cert found in "
                    "/tmp/bbl_capture/mtls.fresh/paired — print.* publishes "
                    "will fall back to plugin (and likely drop)\n",
                    dev_id.c_str());
                std::fflush(stderr);
            }
            m_lan_uplink->add_device(u);

            router::LanUploadSinkDevice s;
            s.dev_id      = dev_id;
            s.printer_ip  = lan_ip;
            s.access_code = access_code;
            m_lan_sink->add_device(s);

            // CloudUploadSink also wants dev_ip + access_code so the
            // plugin's start_send_gcode_to_sdcard doesn't return -1.
            // Register here while we have a populated lan_ip; if
            // lan_ip is missing the slicer's cloud upload path won't
            // work, period.
            if (m_cloud_sink) {
                router::CloudUploadSink::Device cs;
                cs.dev_id      = dev_id;
                cs.printer_ip  = lan_ip;
                cs.access_code = access_code;
                m_cloud_sink->add_device(std::move(cs));
            }
        }
        {
            router::CloudUplinkConfig u;
            u.dev_id      = dev_id;
            u.access_code = access_code;
            m_cloud_uplink->add_device(u);
        }
    }

    m_devices.emplace(dev_id, std::move(state));
}

void BridgeApp::update_lan_ip_locked(DeviceState&       state,
                                     const std::string& lan_ip) {

    state.lan_ip = lan_ip;

    // Passive advertise-only mode keeps every uplink/sink/camera null —
    // there's nothing per-device on the proxy path to update.
    if (!m_lan_uplink) return;

    // LanUplink / LanUploadSink: add_device replaces the previous config.
    // Skip if lan_ip went empty (printer fell off the LAN); the cloud
    // uplink retains the device.
    if (!lan_ip.empty()) {
        router::LanUplinkConfig u;
        u.dev_id      = state.dev_id;
        u.printer_ip  = lan_ip;
        u.access_code = state.access_code;
        auto mtls = resolve_mtls_paths(state.dev_id);
        u.mtls_cert_path = mtls.first;
        u.mtls_key_path  = mtls.second;
        std::fprintf(stderr,
            "[bridge-app] (update_lan_ip) dev=%s mtls_cert=%s mtls_key=%s\n",
            state.dev_id.c_str(),
            u.mtls_cert_path.empty() ? "<missing>" : u.mtls_cert_path.c_str(),
            u.mtls_key_path.empty()  ? "<missing>" : u.mtls_key_path.c_str());
        std::fflush(stderr);
        m_lan_uplink->add_device(u);

        router::LanUploadSinkDevice s;
        s.dev_id      = state.dev_id;
        s.printer_ip  = lan_ip;
        s.access_code = state.access_code;
        m_lan_sink->add_device(s);

        if (m_cloud_sink) {
            router::CloudUploadSink::Device cs;
            cs.dev_id      = state.dev_id;
            cs.printer_ip  = lan_ip;
            cs.access_code = state.access_code;
            m_cloud_sink->add_device(std::move(cs));
        }
    }

    // LanCameraSource has its config baked in at construction (its
    // ctor takes a `LanCameraSourceConfig`), so we recreate it and
    // re-attach. We DO NOT close() the existing one mid-stream — if a
    // slicer is currently watching the camera, the RTSP session will
    // disconnect on the next TEARDOWN/PLAY cycle. The CameraSourceRouter
    // is sticky on the currently-chosen source for the lifetime of one
    // open(), so an in-flight stream stays put.
    router::LanCameraSourceConfig lc;
    lc.dev_id         = state.dev_id;
    lc.printer_ip     = lan_ip;
    lc.access_code    = state.access_code;
    lc.slicer_net_ver = m_cfg.slicer_net_ver;
    lc.slicer_dev_ver = m_cfg.ssdp_default_firmware; // see ctor-site comment in connect path
    lc.slicer_cli_id  = m_cfg.slicer_cli_id;
    lc.slicer_cli_ver = m_cfg.slicer_cli_ver;
    // Local-port-6000 fallback URL, kept aligned with add_device_locked.
    if (!lan_ip.empty() && !state.access_code.empty()) {
        std::string lf = "bambu:///local/" + lan_ip
                         + ".?port=6000&user=bblp&passwd=" + state.access_code;
        lf += "&device="  + state.dev_id;
        lf += "&net_ver=" + m_cfg.slicer_net_ver;
        lf += "&dev_ver=" + m_cfg.ssdp_default_firmware;
        lf += "&cli_id="  + m_cfg.slicer_cli_id;
        lf += "&cli_ver=" + m_cfg.slicer_cli_ver;
        lc.local_fallback_url = std::move(lf);
    }
    state.lan_cam  = std::make_shared<router::LanCameraSource>(lc);
    state.lan_cam->attach_source_handle(m_bambu_source);
    if (state.cam_router) state.cam_router->set_lan_source(state.lan_cam);

    // JpegCameraSource (A1/P1 port-6000) is likewise config-baked at
    // construction and was created in add_device_locked when lan_ip may
    // still have been empty (cloud-add before LAN discovery) — leaving it
    // with an empty printer_ip so it open-FAILs and the router falls through
    // to the libBambuSource local path (which would-blocks on the A1 JPEG
    // camera). Rebuild it with the live IP so the JPEG client can connect.
    if (state.jpeg_cam && !lan_ip.empty()) {
        router::JpegCameraSourceConfig jc;
        jc.dev_id      = state.dev_id;
        jc.printer_ip  = lan_ip;
        jc.access_code = state.access_code;
        state.jpeg_cam = std::make_shared<router::JpegCameraSource>(jc);
        if (state.cam_router) state.cam_router->set_jpeg_source(state.jpeg_cam);
    }

    // Storage proxy: VirtualTunnelServer captured an empty
    // printer_lan_ip at add_device time (cloud snapshots rarely carry
    // dev_ip — SSDP fills it later). Push the fresh IP in so the next
    // session that opens uses the real printer's address. Existing
    // sessions keep the IP they accepted with; that's fine, mid-stream
    // IP swaps would only happen if the user changed router state.
    if (m_vtun) m_vtun->update_printer_lan_ip(state.dev_id, lan_ip);
}

void BridgeApp::remove_device_locked(const std::string& dev_id) {
    auto it = m_devices.find(dev_id);
    if (it == m_devices.end()) return;

    if (m_ssdp) m_ssdp->remove_device(dev_id);
    if (m_mqtt) m_mqtt->remove_device(dev_id);
    if (m_ftps) m_ftps->remove_device(dev_id);
    if (m_rtsp) m_rtsp->remove_device(dev_id);
    if (m_vtun) m_vtun->remove_device(dev_id);

    // Drop native-router state (closes any open LocalControlTunnel).
    if (m_native_storage) m_native_storage->unregister_device(dev_id);

    if (m_lan_uplink)   m_lan_uplink->remove_device(dev_id);
    if (m_cloud_uplink) m_cloud_uplink->remove_device(dev_id);
    if (m_lan_sink)     m_lan_sink->remove_device(dev_id);
    // CloudUploadSink has no per-device state today; nothing to remove.

    // GUI-supplied release callback drops the per-dev PFS in the
    // BridgeStorageBackend. Headless leaves this null.
    if (m_storage_release_cb) {
        m_storage_release_cb(dev_id);
    }

    m_devices.erase(it);
}

// ---------------------------------------------------------------------------
// Public lifecycle entrypoints.
// ---------------------------------------------------------------------------

bool BridgeApp::poll_inventory_once() {
    if (!m_initialised.load()) {
        if (!initialise()) return false;
    }
    reconcile_once();
    return true;
}

void BridgeApp::poll_loop() {
    while (!m_stop.load()) {
        reconcile_once();
        std::unique_lock<std::mutex> lk(m_stop_mu);
        m_stop_cv.wait_for(lk, m_cfg.inventory_poll,
                           [&] { return m_stop.load(); });
    }
}

int BridgeApp::run() {
    if (!initialise()) return 1;

    m_stop.store(false);
    m_poll_thread = std::thread(&BridgeApp::poll_loop, this);

    // Block until shutdown() flips m_stop.
    {
        std::unique_lock<std::mutex> lk(m_stop_mu);
        m_stop_cv.wait(lk, [&] { return m_stop.load(); });
    }
    if (m_poll_thread.joinable()) m_poll_thread.join();

    teardown();
    return 0;
}

void BridgeApp::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_stop_mu);
        m_stop.store(true);
    }
    m_stop_cv.notify_all();
}

std::vector<BridgeApp::DeviceBinding> BridgeApp::device_bindings() const {
    std::vector<DeviceBinding> out;
    std::lock_guard<std::mutex> lk(m_devices_mu);
    out.reserve(m_devices.size());
    for (const auto& kv : m_devices) {
        DeviceBinding b;
        b.dev_id    = kv.second.dev_id;
        b.lan_ip    = kv.second.lan_ip;
        b.mqtt_port = kv.second.mqtt_port;
        b.ftps_port = kv.second.ftps_port;
        b.rtsp_port = kv.second.rtsp_port;
        b.vtun_port = kv.second.vtun_port;
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace headless
} // namespace bridge
} // namespace Slic3r
