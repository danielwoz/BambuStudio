// Bambu Bridge — shared printer FSM (harness mocks).
//
// Tiny state machine driving the per-printer mock's response to inbound
// cloud commands. Mirrors the slicer-visible gcode_state values and the
// command vocabulary listed in test_harness_plan.md §3:
//
//   * pause           — RUNNING -> PAUSE
//   * resume          — PAUSE   -> RUNNING
//   * stop            — anything -> FINISH
//   * push_all        — re-emit a full push_status of the current state
//   * start a job     — IDLE    -> PREPARE -> RUNNING (FSM-driven; the
//                       real command vocabulary is project_3mf/start_print
//                       on the wire, but v1 only models a trivial
//                       "begin printing" trigger)
//
// The FSM lives behind one mutex; the MockPlugin's worker thread is
// the only producer. Tests synchronously drive transitions via
// MockPlugin::publish_to_device — they call into the FSM directly when
// they want deterministic state changes.

#ifndef SLIC3R_BAMBU_BRIDGE_MOCKS_PRINTER_FSM_HPP
#define SLIC3R_BAMBU_BRIDGE_MOCKS_PRINTER_FSM_HPP

#include <chrono>
#include <mutex>
#include <string>

namespace Slic3r {
namespace bridge {
namespace mocks {

enum class GcodeState {
    Idle,
    Prepare,
    Running,
    Pause,
    Finish,
};

inline const char* gcode_state_str(GcodeState s) {
    switch (s) {
    case GcodeState::Idle:    return "IDLE";
    case GcodeState::Prepare: return "PREPARE";
    case GcodeState::Running: return "RUNNING";
    case GcodeState::Pause:   return "PAUSE";
    case GcodeState::Finish:  return "FINISH";
    }
    return "UNKNOWN";
}

class PrinterFsm {
public:
    // Drive the printer toward `next`. Idempotent; transitions only
    // happen when the move is valid (IDLE->PREPARE->RUNNING->PAUSE/FINISH
    // chain). Returns the new state.
    GcodeState set_state(GcodeState next) {
        std::lock_guard<std::mutex> lk(m_mu);
        // Tiny validity matrix — anything goes from Idle/Prepare;
        // Pause<->Running is allowed; Finish is terminal except an
        // explicit re-init from Idle.
        if (m_state == GcodeState::Finish && next != GcodeState::Idle)
            return m_state;
        m_state = next;
        return m_state;
    }

    GcodeState state() const {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_state;
    }

    // Apply one of the ~8 commands the FSM understands. Returns true if
    // the command was recognised (the caller may still want to emit a
    // push_status). Unknown commands are silently dropped.
    bool apply_command(const std::string& cmd) {
        std::lock_guard<std::mutex> lk(m_mu);
        if (cmd == "pause") {
            if (m_state == GcodeState::Running) m_state = GcodeState::Pause;
            return true;
        }
        if (cmd == "resume") {
            if (m_state == GcodeState::Pause) m_state = GcodeState::Running;
            return true;
        }
        if (cmd == "stop") {
            m_state = GcodeState::Finish;
            return true;
        }
        if (cmd == "push_all") {
            return true; // caller emits push_status; FSM untouched
        }
        if (cmd == "start") {
            if (m_state == GcodeState::Idle) m_state = GcodeState::Prepare;
            else if (m_state == GcodeState::Prepare) m_state = GcodeState::Running;
            return true;
        }
        if (cmd == "unload_filament"
            || cmd == "ams_filament_setting"
            || cmd == "gcode_line"
            || cmd == "request_camera_url") {
            return true;
        }
        return false;
    }

    // Layer / time accessors. Bumped by tick() and emitted in each
    // push_status. The numbers don't have to be physically accurate —
    // tests assert on monotonicity and on the IDLE->FINISH chain.
    void tick() {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_state == GcodeState::Running) {
            if (m_layer < m_total_layer) ++m_layer;
            if (m_mc_remaining_time > 0) --m_mc_remaining_time;
            if (m_mc_percent < 100) ++m_mc_percent;
        }
    }

    int  mc_percent()         const { std::lock_guard<std::mutex> lk(m_mu); return m_mc_percent; }
    int  mc_remaining_time()  const { std::lock_guard<std::mutex> lk(m_mu); return m_mc_remaining_time; }
    int  layer_num()          const { std::lock_guard<std::mutex> lk(m_mu); return m_layer; }
    int  total_layer_num()    const { std::lock_guard<std::mutex> lk(m_mu); return m_total_layer; }

private:
    mutable std::mutex m_mu;
    GcodeState m_state              = GcodeState::Idle;
    int        m_layer              = 0;
    int        m_total_layer        = 100;
    int        m_mc_percent         = 0;
    int        m_mc_remaining_time  = 60;  // minutes
};

} // namespace mocks
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_MOCKS_PRINTER_FSM_HPP
