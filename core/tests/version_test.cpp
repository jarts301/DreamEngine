#include "dream/core/version.h"

#include <doctest/doctest.h>

#include <format>
#include <string>

using dream::core::Version;

TEST_CASE("engine_version matches the CMake project version") {
    const Version version = dream::core::engine_version();
    const std::string text = std::format("{}.{}.{}", version.major_number, version.minor_number,
                                         version.patch_number);

    CHECK(text == DREAM_TEST_EXPECTED_VERSION);
}

TEST_CASE("versions order by major, then minor, then patch") {
    constexpr Version base{.major_number = 1, .minor_number = 2, .patch_number = 3};

    CHECK(base < Version{.major_number = 2});
    CHECK(base < Version{.major_number = 1, .minor_number = 3});
    CHECK(base < Version{.major_number = 1, .minor_number = 2, .patch_number = 4});
    CHECK(base == Version{.major_number = 1, .minor_number = 2, .patch_number = 3});
}
