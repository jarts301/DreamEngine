#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace dream::core {

using TimePoint = std::chrono::steady_clock::time_point;

// Monotonic time, unaffected by changes to the system clock.
[[nodiscard]] inline TimePoint now() noexcept {
    return std::chrono::steady_clock::now();
}

class Stopwatch {
public:
    void reset() noexcept {
        m_start = now();
    }

    [[nodiscard]] double elapsed_seconds() const noexcept {
        return std::chrono::duration<double>(now() - m_start).count();
    }

private:
    TimePoint m_start = now();
};

struct FrameTime {
    double delta_seconds = 0.0;
    double elapsed_seconds = 0.0;
    std::uint64_t frame_index = 0;
};

// Measures time between frames. The delta is capped, so a long pause such as a debugger break or
// the app being in the background does not produce one huge simulation step.
class Clock {
public:
    explicit Clock(double max_delta_seconds = 0.25) noexcept : m_max_delta(max_delta_seconds) {}

    FrameTime tick() noexcept {
        return tick_at(now());
    }

    // Same as tick(), with the time supplied by the caller, for tests and replays.
    FrameTime tick_at(TimePoint time) noexcept {
        if (m_previous.has_value()) {
            const double real_delta = std::chrono::duration<double>(time - *m_previous).count();
            m_frame.delta_seconds = std::clamp(real_delta, 0.0, m_max_delta);
            m_frame.elapsed_seconds += m_frame.delta_seconds;
            ++m_frame.frame_index;
        }
        m_previous = time;
        return m_frame;
    }

private:
    double m_max_delta;
    std::optional<TimePoint> m_previous;
    FrameTime m_frame;
};

// Turns variable frame times into a whole number of equal steps, for simulations whose results
// shouldn't depend on the frame rate. Time left over carries into the next frame.
class FixedTimestep {
public:
    // At most `max_steps` steps are run per frame. Time beyond that is dropped, so a device too slow
    // to keep up runs the simulation slower instead of falling further behind each frame.
    explicit FixedTimestep(double step_seconds = 1.0 / 60.0, int max_steps = 8) noexcept
        : m_step(step_seconds), m_max_steps(max_steps) {}

    // Adds a frame's time and returns how many steps to run now.
    int advance(double delta_seconds) noexcept {
        m_accumulated += std::max(delta_seconds, 0.0);
        int steps = 0;
        while (m_accumulated >= m_step && steps < m_max_steps) {
            m_accumulated -= m_step;
            ++steps;
        }
        if (steps == m_max_steps) {
            m_accumulated = std::min(m_accumulated, m_step);
        }
        return steps;
    }

    [[nodiscard]] double step_seconds() const noexcept {
        return m_step;
    }

    // How far the current frame is from the last step towards the next, from 0 to 1. Drawing objects
    // this far between their last two simulated states keeps motion smooth at any frame rate.
    [[nodiscard]] double alpha() const noexcept {
        return std::clamp(m_accumulated / m_step, 0.0, 1.0);
    }

private:
    double m_step;
    int m_max_steps;
    double m_accumulated = 0.0;
};

} // namespace dream::core
