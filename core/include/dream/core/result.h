#pragma once

#include "dream/core/assert.h"

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace dream::core {

enum class ErrorCode : std::uint32_t {
    Unknown,
    InvalidArgument,
    NotFound,
    OutOfMemory,
    ParseError,
};

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
};

// Holds either a value or an Error. The engine has no exceptions, so fallible functions return
// this. Reading the wrong side is a programming error and is caught by DREAM_ASSERT.
template <typename T> class [[nodiscard]] Result {
    static_assert(!std::is_same_v<T, Error>, "Result<Error> would be ambiguous");

public:
    Result(T value) : m_storage(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : m_storage(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return m_storage.index() == 0;
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        DREAM_ASSERT(has_value(), "Result holds an error");
        return *std::get_if<0>(&m_storage);
    }

    [[nodiscard]] const T& value() const& {
        DREAM_ASSERT(has_value(), "Result holds an error");
        return *std::get_if<0>(&m_storage);
    }

    [[nodiscard]] T&& value() && {
        DREAM_ASSERT(has_value(), "Result holds an error");
        return std::move(*std::get_if<0>(&m_storage));
    }

    [[nodiscard]] const Error& error() const& {
        DREAM_ASSERT(!has_value(), "Result holds a value");
        return *std::get_if<1>(&m_storage);
    }

    [[nodiscard]] T value_or(T fallback) const& {
        return has_value() ? *std::get_if<0>(&m_storage) : std::move(fallback);
    }

    [[nodiscard]] T value_or(T fallback) && {
        return has_value() ? std::move(*std::get_if<0>(&m_storage)) : std::move(fallback);
    }

private:
    std::variant<T, Error> m_storage;
};

template <> class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return !m_error.has_value();
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] const Error& error() const& {
        DREAM_ASSERT(!has_value(), "Result holds a value");
        return *m_error;
    }

private:
    std::optional<Error> m_error;
};

} // namespace dream::core
