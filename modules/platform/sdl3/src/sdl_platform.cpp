#include "dream/core/module_registry.h"
#include "dream/platform/platform.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::modules {
namespace {

using namespace platform;

// How often a paused program checks for app events. Short enough to feel instant on return.
constexpr Sint32 k_wait_slice_ms = 50;

core::Error sdl_error(core::ErrorCode code, std::string_view action) {
    return core::Error{code, std::format("{}: {}", action, SDL_GetError())};
}

Key offset_key(Key first, int offset) {
    return static_cast<Key>(static_cast<int>(first) + offset);
}

Key translate_scancode(SDL_Scancode scancode) {
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
        return offset_key(Key::A, scancode - SDL_SCANCODE_A);
    }
    if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
        return offset_key(Key::Digit1, scancode - SDL_SCANCODE_1);
    }
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12) {
        return offset_key(Key::F1, scancode - SDL_SCANCODE_F1);
    }

    switch (scancode) {
    case SDL_SCANCODE_0:
        return Key::Digit0;
    case SDL_SCANCODE_SPACE:
        return Key::Space;
    case SDL_SCANCODE_RETURN:
        return Key::Enter;
    case SDL_SCANCODE_ESCAPE:
        return Key::Escape;
    case SDL_SCANCODE_TAB:
        return Key::Tab;
    case SDL_SCANCODE_BACKSPACE:
        return Key::Backspace;
    case SDL_SCANCODE_DELETE:
        return Key::Delete;
    case SDL_SCANCODE_HOME:
        return Key::Home;
    case SDL_SCANCODE_END:
        return Key::End;
    case SDL_SCANCODE_LEFT:
        return Key::Left;
    case SDL_SCANCODE_RIGHT:
        return Key::Right;
    case SDL_SCANCODE_UP:
        return Key::Up;
    case SDL_SCANCODE_DOWN:
        return Key::Down;
    case SDL_SCANCODE_LSHIFT:
        return Key::LeftShift;
    case SDL_SCANCODE_RSHIFT:
        return Key::RightShift;
    case SDL_SCANCODE_LCTRL:
        return Key::LeftControl;
    case SDL_SCANCODE_RCTRL:
        return Key::RightControl;
    case SDL_SCANCODE_LALT:
        return Key::LeftAlt;
    case SDL_SCANCODE_RALT:
        return Key::RightAlt;
    case SDL_SCANCODE_AC_BACK:
        return Key::Back;
    default:
        return Key::Unknown;
    }
}

std::optional<PointerButton> translate_mouse_button(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return PointerButton::Left;
    case SDL_BUTTON_RIGHT:
        return PointerButton::Right;
    case SDL_BUTTON_MIDDLE:
        return PointerButton::Middle;
    default:
        return std::nullopt;
    }
}

class SdlPlatform final : public Platform {
public:
    SdlPlatform(std::string organization, std::string application)
        : m_organization(std::move(organization)), m_application(std::move(application)) {
        SDL_AddEventWatch(&SdlPlatform::watch_app_event, this);
    }

    ~SdlPlatform() override {
        SDL_RemoveEventWatch(&SdlPlatform::watch_app_event, this);
        if (m_window != nullptr) {
            SDL_DestroyWindow(m_window);
        }
        SDL_Quit();
    }

    SdlPlatform(const SdlPlatform&) = delete;
    SdlPlatform& operator=(const SdlPlatform&) = delete;

