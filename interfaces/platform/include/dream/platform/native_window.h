#pragma once

#include <variant>

namespace dream::platform {

// The operating system's own window objects, passed to the graphics layer as plain pointers so
// neither side needs to know the other's backend. `std::monostate` means no real window exists.

struct Win32Window {
    void* instance = nullptr; // HINSTANCE
    void* window = nullptr;   // HWND
};

struct AndroidWindow {
    void* window = nullptr; // ANativeWindow*
};

using NativeWindow = std::variant<std::monostate, Win32Window, AndroidWindow>;

} // namespace dream::platform
