#pragma once

#include <variant>

namespace dream::platform {

// The window objects a platform hands the graphics layer, as plain pointers so neither side needs
// to know the other's backend. `std::monostate` means no real window exists.

// A web page that shows the program's frames in a browser (ADR 0039). `canvas` is a
// platform::WebCanvas, declared in web_canvas.h, which outlives every device attached to it.
struct WebWindow {
    void* canvas = nullptr;
};

using NativeWindow = std::variant<std::monostate, WebWindow>;

} // namespace dream::platform
