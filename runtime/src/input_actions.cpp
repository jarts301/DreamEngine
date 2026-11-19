#include "dream/runtime/input_actions.h"

#include "dream/core/assert.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

namespace dream::runtime {
namespace {

using input_device::GamepadAxis;
using input_device::GamepadButton;
using platform::Key;
using platform::PointerButton;

struct NamedButton {
    std::string_view name;
    GamepadButton button;
};

constexpr NamedButton k_gamepad_buttons[] = {
    {"south", GamepadButton::South},
    {"east", GamepadButton::East},
    {"west", GamepadButton::West},
    {"north", GamepadButton::North},
    {"a", GamepadButton::South},
    {"b", GamepadButton::East},
    {"x", GamepadButton::West},
    {"y", GamepadButton::North},
    {"back", GamepadButton::Back},
    {"guide", GamepadButton::Guide},
    {"start", GamepadButton::Start},
    {"left_stick_click", GamepadButton::LeftStickClick},
    {"right_stick_click", GamepadButton::RightStickClick},
    {"left_shoulder", GamepadButton::LeftShoulder},
    {"right_shoulder", GamepadButton::RightShoulder},
    {"dpad_up", GamepadButton::DpadUp},
    {"dpad_down", GamepadButton::DpadDown},
    {"dpad_left", GamepadButton::DpadLeft},
    {"dpad_right", GamepadButton::DpadRight},
};

// The analog inputs, in the order InputActions keeps them after the gamepad buttons.
constexpr std::string_view k_gamepad_analogs[] = {
    "left_stick_up",  "left_stick_down",  "left_stick_left",  "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
    "left_trigger",   "right_trigger",
};

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
    constexpr std::string_view k_gamepad_prefix = "gamepad:";
    if (binding.starts_with(k_gamepad_prefix)) {
        const std::string_view name = binding.substr(k_gamepad_prefix.size());
        for (const NamedButton& named : k_gamepad_buttons) {
            if (named.name == name) {
                return InputActions::k_first_gamepad_button + static_cast<std::size_t>(named.button);
            }
        }
        for (std::size_t index = 0; index < std::size(k_gamepad_analogs); ++index) {
            if (k_gamepad_analogs[index] == name) {
                return InputActions::k_first_gamepad_analog + index;
            }
        }
    }
    return std::nullopt;
}

} // namespace

core::Result<InputActions> InputActions::from_config(const core::Config& config) {
    constexpr std::string_view k_table = "input.actions";

    InputActions actions;
    for (const auto& [key, deadzone] :
         {std::pair{"input.stick_deadzone", &actions.m_stick_deadzone},
          std::pair{"input.trigger_deadzone", &actions.m_trigger_deadzone}}) {
        if (!config.contains(key)) {
            continue;
        }
        const std::optional<double> value = config.get_float(key);
        if (!value.has_value() || *value < 0.0 || *value >= 1.0) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               std::format("Setting '{}' must be a number from 0 to below 1", key)};
        }
        *deadzone = static_cast<float>(*value);
    }
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

float InputActions::value(std::string_view action) const {
    const auto entry = m_actions.find(action);
    DREAM_ASSERT(entry != m_actions.end(), "This input action is not in the project configuration");
    if (entry == m_actions.end()) {
        return 0.0f;
    }
    float strongest = 0.0f;
    for (const std::size_t input : entry->second) {
        strongest = std::max(strongest, m_inputs[input].value);
    }
    return strongest;
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
    state.value = down ? 1.0f : 0.0f;
    (down ? state.pressed : state.released) = true;
}

void InputActions::set_value(std::size_t input, float value) noexcept {
    InputState& state = m_inputs[input];
    state.value = value;
    const bool down = state.down ? value >= 0.4f : value >= 0.5f;
    if (state.down != down) {
        state.down = down;
        (down ? state.pressed : state.released) = true;
        if (down) {
            m_last_source = Source::Gamepad;
        }
    }
}

void InputActions::apply(const platform::KeyChanged& event) noexcept {
    if (event.key < Key::Count) {
        set_down(key_index(event.key), event.pressed);
        if (event.pressed) {
            m_last_source = Source::KeyboardMouse;
        }
    }
}

void InputActions::apply(const platform::PointerButtonChanged& event) noexcept {
    if (event.button < PointerButton::Count) {
        set_down(pointer_index(event.button), event.pressed);
        if (event.pressed) {
            m_last_source =
                event.button == PointerButton::Touch ? Source::Touch : Source::KeyboardMouse;
        }
    }
}

void InputActions::apply(std::span<const input_device::GamepadState> gamepads) noexcept {
    std::array<bool, k_gamepad_button_count> buttons{};
    std::array<float, k_gamepad_analog_count> analogs{};
    const auto past = [](float value, float deadzone) {
        return value <= deadzone ? 0.0f : std::min((value - deadzone) / (1.0f - deadzone), 1.0f);
    };
    for (const input_device::GamepadState& pad : gamepads) {
        for (std::size_t button = 0; button < k_gamepad_button_count; ++button) {
            buttons[button] = buttons[button] || pad.buttons[button];
        }
        // Each stick's deadzone is a circle, so pushing diagonally reaches it as soon as pushing
        // straight does, and the rest of its reach is stretched back to 0 to 1.
        const auto stick = [&](GamepadAxis x_axis, GamepadAxis y_axis, std::size_t first) {
            const float x = pad.axis(x_axis);
            const float y = pad.axis(y_axis);
            const float length = std::sqrt(x * x + y * y);
            if (length <= m_stick_deadzone) {
                return;
            }
            const float scale = past(std::min(length, 1.0f), m_stick_deadzone) / length;
            const float sx = x * scale;
            const float sy = y * scale;
            analogs[first] = std::max(analogs[first], sy);
            analogs[first + 1] = std::max(analogs[first + 1], -sy);
            analogs[first + 2] = std::max(analogs[first + 2], -sx);
            analogs[first + 3] = std::max(analogs[first + 3], sx);
        };
        stick(GamepadAxis::LeftX, GamepadAxis::LeftY, 0);
        stick(GamepadAxis::RightX, GamepadAxis::RightY, 4);
        analogs[8] = std::max(analogs[8], past(pad.axis(GamepadAxis::LeftTrigger), m_trigger_deadzone));
        analogs[9] =
            std::max(analogs[9], past(pad.axis(GamepadAxis::RightTrigger), m_trigger_deadzone));
    }
    for (std::size_t button = 0; button < k_gamepad_button_count; ++button) {
        const std::size_t input = k_first_gamepad_button + button;
        if (buttons[button] && !m_inputs[input].down) {
            m_last_source = Source::Gamepad;
        }
        set_down(input, buttons[button]);
    }
    for (std::size_t analog = 0; analog < k_gamepad_analog_count; ++analog) {
        set_value(k_first_gamepad_analog + analog, analogs[analog]);
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
