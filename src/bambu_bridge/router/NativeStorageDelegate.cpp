// Bambu Bridge — native storage delegate.
//
// See NativeStorageDelegate.hpp for the design rationale. This .cpp file
// is the wire-format work: build the `{mtype, cmdtype, sequence, req}`
// envelope LocalControlTunnel expects, parse the reply back out into the
// `{result, reply}` shape VirtualTunnelServer's session_loop wants to
// splice into its own envelope.

#include "NativeStorageDelegate.hpp"

#include "LocalControlTunnel.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <utility>

namespace Slic3r {
namespace bridge {
namespace router {

namespace {

constexpr int kCtrlMtype = 0x3001;

std::atomic<uint32_t> g_seq{1};

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char) std::toupper(c); });
    return s;
}

bool contains_token(const std::string& haystack_upper,
                    const std::string& needle) {
    return haystack_upper.find(needle) != std::string::npos;
}

}  // namespace

bool NativeStorageDelegate::model_is_native(const std::string& model) {
    // Bambu vendor model strings observed in the wild:
    //   "3DPrinter-X1"  (X1)
    //   "3DPrinter-X1-Carbon" (X1C)
    //   "C12"           (X1E sometimes appears this way)
    //   "C11"           (X1, older firmware)
    //   "C13"           (P1P)
    //   "C14"           (P1S — sometimes also "P1S")
    //   "N1"            (A1 mini)
    //   "N2S"           (A1)
    //   "C16"           (H2D)
    //   "C18"           (H2S)
    // The user-visible config sometimes carries "X1C", "P1S", "P1P",
    // "A1", "A1mini", "H2S", "H2D" directly. We match on substrings of
    // either spelling.
    if (model.empty()) return false;
    const std::string up = to_upper(model);
    // Family substrings for native-legacy-protocol speakers.
    if (contains_token(up, "X1")) return true;   // X1, X1C, X1E
    if (contains_token(up, "P1")) return true;   // P1S, P1P
    // The C11..C14 internal codes are the same printers; allow them too.
    if (contains_token(up, "C11")) return true;  // X1
    if (contains_token(up, "C12")) return true;  // X1E
    if (contains_token(up, "C13")) return true;  // P1P
    if (contains_token(up, "C14")) return true;  // P1S
    return false;
}

std::string NativeStorageDelegate::normalize_model_tag(const std::string& model) {
    if (model.empty()) return "";
    const std::string up = to_upper(model);
    if (contains_token(up, "X1C") || contains_token(up, "CARBON")) return "X1C";
    if (contains_token(up, "X1E")) return "X1E";
    if (contains_token(up, "X1"))  return "X1";
    if (contains_token(up, "P1S")) return "P1S";
    if (contains_token(up, "P1P")) return "P1P";
    if (contains_token(up, "H2S")) return "H2S";
    if (contains_token(up, "H2D")) return "H2D";
    if (contains_token(up, "A1"))  return "A1";
    return model;
}

NativeStorageDelegate::~NativeStorageDelegate() = default;

void NativeStorageDelegate::register_device(const std::string& dev_id,
                                            const std::string& model) {
    auto entry    = std::make_shared<DeviceEntry>();
    entry->model  = model;
    entry->native = model_is_native(model);

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_devices[dev_id] = entry;
    }

    const std::string tag = normalize_model_tag(model);
    if (entry->native) {
        std::fprintf(stderr,
                     "[bridge-app] dev=%s model=%s -> using NativeStorageDelegate (port 6000)\n",
                     dev_id.c_str(), tag.c_str());
    } else {
        std::fprintf(stderr,
                     "[bridge-app] dev=%s model=%s -> using plugin storage delegate (model not in native list)\n",
                     dev_id.c_str(), tag.c_str());
    }
    std::fflush(stderr);
}

void NativeStorageDelegate::unregister_device(const std::string& dev_id) {
    std::shared_ptr<DeviceEntry> entry;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        auto it = m_devices.find(dev_id);
        if (it == m_devices.end()) return;
        entry = std::move(it->second);
        m_devices.erase(it);
    }
    // entry's tunnel (if any) is closed in its destructor under tunnel_mu.
    if (entry) {
        std::lock_guard<std::mutex> tk(entry->tunnel_mu);
        if (entry->tunnel) entry->tunnel->shutdown();
        entry->tunnel.reset();
    }
}

void NativeStorageDelegate::set_fallback(StorageDelegateFn fallback) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_fallback = std::move(fallback);
}

std::shared_ptr<NativeStorageDelegate::DeviceEntry>
NativeStorageDelegate::find_(const std::string& dev_id) {
    std::lock_guard<std::mutex> lk(m_mu);
    auto it = m_devices.find(dev_id);
    if (it == m_devices.end()) return nullptr;
    return it->second;
}

StorageDelegateFn NativeStorageDelegate::make_delegate() {
    std::weak_ptr<NativeStorageDelegate> weak = shared_from_this();
    return [weak](const std::string& real_dev_id,
                  const std::string& real_lan_ip,
                  const std::string& access_code,
                  const std::string& dev_ver,
                  const std::string& net_ver,
                  const std::string& cli_id,
                  const std::string& cli_ver,
                  int                cmdtype,
                  std::string        request_body_json,
                  std::function<void(int, std::string)> reply_cb) {
        auto self = weak.lock();
        if (!self) {
            if (reply_cb) reply_cb(-1, "{}");
            return;
        }
        auto entry = self->find_(real_dev_id);
        if (entry && entry->native) {
            self->handle_native_(entry, real_dev_id, real_lan_ip, access_code,
                                 cmdtype, std::move(request_body_json),
                                 std::move(reply_cb));
        } else {
            // Not registered or non-native model — go to fallback.
            self->handle_fallback_(real_dev_id, real_lan_ip, access_code,
                                   dev_ver, net_ver, cli_id, cli_ver,
                                   cmdtype, std::move(request_body_json),
                                   std::move(reply_cb));
        }
    };
}

