// Bambu Bridge — direct-LAN uplink implementation.
//
// See LanUplink.hpp for the design discussion. Per-device state is
// largely a routing table: the proprietary plugin owns the LAN TLS+MQTT
// transport, so this file is just plumbing between IUplink callbacks
// and the `BambuNetworkingPluginHandle` LAN-side wrappers.

#include "LanUplink.hpp"

#include "../BambuNetworkingPluginHandle.hpp"
#include "../EncMsgEnvelope.hpp"
#include "RawMqttPublisher.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

// Cheap-and-honest check: is the top-level key of this JSON payload
// literally `"print"`? We don't need a full parse — control payloads are
// always `{"print":{...}}` with no leading whitespace beyond what the
// slicer happens to emit. nlohmann::json::accept + a key probe would
// also work but parsing the whole document just to look at the first
// key is wasteful when the slicer publishes 64-bit-deep status frames
// 10x/sec. Robust enough for the only thing we care about: routing
// `print.command=*` vs `pushing.*` / `info.*`.
bool payload_is_print_control(const std::string& json) {
    // Skip leading whitespace.
    size_t i = 0;
    while (i < json.size() &&
           (json[i] == ' ' || json[i] == '\t' ||
            json[i] == '\n' || json[i] == '\r')) ++i;
    if (i >= json.size() || json[i] != '{') return false;
    ++i;
    while (i < json.size() &&
           (json[i] == ' ' || json[i] == '\t' ||
            json[i] == '\n' || json[i] == '\r')) ++i;
    if (i >= json.size() || json[i] != '"') return false;
    ++i;
    // Expect literal "print" followed by closing quote.
    static const char kPrint[] = "print";
    constexpr size_t  kPlen    = sizeof(kPrint) - 1;
    if (i + kPlen >= json.size()) return false;
    if (std::memcmp(json.data() + i, kPrint, kPlen) != 0) return false;
    if (json[i + kPlen] != '"') return false;
    return true;
}

// Resolve the path to the python helper. The script lives next to the
// compiled bridge binary in tree under src/bambu_bridge/router/. At
// runtime we look for it relative to:
//   1) BBL_BRIDGE_RAW_MQTT_HELPER env var (override)
//   2) /proc/self/exe's parent directory chain (build/src -> ../../src/bambu_bridge/router/)
//   3) the source-tree canonical path
// Cached after first successful resolution.
std::string resolve_helper_path_once() {
    static std::string cached;
    static std::once_flag flag;
    std::call_once(flag, []{
        // 1) explicit override
        if (const char* env = std::getenv("BBL_BRIDGE_RAW_MQTT_HELPER");
            env && *env) {
            struct stat st;
            if (::stat(env, &st) == 0) { cached = env; return; }
        }
        // 2) walk up from /proc/self/exe
        char buf[4096] = {0};
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            std::string exe(buf, buf + n);
            // strip filename
            auto slash = exe.find_last_of('/');
            std::string dir = (slash == std::string::npos)
                ? std::string(".") : exe.substr(0, slash);
            // Common layouts:
            //   build/src/bambu-studio          -> ../../src/bambu_bridge/router/
            //   build/bin/bambu-studio          -> ../../src/bambu_bridge/router/
            //   <prefix>/bin/bambu-studio       -> ../share/... (unused today)
            const std::vector<std::string> candidates = {
                dir + "/../../src/bambu_bridge/router/raw_mqtt_publish.py",
                dir + "/../../../src/bambu_bridge/router/raw_mqtt_publish.py",
                dir + "/raw_mqtt_publish.py",
            };
            for (const auto& p : candidates) {
                struct stat st;
                if (::stat(p.c_str(), &st) == 0) { cached = p; return; }
            }
        }
        // 3) canonical source-tree path (developer machines)
        const char* canonical =
            "/home/danielwoz/BambuStudio-bridge/src/bambu_bridge/router/"
            "raw_mqtt_publish.py";
        struct stat st;
        if (::stat(canonical, &st) == 0) { cached = canonical; return; }
        cached.clear();  // empty -> caller logs and falls through
    });
    return cached;
}

// Write payload to a private temp file. Returns path, empty on failure.
std::string write_tmp_payload(const std::vector<uint8_t>& payload,
                              const std::string& dev_id) {
    char tmpl[] = "/tmp/bblbridge_print_XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        std::fprintf(stderr,
            "[print-via-cert dev=%s] mkstemp failed: %s\n",
            dev_id.c_str(), std::strerror(errno));
        return {};
    }
    size_t off = 0;
    while (off < payload.size()) {
        ssize_t w = ::write(fd, payload.data() + off, payload.size() - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            ::close(fd); ::unlink(tmpl);
            return {};
        }
        off += static_cast<size_t>(w);
    }
    ::close(fd);
    return std::string(tmpl);
}

