#include "dream/runtime/scene_document.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <variant>

namespace dream::runtime {
namespace {

constexpr std::string_view k_entities = "entities";

core::Error document_error(std::string message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::move(message)};
}

std::string_view trim(std::string_view text) {
    const std::size_t start = text.find_first_not_of(" \t\r");
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r");
    return text.substr(start, end - start + 1);
}

// The dotted parts of a table header or key, with quotes and the spaces around dots removed.
std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> parts;
    std::string part;
    char quote = 0;
    for (const char c : path) {
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else {
                part += c;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '.') {
            parts.emplace_back(trim(part));
            part.clear();
        } else {
            part += c;
        }
    }
    parts.emplace_back(trim(part));
    return parts;
}

std::string join(const std::vector<std::string>& parts, std::size_t count) {
    std::string joined;
    for (std::size_t index = 0; index < count; ++index) {
        if (index > 0) {
            joined += '.';
        }
        joined += parts[index];
    }
    return joined;
}

// Where each table and setting first appears in the text, by dotted path, for putting them back
// in the file's order: TOML readers keep them sorted.
std::map<std::string, std::size_t, std::less<>> appearance_order(std::string_view text) {
    std::map<std::string, std::size_t, std::less<>> order;
    std::size_t next = 0;
    const auto note = [&](const std::vector<std::string>& parts) {
        for (std::size_t count = 1; count <= parts.size(); ++count) {
            order.try_emplace(join(parts, count), next++);
        }
    };
    std::vector<std::string> table;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = trim(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            const std::size_t close = line.find(']');
            if (close != std::string_view::npos && !line.starts_with("[[")) {
                table = split_path(line.substr(1, close - 1));
                note(table);
            }
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        std::vector<std::string> key = table;
        for (std::string& part : split_path(trim(line.substr(0, equals)))) {
            key.push_back(std::move(part));
        }
        note(key);
    }
    return order;
}

std::size_t order_of(const std::map<std::string, std::size_t, std::less<>>& order,
                     std::string_view path) {
    const auto found = order.find(path);
    return found == order.end() ? std::numeric_limits<std::size_t>::max() : found->second;
}

std::string child_path(std::string_view table, std::string_view name) {
    return table.empty() ? std::string(name) : std::format("{}.{}", table, name);
}

SceneTable read_table(const core::Config& config, std::string_view path,
                      const std::map<std::string, std::size_t, std::less<>>& order) {
    std::vector<std::string_view> keys = config.keys_in(path);
    std::vector<std::string_view> tables = config.tables_in(path);
    const auto by_order = [&](std::string_view a, std::string_view b) {
        return order_of(order, child_path(path, a)) < order_of(order, child_path(path, b));
    };
    std::ranges::stable_sort(keys, by_order);
    std::ranges::stable_sort(tables, by_order);

    SceneTable table;
    for (const std::string_view key : keys) {
        if (const core::ConfigValue* value = config.value(child_path(path, key))) {
            table.set(key, *value);
        }
    }
    for (const std::string_view name : tables) {
        table.add_table(name) = read_table(config, child_path(path, name), order);
    }
    return table;
}

bool is_bare_key(std::string_view key) {
    return !key.empty() && std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

std::string quoted(std::string_view text) {
    std::string result = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                result += std::format("\\u{:04X}", static_cast<unsigned>(c));
            } else {
                result += c;
            }
        }
    }
    result += '"';
    return result;
}

std::string key_text(std::string_view key) {
    return is_bare_key(key) ? std::string(key) : quoted(key);
}

// The shortest text that reads back as the same double, always with a point or an exponent, so
// it stays a float.
std::string float_text(double value) {
    if (std::isnan(value)) {
        return "nan";
    }
    if (std::isinf(value)) {
        return value > 0.0 ? "inf" : "-inf";
    }
    char buffer[64];
    const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string text(buffer, error == std::errc{} ? end : buffer);
    if (text.find_first_of(".eEn") == std::string::npos) {
        text += ".0";
    }
    return text;
}

