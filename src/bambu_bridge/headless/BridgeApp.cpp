// Bambu Bridge — headless multi-device orchestrator (phase 10).
//
// See BridgeApp.hpp for the high-level design / contract.

#include "BridgeApp.hpp"

#include "../BambuNetworkingPluginHandle.hpp"
#include "../BambuSourceHandle.hpp"
#include "../CloudInventory.hpp"
#include "../Verbose.hpp"
#include "../tls/CertFactory.hpp"
#include "../server/SsdpResponder.hpp"
#include "../server/SsdpListener.hpp"
#include "../server/MqttBroker.hpp"
#include "../server/FtpsServer.hpp"
#include "../server/RtspServer.hpp"
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
#include "../router/NullCameraSource.hpp"
#include "../router/SessionRouter.hpp"
#include "../router/UploadSinkRouter.hpp"
#include "../router/CameraSourceRouter.hpp"
#include "../router/UplinkHealth.hpp"

#include <algorithm>
#include <cstdio>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>

namespace Slic3r {
namespace bridge {
namespace headless {

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
    // If the vtun server already exists (initialise() ran), wire it
    // through immediately. Otherwise initialise() will pick it up.
    if (m_vtun && m_storage_delegate) {
        m_vtun->attach_storage_delegate(m_storage_delegate);
    }
    std::fprintf(stderr,
        "[bridge-app] attach_storage_delegate set=%d (vtun=%p)\n",
        m_storage_delegate ? 1 : 0,
        static_cast<void*>(m_vtun.get()));
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
            std::fprintf(stderr,
                "[bridge-app] plugin init failed (path='%s'). "
                "Refusing to start.\n",
                m_cfg.plugin_path.c_str());
            m_plugin.reset();
            return false;
        }
        std::fprintf(stderr,
            "[bridge-app] plugin loaded; user_login=%s server_conn=%s\n",
            m_plugin->is_user_login()       ? "yes" : "no",
            m_plugin->is_server_connected() ? "yes" : "no");
    } else if (m_plugin_injected) {
        std::fprintf(stderr, "[bridge-app] using injected plugin handle\n");
    } else {
        std::fprintf(stderr,
            "[bridge-app] host-driven mode: no plugin handle in bridge "
            "(slicer's NetworkAgent is the sole plugin consumer)\n");
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
            std::fprintf(stderr,
                "[bridge-app] libBambuSource.so could not be loaded "
                "(path='%s'). Camera re-serve will refuse open() for every "
                "device; everything else still works.\n",
                m_cfg.bambu_source_path.c_str());
        } else {
            std::fprintf(stderr, "[bridge-app] BambuSource library loaded\n");
        }
    } else {
        std::fprintf(stderr, "[bridge-app] using injected BambuSource handle\n");
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
        std::fprintf(stderr,
            "[bridge-app] CertFactory construction failed: %s\n", ex.what());
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
                    std::fprintf(stderr,
                        "[ssdp-listener] heard untracked dev_id=%s "
                        "ip=%s name='%s' model='%s'\n",
                        heard.dev_id.c_str(), heard.lan_ip.c_str(),
                        heard.name.c_str(), heard.model.c_str());
                    return;
                }
                if (it->second.lan_ip == heard.lan_ip) return; // no-op

                std::fprintf(stderr,
                    "[ssdp-listener] dev_id=%s lan_ip: %s -> %s\n",
                    heard.dev_id.c_str(),
                    it->second.lan_ip.empty() ? "(none)"
                                              : it->second.lan_ip.c_str(),
                    heard.lan_ip.c_str());
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
        // vtun routes every storage JSON-RPC frame through the
        // StorageDelegate (PrinterFileSystem-via-BridgeStorageBackend
        // path). Direct libBambuSource access was removed when the
        // delegate path proved able to handle every case the bridge
        // serves.
        if (m_storage_delegate) {
            m_vtun->attach_storage_delegate(m_storage_delegate);
        }
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

    std::fprintf(stderr,
        "[bridge-app] servers up: ssdp=%c mqtt=%c ftps=%c rtsp=%c vtun=%c "
        "bind=%s poll=%llds\n",
        m_ssdp ? '1':'0', m_mqtt ? '1':'0',
        m_ftps ? '1':'0', m_rtsp ? '1':'0', m_vtun ? '1':'0',
        m_cfg.lan_iface_bind.c_str(),
        static_cast<long long>(m_cfg.inventory_poll.count()));

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
            std::fprintf(stderr,
                "[bridge-app] dev_id=%s lan_ip=%s expired (no NOTIFY for "
                "%llds); clearing\n",
                s.dev_id.c_str(), s.lan_ip.c_str(),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        now - s.lan_ip_last_seen).count()));
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
            std::fprintf(stderr,
                "[bridge-app] printer_source callback threw: %s — keeping "
                "previous virtual-printer set\n", ex.what());
            return;
        }
        set_virtual_printers(std::move(printers));
        return;
    }

    // Standalone (`--bridge-only`) path: bridge owns the plugin and
    // polls cloud inventory directly. Only runs when host_drives_inventory
    // was off, which is what gates m_inventory's construction.
    if (!m_inventory) {
        std::fprintf(stderr, "[bridge-app] reconcile skipped: no inventory\n");
        return;
    }

    const bool ok = m_inventory->refresh();
    m_inventory->probe_lan_reachability();
    auto snap = m_inventory->snapshot();
    if (Slic3r::bridge::verbose()) {
        std::fprintf(stderr,
            "[bridge-app] reconcile: refresh=%s snapshot=%zu devices "
            "(login=%s server=%s)\n",
            ok ? "ok" : "fail", snap.size(),
            m_plugin && m_plugin->is_user_login()       ? "yes" : "no",
            m_plugin && m_plugin->is_server_connected() ? "yes" : "no");
    }

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
    std::lock_guard<std::mutex> lk(m_devices_mu);

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
    // If the cloud snapshot supplied a non-empty lan_ip, treat it as
    // freshly-seen so the staleness pass doesn't immediately expire it
    // before the listener has heard the printer's own NOTIFY.
    if (!lan_ip.empty())
        state.lan_ip_last_seen = std::chrono::steady_clock::now();
    state.access_code = access_code;
    state.index       = m_next_index++;
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
        std::fprintf(stderr,
            "[bridge-app] cert mint failed for dev_id=%s: %s — skipping\n",
            dev_id.c_str(), ex.what());
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
        std::fprintf(stderr,
            "[bridge-app] SSDP LOCATION host = %s\n",
            m_ssdp_advertise_ip.c_str());
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
            std::fprintf(stderr,
                "[bridge-app] mqtt add_device dev_id=%s failed: %s\n",
                dev_id.c_str(), ex.what());
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
            std::fprintf(stderr,
                "[bridge-app] ftps add_device dev_id=%s failed: %s\n",
                dev_id.c_str(), ex.what());
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
            std::fprintf(stderr,
                "[bridge-app] vtun add_device dev_id=%s failed: %s\n",
                dev_id.c_str(), ex.what());
        }
    }

    // Proxy-mode-only: camera router + LAN/cloud uplinks + sinks. In
    // passive advertise-only mode (the default) these stay null and we
    // never call `connect_printer` on the shared plugin — which is what
    // would otherwise tear down the GUI app's printer session.
    if (m_lan_uplink) {
        state.cam_router = std::make_shared<router::CameraSourceRouter>(dev_id);
        router::LanCameraSourceConfig lc;
        lc.dev_id        = dev_id;
        lc.printer_ip    = lan_ip;
        lc.access_code   = access_code;
        lc.url_override  = vp.camera_url;
        state.lan_cam  = std::make_shared<router::LanCameraSource>(lc);
        state.lan_cam->attach_source_handle(m_bambu_source);
        router::CloudCameraSourceConfig cc;
        cc.dev_id        = dev_id;
        cc.url_override  = vp.camera_url;
        state.cloud_cam = std::make_shared<router::CloudCameraSource>(cc);
        state.cloud_cam->attach_plugin(m_plugin);
        state.cloud_cam->attach_source_handle(m_bambu_source);
        state.cam_router->set_lan_source(state.lan_cam);
        state.cam_router->set_cloud_source(state.cloud_cam);
        state.cam_router->set_null_source(m_null_camera);
        state.cam_router->set_health_monitor(m_health);

        if (m_rtsp) {
            server::RtspVirtualDevice rdev;
            rdev.dev_id      = dev_id;
            rdev.lan_ip      = m_cfg.lan_iface_bind;
            rdev.port        = state.rtsp_port;
            rdev.access_code = access_code;
            rdev.cert        = cert;
            rdev.source      = state.cam_router;
            try { m_rtsp->add_device(std::move(rdev)); }
            catch (const std::exception& ex) {
                std::fprintf(stderr,
                    "[bridge-app] rtsp add_device dev_id=%s failed: %s\n",
                    dev_id.c_str(), ex.what());
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

    std::fprintf(stderr,
        "[bridge-app] add dev_id=%s name='%s' access_code='%s' lan_ip=%s "
        "ports={mqtt=%u, ftps=%u, rtsp=%u, vtun=%u}\n",
        dev_id.c_str(),
        vp.dev_name.c_str(),
        access_code.c_str(),
        lan_ip.c_str(),
        unsigned(state.mqtt_port), unsigned(state.ftps_port),
        unsigned(state.rtsp_port), unsigned(state.vtun_port));

    m_devices.emplace(dev_id, std::move(state));
}

void BridgeApp::update_lan_ip_locked(DeviceState&       state,
                                     const std::string& lan_ip) {
    std::fprintf(stderr,
        "[bridge-app] dev_id=%s lan_ip flipped %s -> %s; reconfiguring LAN endpoints\n",
        state.dev_id.c_str(), state.lan_ip.c_str(), lan_ip.c_str());

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
    lc.dev_id      = state.dev_id;
    lc.printer_ip  = lan_ip;
    lc.access_code = state.access_code;
    state.lan_cam  = std::make_shared<router::LanCameraSource>(lc);
    state.lan_cam->attach_source_handle(m_bambu_source);
    if (state.cam_router) state.cam_router->set_lan_source(state.lan_cam);

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

    std::fprintf(stderr, "[bridge-app] remove dev_id=%s\n", dev_id.c_str());

    if (m_ssdp) m_ssdp->remove_device(dev_id);
    if (m_mqtt) m_mqtt->remove_device(dev_id);
    if (m_ftps) m_ftps->remove_device(dev_id);
    if (m_rtsp) m_rtsp->remove_device(dev_id);
    if (m_vtun) m_vtun->remove_device(dev_id);

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
