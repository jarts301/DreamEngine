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

TEST_CASE("FixedTimestep runs whole steps and carries the remainder") {
    dream::core::FixedTimestep steps(0.01);
    CHECK(steps.step_seconds() == doctest::Approx(0.01));

    CHECK(steps.advance(0.025) == 2);
    CHECK(steps.alpha() == doctest::Approx(0.5));
    CHECK(steps.advance(0.004) == 0);
    CHECK(steps.alpha() == doctest::Approx(0.9));
    CHECK(steps.advance(0.001) == 1);
    CHECK(steps.alpha() == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(steps.advance(-1.0) == 0);
}

TEST_CASE("FixedTimestep drops time it can't catch up on") {
    dream::core::FixedTimestep steps(0.01, 4);
    CHECK(steps.advance(1.0) == 4);
    CHECK(steps.alpha() == doctest::Approx(1.0));
    CHECK(steps.advance(0.0) == 1);
    CHECK(steps.advance(0.0) == 0);
}
