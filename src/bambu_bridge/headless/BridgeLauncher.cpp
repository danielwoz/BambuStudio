// Bambu Bridge — multi-process launcher implementation.

#include "BridgeLauncher.hpp"

#include <nlohmann/json.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace headless {

namespace {

// Sigchld-safe: only sets a flag. main thread polls.
std::atomic<int>           g_stop_signal{0};
std::vector<pid_t>         g_children;

void on_signal(int sig) {
    g_stop_signal.store(sig);
}

void forward_signal_to_children(int sig) {
    for (pid_t pid : g_children) {
        if (pid > 0) ::kill(pid, sig);
    }
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    // SIGCHLD: just wake select/sleep, no work in handler.
    struct sigaction child_sa{};
    child_sa.sa_handler = SIG_DFL;
    child_sa.sa_flags   = SA_NOCLDSTOP;
    ::sigaction(SIGCHLD, &child_sa, nullptr);
    // Ignore SIGPIPE; children may pipe-fail us on TLS teardown.
    ::signal(SIGPIPE, SIG_IGN);
}

// Read $HOME/.config/BambuStudio/BambuStudio.conf and return the keys of
// the `access_code` object. Those keys are the real device serials
// (cloud-bound printers). Returns empty if the conf file isn't readable
// or doesn't have an access_code section.
std::vector<std::string> read_dev_ids_from_conf() {
    std::vector<std::string> out;
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        if (struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir)
            home = pw->pw_dir;
    }
    if (!home || !*home) return out;
    std::string path = std::string(home) + "/.config/BambuStudio/BambuStudio.conf";
    std::ifstream f(path);
    if (!f.good()) return out;
    nlohmann::json conf;
    try { f >> conf; }
    catch (const std::exception& e) {
        std::fprintf(stderr,
            "[bridge-multi] failed to parse %s: %s\n",
            path.c_str(), e.what());
        return out;
    }
    auto it = conf.find("access_code");
    if (it == conf.end() || !it->is_object()) return out;
    for (auto kv = it->begin(); kv != it->end(); ++kv) {
        const std::string& dev_id = kv.key();
        // Skip the FFFF-mangled mirror entries — those are the virtual
        // serials the bridge advertises, not real printers.
        if (dev_id.size() >= 4 && dev_id.compare(0, 4, "FFFF") == 0)
            continue;
        out.push_back(dev_id);
    }
    return out;
}

// Walks argv looking for `--<key> VALUE` style args. Returns a copy of
// the value if found, otherwise empty. Does not modify argv.
std::string get_arg(int argc, char** argv, const char* key) {
    const std::string want = std::string("--") + key;
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && want == argv[i]) return std::string(argv[i + 1]);
    }
    return {};
}

// Collect every `--<key> VALUE` occurrence.
std::vector<std::string> get_all_args(int argc, char** argv, const char* key) {
    std::vector<std::string> out;
    const std::string want = std::string("--") + key;
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && want == argv[i]) out.emplace_back(argv[i + 1]);
    }
    return out;
}

bool has_flag(int argc, char** argv, const char* key) {
    const std::string want = std::string("--") + key;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && want == argv[i]) return true;
    }
    return false;
}

uint16_t parse_u16(const std::string& s, uint16_t def) {
    if (s.empty()) return def;
    try { return static_cast<uint16_t>(std::stoi(s)); }
    catch (...) { return def; }
}

