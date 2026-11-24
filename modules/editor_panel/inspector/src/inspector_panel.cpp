// The editor's inspector (ADR 0046): the selected entity's name, parent and components, with a
// control for each setting. Components that describe their fields get theirs, with their values
// when left out shown too; other settings, and a game's own components, get one by the value's
// type.

#include "dream/core/module_registry.h"
#include "dream/editor_panel/editor_panel.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace editor_panel;
using core::ConfigValue;
using core::FieldInfo;
using core::FieldKind;

// A number from a setting that holds one, whether integer or float.
std::optional<double> number_of(const ConfigValue& value) {
    if (const auto* real = std::get_if<double>(&value)) {
        return *real;
    }
    if (const auto* whole = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*whole);
    }
    return std::nullopt;
}

std::vector<float> floats_of(const ConfigValue& value, std::size_t count) {
    std::vector<float> floats(count, 0.0f);
    if (const auto* list = std::get_if<std::vector<double>>(&value)) {
        for (std::size_t index = 0; index < count && index < list->size(); ++index) {
            floats[index] = static_cast<float>((*list)[index]);
        }
    }
    return floats;
}

ConfigValue list_of(const std::vector<float>& floats) {
    std::vector<double> list;
    for (const float value : floats) {
        // Rounded as a person would type it, so files don't fill with 0.30000001.
        list.push_back(std::round(static_cast<double>(value) * 1.0e5) / 1.0e5);
    }
    return list;
}

double rounded(float value) {
    return std::round(static_cast<double>(value) * 1.0e5) / 1.0e5;
}

class InspectorPanel final : public EditorPanel {
public:
    std::string_view title() const override {
        return "Inspector";
    }

    Side side() const override {
        return Side::Right;
    }

    void draw(debug_ui::DebugUi& ui, SceneModel& scene) override {
        const std::string entity = scene.selected();
        if (entity.empty()) {
            ui.text_disabled("Select an entity to see its components");
            return;
        }
        draw_entity(ui, scene, entity);
        ui.separator();
        for (const std::string& component : scene.components(entity)) {
            if (!ui.collapsing_header(std::format("{}##header.{}", component, component))) {
                continue;
            }
            draw_component(ui, scene, entity, component);
            if (ui.button(std::format("Remove {}##remove.{}", component, component))) {
                scene.remove_component(entity, component);
                // Its settings are gone; drawing more of them this frame would read them.
                return;
            }
        }
        ui.separator();
        draw_add_component(ui, scene, entity);
        if (const std::string problem = scene.problem(); !problem.empty()) {
            ui.separator();
            ui.text("The scene can't be built as it is:");
            ui.text_disabled(problem);
        }
    }

private:
    void draw_entity(debug_ui::DebugUi& ui, SceneModel& scene, const std::string& entity) {
        std::string name = entity;
        if (ui.input_text("Name", name)) {
            if (const core::Result<void> renamed = scene.rename_entity(entity, name);
                !renamed.has_value()) {
                m_message = renamed.error().message;
            } else {
                m_message.clear();
            }
        }
        std::vector<std::string> parents{"(none)"};
        int current = 0;
        const std::string parent = scene.parent(entity);
        for (const std::string& other : scene.entities()) {
            if (other != scene.selected()) {
                if (other == parent) {
                    current = static_cast<int>(parents.size());
                }
                parents.push_back(other);
            }
        }
        if (ui.combo("Parent", current, parents)) {
            const std::string chosen = current == 0 ? std::string() : parents[current];
            if (const core::Result<void> set = scene.set_parent(scene.selected(), chosen);
                !set.has_value()) {
                m_message = set.error().message;
            } else {
                m_message.clear();
            }
        }
        if (!m_message.empty()) {
            ui.text_disabled(m_message);
        }
    }

    void draw_component(debug_ui::DebugUi& ui, SceneModel& scene, const std::string& entity,
                        const std::string& component) {
        const std::vector<std::string> present = scene.settings(entity, component);
        std::vector<std::string> described;
        if (const core::ComponentInfo* info = scene.component_type(component)) {
            for (const FieldInfo& field : info->fields) {
                described.push_back(field.name);
                draw_field(ui, scene, entity, component, field);
            }
        } else {
            ui.text_disabled("A game's own component, edited by its values");
        }
        for (const std::string& key : present) {
            if (std::ranges::find(described, key) == described.end()) {
                draw_setting(ui, scene, entity, component, key);
            }
        }
    }

