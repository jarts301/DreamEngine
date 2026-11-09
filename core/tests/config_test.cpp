#include "dream/core/config.h"

#include <doctest/doctest.h>

#include <ostream> // Needed for doctest to print std::string_view.
#include <string>

using namespace dream::core;
using doctest::Approx;

namespace {

constexpr const char* k_sample = R"(
title = "Dream"
fullscreen = true
width = 1920
scale = 1.5
whole_scale = 2

[modules.windows]
rhi = ["d3d12", "vulkan"]
physics = []
)";

} // namespace

TEST_CASE("Config reads every supported value type") {
    const Result<Config> result = Config::parse(k_sample);
    REQUIRE(result.has_value());
    const Config& config = result.value();

    CHECK(config.get_string("title").value_or("") == "Dream");
    CHECK(config.get_bool("fullscreen").value_or(false));
    CHECK(config.get_int("width").value_or(0) == 1920);
    CHECK(config.get_float("scale").value_or(0.0) == Approx(1.5));
}

TEST_CASE("Config reads integers as floats, but not the reverse") {
    const Result<Config> result = Config::parse(k_sample);
    REQUIRE(result.has_value());

    CHECK(result.value().get_float("whole_scale").value_or(0.0) == Approx(2.0));
    CHECK_FALSE(result.value().get_int("scale").has_value());
}

TEST_CASE("Config uses dotted paths for nested tables") {
    const Result<Config> result = Config::parse(k_sample);
    REQUIRE(result.has_value());

    const auto backends = result.value().get_string_list("modules.windows.rhi");
    REQUIRE(backends.has_value());
    REQUIRE(backends->size() == 2);
    CHECK((*backends)[0] == "d3d12");
    CHECK((*backends)[1] == "vulkan");

    const auto empty = result.value().get_string_list("modules.windows.physics");
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("Config returns nothing for a missing key or the wrong type") {
    const Result<Config> result = Config::parse(k_sample);
    REQUIRE(result.has_value());
    const Config& config = result.value();

    CHECK_FALSE(config.contains("missing"));
    CHECK_FALSE(config.get_string("missing").has_value());
    CHECK_FALSE(config.get_int("modules.windows").has_value());
    CHECK(config.contains("title"));
    CHECK_FALSE(config.get_int("title").has_value());
    CHECK_FALSE(config.get_string("width").has_value());
}

TEST_CASE("Config lists the values directly inside a table") {
    const Result<Config> result = Config::parse(R"(
[input]
speed = 1

[input.actions]
jump = ["key:space"]
fire = ["mouse:left"]

[input.actions.nested]
ignored = 1

[input_other]
unrelated = 1
)");
    REQUIRE(result.has_value());

    const auto names = result.value().keys_in("input.actions");
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "fire");
    CHECK(names[1] == "jump");

    CHECK(result.value().keys_in("input").size() == 1);
    CHECK(result.value().keys_in("missing").empty());
}

TEST_CASE("Config accepts empty text") {
    CHECK(Config::parse("").has_value());
}

TEST_CASE("Config reports where a syntax error is") {
    const Result<Config> result = Config::parse("valid = 1\nbroken = \n");
    REQUIRE_FALSE(result.has_value());

    CHECK(result.error().code == ErrorCode::ParseError);
    CHECK(result.error().message.find("Line 2") != std::string::npos);
}

TEST_CASE("Config rejects values it cannot represent") {
    const Result<Config> date = Config::parse("when = 2026-11-09");
    REQUIRE_FALSE(date.has_value());
    CHECK(date.error().message.find("when") != std::string::npos);

    const Result<Config> mixed = Config::parse("list = [\"a\", 1]");
    REQUIRE_FALSE(mixed.has_value());
    CHECK(mixed.error().message.find("list") != std::string::npos);
}
