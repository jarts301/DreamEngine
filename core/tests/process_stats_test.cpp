#include "dream/core/process_stats.h"

#include "dream/core/clock.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <vector>

using namespace dream::core;

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))

TEST_CASE("process memory is known, and grows when the process uses more") {
    const std::optional<ProcessMemory> before = process_memory();
    REQUIRE(before.has_value());
    CHECK(before->resident_bytes > 0);
    CHECK(before->private_bytes > 0);
    CHECK(before->peak_resident_bytes >= before->resident_bytes);

    // Zeroing the memory touches every page, so the system has to provide all of it. Half of it
    // is asked of each figure, leaving room for what the allocator and the system round or share.
    constexpr std::size_t k_size = 64 * 1024 * 1024;
    const std::vector<std::byte> held(k_size);

    const std::optional<ProcessMemory> during = process_memory();
    REQUIRE(during.has_value());
    CHECK(during->resident_bytes >= before->resident_bytes + k_size / 2);
    CHECK(during->private_bytes >= before->private_bytes + k_size / 2);
    CHECK(during->peak_resident_bytes >= during->resident_bytes);
    CHECK(held.size() == k_size);
}

TEST_CASE("process CPU time grows while the process works") {
    const std::optional<double> start = process_cpu_seconds();
    REQUIRE(start.has_value());

    // Busy until the CPU time moves past Windows' 15 ms tick, or a generous limit passes.
    const Stopwatch wall;
    volatile std::size_t work = 0;
    double now = *start;
    while (now < *start + 0.05 && wall.elapsed_seconds() < 5.0) {
        for (int i = 0; i < 100000; ++i) {
            work = work + 1;
        }
        now = process_cpu_seconds().value_or(*start);
    }
    CHECK(now > *start);
}

#endif

TEST_CASE("heap counters add up the allocations and frees they are told of") {
    const HeapStats before = heap_stats();

    detail::count_heap_allocation(100, 112);
    detail::count_heap_allocation(20, 32);
    const HeapStats during = heap_stats();
    CHECK(during.allocations == before.allocations + 2);
    CHECK(during.allocated_bytes == before.allocated_bytes + 120);
    CHECK(during.live_bytes == before.live_bytes + 144);
    CHECK(is_heap_counted());

    detail::count_heap_free(112);
    detail::count_heap_free(32);
    const HeapStats after = heap_stats();
    CHECK(after.frees == before.frees + 2);
    CHECK(after.live_bytes == before.live_bytes);
    CHECK(after.allocated_bytes == during.allocated_bytes);
}
