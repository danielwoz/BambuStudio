// See CameraFrameFanout.hpp for the rationale.

#include "CameraFrameFanout.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unordered_map>

namespace Slic3r {
namespace bridge {
namespace router {

// ----- Cursor ---------------------------------------------------------------

CameraFrameFanout::Cursor::Cursor(std::shared_ptr<CameraFrameFanout> fanout,
                                  std::size_t                          id)
    : m_fanout(std::move(fanout)), m_id(id) {}

CameraFrameFanout::Cursor::~Cursor() {
    if (m_fanout) m_fanout->unregister_cursor(m_id);
}

std::optional<server::VideoFrame>
CameraFrameFanout::Cursor::next_frame(int timeout_ms) {
    if (!m_fanout) return std::nullopt;
    return m_fanout->wait_and_pop(m_id, timeout_ms);
}

// ----- CameraFrameFanout ----------------------------------------------------

std::shared_ptr<CameraFrameFanout>
CameraFrameFanout::create(std::shared_ptr<server::ICameraSource> upstream) {
    return std::make_shared<CameraFrameFanout>(std::move(upstream), Config{});
}

std::shared_ptr<CameraFrameFanout>
CameraFrameFanout::create(std::shared_ptr<server::ICameraSource> upstream,
                          Config                                  cfg) {
    return std::make_shared<CameraFrameFanout>(std::move(upstream), cfg);
}

CameraFrameFanout::CameraFrameFanout(
        std::shared_ptr<server::ICameraSource> upstream,
        Config                                  cfg)
    : m_upstream(std::move(upstream)),
      m_cfg(cfg),
      m_ring(cfg.ring_capacity) {}

CameraFrameFanout::~CameraFrameFanout() {
    close();
}

bool CameraFrameFanout::open() {
    if (m_running.load()) return true;
    if (!m_upstream) return false;
    if (!m_upstream->is_open() && !m_upstream->open()) {
        std::fprintf(stderr,
            "[camera-fanout] upstream->open() FAILED — fanout will stay closed\n");
        std::fflush(stderr);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_mu);
        for (auto& s : m_ring) { s.valid = false; }
        m_writer_pos = 0;
        m_eos.store(false);
    }
    m_running.store(true);
    m_reader = std::thread(&CameraFrameFanout::reader_loop, this);
    return true;
}

void CameraFrameFanout::close() {
    if (!m_running.exchange(false)) {
        // Already closed. Still join any zombie thread (from a torn-down
        // ctor or a previous open() that we exchanged from underneath).
        if (m_reader.joinable()) m_reader.join();
        return;
    }
    // Wake the reader if it's mid-wait, wake all cursors so wait_and_pop
    // returns nullopt promptly.
    m_cv.notify_all();
    if (m_reader.joinable()) m_reader.join();
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& s : m_ring) { s.valid = false; }
    m_writer_pos = 0;
}

bool CameraFrameFanout::is_open() const {
    return m_running.load() && !m_eos.load();
}

server::ICameraSource::StreamInfo CameraFrameFanout::info() const {
    if (!m_upstream) return {};
    return m_upstream->info();
}

std::shared_ptr<CameraFrameFanout::Cursor>
CameraFrameFanout::create_cursor() {
    std::size_t id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        id = register_cursor_locked();
    }
    return std::make_shared<Cursor>(shared_from_this(), id);
}

std::size_t CameraFrameFanout::frames_produced() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_writer_pos;
}

std::size_t CameraFrameFanout::active_cursors() const {
    std::lock_guard<std::mutex> lk(m_mu);
    return m_cursors.size();
}

// ----- reader thread --------------------------------------------------------

