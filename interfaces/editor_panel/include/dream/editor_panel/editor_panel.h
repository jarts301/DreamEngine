#pragma once

#include "dream/core/config.h"
#include "dream/core/fields.h"
#include "dream/core/result.h"
#include "dream/debug_ui/debug_ui.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dream::editor_panel {

// The editor's panels (ADR 0046), such as its entity tree and its inspector, as modules a project
// lists, all of them, in order:
//
//     [modules.windows]
//     editor_panel = ["hierarchy", "inspector"]
//
// Each draws one window with the debug interface's widgets, and sees and changes the scene being
// edited through SceneModel, by the names its scene file gives entities. A project can add panels
// of its own the same way.

// The scene being edited, as a panel sees it. Every change is made to the scene file's text, which
// the editor saves, and the world in the scene view is built again from it.
class SceneModel {
public:
    virtual ~SceneModel() = default;

    // The entities, in the scene file's order.
    [[nodiscard]] virtual std::vector<std::string> entities() const = 0;
    // Empty for none.
    [[nodiscard]] virtual std::string parent(std::string_view entity) const = 0;
    [[nodiscard]] virtual std::vector<std::string> components(std::string_view entity) const = 0;
    // The component's settings, in the file's order. Those in tables inside it have dotted
    // names, such as an animator's "clips.walk.fps".
    [[nodiscard]] virtual std::vector<std::string> settings(std::string_view entity,
                                                            std::string_view component) const = 0;
    [[nodiscard]] virtual std::optional<core::ConfigValue>
    setting(std::string_view entity, std::string_view component, std::string_view key) const = 0;

    virtual void set_setting(std::string_view entity, std::string_view component,
                             std::string_view key, core::ConfigValue value) = 0;
    virtual void remove_setting(std::string_view entity, std::string_view component,
                                std::string_view key) = 0;
    virtual void add_component(std::string_view entity, std::string_view component) = 0;
    virtual void remove_component(std::string_view entity, std::string_view component) = 0;

    // Adds an entity named `name`, or `name` and a number when that is taken, and selects it.
    virtual std::string add_entity(std::string_view name) = 0;
    // Its children move to its parent.
    virtual void remove_entity(std::string_view entity) = 0;
    [[nodiscard]] virtual core::Result<void> rename_entity(std::string_view from,
                                                           std::string_view to) = 0;
    // An empty `parent` removes it.
    [[nodiscard]] virtual core::Result<void> set_parent(std::string_view entity,
                                                        std::string_view parent) = 0;

    // The components the program registered, with the fields each described.
    [[nodiscard]] virtual std::vector<core::ComponentInfo> component_types() const = 0;
    // Null for a component the program doesn't know, such as a game's own.
    [[nodiscard]] virtual const core::ComponentInfo*
    component_type(std::string_view component) const = 0;

    // Empty when nothing is selected.
    [[nodiscard]] virtual std::string selected() const = 0;
    virtual void select(std::string_view entity) = 0;

    // Why the world couldn't be built from the scene as it is, or empty. The scene keeps the
    // change, so the next one can fix it.
    [[nodiscard]] virtual std::string problem() const = 0;
};

class EditorPanel {
public:
    static constexpr std::string_view k_module_interface = "editor_panel";

    enum class Side { Left, Right };

    virtual ~EditorPanel() = default;

    // The window's title.
    [[nodiscard]] virtual std::string_view title() const = 0;
    // Where the editor places the window the first time it opens.
    [[nodiscard]] virtual Side side() const {
        return Side::Left;
    }

    // Draws the window's contents: the editor opens and closes the window around it.
    virtual void draw(debug_ui::DebugUi& ui, SceneModel& scene) = 0;
};

} // namespace dream::editor_panel