// Generate a slicer-shaped client_id: "slicer:<unix>:<rand>".
std::string make_client_id() {
    static std::mt19937 s_rng{std::random_device{}()};
    static std::mutex   s_mu;
    static std::atomic<unsigned> s_counter{0};
    char buf[64];
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    unsigned r;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        r = std::uniform_int_distribution<unsigned>(0, 0xFFFF)(s_rng);
    }
    unsigned c = ++s_counter;
    std::snprintf(buf, sizeof(buf), "bridge:%lld:%04x%04x",
                  static_cast<long long>(sec), r, c & 0xFFFF);
    return buf;
}

// Synchronously run raw_mqtt_publish.py with the given args. Returns
// the helper's exit status (0 = PUBACK), or a negative value on
// spawn/wait failure.
int spawn_raw_mqtt_helper(const std::string& helper_path,
                          const std::string& printer_ip,
                          uint16_t           printer_port,
                          const std::string& cert_path,
                          const std::string& key_path,
                          const std::string& access_code,
                          const std::string& client_id,
                          const std::string& topic,
                          uint8_t            qos,
                          const std::string& payload_file,
                          const std::string& dev_id) {
    // Build argv. Strings owned by the calling frame; argv[] points
    // into them. No exec interpolation — args go via execve.
    std::string port_s = std::to_string(printer_port);
    std::string qos_s  = std::to_string(static_cast<int>(qos));

    std::vector<const char*> argv = {
        "python3", helper_path.c_str(),
        "--ip",          printer_ip.c_str(),
        "--port",        port_s.c_str(),
        "--cert",        cert_path.c_str(),
        "--key",         key_path.c_str(),
        "--user",        "bblp",
        "--pass",        access_code.c_str(),
        "--client-id",   client_id.c_str(),
        "--topic",       topic.c_str(),
        "--qos",         qos_s.c_str(),
        "--payload-file",payload_file.c_str(),
        nullptr,
    };

    pid_t pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr,
            "[print-via-cert dev=%s] fork failed: %s\n",
            dev_id.c_str(), std::strerror(errno));
        return -1;
    }
    if (pid == 0) {
        // Child. Reopen stdin from /dev/null (helper reads payload via
        // --payload-file). stdout/stderr inherit the bridge's so the
        // helper's diagnostics flow to the bridge log.
        int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        ::execvp("python3", const_cast<char* const*>(argv.data()));
        // exec failed
        std::fprintf(stderr,
            "[print-via-cert dev=%s] execvp python3 failed: %s\n",
            dev_id.c_str(), std::strerror(errno));
        std::_Exit(127);
    }
    // Parent: wait with a wall-clock guard so a stuck helper can't pin
    // the broker thread.
    int status = 0;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(15);
    while (true) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr,
                "[print-via-cert dev=%s] waitpid failed: %s\n",
                dev_id.c_str(), std::strerror(errno));
            return -2;
        }
        // r == 0: still running
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr,
                "[print-via-cert dev=%s] helper timeout — killing pid=%d\n",
                dev_id.c_str(), pid);
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -3;
        }
        ::usleep(20 * 1000);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -100 - WTERMSIG(status);
    return -200;
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct LanUplink::Impl {
    struct DeviceState {
        LanUplinkConfig                                cfg;
        // Topic → refcount. Cloud / LAN both use the same convention:
        // first add → SUBSCRIBE on the wire; last drop → UNSUBSCRIBE on
        // the wire. The plugin's LAN broker, like real Bambu LAN MQTT,
        // auto-pushes `device/<dev_id>/report` after connect_printer
        // succeeds — but we still refcount slicer-side subscriptions so
        // attach_downstream cleanup stays hygienic.
        std::unordered_map<std::string, int> topic_refs;
    };

    std::shared_ptr<BambuNetworkingPluginHandle>                  handle;
    // Ship 7 — optional native enc_msg envelope wrapper. When set,
    // on_publish wraps print.* payloads before cert+key publish; otherwise
    // the raw payload is passed through unchanged (legacy path).
    std::shared_ptr<EncMsgEnvelope>                               enc_msg;
    mutable std::mutex                                            mu;
    std::unordered_map<std::string, std::unique_ptr<DeviceState>> devices;
    std::unordered_map<std::string,
                       server::IUplink::DownstreamPublisher>      downstreams;

    // The plugin only supports ONE active LAN connection at a time. Track
    // which dev_id currently owns it; is_connected(dev_id) returns true
    // only when the plugin reports up AND `current_connected_dev_id`
    // matches the caller's dev_id.
    std::string                                                   current_connected_dev_id;

    DeviceState* find_locked(const std::string& dev_id) {
        auto it = devices.find(dev_id);
        return it == devices.end() ? nullptr : it->second.get();
    }
};

