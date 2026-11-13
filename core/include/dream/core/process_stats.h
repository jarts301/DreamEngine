#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// What the operating system and the heap say about this process, for profiling. See
// docs/adr/0016-profiling-resources.md.

namespace dream::core {

struct ProcessMemory {
    // Physical memory the process uses now, and the most it has used since it started: the
    // resident set on Linux (VmRSS, VmHWM), the working set on Windows. Shared libraries count.
    std::uint64_t resident_bytes = 0;
    std::uint64_t peak_resident_bytes = 0;

    // Memory that belongs to this process alone. On Windows, private bytes: everything committed,
    // in RAM or in the page file. On Linux, anonymous memory in RAM plus what is swapped out.
    std::uint64_t private_bytes = 0;
};

// Nothing on platforms where it isn't known. Costs a system call or two, so call it a few times a
// second at most.
[[nodiscard]] std::optional<ProcessMemory> process_memory();

// The whole system's memory, for deciding how much room is left. See
// docs/adr/0018-memory-budgets.md.
struct SystemMemory {
    std::uint64_t total_physical_bytes = 0;

    // RAM that can be given to programs without swapping: MemAvailable on Linux, available
    // physical memory on Windows. Below a little of this, the system starts paging.
    std::uint64_t available_physical_bytes = 0;

    // How much more this process can allocate before allocations fail or the system ends it. On
    // Windows, what it can still commit. On Linux, available RAM plus free swap, and no more than
    // the commit limit when the system doesn't overcommit.
    std::uint64_t available_commit_bytes = 0;
};

// Nothing on platforms where it isn't known. Costs a system call or two.
[[nodiscard]] std::optional<SystemMemory> system_memory();

// CPU time used by all of the process's threads so far, in user and kernel mode. Windows updates it
// once per scheduler tick, about every 15 ms, so short intervals read unevenly.
[[nodiscard]] std::optional<double> process_cpu_seconds();

// Counts of the global operator new and delete since the program started. They are only counted
// when the program is built with the engine's counting versions, which dream_add_game adds to
// games on PC while profiling is compiled in; otherwise everything stays zero.
struct HeapStats {
    std::uint64_t allocations = 0;
    std::uint64_t frees = 0;
    // What was asked for, summed over every allocation.
    std::uint64_t allocated_bytes = 0;
    // What is allocated now, as the allocator rounds each block up. Zero where the platform can't
    // tell a block's size.
    std::uint64_t live_bytes = 0;
};

// Safe from any thread. The fields are read one at a time, so they can be slightly out of step
// while other threads allocate.
[[nodiscard]] HeapStats heap_stats() noexcept;

// Whether the counting operator new is in this program.
[[nodiscard]] bool is_heap_counted() noexcept;

namespace detail {

// Called by the counting operator new and delete, from any thread, even before main.
void count_heap_allocation(std::size_t requested_bytes, std::size_t block_bytes) noexcept;
void count_heap_free(std::size_t block_bytes) noexcept;

} // namespace detail

} // namespace dream::core