// Per-child plugin config directory. Each child gets its own
// `<parent>/bridge-multi/<dev_id>/`. Subdirs/files in the parent config
// dir are symlinked into the child dir, EXCEPT the plugin's mutable
// state files (`BambuNetworkEngine.conf`, `BambuStudio.conf`) which are
// copied so the two children don't race on the same fd.
// Returns the child dir path, or empty on failure.
std::string prepare_child_config_dir(const std::string& dev_id) {
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
        if (struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir)
            home = pw->pw_dir;
    }
    if (!home || !*home) return {};
    fs::path parent = fs::path(home) / ".config" / "BambuStudio";
    fs::path child  = parent / "bridge-multi" / dev_id;
    std::error_code ec;
    fs::create_directories(child, ec);
    if (ec) {
        std::fprintf(stderr,
            "[bridge-multi] could not create %s: %s\n",
            child.c_str(), ec.message().c_str());
        return {};
    }

    // Mutable per-process plugin state: must be a copy so each child
    // owns its own writable file. Re-seed every spawn so we start from
    // the parent's latest known-good state — the plugin will overwrite
    // it with child-specific changes as it runs.
    static const char* kMutableFiles[] = {
        "BambuNetworkEngine.conf",
        "BambuStudio.conf",
    };
    for (const char* name : kMutableFiles) {
        fs::path src = parent / name;
        fs::path dst = child  / name;
        if (!fs::exists(src, ec)) continue;
        fs::remove(dst, ec); // overwrite any prior copy
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::fprintf(stderr,
                "[bridge-multi] copy %s → %s failed: %s\n",
                src.c_str(), dst.c_str(), ec.message().c_str());
            ec.clear();
        }
    }

    // Everything else: symlink so the child sees the same plugins/,
    // bridge/certs/, system/, printers/, etc. Skip the mutable files
    // above and skip our own bridge-multi/ subdir to avoid loops.
    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name == "bridge-multi") continue;
        bool is_mutable = false;
        for (const char* m : kMutableFiles) if (name == m) { is_mutable = true; break; }
        if (is_mutable) continue;
        fs::path link = child / name;
        fs::remove(link, ec); // remove stale symlink/file
        ec.clear();
        fs::create_symlink(entry.path(), link, ec);
        if (ec) {
            std::fprintf(stderr,
                "[bridge-multi] symlink %s → %s failed: %s\n",
                entry.path().c_str(), link.c_str(), ec.message().c_str());
            ec.clear();
        }
    }
    return child.string();
}

// Build the argv for one child. Caller must keep the returned strings
// alive for the duration of execv() (we stash them in a vector here and
// return as char* pointers into it via vector<string>).
struct ChildArgs {
    std::vector<std::string> storage;
    std::vector<char*>       argv;
};
ChildArgs build_child_args(const std::string& self_path,
                           const std::string& dev_id,
                           uint16_t           mqtt_base,
                           uint16_t           ftps_base,
                           uint16_t           rtsp_base,
                           uint16_t           vtun_base,
                           const std::string& config_dir) {
    ChildArgs c;
    c.storage = {
        self_path,
        "--bridge-only",
        "--only-dev-id",     dev_id,
        "--mqtt-port-base",  std::to_string(mqtt_base),
        "--ftps-port-base",  std::to_string(ftps_base),
        "--rtsp-port-base",  std::to_string(rtsp_base),
        "--vtun-port-base",  std::to_string(vtun_base),
    };
    if (!config_dir.empty()) {
        c.storage.emplace_back("--config-dir");
        c.storage.emplace_back(config_dir);
    }
    for (auto& s : c.storage) c.argv.push_back(const_cast<char*>(s.c_str()));
    c.argv.push_back(nullptr);
    return c;
}

void print_usage() {
    std::fprintf(stderr,
        "usage: BambuStudio --bridge-multi [options]\n"
        "\n"
        "Spawns one --bridge-only child per real printer so each child has\n"
        "its own plugin instance and LAN slot. Supervises them: forwards\n"
        "SIGINT/SIGTERM, waits, returns the worst child exit code.\n"
        "\n"
        "Options:\n"
        "  --printer <real_sn>      Add a printer to the launch set. Repeat\n"
        "                           for each printer. If omitted, reads\n"
        "                           ~/.config/BambuStudio/BambuStudio.conf\n"
        "                           access_code section.\n"
        "  --mqtt-port-base N       First child's MQTT port (default 8883).\n"
        "                           Each subsequent child gets +1.\n"
        "  --ftps-port-base N       Default 39990. +1 per child.\n"
        "  --rtsp-port-base N       Default 38322. +1 per child.\n"
        "  -h, --help               Show this help and exit.\n");
}

} // namespace

bool is_bridge_multi(int argc, char** argv) {
    return has_flag(argc, argv, "bridge-multi");
}