void NativeStorageDelegate::handle_fallback_(
        const std::string& real_dev_id,
        const std::string& real_lan_ip,
        const std::string& access_code,
        const std::string& dev_ver,
        const std::string& net_ver,
        const std::string& cli_id,
        const std::string& cli_ver,
        int                cmdtype,
        std::string        request_body_json,
        std::function<void(int, std::string)> reply_cb) {
    StorageDelegateFn fb;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        fb = m_fallback;
    }
    if (fb) {
        fb(real_dev_id, real_lan_ip, access_code, dev_ver, net_ver,
           cli_id, cli_ver, cmdtype, std::move(request_body_json),
           std::move(reply_cb));
    } else {
        // No fallback installed (headless mode without a GUI). Refuse
        // cleanly so the slicer side sees a not-supported reply and
        // backs off.
        if (reply_cb) reply_cb(/*rc=*/-1, /*reply=*/"{}");
    }
}

void NativeStorageDelegate::handle_native_(
        const std::shared_ptr<DeviceEntry>& entry,
        const std::string& dev_id,
        const std::string& real_lan_ip,
        const std::string& access_code,
        int                cmdtype,
        std::string        request_body_json,
        std::function<void(int, std::string)> reply_cb) {
    if (real_lan_ip.empty()) {
        std::fprintf(stderr,
                     "[native-storage] dev=%s no LAN IP, refusing cmdtype=%d\n",
                     dev_id.c_str(), cmdtype);
        std::fflush(stderr);
        if (reply_cb) reply_cb(-1, "{}");
        return;
    }

    // Serialize per-device tunnel access. The vtun server can fan multiple
    // session threads at us (in theory) — keep one tunnel per device, one
    // request at a time.
    std::lock_guard<std::mutex> tk(entry->tunnel_mu);

    // Lazily open the tunnel. If it dropped between calls, reopen.
    if (!entry->tunnel || !entry->tunnel->is_open()) {
        entry->tunnel = std::make_unique<LocalControlTunnel>(
            real_lan_ip, /*port=*/6000, access_code);
        int rc = entry->tunnel->connect_and_open(/*timeout_ms=*/10000);
        if (rc != 0) {
            std::fprintf(stderr,
                         "[native-storage] dev=%s tunnel open failed rc=%d, "
                         "falling back to plugin\n",
                         dev_id.c_str(), rc);
            std::fflush(stderr);
            entry->tunnel.reset();
            // Tunnel open failed; let the fallback delegate (PFS-via-plugin)
            // have a crack at it. Most useful when the printer is in a
            // weird state and the legacy port is genuinely unreachable.
            handle_fallback_(dev_id, real_lan_ip, access_code,
                             /*dev_ver=*/"", /*net_ver=*/"",
                             /*cli_id=*/"", /*cli_ver=*/"",
                             cmdtype, std::move(request_body_json),
                             std::move(reply_cb));
            return;
        }
    }

    // Parse the inner `req` body (slicer-side already serialized it).
    nlohmann::json req_body;
    try {
        req_body = nlohmann::json::parse(request_body_json);
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "[native-storage] dev=%s bad request_body_json: %s\n",
                     dev_id.c_str(), ex.what());
        std::fflush(stderr);
        if (reply_cb) reply_cb(-1, "{}");
        return;
    }
    if (!req_body.is_object()) req_body = nlohmann::json::object();

    // Build the wire envelope LocalControlTunnel expects:
    //   {"mtype":12289,"cmdtype":N,"sequence":S,"req":{...}}
    nlohmann::json wire = {
        {"mtype",    kCtrlMtype},
        {"cmdtype",  cmdtype},
        {"sequence", (int) g_seq.fetch_add(1, std::memory_order_relaxed)},
        {"req",      std::move(req_body)},
    };
    std::string body = wire.dump();

    std::string          reply_json;
    std::vector<uint8_t> reply_bin;
    int rc = entry->tunnel->request(body, reply_json, reply_bin, /*timeout_ms=*/8000);
    if (rc != 0) {
        std::fprintf(stderr,
                     "[native-storage] dev=%s cmdtype=%d wire-rc=%d\n",
                     dev_id.c_str(), cmdtype, rc);
        std::fflush(stderr);
        // Tunnel error — close it so the next call reopens fresh.
        entry->tunnel->shutdown();
        entry->tunnel.reset();
        if (reply_cb) reply_cb(-1, "{}");
        return;
    }

    // Extract the inner `reply` and `result` from the printer's response so
    // the vtun session_loop can splice them into its own envelope.
    int            app_result = 0;
    nlohmann::json reply_obj  = nlohmann::json::object();
    try {
        auto parsed = nlohmann::json::parse(reply_json);
        if (parsed.contains("result") && parsed["result"].is_number_integer()) {
            app_result = parsed["result"].get<int>();
        }
        if (parsed.contains("reply")) {
            reply_obj = parsed["reply"];
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "[native-storage] dev=%s bad reply JSON: %s body=%.200s\n",
                     dev_id.c_str(), ex.what(), reply_json.c_str());
        std::fflush(stderr);
    }

    std::fprintf(stderr,
                 "[native-storage] dev=%s cmdtype=%d result=%d reply_bytes=%zu\n",
                 dev_id.c_str(), cmdtype, app_result, reply_json.size());
    std::fflush(stderr);

    if (reply_cb) reply_cb(app_result, reply_obj.dump());
}

}  // namespace router
}  // namespace bridge
}  // namespace Slic3r
