// Gamepads through SDL3 (zlib license), on every platform SDL supports: XInput, DirectInput and
// Windows.Gaming.Input on Windows, and Android's own gamepad support through SDL's Java side
// (ADR 0035). SDL's gamepad database names each button by its position, so South is the bottom
// face button on every family.

#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/input_device/input_device.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dream::modules {
namespace {

using namespace input_device;

constexpr std::pair<GamepadButton, SDL_GamepadButton> k_buttons[] = {
    {GamepadButton::South, SDL_GAMEPAD_BUTTON_SOUTH},
    {GamepadButton::East, SDL_GAMEPAD_BUTTON_EAST},
    {GamepadButton::West, SDL_GAMEPAD_BUTTON_WEST},
    {GamepadButton::North, SDL_GAMEPAD_BUTTON_NORTH},
    {GamepadButton::Back, SDL_GAMEPAD_BUTTON_BACK},
    {GamepadButton::Guide, SDL_GAMEPAD_BUTTON_GUIDE},
    {GamepadButton::Start, SDL_GAMEPAD_BUTTON_START},
    {GamepadButton::LeftStickClick, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {GamepadButton::RightStickClick, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {GamepadButton::LeftShoulder, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {GamepadButton::RightShoulder, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {GamepadButton::DpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {GamepadButton::DpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {GamepadButton::DpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {GamepadButton::DpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};
static_assert(std::size(k_buttons) == static_cast<std::size_t>(GamepadButton::Count));

GamepadKind kind_of(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
        return GamepadKind::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return GamepadKind::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return GamepadKind::NintendoSwitch;
    default:
        return GamepadKind::Other;
    }
}

// SDL reports sticks from -32768 to 32767, with +Y down, and triggers from 0 to 32767.
float stick(Sint16 value) {
    return std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
}

class SdlGamepads final : public InputDevice {
public:
    SdlGamepads() {
        // Told when a gamepad comes or goes, without taking the event from the platform's queue.
        SDL_AddEventWatch(&SdlGamepads::watch, this);
    }

    SdlGamepads(const SdlGamepads&) = delete;
    SdlGamepads& operator=(const SdlGamepads&) = delete;

    ~SdlGamepads() override {
        SDL_RemoveEventWatch(&SdlGamepads::watch, this);
        for (Open& open : m_open) {
            SDL_CloseGamepad(open.gamepad);
        }
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }

    void update() override {
        // SDL's event loop does this when the platform pumps it; done again here for programs
        // whose platform doesn't use SDL.
        SDL_UpdateGamepads();
        if (m_changed.exchange(false)) {
            reconnect();
        }
        // Read here, so the events aren't needed: dropped, so they don't pile up when no one
        // else drains SDL's queue.
        SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED);

        for (std::size_t index = 0; index < m_open.size(); ++index) {
            SDL_Gamepad* const gamepad = m_open[index].gamepad;
            GamepadState& state = m_states[index];
            for (const auto& [ours, sdl] : k_buttons) {
                state.buttons[static_cast<std::size_t>(ours)] = SDL_GetGamepadButton(gamepad, sdl);
            }
            const auto axis = [&](SDL_GamepadAxis which) { return SDL_GetGamepadAxis(gamepad, which); };
            state.axes[static_cast<std::size_t>(GamepadAxis::LeftX)] = stick(axis(SDL_GAMEPAD_AXIS_LEFTX));
            state.axes[static_cast<std::size_t>(GamepadAxis::LeftY)] = -stick(axis(SDL_GAMEPAD_AXIS_LEFTY));
            state.axes[static_cast<std::size_t>(GamepadAxis::RightX)] =
                stick(axis(SDL_GAMEPAD_AXIS_RIGHTX));
            state.axes[static_cast<std::size_t>(GamepadAxis::RightY)] =
                -stick(axis(SDL_GAMEPAD_AXIS_RIGHTY));
            state.axes[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] =
                std::max(stick(axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)), 0.0f);
            state.axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] =
                std::max(stick(axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)), 0.0f);
        }
    }

    std::span<const GamepadState> gamepads() const override {
        return m_states;
    }

    bool rumble(std::uint32_t id, float low, float high, std::uint32_t milliseconds) override {
        const auto found = std::ranges::find(m_open, id, &Open::id);
        if (found == m_open.end()) {
            return false;
        }
        const auto strength = [](float value) {
            return static_cast<Uint16>(std::clamp(value, 0.0f, 1.0f) * 65535.0f);
        };
        return SDL_RumbleGamepad(found->gamepad, strength(low), strength(high), milliseconds);
    }

    // Opens gamepads that came, and closes those that went, keeping the rest in their order.
    void reconnect() {
        int count = 0;
        SDL_JoystickID* const ids = SDL_GetGamepads(&count);
        const std::vector<SDL_JoystickID> connected(ids, ids + std::max(count, 0));
        SDL_free(ids);

        for (std::size_t index = m_open.size(); index-- > 0;) {
            if (std::ranges::find(connected, m_open[index].id) == connected.end()) {
                DREAM_LOG_INFO("Gamepad disconnected: {}", m_states[index].name);
                SDL_CloseGamepad(m_open[index].gamepad);
                m_open.erase(m_open.begin() + static_cast<std::ptrdiff_t>(index));
                m_states.erase(m_states.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        for (const SDL_JoystickID id : connected) {
            if (std::ranges::find(m_open, id, &Open::id) != m_open.end()) {
                continue;
            }
            SDL_Gamepad* const gamepad = SDL_OpenGamepad(id);
            if (gamepad == nullptr) {
                DREAM_LOG_WARNING("Could not open a gamepad: {}", SDL_GetError());
                continue;
            }
            const char* const name = SDL_GetGamepadName(gamepad);
            GamepadState state{.id = id,
                               .name = name != nullptr ? name : "Gamepad",
                               .kind = kind_of(SDL_GetGamepadType(gamepad))};
            DREAM_LOG_INFO("Gamepad connected: {}", state.name);
            m_open.push_back(Open{.id = id, .gamepad = gamepad});
            m_states.push_back(std::move(state));
        }
    }

    // On SDL's thread that adds the event, which may not be the main one.
    static bool SDLCALL watch(void* self, SDL_Event* event) {
        if (event->type == SDL_EVENT_GAMEPAD_ADDED || event->type == SDL_EVENT_GAMEPAD_REMOVED) {
            static_cast<SdlGamepads*>(self)->m_changed.store(true);
        }
        return true;
    }

private:
    struct Open {
        std::uint32_t id = 0;
        SDL_Gamepad* gamepad = nullptr;
    };

    std::vector<Open> m_open;
    std::vector<GamepadState> m_states;
    // Starts true, so the gamepads already connected are opened on the first update.
    std::atomic<bool> m_changed{true};
};

core::Result<std::unique_ptr<InputDevice>> create(const core::Config& /*config*/) {
    // SDL's default stays: while another program has the focus, its gamepad input is ignored, so a
    // game in the background doesn't move with someone else's play.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        return core::Error{core::ErrorCode::Unknown,
                           std::string("Could not start SDL's gamepads: ") + SDL_GetError()};
    }
    return std::unique_ptr<InputDevice>{std::make_unique<SdlGamepads>()};
}

} // namespace

[[nodiscard]] core::Result<void>
register_input_device_sdl3_gamepad(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<InputDevice>{.name = "sdl3_gamepad", .create = &create});
}

} // namespace dream::modules
