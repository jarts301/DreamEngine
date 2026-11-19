#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace dream::input_device {

// Input devices beyond the keyboard, mouse and touch screen that the platform reports: gamepads
// (ADR 0035). The runtime reads them every frame and feeds them to the game's input actions, so a
// game binds "gamepad:south" as it binds "key:space". Multiplayer games can also read each
// gamepad on its own.

// Buttons by where they sit, so a binding means the same on every family of gamepad: South is A
// on Xbox, Cross on PlayStation and B on Nintendo Switch.
enum class GamepadButton : std::uint8_t {
    South,
    East,
    West,
    North,
    Back,  // View on Xbox, Share or Create on PlayStation, Minus on Switch.
    Guide, // The logo button, which some systems keep for themselves.
    Start, // Menu on Xbox, Options on PlayStation, Plus on Switch.
    LeftStickClick,
    RightStickClick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Count,
};

enum class GamepadAxis : std::uint8_t {
    // From -1 to 1, as the device reports them, without a deadzone: +X to the right, and +Y up,
    // away from the player.
    LeftX,
    LeftY,
    RightX,
    RightY,
    // From 0, released, to 1, fully pulled.
    LeftTrigger,
    RightTrigger,
    Count,
};

// Which family a gamepad belongs to, for showing the right button names and pictures.
enum class GamepadKind : std::uint8_t { Other, Xbox, PlayStation, NintendoSwitch };

struct GamepadState {
    // Stays the same while the gamepad is connected, and isn't reused while the program runs.
    std::uint32_t id = 0;
    std::string name;
    GamepadKind kind = GamepadKind::Other;
    std::array<bool, static_cast<std::size_t>(GamepadButton::Count)> buttons{};
    std::array<float, static_cast<std::size_t>(GamepadAxis::Count)> axes{};

    [[nodiscard]] bool button(GamepadButton which) const noexcept {
        return which < GamepadButton::Count && buttons[static_cast<std::size_t>(which)];
    }
    [[nodiscard]] float axis(GamepadAxis which) const noexcept {
        return which < GamepadAxis::Count ? axes[static_cast<std::size_t>(which)] : 0.0f;
    }
};

class InputDevice {
public:
    static constexpr std::string_view k_module_interface = "input_device";

    virtual ~InputDevice() = default;

    // Reads every connected gamepad, and notices gamepads plugged in or taken away. The runtime
    // calls it once per frame, after the platform's events.
    virtual void update() = 0;

    // The connected gamepads, in the order they were connected, as of the last update.
    [[nodiscard]] virtual std::span<const GamepadState> gamepads() const = 0;

    // Shakes a gamepad: its low-frequency (heavy) and high-frequency (light) motors, each from 0 to
    // 1, for `milliseconds`, replacing any rumble before. False when the gamepad has no motors, or
    // isn't connected.
    virtual bool rumble(std::uint32_t id, float low, float high, std::uint32_t milliseconds) = 0;
};

} // namespace dream::input_device