    core::Result<void> create_window(const WindowSettings& settings) override {
        m_window = SDL_CreateWindow(settings.title.c_str(), static_cast<int>(settings.width),
                                    static_cast<int>(settings.height),
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (m_window == nullptr) {
            return sdl_error(core::ErrorCode::Unknown, "Could not create a window");
        }
        return {};
    }

    SurfaceSize surface_size() const override {
        int width = 0;
        int height = 0;
        if (m_window != nullptr) {
            SDL_GetWindowSizeInPixels(m_window, &width, &height);
        }
        return SurfaceSize{static_cast<std::uint32_t>(std::max(width, 0)),
                           static_cast<std::uint32_t>(std::max(height, 0))};
    }

    NativeWindow native_window() const override {
        if (m_window == nullptr) {
            return {};
        }
        const SDL_PropertiesID properties = SDL_GetWindowProperties(m_window);

#if defined(SDL_PLATFORM_ANDROID)
        // Null while the app is in the background, because Android has taken the surface away.
        void* const window =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
        return window != nullptr ? NativeWindow{AndroidWindow{window}} : NativeWindow{};
#elif defined(SDL_PLATFORM_WIN32)
        void* const window =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        void* const instance =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
        return window != nullptr ? NativeWindow{Win32Window{instance, window}} : NativeWindow{};
#else
        return {};
#endif
    }

    float display_scale() const override {
        const float scale = m_window != nullptr ? SDL_GetWindowDisplayScale(m_window) : 0.0f;
        return scale > 0.0f ? scale : 1.0f;
    }

    SurfaceRect safe_area() const override {
        const SurfaceSize size = surface_size();
        SurfaceRect whole{0, 0, size.width, size.height};
        SDL_Rect area;
        if (m_window == nullptr || !SDL_GetWindowSafeArea(m_window, &area) || area.w <= 0 ||
            area.h <= 0) {
            return whole;
        }
        // SDL reports window units, which differ from pixels on high-DPI screens.
        const float density = SDL_GetWindowPixelDensity(m_window);
        const auto pixels = [density](int value) {
            return static_cast<std::int32_t>(std::lround(static_cast<float>(value) * density));
        };
        const std::int32_t left = std::clamp(pixels(area.x), 0, static_cast<std::int32_t>(size.width));
        const std::int32_t top = std::clamp(pixels(area.y), 0, static_cast<std::int32_t>(size.height));
        const std::int32_t right = std::clamp(pixels(area.x + area.w), left,
                                              static_cast<std::int32_t>(size.width));
        const std::int32_t bottom = std::clamp(pixels(area.y + area.h), top,
                                               static_cast<std::int32_t>(size.height));
        return SurfaceRect{left, top, static_cast<std::uint32_t>(right - left),
                           static_cast<std::uint32_t>(bottom - top)};
    }

    void set_text_input(bool enabled) override {
        if (m_window == nullptr) {
            return;
        }
        if (enabled) {
            SDL_StartTextInput(m_window);
        } else {
            SDL_StopTextInput(m_window);
        }
    }

    std::optional<Event> poll_event() override {
        SDL_PumpEvents();
        if (std::optional<Event> pending = take_pending()) {
            return pending;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            translate_into_pending(event);
            if (std::optional<Event> pending = take_pending()) {
                return pending;
            }
        }
        return take_pending();
    }

    Event wait_event() override {
        while (true) {
            if (std::optional<Event> pending = take_pending()) {
                return *std::move(pending);
            }

            // SDL_WaitEvent would never wake for the app events, which skip SDL's queue.
            SDL_Event event;
            if (SDL_WaitEventTimeout(&event, k_wait_slice_ms)) {
                translate_into_pending(event);
            }
        }
    }

    core::Result<std::vector<std::byte>> read_file(FileRoot root, std::string_view path) override {
        const core::Result<std::string> full_path = resolve(root, path);
        if (!full_path.has_value()) {
            return full_path.error();
        }

        std::size_t size = 0;
        void* const data = SDL_LoadFile(full_path.value().c_str(), &size);
        if (data == nullptr) {
            return sdl_error(core::ErrorCode::NotFound, std::format("Could not read '{}'", path));
        }

        const auto* const bytes = static_cast<const std::byte*>(data);
        std::vector<std::byte> contents(bytes, bytes + size);
        SDL_free(data);
        return contents;
    }

    core::Result<void> write_user_file(std::string_view path,
                                       std::span<const std::byte> data) override {
        const core::Result<std::string> full_path = resolve(FileRoot::User, path);
        if (!full_path.has_value()) {
            return full_path.error();
        }

        const std::string& target = full_path.value();
        const std::string parent = target.substr(0, target.find_last_of('/'));
        if (!SDL_CreateDirectory(parent.c_str())) {
            return sdl_error(core::ErrorCode::IoError,
                             std::format("Could not create the folder for '{}'", path));
        }

        // Writing to a temporary name first means a crash can't leave a half-written file.
        const std::string temporary = target + ".tmp";
        if (!SDL_SaveFile(temporary.c_str(), data.data(), data.size())) {
            return sdl_error(core::ErrorCode::IoError, std::format("Could not write '{}'", path));
        }
        if (!SDL_RenamePath(temporary.c_str(), target.c_str())) {
            return sdl_error(core::ErrorCode::IoError, std::format("Could not replace '{}'", path));
        }
        return {};
    }

private:
    core::Result<std::string> resolve(FileRoot root, std::string_view path) {
        if (root == FileRoot::User) {
            return user_root(path);
        }

#if defined(SDL_PLATFORM_ANDROID)
        // SDL looks up relative paths inside the APK.
        return std::string(path);
#else
        const char* const base = SDL_GetBasePath();
        if (base == nullptr) {
            return sdl_error(core::ErrorCode::IoError, "Could not find the program's folder");
        }
        return std::format("{}assets/{}", base, path);
#endif
    }

    core::Result<std::string> user_root(std::string_view path) {
        if (m_user_root.empty()) {
            char* const root = SDL_GetPrefPath(m_organization.c_str(), m_application.c_str());
            if (root == nullptr) {
                return sdl_error(core::ErrorCode::IoError, "Could not find user storage");
            }
            m_user_root = root;
            SDL_free(root);
            std::ranges::replace(m_user_root, '\\', '/');
        }
        return std::format("{}{}", m_user_root, path);
    }

    // SDL never queues the app lifecycle events. It only hands them to event watchers, from inside
    // its event pump on Android and possibly from another thread elsewhere.
    static bool SDLCALL watch_app_event(void* user_data, SDL_Event* event) {
        auto* const self = static_cast<SdlPlatform*>(user_data);
        switch (event->type) {
        case SDL_EVENT_TERMINATING:
            self->push_pending(QuitRequested{});
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            self->push_pending(AppPaused{});
            break;
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            self->push_pending(SurfaceLost{});
            break;
        case SDL_EVENT_WILL_ENTER_FOREGROUND:
            self->push_pending(SurfaceRestored{});
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            self->push_pending(AppResumed{});
            break;
        case SDL_EVENT_LOW_MEMORY:
            self->push_pending(LowMemory{});
            break;
        default:
            break;
        }
        return true;
    }

    void push_pending(Event event) {
        const std::lock_guard lock(m_pending_mutex);
        m_pending.push_back(std::move(event));
    }

    std::optional<Event> take_pending() {
        const std::lock_guard lock(m_pending_mutex);
        if (m_pending.empty()) {
            return std::nullopt;
        }
        Event event = std::move(m_pending.front());
        m_pending.pop_front();
        return event;
    }

    void translate_into_pending(const SDL_Event& event) {
        if (std::optional<Event> translated = translate(event)) {
            push_pending(*std::move(translated));
        }
    }

    std::optional<Event> translate(const SDL_Event& event) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            return QuitRequested{};

#if !defined(SDL_PLATFORM_ANDROID)
        // Android reports these through the background events above.
        case SDL_EVENT_WINDOW_MINIMIZED:
            return AppPaused{};
        case SDL_EVENT_WINDOW_RESTORED:
            return AppResumed{};
#endif

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            return SurfaceResized{surface_size()};

        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const Key key = translate_scancode(event.key.scancode);
            if (event.key.repeat || key == Key::Unknown) {
                return std::nullopt;
            }
            return KeyChanged{key, event.key.down};
        }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const std::optional<PointerButton> button = translate_mouse_button(event.button.button);
            if (!button.has_value()) {
                return std::nullopt;
            }
            return PointerButtonChanged{*button, event.button.down};
        }

        case SDL_EVENT_MOUSE_MOTION: {
            // SDL reports the mouse in window units, which differ from pixels on high-DPI screens.
            const float density = m_window != nullptr ? SDL_GetWindowPixelDensity(m_window) : 1.0f;
            return PointerMoved{event.motion.x * density, event.motion.y * density};
        }

        case SDL_EVENT_MOUSE_WHEEL: {
            const float flip = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
            return PointerWheel{event.wheel.x * flip, event.wheel.y * flip};
        }

        case SDL_EVENT_TEXT_INPUT:
            return TextEntered{std::string(event.text.text != nullptr ? event.text.text : "")};

        case SDL_EVENT_FINGER_DOWN:
            ++m_fingers_down;
            if (m_fingers_down != 1) {
                return std::nullopt;
            }
            // Report where the finger is before reporting the press.
            push_pending(finger_position(event));
            return PointerButtonChanged{PointerButton::Touch, true};

        case SDL_EVENT_FINGER_MOTION:
            return m_fingers_down == 1 ? std::optional<Event>(finger_position(event))
                                       : std::nullopt;

        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            m_fingers_down = std::max(m_fingers_down - 1, 0);
            if (m_fingers_down != 0) {
                return std::nullopt;
            }
            return PointerButtonChanged{PointerButton::Touch, false};

        default:
            return std::nullopt;
        }
    }

    Event finger_position(const SDL_Event& event) const {
        const SurfaceSize size = surface_size();
        return PointerMoved{event.tfinger.x * static_cast<float>(size.width),
                            event.tfinger.y * static_cast<float>(size.height)};
    }

    std::string m_organization;
    std::string m_application;
    std::string m_user_root;
    SDL_Window* m_window = nullptr;
    std::mutex m_pending_mutex;
    std::deque<Event> m_pending;
    int m_fingers_down = 0;
};

core::Result<std::unique_ptr<Platform>> create(const core::Config& config) {
    SDL_SetMainReady();

    // By default SDL freezes the program inside its event functions while on Android's background,
    // so the game would only hear about the pause after coming back.
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");

    // Touches are reported as touches, so they don't also arrive as mouse clicks.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        return sdl_error(core::ErrorCode::Unknown, "Could not start SDL");
    }

    return std::unique_ptr<Platform>{std::make_unique<SdlPlatform>(
        std::string(config.get_string("app.organization").value_or("DreamEngine")),
        std::string(config.get_string("app.name").value_or("DreamGame")))};
}

} // namespace

[[nodiscard]] core::Result<void> register_platform_sdl3(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Platform>{.name = "sdl3", .create = &create});
}

} // namespace dream::modules
