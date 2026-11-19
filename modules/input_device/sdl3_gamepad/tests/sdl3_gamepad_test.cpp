#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/input_device/input_device.h"

#include <SDL3/SDL.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void>
register_input_device_sdl3_gamepad(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::input_device;
using doctest::Approx;

namespace {

std::unique_ptr<InputDevice> make() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_input_device_sdl3_gamepad(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"sdl3_gamepad"};
    auto created = registry.create<InputDevice>(preference, config);
    REQUIRE_MESSAGE(created.has_value(), created.error().message);
    return std::move(created.value().instance);
}

constexpr const char* k_name = "Dream Test Pad";

struct Rumbled {
    Uint16 low = 0;
    Uint16 high = 0;
};

// A gamepad that SDL makes up, which a test presses and moves.
class VirtualPad {
public:
    VirtualPad() {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = k_name;
        desc.userdata = &rumbled;
        desc.Rumble = [](void* userdata, Uint16 low, Uint16 high) {
            *static_cast<Rumbled*>(userdata) = Rumbled{low, high};
            return true;
        };
        id = SDL_AttachVirtualJoystick(&desc);
        REQUIRE_MESSAGE(id != 0, SDL_GetError());
        joystick = SDL_OpenJoystick(id);
        REQUIRE(joystick != nullptr);
    }
    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;
    ~VirtualPad() {
        SDL_CloseJoystick(joystick);
        SDL_DetachVirtualJoystick(id);
    }

    void press(SDL_GamepadButton button, bool down) {
        SDL_SetJoystickVirtualButton(joystick, button, down);
    }
    void move(SDL_GamepadAxis axis, Sint16 value) {
        SDL_SetJoystickVirtualAxis(joystick, axis, value);
    }

    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    Rumbled rumbled;
};

const GamepadState* find_pad(const InputDevice& device) {
    const auto pads = device.gamepads();
    const auto found = std::ranges::find(pads, std::string(k_name), &GamepadState::name);
    return found == pads.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("sdl3_gamepad finds a gamepad that is plugged in, and reads it") {
    const std::unique_ptr<InputDevice> device = make();
    device->update();
    CHECK(find_pad(*device) == nullptr);

    VirtualPad pad;
    device->update();
    const GamepadState* state = find_pad(*device);
    REQUIRE(state != nullptr);
    CHECK(state->id == pad.id);
    CHECK_FALSE(state->button(GamepadButton::South));

    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    pad.press(SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
    // SDL's up is negative; ours is positive.
    pad.move(SDL_GAMEPAD_AXIS_LEFTY, -32768);
    pad.move(SDL_GAMEPAD_AXIS_RIGHTX, 16384);
    pad.move(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    device->update();
    state = find_pad(*device);
    REQUIRE(state != nullptr);
    CHECK(state->button(GamepadButton::South));
    CHECK(state->button(GamepadButton::DpadLeft));
    CHECK_FALSE(state->button(GamepadButton::East));
    CHECK(state->axis(GamepadAxis::LeftY) == Approx(1.0f));
    CHECK(state->axis(GamepadAxis::RightX) == Approx(0.5f).epsilon(0.01));
    CHECK(state->axis(GamepadAxis::RightTrigger) == Approx(1.0f));
    CHECK(state->axis(GamepadAxis::LeftTrigger) == Approx(0.0f));
}

TEST_CASE("sdl3_gamepad forgets a gamepad that is taken away") {
    const std::unique_ptr<InputDevice> device = make();
    {
        VirtualPad pad;
        device->update();
        REQUIRE(find_pad(*device) != nullptr);
    }
    device->update();
    CHECK(find_pad(*device) == nullptr);
}

TEST_CASE("sdl3_gamepad rumbles a gamepad that has motors") {
    const std::unique_ptr<InputDevice> device = make();
    VirtualPad pad;
    device->update();
    const GamepadState* const state = find_pad(*device);
    REQUIRE(state != nullptr);

    CHECK(device->rumble(state->id, 1.0f, 0.5f, 100));
    CHECK(pad.rumbled.low == 65535);
    CHECK(pad.rumbled.high == 32767);
    CHECK_FALSE(device->rumble(state->id + 1000, 1.0f, 1.0f, 100));
}
