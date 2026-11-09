#include "dream/core/config.h"

#include <algorithm>
#include <format>
#include <utility>

// vcpkg's compiled toml++ library is built with exceptions, so it cannot be linked into this
// no-exceptions build. Header-only mode compiles the parser here with the engine's own flags.
#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 0
#define TOML_ENABLE_FORMATTERS 0
#define TOML_ENABLE_WINDOWS_COMPAT 0
#include <toml++/toml.hpp>

namespace dream::core {
namespace {

using ValueMap = std::map<std::string, ConfigValue, std::less<>>;
using TableSet = std::set<std::string, std::less<>>;

Error unsupported_value(std::string_view key, std::string_view reason) {
    return Error{ErrorCode::ParseError, std::format("Key '{}': {}", key, reason)};
}

Result<void> flatten(const toml::table& table, std::string_view prefix, ValueMap& out,
                     TableSet& tables) {
    for (auto&& [name, node] : table) {
        std::string key = prefix.empty() ? std::string(name.str())
                                         : std::format("{}.{}", prefix, name.str());

        if (const toml::table* const child = node.as_table()) {
            tables.insert(key);
            Result<void> nested = flatten(*child, key, out, tables);
            if (!nested.has_value()) {
                return nested;
            }
        } else if (const auto* const boolean = node.as_boolean()) {
            out.insert_or_assign(std::move(key), ConfigValue{boolean->get()});
        } else if (const auto* const integer = node.as_integer()) {
            out.insert_or_assign(std::move(key), ConfigValue{integer->get()});
        } else if (const auto* const floating = node.as_floating_point()) {
            out.insert_or_assign(std::move(key), ConfigValue{floating->get()});
        } else if (const auto* const text = node.as_string()) {
            out.insert_or_assign(std::move(key), ConfigValue{text->get()});
        } else if (const toml::array* const array = node.as_array()) {
            const bool all_numbers =
                !array->empty() &&
                std::all_of(array->begin(), array->end(), [](const toml::node& element) {
                    return element.is_integer() || element.is_floating_point();
                });

            if (all_numbers) {
                std::vector<double> numbers;
                numbers.reserve(array->size());
                for (const toml::node& element : *array) {
                    numbers.push_back(element.value_or(0.0));
                }
                out.insert_or_assign(std::move(key), ConfigValue{std::move(numbers)});
                continue;
            }

            std::vector<std::string> items;
            items.reserve(array->size());
            for (const toml::node& element : *array) {
                const auto* const item = element.as_string();
                if (item == nullptr) {
                    return unsupported_value(key, "an array must hold only strings or only numbers");
                }
                items.push_back(item->get());
            }
            out.insert_or_assign(std::move(key), ConfigValue{std::move(items)});
        } else {
            return unsupported_value(key, "this value type is not supported");
        }
    }
    return {};
}

} // namespace

Result<Config> Config::parse(std::string_view text) {
    toml::parse_result parsed = toml::parse(text);
    if (!parsed) {
        const toml::parse_error& error = parsed.error();
        return Error{ErrorCode::ParseError,
                     std::format("Line {}, column {}: {}", error.source().begin.line,
                                 error.source().begin.column, error.description())};
    }

    Config config;
    Result<void> flattened = flatten(parsed.table(), {}, config.m_values, config.m_tables);
    if (!flattened.has_value()) {
        return flattened.error();
    }
    return config;
}

const ConfigValue* Config::find(std::string_view key) const {
    const auto entry = m_values.find(key);
    return entry == m_values.end() ? nullptr : &entry->second;
}

bool Config::contains(std::string_view key) const {
    return find(key) != nullptr;
}

std::vector<std::string_view> Config::keys_in(std::string_view table) const {
    const std::string prefix = std::format("{}.", table);

    std::vector<std::string_view> names;
    for (auto entry = m_values.lower_bound(prefix);
         entry != m_values.end() && entry->first.starts_with(prefix); ++entry) {
        const std::string_view name = std::string_view(entry->first).substr(prefix.size());
        if (name.find('.') == std::string_view::npos) {
            names.push_back(name);
        }
    }
    return names;
}

std::vector<std::string_view> Config::tables_in(std::string_view table) const {
    const std::string prefix = table.empty() ? std::string() : std::format("{}.", table);

    std::vector<std::string_view> names;
    for (auto entry = m_tables.lower_bound(prefix);
         entry != m_tables.end() && entry->starts_with(prefix); ++entry) {
        const std::string_view name = std::string_view(*entry).substr(prefix.size());
        if (name.find('.') == std::string_view::npos) {
            names.push_back(name);
        }
    }
    return names;
}

std::optional<bool> Config::get_bool(std::string_view key) const {
    const ConfigValue* const value = find(key);
    const bool* const result = std::get_if<bool>(value);
    return result == nullptr ? std::nullopt : std::optional<bool>(*result);
}

std::optional<std::int64_t> Config::get_int(std::string_view key) const {
    const ConfigValue* const value = find(key);
    const std::int64_t* const result = std::get_if<std::int64_t>(value);
    return result == nullptr ? std::nullopt : std::optional<std::int64_t>(*result);
}

std::optional<double> Config::get_float(std::string_view key) const {
    const ConfigValue* const value = find(key);
    if (const double* const result = std::get_if<double>(value)) {
        return *result;
    }
    if (const std::int64_t* const result = std::get_if<std::int64_t>(value)) {
        return static_cast<double>(*result);
    }
    return std::nullopt;
}

std::optional<std::string_view> Config::get_string(std::string_view key) const {
    const ConfigValue* const value = find(key);
    const std::string* const result = std::get_if<std::string>(value);
    return result == nullptr ? std::nullopt : std::optional<std::string_view>(*result);
}

std::optional<std::span<const std::string>> Config::get_string_list(std::string_view key) const {
    const ConfigValue* const value = find(key);
    const auto* const result = std::get_if<std::vector<std::string>>(value);
    return result == nullptr ? std::nullopt : std::optional<std::span<const std::string>>(*result);
}

std::optional<std::span<const double>> Config::get_float_list(std::string_view key) const {
    const ConfigValue* const value = find(key);
    if (const auto* const numbers = std::get_if<std::vector<double>>(value)) {
        return std::span<const double>(*numbers);
    }
    // TOML's `[]` has no element type, so an empty array is stored as an empty list of strings.
    const auto* const strings = std::get_if<std::vector<std::string>>(value);
    if (strings != nullptr && strings->empty()) {
        return std::span<const double>();
    }
    return std::nullopt;
}

} // namespace dream::core
