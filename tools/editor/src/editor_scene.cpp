#include "editor_scene.h"

#include "dream/core/log.h"

#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace dream::editor {
namespace {

using runtime::SceneTable;

// A dotted key's table and last part: "clips.walk.fps" is "fps" in the table clips.walk.
std::pair<std::vector<std::string>, std::string> split_key(std::string_view key) {
    std::vector<std::string> tables;
    std::size_t start = 0;
    for (std::size_t dot = key.find('.'); dot != std::string_view::npos;
         dot = key.find('.', start)) {
        tables.emplace_back(key.substr(start, dot - start));
        start = dot + 1;
    }
    return {std::move(tables), std::string(key.substr(start))};
}

const SceneTable* find_table(const SceneTable* table, const std::vector<std::string>& path) {
    for (const std::string& part : path) {
        if (table == nullptr) {
            return nullptr;
        }
        table = table->table(part);
    }
    return table;
}

void collect_settings(const SceneTable& table, const std::string& prefix,
                      std::vector<std::string>& keys) {
    for (const auto& [key, value] : table.values()) {
        keys.push_back(prefix + key);
    }
    for (const auto& [name, child] : table.tables()) {
        collect_settings(child, prefix + name + ".", keys);
    }
}

std::filesystem::file_time_type time_of(const std::filesystem::path& file) {
    std::error_code error;
    const std::filesystem::file_time_type time = std::filesystem::last_write_time(file, error);
    return error ? std::filesystem::file_time_type{} : time;
}

} // namespace

core::Result<void> EditorScene::open(std::filesystem::path file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return core::Error{core::ErrorCode::NotFound,
                           std::format("'{}' can't be read", file.string())};
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    core::Result<runtime::SceneDocument> document = runtime::SceneDocument::parse(text);
    if (!document.has_value()) {
        return core::Error{document.error().code,
                           std::format("'{}': {}", file.string(), document.error().message)};
    }
    m_document = std::move(document).value();
    m_file = std::move(file);
    m_file_time = time_of(m_file);
    m_undo.clear();
    m_redo.clear();
    m_unsaved = false;
    if (!m_selected.empty() && m_document.entity(m_selected) == nullptr) {
        m_selected.clear();
    }
    rebuild();
    return {};
}

core::Result<void> EditorScene::save() {
    if (m_file.empty()) {
        return core::Error{core::ErrorCode::InvalidArgument, "No scene is open"};
    }
    // Written beside it first, then moved over it, so a failed write leaves the old file whole.
    const std::filesystem::path temporary = m_file.string() + ".saving";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::string text = m_document.to_toml();
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return core::Error{core::ErrorCode::IoError,
                               std::format("'{}' can't be written", temporary.string())};
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, m_file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return core::Error{core::ErrorCode::IoError,
                           std::format("'{}' can't be replaced", m_file.string())};
    }
    m_file_time = time_of(m_file);
    m_unsaved = false;
    DREAM_LOG_INFO("Saved '{}'", m_file.string());
    return {};
}

core::Result<void> EditorScene::reload() {
    return open(m_file);
}

bool EditorScene::changed_on_disk() const {
    return !m_file.empty() && time_of(m_file) != m_file_time;
}

void EditorScene::undo() {
    if (m_undo.empty()) {
        return;
    }
    m_redo.push_back(std::move(m_document));
    m_document = std::move(m_undo.back());
    m_undo.pop_back();
    // The next change starts a step of its own.
    m_last_change = {};
    m_unsaved = true;
    rebuild();
}

void EditorScene::redo() {
    if (m_redo.empty()) {
        return;
    }
    m_undo.push_back(std::move(m_document));
    m_document = std::move(m_redo.back());
    m_redo.pop_back();
    m_last_change = {};
    m_unsaved = true;
    rebuild();
}

void EditorScene::before_change() {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_last_change > std::chrono::milliseconds(500)) {
        m_undo.push_back(m_document);
        // Enough to undo a long session's slips, without keeping every scene forever.
        if (m_undo.size() > 200) {
            m_undo.erase(m_undo.begin());
        }
    }
    m_last_change = now;
    m_redo.clear();
}

void EditorScene::after_change() {
    m_unsaved = true;
    if (!m_selected.empty() && m_document.entity(m_selected) == nullptr) {
        m_selected.clear();
    }
    rebuild();
}

void EditorScene::rebuild() {
    core::Result<std::unique_ptr<runtime::World>> built = m_loader.parse(m_document.to_toml());
    if (!built.has_value()) {
        m_problem = built.error().message;
        return;
    }
    m_problem.clear();
    m_world = std::move(built).value();
    ++m_world_version;
}

runtime::Entity EditorScene::entity_in_world(std::string_view name) const {
    return m_world == nullptr || name.empty() ? runtime::k_no_entity : m_world->find(name);
}

std::vector<std::string> EditorScene::entities() const {
    return m_document.entity_names();
}

