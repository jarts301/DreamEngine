#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::runtime {

// One table of a scene file: its settings and the tables inside it, in the file's order.
class SceneTable {
public:
    using Value = std::pair<std::string, core::ConfigValue>;
    using Table = std::pair<std::string, SceneTable>;

    [[nodiscard]] const std::vector<Value>& values() const noexcept {
        return m_values;
    }
    [[nodiscard]] const std::vector<Table>& tables() const noexcept {
        return m_tables;
    }

    // Null when there is no such setting.
    [[nodiscard]] const core::ConfigValue* value(std::string_view key) const;
    // Replaces the setting in place, or adds it at the end.
    void set(std::string_view key, core::ConfigValue value);
    // Whether there was one to remove.
    bool remove(std::string_view key);

    // Null when there is no such table.
    [[nodiscard]] SceneTable* table(std::string_view name);
    [[nodiscard]] const SceneTable* table(std::string_view name) const;
    // The table, added at the end when there is none.
    SceneTable& add_table(std::string_view name);
    bool remove_table(std::string_view name);
    // Whether there was one by `from`, and none by `to`, to rename. Its place stays.
    bool rename_table(std::string_view from, std::string_view to);

    friend bool operator==(const SceneTable&, const SceneTable&) = default;

private:
    std::vector<Value> m_values;
    std::vector<Table> m_tables;
};

// A scene file, to change and write back (ADR 0046): what the editor edits and saves. Its
// entities are the [entities.<name>] tables, each with a `parent` setting or none, and a table per
// component; other top-level tables are kept as they are.
//
// Saving keeps the order of tables and settings, and the comment lines at the top of the file, but
// not comments elsewhere, which TOML readers drop. Values keep their types: 2 stays an integer, and
// 2.0 a float.
class SceneDocument {
public:
    [[nodiscard]] static core::Result<SceneDocument> parse(std::string_view text);

    // The scene as TOML text, which parse reads back to the same document.
    [[nodiscard]] std::string to_toml() const;

    [[nodiscard]] std::vector<std::string> entity_names() const;
    [[nodiscard]] SceneTable* entity(std::string_view name);
    [[nodiscard]] const SceneTable* entity(std::string_view name) const;

    // Adds an entity with no components at the end, named `name`, or `name` and a number when that
    // is taken. Returns the name it got.
    std::string add_entity(std::string_view name);
    // Also removes it as a parent: its children move to its own parent.
    bool remove_entity(std::string_view name);
    // Also renames it where it is a parent. Fails when `to` is taken or isn't a valid name.
    [[nodiscard]] core::Result<void> rename_entity(std::string_view from, std::string_view to);
    // An empty `parent` removes it. Fails when that would make an entity its own ancestor.
    [[nodiscard]] core::Result<void> set_parent(std::string_view entity, std::string_view parent);
    [[nodiscard]] std::string parent(std::string_view entity) const;

    // Letters, digits, underscores and dashes, as a TOML table name needs without quotes.
    [[nodiscard]] static bool valid_name(std::string_view name) noexcept;

    [[nodiscard]] const SceneTable& root() const noexcept {
        return m_root;
    }

    friend bool operator==(const SceneDocument&, const SceneDocument&) = default;

private:
    [[nodiscard]] SceneTable& entities();

    std::string m_header;
    SceneTable m_root;
};

// A setting's value as TOML writes it, such as `[1.0, 2.0, 3.0]` or `"text"`.
[[nodiscard]] std::string toml_value(const core::ConfigValue& value);

} // namespace dream::runtime
