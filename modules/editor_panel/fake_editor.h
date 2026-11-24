#pragma once

// Fakes for the editor panels' tests: a scene kept in memory, and a debug interface that records
// what a panel shows and plays back what a person would do to it, by the widgets' labels.

#include "dream/editor_panel/editor_panel.h"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dream::editor_panel::tests {

class FakeScene final : public SceneModel {
public:
    struct Component {
        std::string name;
        std::vector<std::pair<std::string, core::ConfigValue>> settings;
    };
    struct Entity {
        std::string name;
        std::string parent;
        std::vector<Component> components;
    };

    std::vector<Entity> scene;
    std::vector<core::ComponentInfo> types;
    std::string selection;

    std::vector<std::string> entities() const override {
        std::vector<std::string> names;
        for (const Entity& entity : scene) {
            names.push_back(entity.name);
        }
        return names;
    }
    std::string parent(std::string_view entity) const override {
        const Entity* found = find(entity);
        return found == nullptr ? std::string() : found->parent;
    }
    std::vector<std::string> components(std::string_view entity) const override {
        std::vector<std::string> names;
        if (const Entity* found = find(entity)) {
            for (const Component& component : found->components) {
                names.push_back(component.name);
            }
        }
        return names;
    }
    std::vector<std::string> settings(std::string_view entity,
                                      std::string_view component) const override {
        std::vector<std::string> keys;
        if (const Component* found = find(entity, component)) {
            for (const auto& [key, value] : found->settings) {
                keys.push_back(key);
            }
        }
        return keys;
    }
    std::optional<core::ConfigValue> setting(std::string_view entity, std::string_view component,
                                             std::string_view key) const override {
        if (const Component* found = find(entity, component)) {
            for (const auto& [name, value] : found->settings) {
                if (name == key) {
                    return value;
                }
            }
        }
        return std::nullopt;
    }
    void set_setting(std::string_view entity, std::string_view component, std::string_view key,
                     core::ConfigValue value) override {
        Component* found = find(entity, component);
        if (found == nullptr) {
            return;
        }
        for (auto& [name, held] : found->settings) {
            if (name == key) {
                held = std::move(value);
                return;
            }
        }
        found->settings.emplace_back(std::string(key), std::move(value));
    }
    void remove_setting(std::string_view entity, std::string_view component,
                        std::string_view key) override {
        if (Component* found = find(entity, component)) {
            std::erase_if(found->settings, [&](const auto& setting) { return setting.first == key; });
        }
    }
    void add_component(std::string_view entity, std::string_view component) override {
        if (Entity* found = find(entity); found != nullptr && find(entity, component) == nullptr) {
            found->components.push_back(Component{std::string(component), {}});
        }
    }
    void remove_component(std::string_view entity, std::string_view component) override {
        if (Entity* found = find(entity)) {
            std::erase_if(found->components,
                          [&](const Component& held) { return held.name == component; });
        }
    }
    std::string add_entity(std::string_view name) override {
        std::string chosen(name);
        for (int number = 2; find(chosen) != nullptr; ++number) {
            chosen = std::format("{}_{}", name, number);
        }
        scene.push_back(Entity{chosen, {}, {}});
        selection = chosen;
        return chosen;
    }
    void remove_entity(std::string_view entity) override {
        const std::string above = parent(entity);
        for (Entity& other : scene) {
            if (other.parent == entity) {
                other.parent = above;
            }
        }
        std::erase_if(scene, [&](const Entity& held) { return held.name == entity; });
        if (selection == entity) {
            selection.clear();
        }
    }
    core::Result<void> rename_entity(std::string_view from, std::string_view to) override {
        if (find(to) != nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument, "taken"};
        }
        for (Entity& other : scene) {
            if (other.parent == from) {
                other.parent = to;
            }
        }
        if (Entity* found = find(from)) {
            found->name = to;
        }
        if (selection == from) {
            selection = to;
        }
        return {};
    }
    core::Result<void> set_parent(std::string_view entity, std::string_view parent_name) override {
        if (Entity* found = find(entity)) {
            found->parent = parent_name;
        }
        return {};
    }
    std::vector<core::ComponentInfo> component_types() const override {
        return types;
    }
    const core::ComponentInfo* component_type(std::string_view component) const override {
        const auto found = std::ranges::find(types, component, &core::ComponentInfo::name);
        return found == types.end() ? nullptr : &*found;
    }
    std::string selected() const override {
        return selection;
    }
    void select(std::string_view entity) override {
        selection = entity;
    }
    std::string problem() const override {
        return {};
    }

    Entity* find(std::string_view name) {
        const auto found = std::ranges::find(scene, name, &Entity::name);
        return found == scene.end() ? nullptr : &*found;
    }
    const Entity* find(std::string_view name) const {
        const auto found = std::ranges::find(scene, name, &Entity::name);
        return found == scene.end() ? nullptr : &*found;
    }
    Component* find(std::string_view entity, std::string_view component) {
        Entity* found = find(entity);
        if (found == nullptr) {
            return nullptr;
        }
        const auto held = std::ranges::find(found->components, component, &Component::name);
        return held == found->components.end() ? nullptr : &*held;
    }
    const Component* find(std::string_view entity, std::string_view component) const {
        const Entity* found = find(entity);
        if (found == nullptr) {
            return nullptr;
        }
        const auto held = std::ranges::find(found->components, component, &Component::name);
        return held == found->components.end() ? nullptr : &*held;
    }
};

