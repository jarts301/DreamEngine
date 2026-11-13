#include "dream/core/process_stats.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

using namespace dream::core;

// The test program links runtime/entry/heap_counting.cpp, as profiled games do.
#if !defined(DREAM_DISABLE_PROFILING) && (defined(_WIN32) || defined(__linux__)) &&                \
    !defined(__ANDROID__)

namespace {

struct alignas(64) OverAligned {
    std::byte bytes[64];
};

// Compilers may remove a new and delete whose memory is never used. Letting the pointer escape
// keeps the allocation real.
void* volatile g_escaped = nullptr;

void escape(void* pointer) {
    g_escaped = pointer;
}

} // namespace

TEST_CASE("The counting operator new counts allocations, frees and the heap in use") {
    REQUIRE(is_heap_counted());

    // Snapshots are taken right around the operations, with no checks in between, because checks
    // can allocate too.
    const HeapStats before = heap_stats();
    auto* const single = new std::uint64_t(7);
    auto* const array = new char[1000];
    auto* const aligned = new OverAligned;
    auto* const quiet = new (std::nothrow) std::uint32_t(3);
    const HeapStats during = heap_stats();
    escape(single);
    escape(array);
    escape(aligned);
    escape(quiet);
    // A pointer can't be looked at once its memory is freed.
    const auto aligned_address = reinterpret_cast<std::uintptr_t>(aligned);
    delete single;
    delete[] array;
    delete aligned;
    delete quiet;
    const HeapStats after = heap_stats();

    CHECK(during.allocations == before.allocations + 4);
    CHECK(during.allocated_bytes ==
          before.allocated_bytes + sizeof(std::uint64_t) + 1000 + sizeof(OverAligned) + 4);
    // Blocks are at least as big as asked for.
    CHECK(during.live_bytes >= before.live_bytes + 1000 + sizeof(OverAligned));
    CHECK(aligned_address % alignof(OverAligned) == 0);

    CHECK(after.frees == during.frees + 4);
    CHECK(after.live_bytes == before.live_bytes);
    CHECK(after.allocations == during.allocations);
}

TEST_CASE("The counting operator new gives each zero-byte allocation its own address") {
    const HeapStats before = heap_stats();
    void* const first = ::operator new(0);
    void* const second = ::operator new(0, std::align_val_t{128});
    const HeapStats during = heap_stats();
    const bool distinct = first != second;
    const bool both_allocated = first != nullptr && second != nullptr;
    const auto second_address = reinterpret_cast<std::uintptr_t>(second);
    ::operator delete(first);
    ::operator delete(second, std::align_val_t{128});
    const HeapStats after = heap_stats();

    CHECK(both_allocated);
    CHECK(distinct);
    CHECK(second_address % 128 == 0);
    CHECK(during.allocations == before.allocations + 2);
    CHECK(after.live_bytes == before.live_bytes);
}

#endif
