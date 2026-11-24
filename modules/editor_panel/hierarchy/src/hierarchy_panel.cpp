// The editor's entity tree (ADR 0046): every entity of the scene under its parent, to select, add
// and remove.

#include "dream/core/module_registry.h"
#include "dream/editor_panel/editor_panel.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dream::modules {
namespace {

using namespace editor_panel;

class HierarchyPanel final : public EditorPanel {
public:
    std::string_view title() const override {
        return "Entities";
    }

    void draw(debug_ui::DebugUi& ui, SceneModel& scene) override {
        if (ui.button("Add##entity")) {
            const std::string parent = scene.selected();
            const std::string added = scene.add_entity("entity");
            // Inside the selected entity, as its child.
            if (!parent.empty()) {
                static_cast<void>(scene.set_parent(added, parent));
            }
            scene.add_component(added, "transform");
        }
        ui.tooltip("Adds an entity inside the selected one, or at the top");
        const std::string selected = scene.selected();
        if (!selected.empty()) {
            ui.same_line();
            if (ui.button("Remove##entity")) {
                scene.remove_entity(selected);
            }
            ui.tooltip("Removes the selected entity; its children move to its parent");
        }
        ui.separator();

        const std::vector<std::string> entities = scene.entities();
        if (entities.empty()) {
            ui.text_disabled("No entities yet");
            return;
        }
        // An entity whose parent is missing shows at the top, as the scene would fail to load it.
        const std::set<std::string> names(entities.begin(), entities.end());
        std::vector<std::string> roots;
        for (const std::string& entity : entities) {
            const std::string parent = scene.parent(entity);
            if (parent.empty() || !names.contains(parent)) {
                roots.push_back(entity);
            }
        }
        // Any not under a root are in a loop of parents, which the scene can't load either: they
        // show at the top too, so they can be fixed.
        std::set<std::string> reachable(roots.begin(), roots.end());
        for (bool grew = true; grew;) {
            grew = false;
            for (const std::string& entity : entities) {
                if (!reachable.contains(entity) && reachable.contains(scene.parent(entity))) {
                    reachable.insert(entity);
                    grew = true;
                }
            }
        }
        for (const std::string& entity : entities) {
            if (!reachable.contains(entity)) {
                roots.push_back(entity);
            }
        }
        std::set<std::string> drawn;
        for (const std::string& root : roots) {
            draw_node(ui, scene, entities, root, drawn);
        }
    }

private:
    void draw_node(debug_ui::DebugUi& ui, SceneModel& scene,
                   const std::vector<std::string>& entities, const std::string& entity,
                   std::set<std::string>& drawn) {
        if (!drawn.insert(entity).second) {
            return;
        }
        std::vector<std::string> children;
        for (const std::string& other : entities) {
            if (other != entity && scene.parent(other) == entity) {
                children.push_back(other);
            }
        }
        bool clicked = false;
        const bool open = ui.tree_node(entity, children.empty(), entity == scene.selected(),
                                       clicked);
        if (clicked) {
            scene.select(entity);
        }
        if (open) {
            for (const std::string& child : children) {
                draw_node(ui, scene, entities, child, drawn);
            }
            ui.tree_pop();
        }
    }
};

core::Result<std::unique_ptr<EditorPanel>> create(const core::Config& /*config*/) {
    return std::unique_ptr<EditorPanel>{std::make_unique<HierarchyPanel>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_editor_panel_hierarchy(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<EditorPanel>{.name = "hierarchy", .create = &create});
}

} // namespace dream::modules
