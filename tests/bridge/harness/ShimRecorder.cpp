// Bambu Bridge — ShimRecorder implementation (harness).

#include "ShimRecorder.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#if defined(__linux__)
#  include <pthread.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

namespace Slic3r {
namespace bridge {
namespace harness {

namespace {

// Best-effort thread name. On Linux we use pthread_getname_np if it has
// been set; fall back to "tid-<gettid>" otherwise. On other platforms
// just stream the std::thread::id.
std::string current_thread_name() {
#if defined(__linux__)
    char buf[32] = {0};
    if (pthread_getname_np(pthread_self(), buf, sizeof(buf)) == 0 && buf[0] != '\0')
        return std::string(buf);
    long tid = static_cast<long>(syscall(SYS_gettid));
    char tidbuf[32];
    std::snprintf(tidbuf, sizeof(tidbuf), "tid-%ld", tid);
    return std::string(tidbuf);
#else
    std::ostringstream oss;
    oss << "tid-" << std::this_thread::get_id();
    return oss.str();
#endif
}

} // namespace

ShimRecorder& ShimRecorder::instance() {
    static ShimRecorder s_inst;
    return s_inst;
}

ShimRecorder::ShimRecorder() = default;

ShimRecorder::~ShimRecorder() {
    // Best-effort drain. The mutex-held flush also closes the FILE*.
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_fp) {
        std::fflush(m_fp);
        std::fclose(m_fp);
        m_fp = nullptr;
    }
    m_active.store(false, std::memory_order_release);
}

bool ShimRecorder::enable_from_env() {
    const char* on = std::getenv("BAMBU_BRIDGE_SHIM");
    if (!on || on[0] == '\0')
        return false;

    const char* path_env = std::getenv("BAMBU_BRIDGE_SHIM_TRACE");
    std::string path;
    if (path_env && path_env[0] != '\0') {
        path = path_env;
    } else {
#if defined(__linux__)
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "/tmp/bambu_bridge_shim_%d.jsonl",
                      static_cast<int>(::getpid()));
        path = buf;
#else
        path = "bambu_bridge_shim.jsonl";
#endif
    }
    return enable(path);
}

bool ShimRecorder::enable(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_mu);
    close_locked();

    std::FILE* fp = std::fopen(path.c_str(), "ab");
    if (!fp) {
        std::fprintf(stderr,
                     "ShimRecorder: failed to open %s: %s\n",
                     path.c_str(), std::strerror(errno));
        return false;
    }
    // Line-buffered so each record() flushes a complete line. Trace
    // analysis tools can tail -f the file.
    std::setvbuf(fp, nullptr, _IOLBF, 0);

    m_fp           = fp;
    m_path         = path;
    m_seq          = 0;
    m_prev_ts_ns   = 0;
    m_bytes_written.store(0);
    m_lines_written.store(0);
    m_cb_id_next.store(1);
    m_callbacks.clear();

    m_active.store(true, std::memory_order_release);
    return true;
}

void ShimRecorder::disable() {
    std::lock_guard<std::mutex> lk(m_mu);
    close_locked();
}

void ShimRecorder::flush() {
    std::lock_guard<std::mutex> lk(m_mu);
    if (m_fp)
        std::fflush(m_fp);
}

void ShimRecorder::close_locked() {
    if (m_fp) {
        std::fflush(m_fp);
        std::fclose(m_fp);
        m_fp = nullptr;
    }
    m_path.clear();
    m_active.store(false, std::memory_order_release);
}

std::int64_t ShimRecorder::now_ns_locked() const {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
               system_clock::now().time_since_epoch()).count();
}

std::string ShimRecorder::active_path() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_path;
}

std::int64_t ShimRecorder::callback_id_for(const void* key) {
    if (!key)
        return -1;
    std::lock_guard<std::mutex> lk(m_mu);
    for (const auto& e : m_callbacks)
        if (e.key == key) return e.id;
    std::int64_t id = m_cb_id_next.fetch_add(1);
    m_callbacks.push_back({key, id});
    return id;
}

void ShimRecorder::record(TraceLine line) {
    if (!m_active.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_fp)
        return;

    const std::int64_t ts = now_ns_locked();
    line.seq      = ++m_seq;
    line.ts_ns    = ts;
    line.delta_ms = (m_prev_ts_ns == 0)
                       ? 0
                       : (ts - m_prev_ts_ns) / 1'000'000LL;
    m_prev_ts_ns  = ts;
    if (line.thread.empty())
        line.thread = current_thread_name();

    const std::string blob = line.to_jsonl();
    const std::size_t n    = std::fwrite(blob.data(), 1, blob.size(), m_fp);
    m_bytes_written.fetch_add(static_cast<std::int64_t>(n));
    m_lines_written.fetch_add(1);
}

void ShimRecorder::record(TraceLib                lib,
                          const char*             fn,
                          nlohmann::json          args,
                          nlohmann::json          ret,
                          nlohmann::json          ret_out_params,
                          std::int64_t            duration_us,
                          std::int64_t            cb_id) {
    if (!m_active.load(std::memory_order_acquire))
        return;

    TraceLine line;
    line.lib            = lib;
    line.fn             = fn ? fn : "";
    line.args           = std::move(args);
    line.ret            = std::move(ret);
    line.ret_out_params = std::move(ret_out_params);
    line.duration_us    = duration_us;
    line.cb_id          = cb_id;
    record(std::move(line));
}

} // namespace harness
} // namespace bridge
} // namespace Slic3r
