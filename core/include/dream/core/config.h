#pragma once

#include "dream/core/result.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dream::core {

using ConfigValue = std::variant<bool, std::int64_t, double, std::string, std::vector<std::string>>;

// Values read from TOML text. Keys use dotted paths, so `rhi` under `[modules.windows]` is
// "modules.windows.rhi". Supported values are booleans, integers, floats, strings and arrays of
// strings; anything else is a parse error.
//
// Getters return nothing when the key is missing or holds a different type.
class Config {
public:
    [[nodiscard]] static Result<Config> parse(std::string_view text);

    [[nodiscard]] bool contains(std::string_view key) const;

    // The names of the values directly inside a table, sorted. Nested tables are not included. The
    // returned views are valid for as long as this Config exists.
    [[nodiscard]] std::vector<std::string_view> keys_in(std::string_view table) const;

    [[nodiscard]] std::optional<bool> get_bool(std::string_view key) const;
    [[nodiscard]] std::optional<std::int64_t> get_int(std::string_view key) const;

    // Also accepts an integer value, so `speed = 2` can be read as a float.
    [[nodiscard]] std::optional<double> get_float(std::string_view key) const;

    // The returned views are valid for as long as this Config exists.
    [[nodiscard]] std::optional<std::string_view> get_string(std::string_view key) const;
    [[nodiscard]] std::optional<std::span<const std::string>>
    get_string_list(std::string_view key) const;

private:
    [[nodiscard]] const ConfigValue* find(std::string_view key) const;

    std::map<std::string, ConfigValue, std::less<>> m_values;
};

} // namespace dream::core
