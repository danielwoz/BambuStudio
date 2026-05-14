// Bambu Bridge — E2E test harness (phase 12).

#include "E2EHarness.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace e2e {

namespace {

// Set of ephemeral-range port bases. Picked above the BridgeAppMultiDevice
// test's 47700 band so two test runners on the same host don't fight.
constexpr uint16_t kMqttPortBase = 48883;
constexpr uint16_t kFtpsPortBase = 49990;
constexpr uint16_t kRtspPortBase = 48322;

} // namespace

E2EHarness::E2EHarness(std::vector<PrinterEndpoint> endpoints)
    : m_endpoints(std::move(endpoints)) {}

E2EHarness::~E2EHarness() {
    stop();
}

std::string E2EHarness::default_daemon_path() {
    // The E2E suite spawns BambuStudio with `--bridge-only` — there is
    // no standalone daemon binary because the proprietary
    // `bambu_networking` plugin fingerprints its host process and
    // refuses to operate unless it's been loaded by BambuStudio.
    //
    // Configure $BAMBU_BRIDGE_E2E_BAMBUSTUDIO to the slicer binary,
    // or rely on $PATH lookup of `BambuStudio`. The standalone bridge
    // build can't run the E2E suite — it needs a full BambuStudio
    // build available on the host.
    if (const char* envv = std::getenv("BAMBU_BRIDGE_E2E_BAMBUSTUDIO");
        envv && *envv) {
        return envv;
    }
    return "BambuStudio";
}

