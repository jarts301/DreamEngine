#include "dream/core/clock.h"

#include <doctest/doctest.h>

using namespace dream::core;
using namespace std::chrono_literals;
using doctest::Approx;

TEST_CASE("the first tick has no delta") {
    Clock clock;
    const FrameTime frame = clock.tick_at(TimePoint{5s});

    CHECK(frame.delta_seconds == 0.0);
    CHECK(frame.elapsed_seconds == 0.0);
    CHECK(frame.frame_index == 0);
}

TEST_CASE("ticks measure the time between frames") {
    Clock clock;
    const TimePoint start{};
    (void)clock.tick_at(start);

    (void)clock.tick_at(start + 10ms);
    const FrameTime frame = clock.tick_at(start + 30ms);

    CHECK(frame.delta_seconds == Approx(0.020));
    CHECK(frame.elapsed_seconds == Approx(0.030));
    CHECK(frame.frame_index == 2);
}

TEST_CASE("a long pause is capped at the maximum delta") {
    Clock clock(0.25);
    const TimePoint start{};
    (void)clock.tick_at(start);

    const FrameTime frame = clock.tick_at(start + 10s);

    CHECK(frame.delta_seconds == Approx(0.25));
    CHECK(frame.elapsed_seconds == Approx(0.25));
}

TEST_CASE("Stopwatch time moves forward and restarts on reset") {
    Stopwatch watch;
    const double first = watch.elapsed_seconds();
    const double second = watch.elapsed_seconds();
    CHECK(first >= 0.0);
    CHECK(second >= first);

    watch.reset();
    CHECK(watch.elapsed_seconds() >= 0.0);
}
