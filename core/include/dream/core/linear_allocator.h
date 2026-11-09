#pragma once

#include "dream/core/memory.h"

#include <cstddef>

namespace dream::core {

// Hands out memory from one block by moving an offset forward, and frees everything at once with
// reset(). Suited to per-frame data. Not thread-safe, and destructors are never called.
class LinearAllocator {
public:
    explicit LinearAllocator(std::size_t capacity, MemoryTag tag = MemoryTag::General);
    ~LinearAllocator();

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    // Returns nullptr when the request does not fit. Alignment must be a power of two.
    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t)) noexcept;

    void reset() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return m_capacity;
    }

    [[nodiscard]] std::size_t used() const noexcept {
        return m_offset;
    }

    // The highest value of used() since construction, for sizing the allocator.
    [[nodiscard]] std::size_t peak_used() const noexcept {
        return m_peak;
    }

private:
    std::byte* m_buffer = nullptr;
    std::size_t m_capacity = 0;
    std::size_t m_offset = 0;
    std::size_t m_peak = 0;
    MemoryTag m_tag = MemoryTag::General;
};

} // namespace dream::core
