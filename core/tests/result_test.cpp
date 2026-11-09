#include "dream/core/result.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>

using namespace dream::core;

namespace {

Result<int> parse_positive(int input) {
    if (input <= 0) {
        return Error{ErrorCode::InvalidArgument, "must be positive"};
    }
    return input;
}

Result<void> check_positive(int input) {
    if (input <= 0) {
        return Error{ErrorCode::InvalidArgument, "must be positive"};
    }
    return {};
}

} // namespace

TEST_CASE("Result holds a value") {
    const Result<int> result = parse_positive(5);
    REQUIRE(result.has_value());
    CHECK(result.value() == 5);
    CHECK(result.value_or(9) == 5);
}

TEST_CASE("Result holds an error") {
    const Result<int> result = parse_positive(-1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
    CHECK(result.error().message == "must be positive");
    CHECK(result.value_or(9) == 9);
}

TEST_CASE("Result supports move-only values") {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(7);
    REQUIRE(result.has_value());

    const std::unique_ptr<int> taken = std::move(result).value();
    CHECK(*taken == 7);
}

TEST_CASE("Result<void> reports success or an error") {
    CHECK(check_positive(1).has_value());

    const Result<void> failed = check_positive(0);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == ErrorCode::InvalidArgument);
}