// ---------------------------------------------------------------------------
// LanUplink public API
// ---------------------------------------------------------------------------

LanUplink::LanUplink()  : m_impl(std::make_unique<Impl>()) {}

LanUplink::~LanUplink() {
    // Drop every registered local-message receiver and disconnect the
    // plugin's LAN session if we hold it. Snapshot under lock first so
    // plugin callbacks acquiring the handle's own mutex don't deadlock
    // against ours.
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    std::vector<std::string> dev_ids;
    bool we_were_connected = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        h = m_impl->handle;
        for (auto& kv : m_impl->devices) dev_ids.push_back(kv.first);
        we_were_connected = !m_impl->current_connected_dev_id.empty();
    }
    if (h) {
        for (const auto& d : dev_ids) h->unregister_local_message_receiver(d);
        if (we_were_connected) h->disconnect_printer();
    }
}

void LanUplink::attach_plugin(std::shared_ptr<BambuNetworkingPluginHandle> handle) {
    // Same pattern as CloudUplink::attach_plugin: re-register every
    // existing device's local-message receiver against the new handle.
    std::vector<std::string> dev_ids;
    std::shared_ptr<BambuNetworkingPluginHandle> old;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        old = m_impl->handle;
        m_impl->handle = handle;
        for (auto& kv : m_impl->devices) dev_ids.push_back(kv.first);
    }
    if (old) {
        for (const auto& d : dev_ids) old->unregister_local_message_receiver(d);
    }
    if (handle) {
        Impl* impl = m_impl.get();
        for (const auto& dev_id : dev_ids) {
            handle->register_local_message_receiver(dev_id,
                [impl, dev_id](std::string topic,
                               std::vector<uint8_t> payload,
                               uint8_t qos) {
                server::IUplink::DownstreamPublisher cb;
                {
                    std::lock_guard<std::mutex> lk(impl->mu);
                    auto it = impl->downstreams.find(dev_id);
                    if (it != impl->downstreams.end()) cb = it->second;
                }
                if (cb) cb(std::move(topic), std::move(payload), qos);
            });
        }
    }
}

std::shared_ptr<BambuNetworkingPluginHandle> LanUplink::plugin_handle() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->handle;
}

void LanUplink::attach_enc_msg_envelope(std::shared_ptr<EncMsgEnvelope> envelope) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->enc_msg = std::move(envelope);
}

std::shared_ptr<EncMsgEnvelope> LanUplink::enc_msg_envelope() const {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->enc_msg;
}

void LanUplink::add_device(LanUplinkConfig cfg) {
    const std::string dev_id     = cfg.dev_id;
    const std::string dev_ip     = cfg.printer_ip;
    const std::string access     = cfg.access_code;
    const bool        use_ssl    = cfg.use_ssl;
    std::fprintf(stderr,
        "[lan-uplink] add_device dev=%s ip=%s ssl=%d ac_len=%zu\n",
        dev_id.c_str(), dev_ip.c_str(), use_ssl ? 1 : 0, access.size());
    std::fflush(stderr);

    std::shared_ptr<BambuNetworkingPluginHandle> h;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto state = std::make_unique<Impl::DeviceState>();
        state->cfg = std::move(cfg);
        m_impl->devices[dev_id] = std::move(state);
        h = m_impl->handle;
    }

    if (!h) return;

    // Register the local-message receiver up front. The plugin may
    // start firing reports as soon as connect_printer lands.
    Impl* impl = m_impl.get();
    h->register_local_message_receiver(dev_id,
        [impl, dev_id](std::string topic,
                       std::vector<uint8_t> payload,
                       uint8_t qos) {
        server::IUplink::DownstreamPublisher cb;
        {
            std::lock_guard<std::mutex> lk(impl->mu);
            auto it = impl->downstreams.find(dev_id);
            if (it != impl->downstreams.end()) cb = it->second;
        }
        if (cb) cb(std::move(topic), std::move(payload), qos);
    });

    // Establish the LAN connection. The plugin only holds one LAN
    // connection at a time — if a different dev_id was previously
    // connected the plugin's own re-entry semantics handle the swap.
    int rc = h->connect_printer(dev_id, dev_ip, "bblp", access, use_ssl);
    std::fprintf(stderr,
        "[lan-uplink] add_device dev=%s connect_printer rc=%d\n",
        dev_id.c_str(), rc);
    std::fflush(stderr);
    if (rc == 0) {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        m_impl->current_connected_dev_id = dev_id;
    }
}