// Records every widget's label, and acts on those named in its scripts once each.
class FakeUi final : public debug_ui::DebugUi {
public:
    std::vector<std::string> shown;
    std::set<std::string> clicks;
    std::map<std::string, std::vector<float>> drags;
    std::map<std::string, int> whole_drags;
    std::map<std::string, std::string> typed;
    std::map<std::string, int> choices;
    std::set<std::string> toggles;
    // Tree nodes closed, by label; the rest are open.
    std::set<std::string> closed;

    [[nodiscard]] bool showed(std::string_view label) const {
        return std::ranges::find(shown, label) != shown.end();
    }

    core::Result<void> initialize(ui::Host&) override {
        return {};
    }
    bool handle_event(const platform::Event&) override {
        return false;
    }
    void begin_frame(const ui::FrameInfo&) override {}
    bool begin_window(std::string_view title, bool*) override {
        shown.emplace_back(title);
        return true;
    }
    void end_window() override {}
    void text(std::string_view text) override {
        shown.emplace_back(text);
    }
    bool button(std::string_view label) override {
        shown.emplace_back(label);
        return clicks.erase(std::string(label)) > 0;
    }
    bool checkbox(std::string_view label, bool& value) override {
        shown.emplace_back(label);
        if (toggles.erase(std::string(label)) > 0) {
            value = !value;
            return true;
        }
        return false;
    }
    bool slider(std::string_view label, float&, float, float) override {
        shown.emplace_back(label);
        return false;
    }
    bool slider(std::string_view label, int&, int, int) override {
        shown.emplace_back(label);
        return false;
    }
    void plot(std::string_view, std::span<const float>, float, float) override {}
    void separator() override {}
    void same_line() override {}
    const ui::DrawList& end_frame() override {
        return m_list;
    }
    bool wants_pointer() const override {
        return false;
    }
    bool wants_keyboard() const override {
        return false;
    }

    void text_disabled(std::string_view text) override {
        shown.emplace_back(text);
    }
    bool selectable(std::string_view label, bool) override {
        return button(label);
    }
    bool tree_node(std::string_view label, bool, bool, bool& clicked) override {
        clicked = button(label);
        return !closed.contains(std::string(label));
    }
    bool collapsing_header(std::string_view label) override {
        shown.emplace_back(label);
        return true;
    }
    bool drag(std::string_view label, std::span<float> values, float, float, float) override {
        shown.emplace_back(label);
        const auto found = drags.find(std::string(label));
        if (found == drags.end()) {
            return false;
        }
        std::ranges::copy_n(found->second.begin(),
                            static_cast<std::ptrdiff_t>(std::min(values.size(), found->second.size())),
                            values.begin());
        drags.erase(found);
        return true;
    }
    bool drag(std::string_view label, int& value, float) override {
        shown.emplace_back(label);
        const auto found = whole_drags.find(std::string(label));
        if (found == whole_drags.end()) {
            return false;
        }
        value = found->second;
        whole_drags.erase(found);
        return true;
    }
    bool color(std::string_view label, std::span<float> rgb) override {
        return drag(label, rgb, 0.0f, 0.0f, 0.0f);
    }
    bool input_text(std::string_view label, std::string& value) override {
        shown.emplace_back(label);
        const auto found = typed.find(std::string(label));
        if (found == typed.end()) {
            return false;
        }
        value = found->second;
        typed.erase(found);
        return true;
    }
    bool combo(std::string_view label, int& selected, std::span<const std::string>) override {
        shown.emplace_back(label);
        const auto found = choices.find(std::string(label));
        if (found == choices.end()) {
            return false;
        }
        selected = found->second;
        choices.erase(found);
        return true;
    }

private:
    ui::DrawList m_list;
};

} // namespace dream::editor_panel::tests
