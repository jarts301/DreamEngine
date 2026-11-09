#pragma once

#include "dream/core/memory.h"

#include <cstddef>

namespace dream::core {

// Hands out fixed-size blocks in constant time. Not thread-safe, and destructors are never called.
class PoolAllocator {
public:
    PoolAllocator(std::size_t block_size, std::size_t block_count,
                  std::size_t block_alignment = alignof(std::max_align_t),
                  MemoryTag tag = MemoryTag::General);
    ~PoolAllocator();

    PoolAllocator(const PoolAllocator&) = delete;
    PoolAllocator& operator=(const PoolAllocator&) = delete;

    // Returns nullptr when every block is in use.
    [[nodiscard]] void* allocate() noexcept;

    // The pointer must have come from allocate() on this pool. A null pointer is ignored.
    void deallocate(void* block) noexcept;

    [[nodiscard]] std::size_t block_count() const noexcept {
        return m_block_count;
    }

    [[nodiscard]] std::size_t available() const noexcept {
        return m_available;
    }

private:
    [[nodiscard]] bool owns(const void* block) const noexcept;

    std::byte* m_buffer = nullptr;
    void* m_free_head = nullptr;
    std::size_t m_stride = 0;
    std::size_t m_alignment = 0;
    std::size_t m_block_count = 0;
    std::size_t m_available = 0;
    MemoryTag m_tag = MemoryTag::General;
};

} // namespace dream::core
