// Bambu Bridge — SignalHandler implementation.
//
// POSIX: self-pipe trick. The signal handler only writes one byte to a
// pipe (async-signal-safe); a worker thread blocks on read() to dispatch
// the user callback in normal context. The previous design called the
// callback directly from the signal handler, which is undefined behaviour
// for any callback that takes a std::mutex lock or notifies a
// std::condition_variable (both of which BridgeApp::shutdown does).
//
// Windows: SetConsoleCtrlHandler. The console control handler already runs
// on a dedicated OS thread (not an async-signal context), so it is safe to
// signal a condition_variable directly; a worker thread dispatches the
// callback to keep the handler return-path fast.

#include "SignalHandler.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <condition_variable>
#  include <mutex>
#else
#  include <cerrno>
#  include <csignal>
#  include <functional>
#  include <fcntl.h>
#  include <signal.h>
#  include <unistd.h>
#endif

namespace Slic3r {
namespace bridge {
namespace headless {

namespace {

#ifdef _WIN32

std::function<void()> g_cb;
std::thread           g_worker;
std::mutex            g_mtx;
std::condition_variable g_cv;
bool                  g_signalled = false;
std::atomic<bool>     g_running{false};
bool                  g_installed = false;

BOOL WINAPI on_ctrl(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: {
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_signalled = true;
        }
        g_cv.notify_all();
        return TRUE;  // handled — suppress default termination
    }
    default:
        return FALSE;
    }
}

void worker_loop() {
    while (g_running.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(g_mtx);
        g_cv.wait(lk, [] { return g_signalled || !g_running.load(std::memory_order_acquire); });
        if (!g_running.load(std::memory_order_acquire)) return;
        g_signalled = false;
        auto cb = g_cb;
        lk.unlock();
        if (cb) {
            try { cb(); } catch (const std::exception&) {}
        }
    }
}

#else  // POSIX

// Process-global state. Set by ctor, cleared by dtor. The signal handler
// reads g_write_fd atomically and writes one byte; that's the only
// signal-context operation it does.
std::atomic<int>  g_write_fd{-1};
int               g_read_fd  = -1;
std::thread       g_worker;
std::atomic<bool> g_running{false};

struct sigaction g_prior_sigint{};
struct sigaction g_prior_sigterm{};
bool             g_installed = false;

extern "C" void on_signal(int /*signo*/) {
    const int fd = g_write_fd.load(std::memory_order_acquire);
    if (fd < 0) return;
    const char b = 1;
    // write() is async-signal-safe per POSIX.1-2017 §2.4.3. EINTR/EAGAIN
    // are both acceptable here — we'd rather drop a redundant notification
    // than risk anything heavier in signal context.
    ssize_t r = ::write(fd, &b, 1);
    (void) r;
}

void worker_loop(std::function<void()> cb) {
    char buf;
    while (g_running.load(std::memory_order_acquire)) {
        ssize_t n = ::read(g_read_fd, &buf, 1);
        if (n <= 0) {
            if (n == 0) return;
            if (errno == EINTR) continue;
            return;
        }
        if (cb) {
            try { cb(); }
            catch (const std::exception& ex) {
            }
        }
    }
}

#endif

} // namespace

#ifdef _WIN32

SignalHandler::SignalHandler(std::function<void()> cb) {
    if (g_installed) {
        throw std::runtime_error(
            "SignalHandler: already installed (process-global)");
    }
    g_cb = std::move(cb);
    g_signalled = false;
    g_running.store(true, std::memory_order_release);
    g_worker = std::thread(worker_loop);

    if (!::SetConsoleCtrlHandler(on_ctrl, TRUE)) {
        g_running.store(false, std::memory_order_release);
        g_cv.notify_all();
        if (g_worker.joinable()) g_worker.join();
        g_cb = nullptr;
        throw std::runtime_error("SignalHandler: SetConsoleCtrlHandler() failed");
    }
    g_installed = true;
}

SignalHandler::~SignalHandler() {
    if (!g_installed) return;
    ::SetConsoleCtrlHandler(on_ctrl, FALSE);
    g_running.store(false, std::memory_order_release);
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    g_cb = nullptr;
    g_installed = false;
}

#else  // POSIX

SignalHandler::SignalHandler(std::function<void()> cb) {
    if (g_installed) {
        throw std::runtime_error(
            "SignalHandler: already installed (process-global)");
    }

    int fds[2];
    if (::pipe(fds) != 0) {
        throw std::runtime_error("SignalHandler: pipe() failed");
    }
    int flags = ::fcntl(fds[1], F_GETFL, 0);
    ::fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);

    g_read_fd = fds[0];
    g_write_fd.store(fds[1], std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    g_worker = std::thread(worker_loop, std::move(cb));

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // not SA_RESTART — recv()/accept() in server
                    // threads should return EINTR so they can observe
                    // stop flags promptly.
    if (::sigaction(SIGINT,  &sa, &g_prior_sigint)  != 0 ||
        ::sigaction(SIGTERM, &sa, &g_prior_sigterm) != 0) {
        g_running.store(false);
        ::close(fds[1]);
        ::close(fds[0]);
        g_write_fd.store(-1);
        g_read_fd = -1;
        if (g_worker.joinable()) g_worker.join();
        throw std::runtime_error("SignalHandler: sigaction() failed");
    }
    g_installed = true;
}

SignalHandler::~SignalHandler() {
    if (!g_installed) return;
    ::sigaction(SIGINT,  &g_prior_sigint,  nullptr);
    ::sigaction(SIGTERM, &g_prior_sigterm, nullptr);

    g_running.store(false, std::memory_order_release);
    const int wfd = g_write_fd.exchange(-1, std::memory_order_acq_rel);
    if (wfd >= 0) ::close(wfd);
    if (g_worker.joinable()) g_worker.join();
    if (g_read_fd >= 0) { ::close(g_read_fd); g_read_fd = -1; }
    g_installed = false;
}

#endif

} // namespace headless
} // namespace bridge
} // namespace Slic3r
