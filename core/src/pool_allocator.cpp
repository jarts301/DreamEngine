#include "dream/core/pool_allocator.h"

#include "dream/core/assert.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <new>

namespace dream::core {
namespace {

// Lives inside each unused block, so the free list needs no extra memory.
struct FreeBlock {
    FreeBlock* next = nullptr;
};

} // namespace

PoolAllocator::PoolAllocator(std::size_t block_size, std::size_t block_count,
                             std::size_t block_alignment, MemoryTag tag)
    : m_alignment(std::max(block_alignment, alignof(FreeBlock))), m_block_count(block_count),
      m_available(block_count), m_tag(tag) {
    DREAM_VERIFY(block_size > 0 && block_count > 0, "PoolAllocator needs a non-zero size");
    DREAM_VERIFY(std::has_single_bit(block_alignment), "Alignment must be a power of two");

    const std::size_t minimum_stride = std::max(block_size, sizeof(FreeBlock));
    m_stride = (minimum_stride + m_alignment - 1) & ~(m_alignment - 1);

    m_buffer = static_cast<std::byte*>(tracked_allocate(m_stride * m_block_count, m_alignment, tag));
    DREAM_VERIFY(m_buffer != nullptr, "PoolAllocator could not allocate its buffer");

    // Built back to front so blocks are handed out in address order.
    FreeBlock* head = nullptr;
    for (std::size_t i = m_block_count; i > 0; --i) {
        head = ::new (m_buffer + (i - 1) * m_stride) FreeBlock{head};
    }
    m_free_head = head;
}

PoolAllocator::~PoolAllocator() {
    tracked_free(m_buffer, m_stride * m_block_count, m_alignment, m_tag);
}

void* PoolAllocator::allocate() noexcept {
    if (m_free_head == nullptr) {
        return nullptr;
    }

    FreeBlock* const block = static_cast<FreeBlock*>(m_free_head);
    m_free_head = block->next;
    --m_available;
    return block;
}

void PoolAllocator::deallocate(void* block) noexcept {
    if (block == nullptr) {
        return;
    }
    DREAM_ASSERT(owns(block), "Block does not belong to this pool");

    m_free_head = ::new (block) FreeBlock{static_cast<FreeBlock*>(m_free_head)};
    ++m_available;
}

bool PoolAllocator::owns(const void* block) const noexcept {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(block);
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(m_buffer);
    if (address < start || address >= start + m_stride * m_block_count) {
        return false;
    }
    return (address - start) % m_stride == 0;
}

} // namespace dream::core
