#include "resource_monitor.h"

#include "dream/core/memory.h"
#include "dream/core/profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>

namespace dream::runtime {

#if !defined(DREAM_DISABLE_PROFILING)
namespace {

constexpr double k_megabyte = 1024.0 * 1024.0;

double megabytes(std::uint64_t bytes) {
    return static_cast<double>(bytes) / k_megabyte;
}

double seconds(core::TimePoint::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

// Value names must outlive the profiler, so they are literals, one per tag.
constexpr std::array<const char*, static_cast<std::size_t>(core::MemoryTag::Count)> k_tag_names{
    "Tracked General (MB)", "Tracked Graphics (MB)", "Tracked Physics (MB)", "Tracked Audio (MB)",
    "Tracked Ui (MB)",      "Tracked Assets (MB)",   "Tracked Game (MB)",
};

} // namespace
#endif

ResourceMonitor::ResourceMonitor(std::uint32_t samples_per_second)
    : m_interval(std::chrono::duration_cast<core::TimePoint::duration>(
          std::chrono::duration<double>(1.0 / std::max<std::uint32_t>(samples_per_second, 1)))) {}

void ResourceMonitor::end_frame(const FrameTiming& timing, rhi::Device* device) {
#if defined(DREAM_DISABLE_PROFILING)
    static_cast<void>(timing);
    static_cast<void>(device);
#else
    if (core::profile_sink() == nullptr) {
        // Differences must not span a time when nobody was recording.
        m_last_heap.reset();
        m_last_cpu_time.reset();
        m_next_sample.reset();
        return;
    }

    DREAM_PROFILE_VALUE("Frame time (ms)", timing.frame_seconds * 1000.0);
    DREAM_PROFILE_VALUE("Frame CPU work (ms)",
                        std::max(timing.frame_seconds - timing.wait_seconds, 0.0) * 1000.0);
    DREAM_PROFILE_VALUE("Fixed steps per frame", timing.fixed_steps);

    if (core::is_heap_counted()) {
        const core::HeapStats heap = core::heap_stats();
        if (m_last_heap.has_value()) {
            DREAM_PROFILE_VALUE("Heap allocations per frame",
                                heap.allocations - m_last_heap->allocations);
            DREAM_PROFILE_VALUE(
                "Heap allocated per frame (KB)",
                static_cast<double>(heap.allocated_bytes - m_last_heap->allocated_bytes) / 1024.0);
        }
        if (heap.live_bytes > 0) {
            DREAM_PROFILE_VALUE("Heap in use (MB)", megabytes(heap.live_bytes));
        }
        m_last_heap = heap;
    }

    const core::TimePoint time = core::now();
    if (!m_next_sample.has_value() || time >= *m_next_sample) {
        sample(time, device);
        // Keeps the cadence, without a burst to catch up after a slow frame.
        m_next_sample = m_next_sample.has_value() ? std::max(*m_next_sample + m_interval, time)
                                                  : time + m_interval;
    }
#endif
}

void ResourceMonitor::sample(core::TimePoint time, rhi::Device* device) {
#if defined(DREAM_DISABLE_PROFILING)
    static_cast<void>(time);
    static_cast<void>(device);
#else
    // Asking the system and the driver takes a little time, which this shows.
    DREAM_PROFILE_ZONE("Profiler sampling");

    if (const std::optional<core::ProcessMemory> memory = core::process_memory()) {
        DREAM_PROFILE_VALUE("Memory resident (MB)", megabytes(memory->resident_bytes));
        DREAM_PROFILE_VALUE("Memory private (MB)", megabytes(memory->private_bytes));
    }

    if (const std::optional<double> cpu_seconds = core::process_cpu_seconds()) {
        if (m_last_cpu_time.has_value() && time > *m_last_cpu_time) {
            // 100% is one core kept busy, so a program using several can go above it.
            DREAM_PROFILE_VALUE("CPU usage (%)", (*cpu_seconds - m_last_cpu_seconds) /
                                                     seconds(time - *m_last_cpu_time) * 100.0);
        }
        m_last_cpu_time = time;
        m_last_cpu_seconds = *cpu_seconds;
    }

    if (device != nullptr) {
        if (const std::optional<rhi::MemoryUsage> video = device->memory_usage()) {
            DREAM_PROFILE_VALUE("GPU memory (MB)", megabytes(video->used_bytes));
            if (video->budget_bytes > 0) {
                DREAM_PROFILE_VALUE("GPU memory budget (MB)", megabytes(video->budget_bytes));
            }
        }
    }

    // Only the tags something has used, so the graphs don't fill with zeros.
    for (std::size_t tag = 0; tag < k_tag_names.size(); ++tag) {
        const core::MemoryStats stats = core::memory_stats(static_cast<core::MemoryTag>(tag));
        if (stats.total_allocations > 0) {
            core::profile_value(k_tag_names[tag], megabytes(stats.current_bytes));
        }
    }
#endif
}

} // namespace dream::runtime
