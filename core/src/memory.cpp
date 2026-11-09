#include "dream/core/memory.h"

#include "dream/core/assert.h"

#include <array>
#include <atomic>
#include <bit>
#include <new>

namespace dream::core {
namespace {

struct TagCounters {
    std::atomic<std::size_t> current_bytes{0};
    std::atomic<std::size_t> peak_bytes{0};
    std::atomic<std::size_t> live_allocations{0};
    std::atomic<std::size_t> total_allocations{0};
};

std::array<TagCounters, static_cast<std::size_t>(MemoryTag::Count)> g_counters;

TagCounters& counters_for(MemoryTag tag) noexcept {
    DREAM_ASSERT(tag < MemoryTag::Count, "Invalid memory tag");
    return g_counters[static_cast<std::size_t>(tag)];
}

void raise_peak(std::atomic<std::size_t>& peak, std::size_t candidate) noexcept {
    std::size_t observed = peak.load(std::memory_order_relaxed);
    while (observed < candidate &&
           !peak.compare_exchange_weak(observed, candidate, std::memory_order_relaxed)) {
    }
}

} // namespace

void* tracked_allocate(std::size_t size, std::size_t alignment, MemoryTag tag) noexcept {
    DREAM_ASSERT(size > 0, "Allocation size must not be zero");
    DREAM_ASSERT(std::has_single_bit(alignment), "Alignment must be a power of two");

    void* const pointer = ::operator new(size, std::align_val_t{alignment}, std::nothrow);
    if (pointer == nullptr) {
        return nullptr;
    }

    TagCounters& counters = counters_for(tag);
    const std::size_t current =
        counters.current_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    raise_peak(counters.peak_bytes, current);
    counters.live_allocations.fetch_add(1, std::memory_order_relaxed);
    counters.total_allocations.fetch_add(1, std::memory_order_relaxed);
    return pointer;
}

void tracked_free(void* pointer, std::size_t size, std::size_t alignment, MemoryTag tag) noexcept {
    if (pointer == nullptr) {
        return;
    }

    ::operator delete(pointer, std::align_val_t{alignment});

    TagCounters& counters = counters_for(tag);
    counters.current_bytes.fetch_sub(size, std::memory_order_relaxed);
    counters.live_allocations.fetch_sub(1, std::memory_order_relaxed);
}

MemoryStats memory_stats(MemoryTag tag) noexcept {
    const TagCounters& counters = counters_for(tag);
    return MemoryStats{
        .current_bytes = counters.current_bytes.load(std::memory_order_relaxed),
        .peak_bytes = counters.peak_bytes.load(std::memory_order_relaxed),
        .live_allocations = counters.live_allocations.load(std::memory_order_relaxed),
        .total_allocations = counters.total_allocations.load(std::memory_order_relaxed),
    };
}

} // namespace dream::core