bool E2EHarness::start(std::chrono::seconds startup_timeout) {
    if (m_endpoints.empty()) {
        std::fprintf(stderr,
            "[e2e-harness] no endpoints configured; not starting daemon\n");
        return false;
    }
    if (m_pid > 0) {
        std::fprintf(stderr, "[e2e-harness] start() called twice\n");
        return false;
    }

    // Pipe for the child's stderr. Stdout goes to /dev/null — the daemon
    // doesn't write much there and the bindings line we care about goes
    // to stderr.
    int stderr_pipe[2] = {-1, -1};
    if (::pipe(stderr_pipe) < 0) {
        std::fprintf(stderr, "[e2e-harness] pipe() failed: %s\n",
            std::strerror(errno));
        return false;
    }

    const std::string bambustudio = default_daemon_path();

    // Build the argv. We deliberately avoid any flag that would require
    // root (SSDP/1900). Port bases are constants above; the per-device
    // table BambuStudio's --bridge-only mode emits will report
    // base+0, +1, ... in dev_id-add order. lan_iface_bind=127.0.0.1 is
    // fine because the real printers are on the LAN and the bridge
    // talks to them via plugin LAN sessions; the SERVER side is purely
    // loopback so the test client can connect without root or NIC
    // privileges.
    std::vector<std::string> args = {
        bambustudio,
        "--bridge-only",
        "--bind", "127.0.0.1",
        "--no-ssdp",
        "--mqtt-port-base", std::to_string(kMqttPortBase),
        "--ftps-port-base", std::to_string(kFtpsPortBase),
        "--rtsp-port-base", std::to_string(kRtspPortBase),
        "--inventory-poll-seconds", "5",
    };

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    const int pid = ::fork();
    if (pid < 0) {
        std::fprintf(stderr, "[e2e-harness] fork() failed: %s\n",
            std::strerror(errno));
        ::close(stderr_pipe[0]); ::close(stderr_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // Child.
        ::close(stderr_pipe[0]);
        ::dup2(stderr_pipe[1], STDERR_FILENO);
        ::close(stderr_pipe[1]);
        // Discard stdout to keep test output tidy.
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { ::dup2(devnull, STDOUT_FILENO); ::close(devnull); }
        ::execvp(bambustudio.c_str(), argv.data());
        // execvp returned — fatal.
        std::fprintf(stderr,
            "[e2e-harness] execvp(%s) failed: %s\n",
            bambustudio.c_str(), std::strerror(errno));
        std::_Exit(127);
    }

    // Parent.
    ::close(stderr_pipe[1]);
    m_pid       = pid;
    m_stderr_fd = stderr_pipe[0];

    // Set the reader fd to non-blocking so the reader loop's select()
    // can drain in chunks without ever blocking past the deadline.
    int flags = ::fcntl(m_stderr_fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(m_stderr_fd, F_SETFL, flags | O_NONBLOCK);

    m_reader = std::thread([this] { reader_loop(); });

    // Block until at least one binding has been seen for an endpoint we
    // were asked about, OR the timeout expires.
    const auto deadline = std::chrono::steady_clock::now() + startup_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            for (const auto& [dev_id, p] : m_bindings) {
                // Match by lan_ip against any endpoint we know about.
                for (auto& ep : m_endpoints) {
                    if (ep.ip == p.lan_ip) {
                        if (ep.dev_id.empty()) ep.dev_id = dev_id;
                        return true;
                    }
                }
            }
        }
        // Has the child died?
        int status = 0;
        const int wp = ::waitpid(m_pid, &status, WNOHANG);
        if (wp == m_pid) {
            std::fprintf(stderr,
                "[e2e-harness] daemon exited prematurely (status=%d)\n",
                status);
            // Reader will hit EOF and bail.
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::fprintf(stderr,
        "[e2e-harness] startup timeout — no device_bindings seen for any "
        "configured endpoint within %llds\n",
        static_cast<long long>(startup_timeout.count()));
    return false;
}

void E2EHarness::stop() {
    if (m_pid > 0) {
        ::kill(m_pid, SIGTERM);
        // Give the daemon up to 5s to flush + exit cleanly.
        for (int i = 0; i < 50; ++i) {
            int status = 0;
            const int wp = ::waitpid(m_pid, &status, WNOHANG);
            if (wp == m_pid) { m_pid = -1; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (m_pid > 0) {
            ::kill(m_pid, SIGKILL);
            int status = 0;
            ::waitpid(m_pid, &status, 0);
            m_pid = -1;
        }
    }
    m_stop_reader.store(true);
    if (m_reader.joinable()) m_reader.join();
    if (m_stderr_fd >= 0) { ::close(m_stderr_fd); m_stderr_fd = -1; }
}

std::optional<DevicePorts>
E2EHarness::ports_for(const std::string& model) const {
    std::lock_guard<std::mutex> lk(m_mu);
    // Find the endpoint with this model tag, look up its lan_ip, then
    // match against the bindings table (which is keyed by dev_id but
    // also stores lan_ip).
    std::string target_ip;
    std::string target_dev;
    for (const auto& ep : m_endpoints) {
        if (ep.model == model) {
            target_ip  = ep.ip;
            target_dev = ep.dev_id;
            break;
        }
    }
    if (target_ip.empty()) return std::nullopt;
    for (const auto& [dev_id, p] : m_bindings) {
        if (p.lan_ip == target_ip) return p;
    }
    return std::nullopt;
}

std::string E2EHarness::captured_stderr() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_stderr_log;
}

void E2EHarness::reader_loop() {
    std::string carry; // partial line across read boundaries
    while (!m_stop_reader.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (m_stderr_fd < 0) return;
        FD_SET(m_stderr_fd, &rfds);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 200 * 1000;
        const int rc = ::select(m_stderr_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (rc <= 0) continue;

        char  buf[4096];
        const ssize_t n = ::read(m_stderr_fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n == 0) return; // EOF
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return;
        }
        carry.append(buf, buf + n);
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_stderr_log.append(buf, buf + n);
        }
        // Pull complete lines out.
        for (;;) {
            const auto nl = carry.find('\n');
            if (nl == std::string::npos) break;
            std::string line = carry.substr(0, nl);
            carry.erase(0, nl + 1);
            parse_line(line);
        }
    }
}

bool E2EHarness::parse_line(const std::string& line) {
    // Format emitted by headless/BridgeApp.cpp:
    //   "[bridge-app] add dev_id=AAAA lan_ip=1.2.3.4 ports={mqtt=38883, ftps=39990, rtsp=38322}"
    // We're tolerant of trailing whitespace and missing fields.
    static const std::regex re(
        R"(\[bridge-app\] add dev_id=(\S+) lan_ip=(\S+) ports=\{mqtt=(\d+),\s*ftps=(\d+),\s*rtsp=(\d+)\})");
    std::smatch m;
    if (!std::regex_search(line, m, re)) return false;
    DevicePorts p;
    p.dev_id = m[1].str();
    p.lan_ip = m[2].str();
    p.mqtt   = static_cast<uint16_t>(std::atoi(m[3].str().c_str()));
    p.ftps   = static_cast<uint16_t>(std::atoi(m[4].str().c_str()));
    p.rtsp   = static_cast<uint16_t>(std::atoi(m[5].str().c_str()));
    p.bind_ip = "127.0.0.1";
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_bindings[p.dev_id] = p;
    }
    return true;
}

} // namespace e2e
} // namespace bridge
} // namespace Slic3r
