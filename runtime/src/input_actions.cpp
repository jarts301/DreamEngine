#include "dream/runtime/input_actions.h"

#include "dream/core/assert.h"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace dream::runtime {
namespace {

using platform::Key;
using platform::PointerButton;

struct NamedKey {
    std::string_view name;
    Key key;
};

constexpr NamedKey k_named_keys[] = {
    {"space", Key::Space},
    {"enter", Key::Enter},
    {"escape", Key::Escape},
    {"tab", Key::Tab},
    {"backspace", Key::Backspace},
    {"delete", Key::Delete},
    {"home", Key::Home},
    {"end", Key::End},
    {"left", Key::Left},
    {"right", Key::Right},
    {"up", Key::Up},
    {"down", Key::Down},
    {"left_shift", Key::LeftShift},
    {"right_shift", Key::RightShift},
    {"left_control", Key::LeftControl},
    {"right_control", Key::RightControl},
    {"left_alt", Key::LeftAlt},
    {"right_alt", Key::RightAlt},
    {"back", Key::Back},
};

Key offset_key(Key first, int offset) {
    return static_cast<Key>(static_cast<int>(first) + offset);
}

std::optional<Key> parse_key(std::string_view name) {
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
        return offset_key(Key::A, name[0] - 'a');
    }
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
        return offset_key(Key::Digit0, name[0] - '0');
    }
    if (name.size() == 2 && name[0] == 'f' && name[1] >= '1' && name[1] <= '9') {
        return offset_key(Key::F1, name[1] - '1');
    }
    if (name.size() == 3 && name[0] == 'f' && name[1] == '1' && name[2] >= '0' && name[2] <= '2') {
        return offset_key(Key::F10, name[2] - '0');
    }
    for (const NamedKey& named : k_named_keys) {
        if (named.name == name) {
            return named.key;
        }
    }
    return std::nullopt;
}

std::size_t key_index(Key key) noexcept {
    return static_cast<std::size_t>(key);
}

std::size_t pointer_index(PointerButton button) noexcept {
    return static_cast<std::size_t>(platform::Key::Count) + static_cast<std::size_t>(button);
}

std::optional<std::size_t> parse_binding(std::string_view binding) {
    constexpr std::string_view k_key_prefix = "key:";
    if (binding.starts_with(k_key_prefix)) {
        const std::optional<Key> key = parse_key(binding.substr(k_key_prefix.size()));
        return key.has_value() ? std::optional(key_index(*key)) : std::nullopt;
    }
    if (binding == "mouse:left") {
        return pointer_index(PointerButton::Left);
    }
    if (binding == "mouse:right") {
        return pointer_index(PointerButton::Right);
    }
    if (binding == "mouse:middle") {
        return pointer_index(PointerButton::Middle);
    }
    if (binding == "touch") {
        return pointer_index(PointerButton::Touch);
    }
    return std::nullopt;
}

} // namespace

core::Result<InputActions> InputActions::from_config(const core::Config& config) {
    constexpr std::string_view k_table = "input.actions";

    InputActions actions;
    for (const std::string_view name : config.keys_in(k_table)) {
        const auto bindings = config.get_string_list(std::format("{}.{}", k_table, name));
        if (!bindings.has_value()) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               std::format("Input action '{}' must be a list of bindings", name)};
        }

        Bindings& inputs = actions.m_actions[std::string(name)];
        for (const std::string& binding : *bindings) {
            const std::optional<std::size_t> input = parse_binding(binding);
            if (!input.has_value()) {
                return core::Error{
                    core::ErrorCode::InvalidArgument,
                    std::format("Input action '{}' has unknown binding '{}'", name, binding)};
            }
            inputs.push_back(*input);
        }
    }
    return actions;
}

template <bool InputActions::InputState::*Field>
bool InputActions::any_binding(std::string_view action) const {
    const auto entry = m_actions.find(action);
    DREAM_ASSERT(entry != m_actions.end(), "This input action is not in the project configuration");
    if (entry == m_actions.end()) {
        return false;
    }
    return std::ranges::any_of(entry->second, [this](std::size_t input) {
        return m_inputs[input].*Field;
    });
}

bool InputActions::is_down(std::string_view action) const {
    return any_binding<&InputState::down>(action);
}

bool InputActions::was_pressed(std::string_view action) const {
    return any_binding<&InputState::pressed>(action);
}

bool InputActions::was_released(std::string_view action) const {
    return any_binding<&InputState::released>(action);
}

void InputActions::begin_frame() noexcept {
    for (InputState& input : m_inputs) {
        input.pressed = false;
        input.released = false;
    }
}

void InputActions::set_down(std::size_t input, bool down) noexcept {
    InputState& state = m_inputs[input];
    if (state.down == down) {
        return;
    }
    state.down = down;
    (down ? state.pressed : state.released) = true;
}

void InputActions::apply(const platform::KeyChanged& event) noexcept {
    if (event.key < Key::Count) {
        set_down(key_index(event.key), event.pressed);
    }
}

void InputActions::apply(const platform::PointerButtonChanged& event) noexcept {
    if (event.button < PointerButton::Count) {
        set_down(pointer_index(event.button), event.pressed);
    }
}

void InputActions::apply(const platform::PointerMoved& event) noexcept {
    m_pointer = PointerPosition{event.x, event.y};
}

void InputActions::release_all() noexcept {
    for (std::size_t input = 0; input < k_input_count; ++input) {
        set_down(input, false);
    }
}

} // namespace dream::runtime