std::string EditorScene::parent(std::string_view entity) const {
    return m_document.parent(entity);
}

std::vector<std::string> EditorScene::components(std::string_view entity) const {
    std::vector<std::string> names;
    if (const SceneTable* table = m_document.entity(entity)) {
        for (const auto& [name, component] : table->tables()) {
            names.push_back(name);
        }
    }
    return names;
}

std::vector<std::string> EditorScene::settings(std::string_view entity,
                                               std::string_view component) const {
    std::vector<std::string> keys;
    const SceneTable* table = m_document.entity(entity);
    if (const SceneTable* held = table == nullptr ? nullptr : table->table(component)) {
        collect_settings(*held, "", keys);
    }
    return keys;
}

std::optional<core::ConfigValue> EditorScene::setting(std::string_view entity,
                                                      std::string_view component,
                                                      std::string_view key) const {
    const SceneTable* table = m_document.entity(entity);
    const auto [path, last] = split_key(key);
    const SceneTable* held = find_table(table == nullptr ? nullptr : table->table(component), path);
    if (held == nullptr) {
        return std::nullopt;
    }
    const core::ConfigValue* value = held->value(last);
    return value == nullptr ? std::nullopt : std::optional<core::ConfigValue>(*value);
}

void EditorScene::set_setting(std::string_view entity, std::string_view component,
                              std::string_view key, core::ConfigValue value) {
    SceneTable* table = m_document.entity(entity);
    if (table == nullptr || table->table(component) == nullptr) {
        return;
    }
    before_change();
    SceneTable* held = table->table(component);
    const auto [path, last] = split_key(key);
    for (const std::string& part : path) {
        held = &held->add_table(part);
    }
    held->set(last, std::move(value));
    after_change();
}

void EditorScene::remove_setting(std::string_view entity, std::string_view component,
                                 std::string_view key) {
    SceneTable* table = m_document.entity(entity);
    const auto [path, last] = split_key(key);
    SceneTable* held = table == nullptr ? nullptr : table->table(component);
    for (const std::string& part : path) {
        held = held == nullptr ? nullptr : held->table(part);
    }
    if (held == nullptr || held->value(last) == nullptr) {
        return;
    }
    before_change();
    held->remove(last);
    after_change();
}

void EditorScene::add_component(std::string_view entity, std::string_view component) {
    SceneTable* table = m_document.entity(entity);
    if (table == nullptr || table->table(component) != nullptr ||
        !runtime::SceneDocument::valid_name(component)) {
        return;
    }
    before_change();
    SceneTable& added = table->add_table(component);
    // The settings a component must have, such as a mesh's model, start empty, so the scene
    // shows what is missing rather than failing silently.
    if (const core::ComponentInfo* info = m_loader.component(component)) {
        for (const core::FieldInfo& field : info->fields) {
            const auto* text = std::get_if<std::string>(&field.fallback);
            if (text != nullptr && text->empty() && field.kind == core::FieldKind::Asset) {
                added.set(field.name, std::string());
            }
        }
    }
    after_change();
}

void EditorScene::remove_component(std::string_view entity, std::string_view component) {
    SceneTable* table = m_document.entity(entity);
    if (table == nullptr || table->table(component) == nullptr) {
        return;
    }
    before_change();
    table->remove_table(component);
    after_change();
}

std::string EditorScene::add_entity(std::string_view name) {
    before_change();
    std::string added = m_document.add_entity(name);
    m_selected = added;
    after_change();
    return added;
}

void EditorScene::remove_entity(std::string_view entity) {
    if (m_document.entity(entity) == nullptr) {
        return;
    }
    before_change();
    m_document.remove_entity(entity);
    after_change();
}

core::Result<void> EditorScene::rename_entity(std::string_view from, std::string_view to) {
    runtime::SceneDocument before = m_document;
    core::Result<void> renamed = m_document.rename_entity(from, to);
    if (!renamed.has_value() || from == to) {
        return renamed;
    }
    std::swap(before, m_document);
    before_change();
    m_document = std::move(before);
    if (m_selected == from) {
        m_selected = to;
    }
    after_change();
    return {};
}

core::Result<void> EditorScene::set_parent(std::string_view entity, std::string_view parent) {
    runtime::SceneDocument before = m_document;
    core::Result<void> set = m_document.set_parent(entity, parent);
    if (!set.has_value()) {
        return set;
    }
    std::swap(before, m_document);
    before_change();
    m_document = std::move(before);
    after_change();
    return {};
}

std::vector<core::ComponentInfo> EditorScene::component_types() const {
    return m_loader.components();
}

const core::ComponentInfo* EditorScene::component_type(std::string_view component) const {
    return m_loader.component(component);
}

void EditorScene::select(std::string_view entity) {
    m_selected = m_document.entity(entity) == nullptr ? std::string() : std::string(entity);
}

} // namespace dream::editor
