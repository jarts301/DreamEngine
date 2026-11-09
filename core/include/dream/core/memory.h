#pragma once

#include <cstddef>
#include <cstdint>

namespace dream::core {

// Budget categories for tracked memory.
enum class MemoryTag : std::uint8_t {
    General,
    Graphics,
    Physics,
    Audio,
    Ui,
    Assets,
    Game,
    Count,
};

struct MemoryStats {
    std::size_t current_bytes = 0;
    std::size_t peak_bytes = 0;
    std::size_t live_allocations = 0;
    std::size_t total_allocations = 0;
};

// Returns nullptr if the system is out of memory. Alignment must be a power of two.
[[nodiscard]] void* tracked_allocate(std::size_t size, std::size_t alignment,
                                     MemoryTag tag) noexcept;

// Size, alignment and tag must match the tracked_allocate call. A null pointer is ignored.
void tracked_free(void* pointer, std::size_t size, std::size_t alignment, MemoryTag tag) noexcept;

// Safe to call from any thread. The fields are read individually, so they may be slightly out of
// step with each other while other threads allocate.
[[nodiscard]] MemoryStats memory_stats(MemoryTag tag) noexcept;

} // namespace dream::core
