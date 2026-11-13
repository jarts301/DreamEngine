#include "dream/core/process_stats.h"

#include <algorithm>
#include <atomic>

#if defined(_WIN32)
#include <windows.h>
// After windows.h, which it needs.
#include <psapi.h>
#elif defined(__linux__)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#endif

namespace dream::core {
namespace {

// Constant-initialized, because operator new runs before any dynamic initialization.
constinit std::atomic<std::uint64_t> g_allocations{0};
constinit std::atomic<std::uint64_t> g_frees{0};
constinit std::atomic<std::uint64_t> g_allocated_bytes{0};
constinit std::atomic<std::uint64_t> g_live_bytes{0};

} // namespace

#if defined(_WIN32)

std::optional<ProcessMemory> process_memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return std::nullopt;
    }
    return ProcessMemory{
        .resident_bytes = counters.WorkingSetSize,
        .peak_resident_bytes = counters.PeakWorkingSetSize,
        .private_bytes = counters.PrivateUsage,
    };
}

std::optional<SystemMemory> system_memory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) {
        return std::nullopt;
    }
    // ullAvailPageFile is what this process can still commit, which is where allocations fail.
    return SystemMemory{
        .total_physical_bytes = status.ullTotalPhys,
        .available_physical_bytes = status.ullAvailPhys,
        .available_commit_bytes = status.ullAvailPageFile,
    };
}

std::optional<double> process_cpu_seconds() {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0) {
        return std::nullopt;
    }
    const auto ticks = [](const FILETIME& time) {
        return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    };
    // FILETIME counts 100-nanosecond intervals.
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;
}

#elif defined(__linux__)

std::optional<ProcessMemory> process_memory() {
    std::FILE* const file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return std::nullopt;
    }

    ProcessMemory memory;
    bool found_resident = false;
    bool found_anonymous = false;
    std::uint64_t anonymous = 0;
    std::uint64_t swapped = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        // Lines look like "VmRSS:     12345 kB".
        const auto read = [&line](const char* key, std::uint64_t& bytes) {
            const std::size_t length = std::strlen(key);
            if (std::strncmp(line, key, length) != 0) {
                return false;
            }
            bytes = std::strtoull(line + length, nullptr, 10) * 1024;
            return true;
        };
        if (read("VmRSS:", memory.resident_bytes)) {
            found_resident = true;
        } else if (read("RssAnon:", anonymous)) {
            found_anonymous = true;
        } else if (!read("VmHWM:", memory.peak_resident_bytes)) {
            read("VmSwap:", swapped);
        }
    }
    std::fclose(file);

    if (!found_resident) {
        return std::nullopt;
    }
    // RssAnon needs Linux 4.5. Before it, only the resident total is known.
    memory.private_bytes = found_anonymous ? anonymous + swapped : memory.resident_bytes;
    return memory;
}

std::optional<SystemMemory> system_memory() {
    std::FILE* const file = std::fopen("/proc/meminfo", "r");
    if (file == nullptr) {
        return std::nullopt;
    }

    std::uint64_t total = 0;
    std::uint64_t available = 0;
    std::uint64_t swap_free = 0;
    std::uint64_t commit_limit = 0;
    std::uint64_t committed = 0;
    bool found_available = false;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        // Lines look like "MemAvailable:    123456 kB".
        const auto read = [&line](const char* key, std::uint64_t& bytes) {
            const std::size_t length = std::strlen(key);
            if (std::strncmp(line, key, length) != 0) {
                return false;
            }
            bytes = std::strtoull(line + length, nullptr, 10) * 1024;
            return true;
        };
        if (read("MemAvailable:", available)) {
            found_available = true;
        } else if (!read("MemTotal:", total) && !read("SwapFree:", swap_free) &&
                   !read("CommitLimit:", commit_limit)) {
            read("Committed_AS:", committed);
        }
    }
    std::fclose(file);
    // MemAvailable needs Linux 3.14.
    if (!found_available) {
        return std::nullopt;
    }

    SystemMemory memory{
        .total_physical_bytes = total,
        .available_physical_bytes = available,
        .available_commit_bytes = available + swap_free,
    };
    // In strict mode (vm.overcommit_memory = 2) allocations fail at the commit limit, as on
    // Windows. Otherwise the limit is only advisory, and running out of RAM and swap is what ends
    // a program.
    if (std::FILE* const mode_file = std::fopen("/proc/sys/vm/overcommit_memory", "r")) {
        const int mode = std::fgetc(mode_file);
        std::fclose(mode_file);
        if (mode == '2') {
            const std::uint64_t room = commit_limit > committed ? commit_limit - committed : 0;
            memory.available_commit_bytes = std::min(memory.available_commit_bytes, room);
        }
    }
    return memory;
}

std::optional<double> process_cpu_seconds() {
    timespec time{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0) {
        return std::nullopt;
    }
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}

#else

std::optional<ProcessMemory> process_memory() {
    return std::nullopt;
}

std::optional<SystemMemory> system_memory() {
    return std::nullopt;
}

std::optional<double> process_cpu_seconds() {
    return std::nullopt;
}

#endif

HeapStats heap_stats() noexcept {
    return HeapStats{
        .allocations = g_allocations.load(std::memory_order_relaxed),
        .frees = g_frees.load(std::memory_order_relaxed),
        .allocated_bytes = g_allocated_bytes.load(std::memory_order_relaxed),
        .live_bytes = g_live_bytes.load(std::memory_order_relaxed),
    };
}

bool is_heap_counted() noexcept {
    // A program that counts has allocated long before anyone asks.
    return g_allocations.load(std::memory_order_relaxed) > 0;
}

namespace detail {

void count_heap_allocation(std::size_t requested_bytes, std::size_t block_bytes) noexcept {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    g_allocated_bytes.fetch_add(requested_bytes, std::memory_order_relaxed);
    g_live_bytes.fetch_add(block_bytes, std::memory_order_relaxed);
}

void count_heap_free(std::size_t block_bytes) noexcept {
    g_frees.fetch_add(1, std::memory_order_relaxed);
    g_live_bytes.fetch_sub(block_bytes, std::memory_order_relaxed);
}

} // namespace detail

} // namespace dream::core
