#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace dream::runtime {

// How close the game is to running out of memory. See docs/adr/0018-memory-budgets.md and
// docs/reports/memory-budgets.md.
enum class MemoryPressure : std::uint8_t {
    // Nothing to do.
    Normal,
    // Getting close: free what can be rebuilt, such as caches, and avoid growing.
    Warning,
    // An allocation may fail soon, or already did: free memory now. The AssetManager refuses new
    // assets meanwhile.
    Critical,
};

[[nodiscard]] std::string_view to_string(MemoryPressure pressure) noexcept;

// What the pressure is decided from. Figures that aren't known are left empty.
struct MemoryReading {
    // The process's private memory (core::ProcessMemory::private_bytes).
    std::optional<std::uint64_t> process_bytes = std::nullopt;
    // The system's free RAM and its room to allocate (core::SystemMemory).
    std::optional<std::uint64_t> available_physical_bytes = std::nullopt;
    std::optional<std::uint64_t> available_commit_bytes = std::nullopt;
    // Whether an allocation has failed since the emergency reserve was last filled.
    bool allocation_failed = false;
};

// Published on the App's event bus when the pressure changes.
struct MemoryPressureChanged {
    MemoryPressure pressure = MemoryPressure::Normal;
    MemoryPressure previous = MemoryPressure::Normal;
    MemoryReading reading = {};
};

// Read from the project configuration, all optional:
//
//     [memory]
//     enabled = true
//     budget_mb = 0              # The most private memory the game means to use. 0: no budget,
//                                # and only the system's room counts.
//     warning_at = 0.85          # The share of the budget that raises a warning.
//     low_system_mb = 1024       # A warning when the system's free RAM falls below this.
//     critical_system_mb = 256   # Critical when the system's room to allocate falls below this.
//     reserve_mb = 32            # Kept aside, and given back when an allocation fails.
//     check_rate = 4             # Checks per second.
//     refuse_loads = true        # Whether new assets are refused while critical.
struct MemorySettings {
    bool enabled = true;
    std::uint64_t budget_bytes = 0;
    double warning_at = 0.85;
    std::uint64_t low_system_bytes = 1024ull * 1024 * 1024;
    std::uint64_t critical_system_bytes = 256ull * 1024 * 1024;
    std::uint64_t reserve_bytes = 32ull * 1024 * 1024;
    std::uint32_t check_rate = 4;
    bool refuse_loads = true;

    [[nodiscard]] static core::Result<MemorySettings> from_config(const core::Config& config);
};

// Decides the memory pressure from readings:
//
// - Critical when an allocation has failed, when the process is over its budget, or when the
//   system's room to allocate is below `critical_system_bytes`.
// - Warning when the process has used `warning_at` of its budget, or when the system's free RAM is
//   below `low_system_bytes`.
//
// To leave a level, a reading must clear its threshold by 10%, so the pressure doesn't flicker when
// memory hovers around one.
class MemoryBudget {
public:
    explicit MemoryBudget(const MemorySettings& settings = {}) : m_settings(settings) {}

    // Returns the pressure after this reading.
    MemoryPressure evaluate(const MemoryReading& reading);

    [[nodiscard]] MemoryPressure pressure() const noexcept {
        return m_pressure;
    }

private:
    MemorySettings m_settings;
    MemoryPressure m_pressure = MemoryPressure::Normal;
};

} // namespace dream::runtime