void LanUplink::remove_device(const std::string& dev_id) {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool was_current = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto it = m_impl->devices.find(dev_id);
        if (it == m_impl->devices.end()) return;
        m_impl->devices.erase(it);
        m_impl->downstreams.erase(dev_id);
        if (m_impl->current_connected_dev_id == dev_id) {
            m_impl->current_connected_dev_id.clear();
            was_current = true;
        }
        h = m_impl->handle;
    }
    if (h) {
        h->unregister_local_message_receiver(dev_id);
        // The plugin only has one LAN connection slot — disconnecting it
        // when the device that owned it goes away. If a different
        // dev_id was the active one, leave the plugin alone.
        if (was_current) h->disconnect_printer();
    }
}

bool LanUplink::is_connected(const std::string& dev_id) const {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool current_match = false;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->devices.count(dev_id) == 0) return false;
        h = m_impl->handle;
        current_match = (m_impl->current_connected_dev_id == dev_id);
    }
    if (!h || !current_match) return false;
    return h->is_local_connected();
}

void LanUplink::on_subscribe(const std::string& dev_id, std::string topic) {
    // The plugin only holds ONE active LAN connection at a time. When a
    // slicer subscribes to a device that isn't currently the plugin's
    // active dev_id, we have to swap — otherwise the plugin's
    // local-message receiver only fires for the previous device and
    // the slicer sees no push_status.
    //
    // We still refcount slicer-side topic subscribes for cleanup
    // hygiene (attach_downstream + on_disconnect coordinate against it).
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    bool need_swap = false;
    bool found = false;
    std::string dev_ip;
    std::string access_code;
    bool use_ssl = true;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto* d = m_impl->find_locked(dev_id);
        found = (d != nullptr);
        if (d) {
            ++d->topic_refs[topic];
            if (m_impl->current_connected_dev_id != dev_id) {
                need_swap   = true;
                dev_ip      = d->cfg.printer_ip;
                access_code = d->cfg.access_code;
                use_ssl     = d->cfg.use_ssl;
                h           = m_impl->handle;
            }
        }
    }
    if (!found) return;
    if (need_swap && h) {
        std::fprintf(stderr,
            "[lan-uplink] SWAP active dev → %s (ip=%s) on slicer subscribe %s\n",
            dev_id.c_str(), dev_ip.c_str(), topic.c_str());
        std::fflush(stderr);
        int rc = h->connect_printer(dev_id, dev_ip, "bblp", access_code, use_ssl);
        if (rc == 0) {
            std::lock_guard<std::mutex> lk(m_impl->mu);
            m_impl->current_connected_dev_id = dev_id;
        } else {
            std::fprintf(stderr,
                "[lan-uplink] SWAP failed rc=%d for dev=%s\n", rc, dev_id.c_str());
            std::fflush(stderr);
        }
    }
}