    // A described field: its value, or the component's when left out, with a control for its
    // kind. A change writes the setting.
    static void draw_field(debug_ui::DebugUi& ui, SceneModel& scene, const std::string& entity,
                           const std::string& component, const FieldInfo& field) {
        const std::optional<ConfigValue> stored = scene.setting(entity, component, field.name);
        const ConfigValue value = stored.value_or(field.fallback);
        const std::string label = std::format("{}##{}.{}", field.name, component, field.name);
        const auto set = [&](ConfigValue changed) {
            scene.set_setting(entity, component, field.name, std::move(changed));
        };
        switch (field.kind) {
        case FieldKind::Number: {
            std::vector<float> one{static_cast<float>(number_of(value).value_or(0.0))};
            if (ui.drag(label, one, 0.01f, field.minimum, field.maximum)) {
                set(rounded(one[0]));
            }
            break;
        }
        case FieldKind::Integer: {
            int whole = static_cast<int>(number_of(value).value_or(0.0));
            if (ui.drag(label, whole, 0.1f)) {
                set(static_cast<std::int64_t>(whole));
            }
            break;
        }
        case FieldKind::Flag: {
            bool flag = std::holds_alternative<bool>(value) && std::get<bool>(value);
            if (ui.checkbox(label, flag)) {
                set(flag);
            }
            break;
        }
        case FieldKind::Text:
        case FieldKind::Asset: {
            std::string text =
                std::holds_alternative<std::string>(value) ? std::get<std::string>(value) : "";
            if (ui.input_text(label, text)) {
                set(text);
            }
            break;
        }
        case FieldKind::Choice: {
            const std::string text =
                std::holds_alternative<std::string>(value) ? std::get<std::string>(value) : "";
            const auto at = std::ranges::find(field.choices, text);
            int index = at == field.choices.end()
                            ? -1
                            : static_cast<int>(at - field.choices.begin());
            if (ui.combo(label, index, field.choices) && index >= 0) {
                set(field.choices[static_cast<std::size_t>(index)]);
            }
            break;
        }
        case FieldKind::Vec2:
        case FieldKind::Vec3: {
            std::vector<float> floats =
                floats_of(value, field.kind == FieldKind::Vec2 ? 2 : 3);
            if (ui.drag(label, floats, 0.01f, field.minimum, field.maximum)) {
                set(list_of(floats));
            }
            break;
        }
        case FieldKind::Color: {
            std::vector<float> rgb = floats_of(value, 3);
            if (ui.color(label, rgb)) {
                set(list_of(rgb));
            }
            break;
        }
        }
        if (!field.help.empty()) {
            ui.tooltip(field.help);
        }
        if (!stored.has_value()) {
            ui.same_line();
            ui.text_disabled("(default)");
        }
    }

    // A setting no field describes: a control by its value's type, and a way to remove it.
    static void draw_setting(debug_ui::DebugUi& ui, SceneModel& scene, const std::string& entity,
                             const std::string& component, const std::string& key) {
        const std::optional<ConfigValue> stored = scene.setting(entity, component, key);
        if (!stored.has_value()) {
            return;
        }
        const std::string label = std::format("{}##{}.{}", key, component, key);
        const auto set = [&](ConfigValue changed) {
            scene.set_setting(entity, component, key, std::move(changed));
        };
        std::visit(
            [&](const auto& held) {
                using Type = std::decay_t<decltype(held)>;
                if constexpr (std::is_same_v<Type, bool>) {
                    bool flag = held;
                    if (ui.checkbox(label, flag)) {
                        set(flag);
                    }
                } else if constexpr (std::is_same_v<Type, std::int64_t>) {
                    int whole = static_cast<int>(held);
                    if (ui.drag(label, whole, 0.1f)) {
                        set(static_cast<std::int64_t>(whole));
                    }
                } else if constexpr (std::is_same_v<Type, double>) {
                    std::vector<float> one{static_cast<float>(held)};
                    if (ui.drag(label, one, 0.01f)) {
                        set(rounded(one[0]));
                    }
                } else if constexpr (std::is_same_v<Type, std::string>) {
                    std::string text = held;
                    if (ui.input_text(label, text)) {
                        set(text);
                    }
                } else if constexpr (std::is_same_v<Type, std::vector<double>>) {
                    if (held.empty() || held.size() > 4) {
                        ui.text(std::format("{}: {} numbers", key, held.size()));
                    } else {
                        std::vector<float> floats = floats_of(held, held.size());
                        if (ui.drag(label, floats, 0.01f)) {
                            set(list_of(floats));
                        }
                    }
                } else {
                    std::string joined;
                    for (const std::string& item : held) {
                        joined += joined.empty() ? item : ", " + item;
                    }
                    ui.text(std::format("{}: [{}]", key, joined));
                }
            },
            *stored);
        ui.same_line();
        if (ui.button(std::format("x##remove.{}.{}", component, key))) {
            scene.remove_setting(entity, component, key);
        }
        ui.tooltip("Removes this setting");
    }

    void draw_add_component(debug_ui::DebugUi& ui, SceneModel& scene, const std::string& entity) {
        const std::vector<std::string> present = scene.components(entity);
        std::vector<std::string> missing;
        for (const core::ComponentInfo& type : scene.component_types()) {
            if (std::ranges::find(present, type.name) == present.end()) {
                missing.push_back(type.name);
            }
        }
        if (missing.empty()) {
            return;
        }
        m_adding = std::clamp(m_adding, 0, static_cast<int>(missing.size()) - 1);
        ui.combo("##add_component", m_adding, missing);
        ui.same_line();
        if (ui.button("Add component")) {
            scene.add_component(entity, missing[static_cast<std::size_t>(m_adding)]);
        }
    }

    std::string m_message;
    int m_adding = 0;
};

core::Result<std::unique_ptr<EditorPanel>> create(const core::Config& /*config*/) {
    return std::unique_ptr<EditorPanel>{std::make_unique<InspectorPanel>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_editor_panel_inspector(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<EditorPanel>{.name = "inspector", .create = &create});
}

} // namespace dream::modules