void write_table(std::string& out, const SceneTable& table, const std::string& path,
                 bool top_level) {
    // A header for a table with settings, or none and no tables, so an empty table survives.
    if (!top_level && (!table.values().empty() || table.tables().empty())) {
        if (!out.empty() && !out.ends_with("\n\n")) {
            out += '\n';
        }
        out += std::format("[{}]\n", path);
    }
    for (const auto& [key, value] : table.values()) {
        out += std::format("{} = {}\n", key_text(key), toml_value(value));
    }
    for (const auto& [name, child] : table.tables()) {
        const std::string child_name = key_text(name);
        write_table(out, child, path.empty() ? child_name : std::format("{}.{}", path, child_name),
                    false);
    }
}

} // namespace

std::string toml_value(const core::ConfigValue& value) {
    return std::visit(
        [](const auto& held) -> std::string {
            using Type = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<Type, bool>) {
                return held ? "true" : "false";
            } else if constexpr (std::is_same_v<Type, std::int64_t>) {
                return std::to_string(held);
            } else if constexpr (std::is_same_v<Type, double>) {
                return float_text(held);
            } else if constexpr (std::is_same_v<Type, std::string>) {
                return quoted(held);
            } else {
                std::string text = "[";
                for (std::size_t index = 0; index < held.size(); ++index) {
                    if (index > 0) {
                        text += ", ";
                    }
                    if constexpr (std::is_same_v<Type, std::vector<std::string>>) {
                        text += quoted(held[index]);
                    } else {
                        text += float_text(held[index]);
                    }
                }
                return text + "]";
            }
        },
        value);
}

const core::ConfigValue* SceneTable::value(std::string_view key) const {
    const auto found = std::ranges::find(m_values, key, &Value::first);
    return found == m_values.end() ? nullptr : &found->second;
}

void SceneTable::set(std::string_view key, core::ConfigValue value) {
    const auto found = std::ranges::find(m_values, key, &Value::first);
    if (found != m_values.end()) {
        found->second = std::move(value);
    } else {
        m_values.emplace_back(std::string(key), std::move(value));
    }
}

bool SceneTable::remove(std::string_view key) {
    return std::erase_if(m_values, [&](const Value& value) { return value.first == key; }) > 0;
}

SceneTable* SceneTable::table(std::string_view name) {
    const auto found = std::ranges::find(m_tables, name, &Table::first);
    return found == m_tables.end() ? nullptr : &found->second;
}

const SceneTable* SceneTable::table(std::string_view name) const {
    const auto found = std::ranges::find(m_tables, name, &Table::first);
    return found == m_tables.end() ? nullptr : &found->second;
}

SceneTable& SceneTable::add_table(std::string_view name) {
    if (SceneTable* existing = table(name)) {
        return *existing;
    }
    return m_tables.emplace_back(std::string(name), SceneTable{}).second;
}

bool SceneTable::remove_table(std::string_view name) {
    return std::erase_if(m_tables, [&](const Table& table) { return table.first == name; }) > 0;
}

bool SceneTable::rename_table(std::string_view from, std::string_view to) {
    if (table(to) != nullptr) {
        return false;
    }
    const auto found = std::ranges::find(m_tables, from, &Table::first);
    if (found == m_tables.end()) {
        return false;
    }
    found->first = to;
    return true;
}

core::Result<SceneDocument> SceneDocument::parse(std::string_view text) {
    const core::Result<core::Config> config = core::Config::parse(text);
    if (!config.has_value()) {
        return config.error();
    }
    SceneDocument document;
    // The comments and blank lines before anything else, such as the scene's description.
    std::string_view rest = text;
    while (!rest.empty()) {
        const std::size_t end = rest.find('\n');
        const std::string_view line = rest.substr(0, end);
        const std::string_view trimmed = trim(line);
        if (!trimmed.empty() && trimmed.front() != '#') {
            break;
        }
        document.m_header += trim(line).empty() ? std::string_view{} : trimmed;
        document.m_header += '\n';
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
    }
    while (document.m_header.ends_with("\n\n")) {
        document.m_header.pop_back();
    }
    if (document.m_header == "\n") {
        document.m_header.clear();
    }
    document.m_root = read_table(config.value(), "", appearance_order(text));
    return document;
}

