// Runs a game out of memory for real, under a limit the test sets on its own process, and checks
// that it survives: the failed allocation is given the emergency reserve, the game is told, frees
// memory, and carries on. It is a program of its own, because the limit can't be lifted.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "dream/core/process_stats.h"
#include "dream/runtime/app.h"
#include "dream/runtime/memory_budget.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/resource.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;

namespace {

constexpr std::size_t k_mb = 1024 * 1024;

// Sanitizers reserve far more address space than any limit here would allow.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DREAM_TEST_CAN_LIMIT_MEMORY 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define DREAM_TEST_CAN_LIMIT_MEMORY 0
#endif
#endif
#if !defined(DREAM_TEST_CAN_LIMIT_MEMORY)
#if defined(_WIN32) || defined(__linux__)
#define DREAM_TEST_CAN_LIMIT_MEMORY 1
#else
#define DREAM_TEST_CAN_LIMIT_MEMORY 0
#endif
#endif

// Lets this process use only `extra` bytes more than it does now, so allocations beyond that fail.
bool limit_memory(std::uint64_t extra) {
#if defined(_WIN32)
    // A job's process memory limit caps committed memory, which is where Windows' allocations
    // fail when the system runs out.
    const std::optional<core::ProcessMemory> memory = core::process_memory();
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!memory.has_value() || job == nullptr) {
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(memory->private_bytes + extra);
    return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits)) != 0 &&
           AssignProcessToJobObject(job, GetCurrentProcess()) != 0;
#elif defined(__linux__)
    // Linux hands out memory it may not have, so allocations fail only against a limit such as
    // this one, on the address space.
    std::FILE* const file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return false;
    }
    std::uint64_t address_space = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "VmSize:", 7) == 0) {
            address_space = std::strtoull(line + 7, nullptr, 10) * 1024;
        }
    }
    std::fclose(file);
    const rlimit limit{.rlim_cur = address_space + extra, .rlim_max = address_space + extra};
    return address_space > 0 && setrlimit(RLIMIT_AS, &limit) == 0;
#else
    static_cast<void>(extra);
    return false;
#endif
}

// Takes 8 MB more every frame, and lets most of it go when told memory is low.
class HungryGame final : public Game {
public:
    core::Result<void> on_start(App& app) override {
        m_subscription = app.events().subscribe<MemoryPressureChanged>(
            [this](const MemoryPressureChanged& change) {
                failures_reported += change.reading.allocation_failed ? 1 : 0;
            });
        return {};
    }

    void on_update(App& /*app*/, const core::FrameTime& /*time*/) override {
        ++frames;
        // Filled with zeros, so the memory is really used.
        held.push_back(std::make_unique<std::vector<std::byte>>(8 * k_mb));
        most_held = std::max(most_held, held.size());
    }

    void on_low_memory(App& /*app*/) override {
        ++low_memory_calls;
        held.resize(held.size() / 4);
    }

    int frames = 0;
    int low_memory_calls = 0;
    int failures_reported = 0;
    std::size_t most_held = 0;
    std::vector<std::unique_ptr<std::vector<std::byte>>> held;

private:
    Subscription m_subscription;
};

} // namespace

TEST_CASE("A game that runs out of memory is told, frees some, and carries on") {
#if DREAM_TEST_CAN_LIMIT_MEMORY
    constexpr int k_frames = 200;
    FakeState state;
    for (int frame = 0; frame < k_frames; ++frame) {
        state.script.push_back(k_end_of_frame);
    }
    // No budget, and no system thresholds: only a failed allocation can raise the pressure, as
    // when the limit comes from outside the game, which the system figures don't show.
    const core::Config config = core::Config::parse(R"(
[app]
max_fps = 0

[memory]
low_system_mb = 0
critical_system_mb = 0
reserve_mb = 32
check_rate = 1000
)")
                                    .value();
    HungryGame game;
    App app(std::make_unique<FakePlatform>(state), config);

    // 200 frames take 1600 MB in total, far past the 160 MB allowed.
    REQUIRE(limit_memory(160 * k_mb));
    const core::Result<void> result = app.run(game);

    REQUIRE(result.has_value());
    CHECK(game.frames == k_frames);
    CHECK(game.low_memory_calls >= 1);
    CHECK(game.failures_reported >= 1);
    CHECK(game.most_held * 8 < 160);
#else
    MESSAGE("This platform or build can't limit the process's memory, so the test is skipped");
#endif
}