void CameraFrameFanout::reader_loop() {
    int idle_streak = 0;
    auto last_stat_log = std::chrono::steady_clock::now();
    std::size_t last_produced_at_log = 0;
    while (m_running.load()) {
        if (!m_upstream->is_open()) {
            // Source dropped (EOS or error). Mark and exit — caller can
            // re-open the fanout to start over with a fresh source.
            m_eos.store(true);
            m_cv.notify_all();
            break;
        }
        auto frame = m_upstream->next_frame(m_cfg.upstream_poll_timeout_ms);
        if (frame) {
            idle_streak = 0;
            std::size_t produced_now = 0;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                auto& slot = m_ring[m_writer_pos % m_ring.size()];
                slot.frame = std::move(*frame);
                slot.valid = true;
                m_writer_pos++;
                produced_now = m_writer_pos;
            }
            m_cv.notify_all();
            // Periodic stats so we can confirm flow rate from the log.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_stat_log >= std::chrono::seconds(5)) {
                const auto dt_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_stat_log).count();
                const std::size_t delta =
                    produced_now - last_produced_at_log;
                const double fps =
                    dt_ms > 0 ? (delta * 1000.0 / dt_ms) : 0.0;
                std::fprintf(stderr,
                    "[camera-fanout] produced=%zu fps=%.1f cursors=%zu\n",
                    produced_now, fps, active_cursors());
                std::fflush(stderr);
                last_stat_log         = now;
                last_produced_at_log  = produced_now;
            }
        } else {
            // Either timeout (upstream blocking) or WOULD_BLOCK (upstream
            // non-blocking). Brief backoff when the streak gets long so we
            // don't burn CPU on a quiescent stream. Reader still wakes
            // frequently enough to honour `close()` promptly.
            if (++idle_streak >= m_cfg.idle_backoff_threshold) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(m_cfg.idle_backoff_us));
            } else {
                std::this_thread::yield();
            }
        }
    }
}

// ----- cursor bookkeeping ---------------------------------------------------

std::size_t CameraFrameFanout::register_cursor_locked() {
    const std::size_t id = m_next_cursor_id++;
    CursorState st;
    // Start at the most recent KEYFRAME visible in the ring (or, if no
    // keyframe is buffered, at the current writer position). H.264 / MJPEG
    // decoders can't render P-frames until they've seen a SPS/PPS/IDR;
    // landing a fresh cursor on the latest IDR (rather than the bleeding
    // edge of the ring) means the slicer sees a usable frame immediately
    // instead of waiting up to one GOP (~1 s @ 30 fps) for the next IDR.
    const std::size_t ring_size = m_ring.size();
    st.position = m_writer_pos;
    if (m_writer_pos > 0) {
        const std::size_t window_start =
            m_writer_pos > ring_size ? m_writer_pos - ring_size : 0;
        for (std::size_t p = m_writer_pos; p > window_start; --p) {
            const auto& s = m_ring[(p - 1) % ring_size];
            if (s.valid && s.frame.is_keyframe) {
                st.position = p - 1;
                break;
            }
        }
    }
    m_cursors.emplace(id, st);
    return id;
}

void CameraFrameFanout::unregister_cursor(std::size_t id) {
    std::lock_guard<std::mutex> lk(m_mu);
    m_cursors.erase(id);
}

std::optional<server::VideoFrame>
CameraFrameFanout::wait_and_pop(std::size_t id, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m_mu);
    auto it = m_cursors.find(id);
    if (it == m_cursors.end()) return std::nullopt;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    // Wait for the cursor's position to be visible at the writer.
    while (it->second.position >= m_writer_pos) {
        if (!m_running.load() || m_eos.load()) return std::nullopt;
        if (m_cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            return std::nullopt;
        }
        it = m_cursors.find(id); // re-find after wait — lk was released
        if (it == m_cursors.end()) return std::nullopt;
    }

    // Slow-consumer policy: if we've fallen too far behind, jump forward
    // to the most recent keyframe within the ring window (or the newest
    // frame if no keyframe is visible). This keeps every consumer's
    // playback fresh without ever blocking the upstream reader.
    const std::size_t ring_size = m_ring.size();
    if (m_writer_pos > ring_size
        && it->second.position + m_cfg.drop_threshold < m_writer_pos) {
        std::size_t target = m_writer_pos - 1;
        const std::size_t window_start =
            m_writer_pos > ring_size ? m_writer_pos - ring_size : 0;
        for (std::size_t p = m_writer_pos; p > window_start; --p) {
            const auto& s = m_ring[(p - 1) % ring_size];
            if (s.valid && s.frame.is_keyframe) {
                target = p - 1;
                break;
            }
        }
        it->second.position = target;
    }

    // Even after the wait/fast-forward, the position must still be in the
    // ring window — fix it up if the writer has lapped us during the wait.
    if (m_writer_pos > ring_size
        && it->second.position + ring_size <= m_writer_pos) {
        it->second.position = m_writer_pos - ring_size;
    }

    auto& slot = m_ring[it->second.position % ring_size];
    if (!slot.valid) {
        // Lost race vs. writer (slot got overwritten before we copied).
        // Caller can retry; we treat as a transient miss.
        return std::nullopt;
    }
    // Copy out (we don't move — other cursors may still read this slot).
    server::VideoFrame out = slot.frame;
    it->second.position++;
    return out;
}

} // namespace router
} // namespace bridge
} // namespace Slic3r
