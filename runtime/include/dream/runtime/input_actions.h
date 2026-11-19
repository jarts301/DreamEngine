#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"
#include "dream/input_device/input_device.h"
#include "dream/platform/event.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::runtime {

// Named game actions, so game code never mentions a specific key or button. Bindings come from the
// project configuration, and an action is active when any of its bindings is:
//
//     [input.actions]
//     jump = ["key:space", "mouse:left", "touch"]
//
// Keys are written as "key:a", "key:0", "key:f1", "key:space", "key:enter", "key:escape",
// "key:tab", "key:backspace", "key:delete", "key:home", "key:end", "key:left", "key:right", "key:up", "key:down", "key:left_shift",
// "key:right_shift", "key:left_control", "key:right_control", "key:left_alt", "key:right_alt" and
// "key:back". Pointers are "mouse:left", "mouse:right", "mouse:middle" and "touch".
//
// Gamepads, when the project lists an `input_device` module (ADR 0035), are "gamepad:" and one of:
//
// - the face buttons by position, "south", "east", "west" and "north", or by their Xbox names,
//   "a", "b", "x" and "y";
// - "back", "guide", "start", "left_shoulder", "right_shoulder", "left_stick_click",
//   "right_stick_click", "dpad_up", "dpad_down", "dpad_left" and "dpad_right";
// - "left_trigger" and "right_trigger";
// - a stick pushed one way: "left_stick_up", "left_stick_down", "left_stick_left",
//   "left_stick_right", and the same for "right_stick_".
//
// Triggers and sticks are analog: value() says how far, from 0 to 1, past a deadzone, and the
// action is down from halfway. Every connected gamepad drives the actions. Deadzones come from
//
//     [input]
//     stick_deadzone = 0.2     # Of the stick's reach, around its centre.
//     trigger_deadzone = 0.1
//
// so a game can move with keys and sticks alike:
//
//     [input.actions]
//     move_left = ["key:a", "key:left", "gamepad:left_stick_left", "gamepad:dpad_left"]
//     move_right = ["key:d", "key:right", "gamepad:left_stick_right", "gamepad:dpad_right"]
//
//     const float x = app.input().axis("move_left", "move_right");
class InputActions {
public:
    struct PointerPosition {
        float x = 0.0f;
        float y = 0.0f;
    };

    // What the player last pressed something on, for showing the right button prompts.
    enum class Source : std::uint8_t { None, KeyboardMouse, Touch, Gamepad };

    InputActions() = default;

    // Fails if a binding isn't recognized.
    [[nodiscard]] static core::Result<InputActions> from_config(const core::Config& config);

    // The action is held down.
    [[nodiscard]] bool is_down(std::string_view action) const;

    // The action started or ended during this frame. A press and release within one frame
    // reports both.
    [[nodiscard]] bool was_pressed(std::string_view action) const;
    [[nodiscard]] bool was_released(std::string_view action) const;

    // How strongly the action is held, from 0 to 1: 1 for a key or button, and how far a trigger
    // or stick is pushed for analog bindings. The largest of its bindings.
    [[nodiscard]] float value(std::string_view action) const;

    // value(positive) - value(negative), from -1 to 1, such as for moving left and right.
    [[nodiscard]] float axis(std::string_view negative, std::string_view positive) const {
        return value(positive) - value(negative);
    }

    [[nodiscard]] Source last_source() const noexcept {
        return m_last_source;
    }

    // Where the mouse or primary finger last was, in pixels.
    [[nodiscard]] PointerPosition pointer_position() const noexcept {
        return m_pointer;
    }

    // Used by the main loop.
    void begin_frame() noexcept;
    void apply(const platform::KeyChanged& event) noexcept;
    void apply(const platform::PointerButtonChanged& event) noexcept;
    void apply(const platform::PointerMoved& event) noexcept;
    // Every connected gamepad's state, once per frame.
    void apply(std::span<const input_device::GamepadState> gamepads) noexcept;

    // Actions don't use these.
    void apply(const platform::PointerWheel& /*event*/) noexcept {}
    void apply(const platform::TextEntered& /*event*/) noexcept {}
    void release_all() noexcept;

private:
    static constexpr std::size_t k_key_count = static_cast<std::size_t>(platform::Key::Count);
    static constexpr std::size_t k_pointer_count =
        static_cast<std::size_t>(platform::PointerButton::Count);
    static constexpr std::size_t k_gamepad_button_count =
        static_cast<std::size_t>(input_device::GamepadButton::Count);
    // Each stick's four directions, then the two triggers.
    static constexpr std::size_t k_gamepad_analog_count = 10;

public:
    // Where each kind of input starts among them all, for the bindings' parser.
    static constexpr std::size_t k_first_gamepad_button = k_key_count + k_pointer_count;
    static constexpr std::size_t k_first_gamepad_analog =
        k_first_gamepad_button + k_gamepad_button_count;
    static constexpr std::size_t k_input_count = k_first_gamepad_analog + k_gamepad_analog_count;

private:
    struct InputState {
        bool down = false;
        bool pressed = false;
        bool released = false;
        float value = 0.0f;
    };

    using Bindings = std::vector<std::size_t>;

    template <bool InputState::*Field>
    [[nodiscard]] bool any_binding(std::string_view action) const;

    void set_down(std::size_t input, bool down) noexcept;
    // An analog input, which is down from halfway and up again below 0.4, so a value resting near
    // the threshold doesn't flicker.
    void set_value(std::size_t input, float value) noexcept;

    std::map<std::string, Bindings, std::less<>> m_actions;
    std::array<InputState, k_input_count> m_inputs{};
    PointerPosition m_pointer;
    float m_stick_deadzone = 0.2f;
    float m_trigger_deadzone = 0.1f;
    Source m_last_source = Source::None;
};

} // namespace dream::runtime
