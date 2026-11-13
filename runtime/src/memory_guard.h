#pragma once

#include "dream/core/clock.h"
#include "dream/core/memory_reserve.h"
#include "dream/runtime/memory_budget.h"

#include <cstdint>
#include <optional>

namespace dream::runtime {

// Watches memory for the App: it holds the emergency reserve, reads the process and the system
// `check_rate` times a second, or at once after an allocation failed, and decides the pressure
// with a MemoryBudget. See docs/adr/0018-memory-budgets.md.
class MemoryGuard {
public:
    explicit MemoryGuard(const MemorySettings& settings);

    // Returns the change when this check changed the pressure.
    std::optional<MemoryPressureChanged> check(core::TimePoint time);

    [[nodiscard]] MemoryPressure pressure() const noexcept {
        return m_budget.pressure();
    }

private:
    MemorySettings m_settings;
    MemoryBudget m_budget;
    std::optional<core::MemoryReserve> m_reserve;
    core::TimePoint::duration m_interval;
    std::optional<core::TimePoint> m_next_check;
    std::uint64_t m_failures_seen = 0;
    // An allocation failed, and the reserve hasn't been taken back since.
    bool m_recovering = false;
};

// The process's and the system's memory, as the system reports it now.
[[nodiscard]] MemoryReading read_memory();

} // namespace dream::runtime