void LanUplink::on_publish(const std::string& dev_id, std::string topic,
                           std::vector<uint8_t> payload, uint8_t qos) {
    std::shared_ptr<BambuNetworkingPluginHandle> h;
    std::shared_ptr<EncMsgEnvelope>              enc;
    bool have_device = false;
    LanUplinkConfig cfg;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        auto it = m_impl->devices.find(dev_id);
        have_device = (it != m_impl->devices.end());
        if (have_device) cfg = it->second->cfg;
        h   = m_impl->handle;
        enc = m_impl->enc_msg;
    }
    if (!have_device) {
        std::fprintf(stderr,
            "[lan-uplink] on_publish DROP dev=%s have_dev=0 bytes=%zu\n",
            dev_id.c_str(), payload.size());
        std::fflush(stderr);
        return;
    }
    std::string json(payload.begin(), payload.end());

    // Route control commands (`{"print":...}`) via the per-printer
    // mTLS cert+key (subprocess to raw_mqtt_publish.py). The
    // proprietary plugin silently drops `print.command=*` from
    // non-UI contexts; the printer's LAN broker accepts the publish
    // when the TLS handshake presents a valid client cert (see
    // DISCOVERY-2026-05-22.md). Other payloads (`pushing.*`, `info.*`)
    // still go through the plugin so `pushall`, `get_version`, etc.
    // keep using the persistent plugin session.
    const bool is_print = payload_is_print_control(json);
    const bool have_cert = !cfg.mtls_cert_path.empty()
                        && !cfg.mtls_key_path.empty();
    if (is_print && have_cert) {
        // Ship 7 — if a native enc_msg envelope wrapper is configured,
        // wrap the print.* payload BEFORE handing it to the cert+key
        // helper. This produces a plugin-compatible signed envelope so
        // newer firmware (which validates the enc_msg signature) accepts
        // the publish. Without an envelope wrapper, the raw payload is
        // published unsigned (legacy path; works on older firmware that
        // doesn't enforce enc_msg).
        bool wrap_ok = true;
        if (enc) {
            try {
                const size_t in_sz = json.size();
                std::string env = enc->wrap(json);
                json = std::move(env);
                payload.assign(json.begin(), json.end());
                std::fprintf(stderr,
                    "[lan-uplink] enc_msg wrap dev=%s in=%zuB out=%zuB\n",
                    dev_id.c_str(), in_sz, payload.size());
                std::fflush(stderr);
            } catch (const std::exception& ex) {
                std::fprintf(stderr,
                    "[lan-uplink] enc_msg wrap FAILED dev=%s: %s; "
                    "falling back to plugin path\n",
                    dev_id.c_str(), ex.what());
                std::fflush(stderr);
                wrap_ok = false;
            }
        }
        if (!wrap_ok) {
            // Skip the cert+key publish (firmware would reject the
            // unsigned payload) and fall through to the plugin path
            // by skipping past the cert+key block below.
        } else {
        const std::string helper = resolve_helper_path_once();
        if (helper.empty()) {
            std::fprintf(stderr,
                "[print-via-cert dev=%s] helper script not found "
                "(set BBL_BRIDGE_RAW_MQTT_HELPER); falling back to plugin\n",
                dev_id.c_str());
            std::fflush(stderr);
        } else {
            // Drop payload to a temp file so the helper reads exact
            // bytes (avoid argv length limits and stdin race).
            const std::string tmpfile = write_tmp_payload(payload, dev_id);
            if (tmpfile.empty()) {
                std::fprintf(stderr,
                    "[print-via-cert dev=%s] tmp file write failed; falling back\n",
                    dev_id.c_str());
                std::fflush(stderr);
            } else {
                const std::string client_id = make_client_id();
                const std::string topic_real =
                    std::string("device/") + dev_id + "/request";
                int rc = spawn_raw_mqtt_helper(
                    helper,
                    cfg.printer_ip,
                    /*port*/ 8883,
                    cfg.mtls_cert_path,
                    cfg.mtls_key_path,
                    cfg.access_code,
                    client_id,
                    topic_real,
                    qos,
                    tmpfile,
                    dev_id);
                ::unlink(tmpfile.c_str());
                std::fprintf(stderr,
                    "[print-via-cert] dev=%s topic=%s qos=%u bytes=%zu rc=%d\n",
                    dev_id.c_str(), topic_real.c_str(),
                    unsigned(qos), payload.size(), rc);
                std::fflush(stderr);
                return;
            }
        }
        }   // end ship-7 wrap_ok branch
    }

    if (!h) {
        std::fprintf(stderr,
            "[lan-uplink] on_publish DROP dev=%s handle=null bytes=%zu\n",
            dev_id.c_str(), payload.size());
        std::fflush(stderr);
        return;
    }

    // Plugin path for everything else — status reads (`pushing.*`),
    // version probes (`info.command=get_version`), and any other
    // non-control publish the slicer issues.
    int rc = h->send_message_to_printer(dev_id, json, static_cast<int>(qos));
    std::fprintf(stderr,
        "[lan-uplink] on_publish dev=%s bytes=%zu qos=%u send_message_to_printer rc=%d\n",
        dev_id.c_str(), payload.size(), unsigned(qos), rc);
    std::fflush(stderr);
}

void LanUplink::on_unsubscribe(const std::string& dev_id, std::string topic) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    auto* d = m_impl->find_locked(dev_id);
    if (!d) return;
    auto it = d->topic_refs.find(topic);
    if (it == d->topic_refs.end()) return;
    if (--it->second <= 0) {
        d->topic_refs.erase(it);
    }
}

void LanUplink::on_disconnect(const std::string& dev_id) {
    // Slicer detached. Match CloudUplink behaviour: we do NOT tear down
    // the plugin's LAN session — the bridge keeps it hot so reports keep
    // flowing for the next slicer that connects. Just drop the
    // per-device downstream publisher.
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->downstreams.erase(dev_id);
}

void LanUplink::attach_downstream(const std::string& dev_id,
                                  DownstreamPublisher publisher) {
    std::lock_guard<std::mutex> lk(m_impl->mu);
    if (publisher) m_impl->downstreams[dev_id] = std::move(publisher);
    else           m_impl->downstreams.erase(dev_id);
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