std::string SceneDocument::to_toml() const {
    std::string out;
    write_table(out, m_root, "", true);
    if (m_header.empty()) {
        return out;
    }
    return m_header + (out.empty() ? "" : "\n") + out;
}

SceneTable& SceneDocument::entities() {
    return m_root.add_table(k_entities);
}

std::vector<std::string> SceneDocument::entity_names() const {
    std::vector<std::string> names;
    if (const SceneTable* all = m_root.table(k_entities)) {
        for (const auto& [name, table] : all->tables()) {
            names.push_back(name);
        }
    }
    return names;
}

SceneTable* SceneDocument::entity(std::string_view name) {
    SceneTable* all = m_root.table(k_entities);
    return all == nullptr ? nullptr : all->table(name);
}

const SceneTable* SceneDocument::entity(std::string_view name) const {
    const SceneTable* all = m_root.table(k_entities);
    return all == nullptr ? nullptr : all->table(name);
}

bool SceneDocument::valid_name(std::string_view name) noexcept {
    return is_bare_key(name);
}

std::string SceneDocument::add_entity(std::string_view name) {
    std::string base = valid_name(name) ? std::string(name) : std::string("entity");
    std::string chosen = base;
    for (int number = 2; entity(chosen) != nullptr; ++number) {
        chosen = std::format("{}_{}", base, number);
    }
    entities().add_table(chosen);
    return chosen;
}

std::string SceneDocument::parent(std::string_view name) const {
    const SceneTable* table = entity(name);
    if (table == nullptr) {
        return {};
    }
    const core::ConfigValue* value = table->value("parent");
    const std::string* text = value == nullptr ? nullptr : std::get_if<std::string>(value);
    return text == nullptr ? std::string() : *text;
}

bool SceneDocument::remove_entity(std::string_view name) {
    if (entity(name) == nullptr) {
        return false;
    }
    const std::string grandparent = parent(name);
    for (const std::string& other : entity_names()) {
        if (other != name && parent(other) == name) {
            SceneTable* child = entity(other);
            if (grandparent.empty()) {
                child->remove("parent");
            } else {
                child->set("parent", grandparent);
            }
        }
    }
    return entities().remove_table(name);
}

core::Result<void> SceneDocument::rename_entity(std::string_view from, std::string_view to) {
    if (entity(from) == nullptr) {
        return document_error(std::format("There is no entity '{}'", from));
    }
    if (from == to) {
        return {};
    }
    if (!valid_name(to)) {
        return document_error(std::format(
            "'{}' can't be a name: use letters, digits, underscores and dashes", to));
    }
    if (entity(to) != nullptr) {
        return document_error(std::format("There is already an entity '{}'", to));
    }
    for (const std::string& other : entity_names()) {
        if (parent(other) == from) {
            entity(other)->set("parent", std::string(to));
        }
    }
    entities().rename_table(from, to);
    return {};
}

core::Result<void> SceneDocument::set_parent(std::string_view name, std::string_view parent_name) {
    SceneTable* table = entity(name);
    if (table == nullptr) {
        return document_error(std::format("There is no entity '{}'", name));
    }
    if (parent_name.empty()) {
        table->remove("parent");
        return {};
    }
    if (entity(parent_name) == nullptr) {
        return document_error(std::format("There is no entity '{}'", parent_name));
    }
    std::set<std::string> seen;
    for (std::string above(parent_name); !above.empty(); above = parent(above)) {
        if (above == name || !seen.insert(above).second) {
            return document_error(
                std::format("'{}' can't be inside '{}', which is inside it", name, parent_name));
        }
    }
    table->set("parent", std::string(parent_name));
    return {};
}

} // namespace dream::runtime
