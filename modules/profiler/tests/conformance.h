#pragma once

// Tests that every profiler backend must pass. A backend's test program defines make_profiler()
// and includes this file once. Backends record events from code being measured, so beyond keeping
// what they record, they must never fail or break the program, whatever order events arrive in.

#include "dream/core/profile.h"
#include "dream/profiler/profiler.h"

#include <doctest/doctest.h>

#include <atomic>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

[[nodiscard]] std::unique_ptr<dream::profiler::Profiler> make_profiler();

namespace conformance {

inline constexpr dream::core::ProfileSite k_outer{"Outer", __FILE__, __LINE__};
inline constexpr dream::core::ProfileSite k_inner{"Inner", __FILE__, __LINE__};

// Installs a profiler as the engine's sink for one test.
class Installed {
public:
    explicit Installed(dream::profiler::Profiler& profiler) {
        dream::core::set_profile_sink(&profiler);
    }
    ~Installed() {
        dream::core::set_profile_sink(nullptr);
    }
    Installed(const Installed&) = delete;
    Installed& operator=(const Installed&) = delete;
};

} // namespace conformance

TEST_CASE("Profiler takes every kind of event, and flushes more than once") {
    using namespace conformance;
    const auto profiler = make_profiler();

    profiler->set_thread_name("Main");
    profiler->begin_zone(k_outer, {});
    profiler->begin_zone(k_inner, "detail \"quoted\"\n");
    profiler->record_value("Value", 3.5);
    profiler->record_message("A message");
    profiler->end_zone(k_inner);
    profiler->end_zone(k_outer);
    profiler->mark_frame();

    CHECK(profiler->flush().has_value());
    CHECK(profiler->flush().has_value());
}

TEST_CASE("Profiler accepts events in any order without failing") {
    using namespace conformance;
    const auto profiler = make_profiler();

    // An end without a beginning, zones left open, a flush in the middle of a zone, and values that
    // aren't numbers.
    profiler->end_zone(k_outer);
    profiler->begin_zone(k_outer, {});
    CHECK(profiler->flush().has_value());
    profiler->begin_zone(k_inner, {});
    profiler->mark_frame();
    profiler->record_value("Not finite", std::numeric_limits<double>::quiet_NaN());
    profiler->record_value("Not finite", std::numeric_limits<double>::infinity());
    CHECK(profiler->flush().has_value());
}

TEST_CASE("Profiler takes events from many threads while flushing") {
    using namespace conformance;
    const auto profiler = make_profiler();

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&profiler, &stop] {
            profiler->set_thread_name("Worker");
            while (!stop.load()) {
                profiler->begin_zone(k_outer, {});
                profiler->record_value("Value", 1.0);
                profiler->end_zone(k_outer);
            }
        });
    }
    for (int flush = 0; flush < 3; ++flush) {
        CHECK(profiler->flush().has_value());
    }
    stop = true;
    for (std::thread& thread : threads) {
        thread.join();
    }
    CHECK(profiler->flush().has_value());
}

TEST_CASE("Profiler receives the engine's profiling macros once installed") {
    using namespace conformance;
    const auto profiler = make_profiler();
    {
        const Installed installed(*profiler);
        REQUIRE(dream::core::profile_sink() == profiler.get());

        DREAM_PROFILE_THREAD("Main");
        {
            DREAM_PROFILE_ZONE("Macro zone");
            DREAM_PROFILE_ZONE_DETAIL("Macro detail", "assets:/file.toml");
            DREAM_PROFILE_VALUE("Macro value", 7);
            DREAM_PROFILE_MESSAGE("Macro message {}", 1);
        }
        DREAM_PROFILE_FRAME();
    }
    CHECK(dream::core::profile_sink() == nullptr);
    CHECK(profiler->flush().has_value());
}
