#include "dream/core/linear_allocator.h"
#include "dream/core/memory.h"
#include "dream/core/pool_allocator.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace dream::core;

namespace {

bool is_aligned(const void* pointer, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

} // namespace

TEST_CASE("tracked allocations update their tag's statistics") {
    const MemoryStats before = memory_stats(MemoryTag::Audio);

    void* const pointer = tracked_allocate(1024, 32, MemoryTag::Audio);
    REQUIRE(pointer != nullptr);
    CHECK(is_aligned(pointer, 32));

    const MemoryStats during = memory_stats(MemoryTag::Audio);
    CHECK(during.current_bytes == before.current_bytes + 1024);
    CHECK(during.live_allocations == before.live_allocations + 1);
    CHECK(during.total_allocations == before.total_allocations + 1);
    CHECK(during.peak_bytes >= during.current_bytes);

    tracked_free(pointer, 1024, 32, MemoryTag::Audio);

    const MemoryStats after = memory_stats(MemoryTag::Audio);
    CHECK(after.current_bytes == before.current_bytes);
    CHECK(after.live_allocations == before.live_allocations);
    CHECK(after.total_allocations == before.total_allocations + 1);
    CHECK(after.peak_bytes == during.peak_bytes);
}

TEST_CASE("LinearAllocator respects alignment") {
    LinearAllocator allocator(1024);

    (void)allocator.allocate(1, 1);
    for (const std::size_t alignment : {2u, 4u, 8u, 16u, 32u, 64u}) {
        void* const pointer = allocator.allocate(3, alignment);
        REQUIRE(pointer != nullptr);
        CHECK(is_aligned(pointer, alignment));
    }
}

TEST_CASE("LinearAllocator allocations do not overlap") {
    LinearAllocator allocator(64);

    void* const first = allocator.allocate(16, 1);
    void* const second = allocator.allocate(16, 1);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    std::memset(first, 0xAA, 16);
    std::memset(second, 0xBB, 16);
    CHECK(static_cast<unsigned char*>(first)[15] == 0xAA);
    CHECK(static_cast<unsigned char*>(second)[0] == 0xBB);
}

TEST_CASE("LinearAllocator returns null when full and recovers on reset") {
    LinearAllocator allocator(32);

    void* const first = allocator.allocate(32, 1);
    REQUIRE(first != nullptr);
    CHECK(allocator.used() == 32);
    CHECK(allocator.allocate(1, 1) == nullptr);

    allocator.reset();
    CHECK(allocator.used() == 0);
    CHECK(allocator.peak_used() == 32);
    CHECK(allocator.allocate(32, 1) == first);
}

TEST_CASE("LinearAllocator rejects a size that would overflow") {
    LinearAllocator allocator(32);
    CHECK(allocator.allocate(SIZE_MAX, 16) == nullptr);
    CHECK(allocator.used() == 0);
}

TEST_CASE("PoolAllocator hands out every block once, then returns null") {
    PoolAllocator pool(24, 8);

    std::vector<void*> blocks;
    for (std::size_t i = 0; i < pool.block_count(); ++i) {
        void* const block = pool.allocate();
        REQUIRE(block != nullptr);
        blocks.push_back(block);
    }

    CHECK(pool.available() == 0);
    CHECK(pool.allocate() == nullptr);

    for (std::size_t i = 1; i < blocks.size(); ++i) {
        CHECK(blocks[i] != blocks[i - 1]);
    }
}

TEST_CASE("PoolAllocator reuses a returned block") {
    PoolAllocator pool(24, 2);

    void* const first = pool.allocate();
    (void)pool.allocate();
    pool.deallocate(first);

    CHECK(pool.available() == 1);
    CHECK(pool.allocate() == first);
}

TEST_CASE("PoolAllocator blocks honour the requested alignment") {
    PoolAllocator pool(10, 4, 64);
    for (int i = 0; i < 4; ++i) {
        CHECK(is_aligned(pool.allocate(), 64));
    }
}

TEST_CASE("PoolAllocator handles blocks smaller than a pointer") {
    PoolAllocator pool(1, 4);

    void* const first = pool.allocate();
    void* const second = pool.allocate();
    pool.deallocate(first);
    pool.deallocate(second);

    CHECK(pool.available() == 4);
    CHECK(pool.allocate() == second);
    CHECK(pool.allocate() == first);
}

TEST_CASE("allocators release their tracked memory when destroyed") {
    const MemoryStats before = memory_stats(MemoryTag::Physics);
    {
        const LinearAllocator linear(256, MemoryTag::Physics);
        const PoolAllocator pool(32, 8, alignof(std::max_align_t), MemoryTag::Physics);
        CHECK(memory_stats(MemoryTag::Physics).live_allocations == before.live_allocations + 2);
    }
    CHECK(memory_stats(MemoryTag::Physics).current_bytes == before.current_bytes);
}
