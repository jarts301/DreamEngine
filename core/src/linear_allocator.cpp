#include "dream/core/linear_allocator.h"

#include "dream/core/assert.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace dream::core {
namespace {

constexpr std::size_t k_buffer_alignment = 64;

} // namespace

LinearAllocator::LinearAllocator(std::size_t capacity, MemoryTag tag)
    : m_buffer(static_cast<std::byte*>(tracked_allocate(capacity, k_buffer_alignment, tag))),
      m_capacity(capacity), m_tag(tag) {
    DREAM_VERIFY(m_buffer != nullptr, "LinearAllocator could not allocate its buffer");
}

LinearAllocator::~LinearAllocator() {
    tracked_free(m_buffer, m_capacity, k_buffer_alignment, m_tag);
}

void* LinearAllocator::allocate(std::size_t size, std::size_t alignment) noexcept {
    DREAM_ASSERT(std::has_single_bit(alignment), "Alignment must be a power of two");

    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(m_buffer) + m_offset;
    const std::uintptr_t mask = alignment - 1;
    const std::size_t padding = static_cast<std::size_t>(((address + mask) & ~mask) - address);

    const std::size_t remaining = m_capacity - m_offset;
    if (padding > remaining || size > remaining - padding) {
        return nullptr;
    }

    std::byte* const result = m_buffer + m_offset + padding;
    m_offset += padding + size;
    m_peak = std::max(m_peak, m_offset);
    return result;
}

void LinearAllocator::reset() noexcept {
    m_offset = 0;
}

} // namespace dream::core
