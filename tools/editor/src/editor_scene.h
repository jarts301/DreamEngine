#pragma once

#include "dream/editor_panel/editor_panel.h"
#include "dream/runtime/scene_document.h"
#include "dream/runtime/scene_loader.h"
#include "dream/runtime/world.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dream::editor {

// The scene being edited (ADR 0046): its file's text as a SceneDocument, which panels change
// through SceneModel, and the world built from it for the scene view. Each change builds the world
// again; one the scene loader refuses keeps the change, shows the problem, and keeps the last world
// that built, so the next change can fix it.
class EditorScene final : public editor_panel::SceneModel {
public:
    explicit EditorScene(runtime::SceneLoader& loader) : m_loader(loader) {}

    // Reads the scene file on disk. Fails when it can't be read or isn't TOML.
    [[nodiscard]] core::Result<void> open(std::filesystem::path file);
    // Writes it back, and remembers that it is saved.
    [[nodiscard]] core::Result<void> save();
    // Reads it again from disk, as when another program changed it, dropping unsaved changes.
    [[nodiscard]] core::Result<void> reload();

    [[nodiscard]] const std::filesystem::path& file() const noexcept {
        return m_file;
    }
    [[nodiscard]] bool is_open() const noexcept {
        return !m_file.empty();
    }
    [[nodiscard]] bool unsaved() const noexcept {
        return m_unsaved;
    }
    // The scene as it is now, as save would write it.
    [[nodiscard]] std::string text() const {
        return m_document.to_toml();
    }
    // Whether the file on disk changed since it was opened or saved here.
    [[nodiscard]] bool changed_on_disk() const;

    // Changes in a row, such as dragging a value, undo as one.
    void undo();
    void redo();
    [[nodiscard]] bool can_undo() const noexcept {
        return !m_undo.empty();
    }
    [[nodiscard]] bool can_redo() const noexcept {
        return !m_redo.empty();
    }

    // Null until a scene has built.
    [[nodiscard]] runtime::World* world() noexcept {
        return m_world.get();
    }
    // Rises each time the world is built again, so the view can put its camera back in.
    [[nodiscard]] std::uint64_t world_version() const noexcept {
        return m_world_version;
    }
    // The entity in the world that has the scene's entity's name.
    [[nodiscard]] runtime::Entity entity_in_world(std::string_view name) const;

    // SceneModel.
    std::vector<std::string> entities() const override;
    std::string parent(std::string_view entity) const override;
    std::vector<std::string> components(std::string_view entity) const override;
    std::vector<std::string> settings(std::string_view entity,
                                      std::string_view component) const override;
    std::optional<core::ConfigValue> setting(std::string_view entity, std::string_view component,
                                             std::string_view key) const override;
    void set_setting(std::string_view entity, std::string_view component, std::string_view key,
                     core::ConfigValue value) override;
    void remove_setting(std::string_view entity, std::string_view component,
                        std::string_view key) override;
    void add_component(std::string_view entity, std::string_view component) override;
    void remove_component(std::string_view entity, std::string_view component) override;
    std::string add_entity(std::string_view name) override;
    void remove_entity(std::string_view entity) override;
    core::Result<void> rename_entity(std::string_view from, std::string_view to) override;
    core::Result<void> set_parent(std::string_view entity, std::string_view parent) override;
    std::vector<core::ComponentInfo> component_types() const override;
    const core::ComponentInfo* component_type(std::string_view component) const override;
    std::string selected() const override {
        return m_selected;
    }
    void select(std::string_view entity) override;
    std::string problem() const override {
        return m_problem;
    }

private:
    // Before a change: remembers the document to undo to, unless the last change was a moment
    // ago, so a drag undoes as one.
    void before_change();
    // After a change: builds the world again.
    void after_change();
    void rebuild();

    runtime::SceneLoader& m_loader;
    std::filesystem::path m_file;
    std::filesystem::file_time_type m_file_time{};
    runtime::SceneDocument m_document;
    std::vector<runtime::SceneDocument> m_undo;
    std::vector<runtime::SceneDocument> m_redo;
    std::chrono::steady_clock::time_point m_last_change{};
    std::unique_ptr<runtime::World> m_world;
    std::uint64_t m_world_version = 0;
    std::string m_selected;
    std::string m_problem;
    bool m_unsaved = false;
};

} // namespace dream::editor
