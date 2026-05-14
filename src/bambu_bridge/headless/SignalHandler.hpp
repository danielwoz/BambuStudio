// Bambu Bridge — RAII SIGINT/SIGTERM trampoline (phase 10).
//
// `SignalHandler` installs `std::function<void()>`-backed handlers for
// SIGINT and SIGTERM in its constructor and restores the previous
// dispositions in the destructor. The function is stored in a process-
// global `std::atomic<std::function<void()>*>` so a `volatile sig_atomic_t`
// flag pattern isn't needed; the only signal-safe operation we do is
// load the atomic pointer and (if non-null) post-set a `sig_atomic_t`
// stop flag the caller reads from its main loop.
//
// Production design:
//   - One instance per process (constructing a second one throws). The
//     signal disposition is global state and chaining `std::function`s
//     would be a footgun — the BridgeApp installs the handler from main.
//   - The callback runs on whatever thread the OS dispatches the signal
//     on (typically the main thread). It MUST be cheap and signal-safe.
//     The BridgeApp registers a callback that just flips
//     `std::atomic<bool>` flags + notifies a `std::condition_variable`
//     to wake the poll thread; the actual shutdown work happens on the
//     main thread after `run()`'s inner loop drops out.
//
// Linux/POSIX only. Windows port deferred — Bambu Bridge is currently
// Linux-only across the board (see SsdpResponder.hpp, MqttBroker.hpp,
// FtpsServer.hpp, RtspServer.hpp — all `#error`-out on `_WIN32`).

#ifndef SLIC3R_BAMBU_BRIDGE_HEADLESS_SIGNAL_HANDLER_HPP
#define SLIC3R_BAMBU_BRIDGE_HEADLESS_SIGNAL_HANDLER_HPP

#ifdef _WIN32
#  error "phase 10 SignalHandler is Linux-only for now"
#endif

#include <functional>

namespace Slic3r {
namespace bridge {
namespace headless {

class SignalHandler {
public:
    // Install handlers for SIGINT + SIGTERM that invoke `on_signal`. The
    // function MUST be signal-safe (no malloc, no locking primitives that
    // can deadlock if the signal arrives mid-critical-section, etc.). In
    // practice we hand in a tiny lambda that flips an `std::atomic<bool>`.
    //
    // Throws std::runtime_error if a SignalHandler is already installed —
    // signal disposition is process-global and chaining is not supported.
    explicit SignalHandler(std::function<void()> on_signal);

    ~SignalHandler();

    SignalHandler(const SignalHandler&)            = delete;
    SignalHandler& operator=(const SignalHandler&) = delete;
};

} // namespace headless
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_HEADLESS_SIGNAL_HANDLER_HPP