int run_bridge_multi(int argc, char** argv) {
    if (has_flag(argc, argv, "help") || has_flag(argc, argv, "h")) {
        print_usage();
        return 0;
    }

    std::vector<std::string> printers = get_all_args(argc, argv, "printer");
    if (printers.empty()) {
        printers = read_dev_ids_from_conf();
        std::fprintf(stderr,
            "[bridge-multi] no --printer flags; loaded %zu devices from "
            "BambuStudio.conf access_code section\n",
            printers.size());
    }
    if (printers.empty()) {
        std::fprintf(stderr,
            "[bridge-multi] no printers to launch (pass --printer <real_sn> "
            "one or more times, or populate BambuStudio.conf first)\n");
        return 2;
    }

    const uint16_t mqtt_base = parse_u16(get_arg(argc, argv, "mqtt-port-base"), 8883);
    const uint16_t ftps_base = parse_u16(get_arg(argc, argv, "ftps-port-base"), 39990);
    const uint16_t rtsp_base = parse_u16(get_arg(argc, argv, "rtsp-port-base"), 38322);
    const uint16_t vtun_base = parse_u16(get_arg(argc, argv, "vtun-port-base"), 39998);

    const std::string self_path = (argc > 0 && argv[0]) ? argv[0] : "BambuStudio";

    install_signal_handlers();
    g_children.assign(printers.size(), 0);

    // Cloud-login stagger (seconds). Bambu's cloud appears to
    // rate-limit / deduplicate concurrent logins from the same
    // account: when 2+ children call connect_server within a few
    // seconds of each other the cloud sometimes leaves one session
    // in a degraded "connect+subscribe OK but send_message refuses
    // with rc_cloud=-2" state. Spacing the spawns lets each child's
    // cloud_bringup complete and settle before the next one starts.
    // Default 30s — empirically sufficient on a 3-printer account.
    // Override via BAMBU_BRIDGE_MULTI_STAGGER_S=<n> for testing.
    int stagger_s = 30;
    if (const char* env = std::getenv("BAMBU_BRIDGE_MULTI_STAGGER_S");
        env && *env) {
        try { stagger_s = std::stoi(env); } catch (...) {}
        if (stagger_s < 0) stagger_s = 0;
    }

    for (size_t i = 0; i < printers.size(); ++i) {
        if (i > 0 && stagger_s > 0) {
            std::fprintf(stderr,
                "[bridge-multi] stagger sleep %ds before spawning %s "
                "(prevents concurrent-cloud-login rate-limit)\n",
                stagger_s, printers[i].c_str());
            std::fflush(stderr);
            std::this_thread::sleep_for(std::chrono::seconds(stagger_s));
        }
        std::string cdir = prepare_child_config_dir(printers[i]);
        auto child = build_child_args(
            self_path, printers[i],
            static_cast<uint16_t>(mqtt_base + i),
            static_cast<uint16_t>(ftps_base + i),
            static_cast<uint16_t>(rtsp_base + i),
            static_cast<uint16_t>(vtun_base + i),
            cdir);
        pid_t pid = ::fork();
        if (pid < 0) {
            std::fprintf(stderr,
                "[bridge-multi] fork failed for %s: %s\n",
                printers[i].c_str(), std::strerror(errno));
            forward_signal_to_children(SIGTERM);
            return 1;
        }
        if (pid == 0) {
            // Child: exec self with the bridge-only argv. No additional
            // setup — we want the new process image to look identical
            // to a stand-alone `BambuStudio --bridge-only` invocation
            // so the plugin can't tell the difference.
            ::execv(self_path.c_str(), child.argv.data());
            // execv only returns on failure.
            std::fprintf(stderr,
                "[bridge-multi] execv %s failed: %s\n",
                self_path.c_str(), std::strerror(errno));
            ::_Exit(127);
        }
        g_children[i] = pid;
        std::fprintf(stderr,
            "[bridge-multi] spawned pid=%d for %s (mqtt=%u ftps=%u rtsp=%u vtun=%u config=%s)\n",
            int(pid), printers[i].c_str(),
            unsigned(mqtt_base + i),
            unsigned(ftps_base + i),
            unsigned(rtsp_base + i),
            unsigned(vtun_base + i),
            cdir.empty() ? "<default>" : cdir.c_str());
    }
    std::fflush(stderr);

    // Supervise loop: waitpid for each child, propagate signal to all
    // on SIGINT/SIGTERM, return the worst exit code.
    int  worst_rc = 0;
    bool forwarded_signal = false;
    size_t alive = g_children.size();
    while (alive > 0) {
        int sig = g_stop_signal.exchange(0);
        if (sig != 0 && !forwarded_signal) {
            std::fprintf(stderr,
                "[bridge-multi] forwarding signal %d to %zu children\n",
                sig, alive);
            forward_signal_to_children(SIGTERM);
            forwarded_signal = true;
        }
        int   status = 0;
        pid_t r = ::waitpid(-1, &status, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            // No more children.
            break;
        }
        // Find which slot this was.
        auto it = std::find(g_children.begin(), g_children.end(), r);
        if (it != g_children.end()) *it = 0;
        --alive;
        int rc = 0;
        if (WIFEXITED(status))   rc = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) rc = 128 + WTERMSIG(status);
        std::fprintf(stderr,
            "[bridge-multi] child pid=%d exited rc=%d (%zu remaining)\n",
            int(r), rc, alive);
        if (rc > worst_rc) worst_rc = rc;
    }
    std::fflush(stderr);
    return worst_rc;
}

} // namespace headless
} // namespace bridge
} // namespace Slic3r
