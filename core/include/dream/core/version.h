#pragma once

#include <compare>
#include <cstdint>

namespace dream::core {

// The fields avoid the names `major` and `minor`, which Android's system headers can define as
// macros.
struct Version {
    std::uint16_t major_number = 0;
    std::uint16_t minor_number = 0;
    std::uint16_t patch_number = 0;

    constexpr auto operator<=>(const Version&) const = default;
};

[[nodiscard]] Version engine_version() noexcept;

} // namespace dream::core
