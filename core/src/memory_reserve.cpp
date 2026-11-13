#include "dream/core/memory_reserve.h"

#include "dream/core/assert.h"
#include "dream/core/process_stats.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace dream::core {
namespace {

// Constant-initialized, because the handler can run at any time, on any thread.
constinit std::atomic<void*> g_block{nullptr};
constinit std::atomic<bool> g_spent{false};
constinit std::atomic<std::uint64_t> g_failures{0};
constinit std::atomic<bool> g_exists{false};
constinit std::atomic<std::new_handler> g_previous{nullptr};

// Nothing here may allocate, because memory has just run out: the text is formatted on the stack
// and written straight to stderr, without the log.
void report_exhausted() noexcept {
    // The heap is only counted in games that list a profiler (ADR 0016).
    char heap[64] = "";
    if (is_heap_counted()) {
        std::snprintf(heap, sizeof(heap), "; C++ heap in use %llu MB",
                      static_cast<unsigned long long>(heap_stats().live_bytes / (1024 * 1024)));
    }
    char text[512];
    std::snprintf(text, sizeof(text),
                  "[FATAL] Out of memory: an allocation failed and the memory reserve is already "
                  "spent (%llu failed allocation attempts%s). The program ends unless the "
                  "allocation was allowed to fail. See docs/reports/memory-budgets.md.\n",
                  static_cast<unsigned long long>(g_failures.load(std::memory_order_relaxed)),
                  heap);
    std::fputs(text, stderr);
    std::fflush(stderr);
}

void on_allocation_failure() {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    // Only one thread gets the block, so it is freed once.
    if (void* const block = g_block.exchange(nullptr, std::memory_order_acq_rel)) {
        std::free(block);
        g_spent.store(true, std::memory_order_release);
        // Returning makes operator new try again, now with the reserve's memory free.
        return;
    }
    report_exhausted();
    std::set_new_handler(g_previous.load(std::memory_order_acquire));
}

void arm_handler() noexcept {
    const std::new_handler current = std::get_new_handler();
    if (current != &on_allocation_failure) {
        g_previous.store(current, std::memory_order_release);
        std::set_new_handler(&on_allocation_failure);
    }
}

} // namespace

MemoryReserve::MemoryReserve(std::size_t bytes) : m_bytes(bytes) {
    const bool another_exists = g_exists.exchange(true);
    DREAM_ASSERT(!another_exists, "Only one MemoryReserve may exist at a time");
    g_failures.store(0, std::memory_order_relaxed);
    // Taken with malloc, not new, so the handler can free it without running a destructor, and
    // the heap counts only the program's own allocations.
    void* const block = bytes > 0 ? std::malloc(bytes) : nullptr;
    g_block.store(block, std::memory_order_release);
    g_spent.store(bytes > 0 && block == nullptr, std::memory_order_release);
    arm_handler();
}

MemoryReserve::~MemoryReserve() {
    // A handler someone installed since stays installed.
    if (std::get_new_handler() == &on_allocation_failure) {
        std::set_new_handler(g_previous.load(std::memory_order_acquire));
    }
    std::free(g_block.exchange(nullptr, std::memory_order_acq_rel));
    g_spent.store(false, std::memory_order_relaxed);
    g_exists.store(false);
}

bool MemoryReserve::is_spent() const noexcept {
    return g_spent.load(std::memory_order_acquire);
}

std::uint64_t MemoryReserve::failed_allocations() const noexcept {
    return g_failures.load(std::memory_order_relaxed);
}

bool MemoryReserve::refill() noexcept {
    if (is_spent()) {
        void* const block = std::malloc(m_bytes);
        if (block == nullptr) {
            return false;
        }
        g_block.store(block, std::memory_order_release);
        g_spent.store(false, std::memory_order_release);
    }
    arm_handler();
    return true;
}

} // namespace dream::core
