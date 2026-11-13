#include "memory_guard.h"

#include "dream/core/process_stats.h"
#include "dream/core/profile.h"

#include <algorithm>
#include <chrono>

namespace dream::runtime {

MemoryReading read_memory() {
    MemoryReading reading;
    if (const std::optional<core::ProcessMemory> process = core::process_memory()) {
        reading.process_bytes = process->private_bytes;
    }
    if (const std::optional<core::SystemMemory> system = core::system_memory()) {
        reading.available_physical_bytes = system->available_physical_bytes;
        reading.available_commit_bytes = system->available_commit_bytes;
    }
    return reading;
}

MemoryGuard::MemoryGuard(const MemorySettings& settings)
    : m_settings(settings), m_budget(settings),
      m_interval(std::chrono::duration_cast<core::TimePoint::duration>(
          std::chrono::duration<double>(1.0 / std::max<std::uint32_t>(settings.check_rate, 1)))) {
    if (m_settings.enabled) {
        m_reserve.emplace(static_cast<std::size_t>(m_settings.reserve_bytes));
    }
}

std::optional<MemoryPressureChanged> MemoryGuard::check(core::TimePoint time) {
    if (!m_settings.enabled) {
        return std::nullopt;
    }

    // A failure is dealt with at once rather than at the next scheduled check.
    const std::uint64_t failures = m_reserve->failed_allocations();
    const bool new_failure = failures > m_failures_seen;
    if (!new_failure && m_next_check.has_value() && time < *m_next_check) {
        return std::nullopt;
    }
    m_next_check = time + m_interval;
    if (new_failure) {
        m_failures_seen = failures;
        m_recovering = true;
    }

    DREAM_PROFILE_ZONE("Memory check");
    MemoryReading reading = read_memory();

    // A failure is always reported as critical at least once. At a later check, when the other
    // readings are normal with their margin, the reserve is taken back, which also proves the
    // memory is there again, and the pressure can fall.
    if (m_recovering && !new_failure) {
        MemoryBudget without_failure = m_budget;
        if (without_failure.evaluate(reading) == MemoryPressure::Normal && m_reserve->refill()) {
            m_recovering = false;
            m_failures_seen = m_reserve->failed_allocations();
        }
    }
    reading.allocation_failed = m_recovering;

    const MemoryPressure previous = m_budget.pressure();
    const MemoryPressure pressure = m_budget.evaluate(reading);

    DREAM_PROFILE_VALUE("Memory pressure", static_cast<int>(pressure));
    if (reading.available_physical_bytes.has_value()) {
        DREAM_PROFILE_VALUE("System RAM available (MB)",
                            static_cast<double>(*reading.available_physical_bytes) / 1048576.0);
    }
    if (reading.available_commit_bytes.has_value()) {
        DREAM_PROFILE_VALUE("System commit available (MB)",
                            static_cast<double>(*reading.available_commit_bytes) / 1048576.0);
    }
    if (m_settings.budget_bytes > 0 && reading.process_bytes.has_value()) {
        DREAM_PROFILE_VALUE("Memory budget used (%)",
                            static_cast<double>(*reading.process_bytes) /
                                static_cast<double>(m_settings.budget_bytes) * 100.0);
    }

    if (pressure == previous) {
        return std::nullopt;
    }
    return MemoryPressureChanged{.pressure = pressure, .previous = previous, .reading = reading};
}

} // namespace dream::runtime
