#pragma once

#include "dream/core/clock.h"
#include "dream/core/process_stats.h"
#include "dream/rhi/rhi.h"

#include <cstdint>
#include <optional>

namespace dream::runtime {

// How one frame of the main loop went.
struct FrameTiming {
    // From the end of the previous frame to the end of this one. After a pause, from when updates
    // resumed.
    double frame_seconds = 0.0;
    // Spent waiting rather than working: in the device's begin_frame and end_frame, where the GPU
    // and the display set the pace, and in the frame limiter's sleep.
    double wait_seconds = 0.0;
    int fixed_steps = 0;
};

// Records the resource values described in docs/adr/0016-profiling-resources.md: frame timing and
// the heap every frame, and process, video and tracked memory `samples_per_second` times a second.
// While no profiler is installed it only checks that, and reads nothing.
class ResourceMonitor {
public:
    explicit ResourceMonitor(std::uint32_t samples_per_second);

    // Call once per frame, before the frame is marked, so a profiler recording a range of frames
    // gets each frame's values with it. `device` may be null.
    void end_frame(const FrameTiming& timing, rhi::Device* device);

private:
    void sample(core::TimePoint time, rhi::Device* device);

    core::TimePoint::duration m_interval;
    std::optional<core::TimePoint> m_next_sample;
    std::optional<core::TimePoint> m_last_cpu_time;
    double m_last_cpu_seconds = 0.0;
    std::optional<core::HeapStats> m_last_heap;
};

} // namespace dream::runtime
