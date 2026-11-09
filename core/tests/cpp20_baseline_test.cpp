// Exercises the C++20 features allowed by docs/adr/0002-cpp-subset.md, so a toolchain that cannot
// compile them fails in CI.

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <compare>
#include <concepts>
#include <cstdint>
#include <format>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <vector>

namespace {

template <std::integral T> constexpr T doubled(T value) {
    return value * 2;
}

consteval int compile_time_value() {
    return 42;
}

struct Point {
    int x = 0;
    int y = 0;

    constexpr auto operator<=>(const Point&) const = default;
};

int sum(std::span<const int> values) {
    int total = 0;
    for (const int value : values) {
        total += value;
    }
    return total;
}

} // namespace

static_assert(doubled(21) == compile_time_value());
static_assert(Point{.x = 1, .y = 2} < Point{.x = 1, .y = 3});
static_assert(std::bit_cast<std::uint32_t>(1.0f) == 0x3F800000u);
static_assert(std::has_single_bit(64u));

TEST_CASE("span") {
    const std::array<int, 3> values{1, 2, 3};
    CHECK(sum(values) == 6);
}

TEST_CASE("format") {
    CHECK(std::format("{}-{}", 7, "dream") == "7-dream");
}

TEST_CASE("ranges") {
    const std::vector<int> values{1, 2, 3, 4};
    std::vector<int> even;
    for (const int value : values | std::views::filter([](int v) { return v % 2 == 0; })) {
        even.push_back(value);
    }
    CHECK(even == std::vector<int>{2, 4});
}

TEST_CASE("source_location") {
    const std::source_location location = std::source_location::current();
    CHECK(location.line() > 0);
}
