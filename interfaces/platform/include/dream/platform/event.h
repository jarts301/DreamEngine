#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace dream::platform {

// clang-format off

// Physical key positions, so WASD stays in the same place on any keyboard layout.
enum class Key : std::uint8_t {
    Unknown,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Space, Enter, Escape, Tab, Backspace, Delete, Home, End,
    Left, Right, Up, Down,
    LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt,
    Back, // The system back button on Android.
    Count,
};

// clang-format on

enum class PointerButton : std::uint8_t {
    Left,
    Right,
    Middle,
    Touch, // Held while at least one finger is on the screen.
    Count,
};

struct SurfaceSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const SurfaceSize&, const SurfaceSize&) = default;
};

// A rectangle of the surface, in pixels from its top-left corner.
struct SurfaceRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const SurfaceRect&, const SurfaceRect&) = default;
};

// The user or the system asked the program to close.
struct QuitRequested {};

// The program stopped being visible and should do as little as possible. On Android it may be
// killed at any time after this without further notice, so save anything important here.
struct AppPaused {};
struct AppResumed {};

// The drawing surface was destroyed, or came back. On Android this happens on every trip to the
// background, and anything tied to the surface must be recreated.
struct SurfaceLost {};
struct SurfaceRestored {};

// `size` is in pixels.
struct SurfaceResized {
    SurfaceSize size;
};

// The system is short of memory and may kill the program unless it frees some.
struct LowMemory {};

// Key repeats are not reported.
struct KeyChanged {
    Key key = Key::Unknown;
    bool pressed = false;
};

struct PointerButtonChanged {
    PointerButton button = PointerButton::Left;
    bool pressed = false;
};

// `x` and `y` are in pixels from the top-left corner of the surface.
struct PointerMoved {
    float x = 0.0f;
    float y = 0.0f;
};

// A scroll wheel or trackpad, in lines. Positive `y` scrolls up, and positive `x` to the right.
struct PointerWheel {
    float x = 0.0f;
    float y = 0.0f;
};

// Text typed while text input is on, in UTF-8. One event may hold several characters.
struct TextEntered {
    std::string text;
};

using Event = std::variant<QuitRequested, AppPaused, AppResumed, SurfaceLost, SurfaceRestored,
                           SurfaceResized, LowMemory, KeyChanged, PointerButtonChanged,
                           PointerMoved, PointerWheel, TextEntered>;

} // namespace dream::platform
