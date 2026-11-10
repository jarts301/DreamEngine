#pragma once

#include "dream/core/result.h"
#include "dream/platform/event.h"
#include "dream/ui/draw_list.h"

#include <span>
#include <string_view>

namespace dream::debug_ui {

// Developer tools drawn over the game: windows of values, buttons and sliders, built anew each
// frame. Only what a debug panel needs, so any immediate-mode library can provide it.
//
// Widgets are called between begin_frame and end_frame. A label is also the widget's identity, so
// two widgets in one window need different labels; text after "##" identifies but isn't shown.
class DebugUi {
public:
    static constexpr std::string_view k_module_interface = "debug_ui";

    virtual ~DebugUi() = default;

    // Call once. `host` must outlive this object.
    [[nodiscard]] virtual core::Result<void> initialize(ui::Host& host) = 0;

    // Returns true if the tools used the event, so the game can ignore it.
    virtual bool handle_event(const platform::Event& event) = 0;

    virtual void begin_frame(const ui::FrameInfo& frame) = 0;

    // Returns false if the window is collapsed, in which case skip its contents. end_window is
    // needed either way. With `open`, the window has a close button that clears it.
    virtual bool begin_window(std::string_view title, bool* open = nullptr) = 0;
    virtual void end_window() = 0;

    virtual void text(std::string_view text) = 0;

    // Returns true when clicked.
    virtual bool button(std::string_view label) = 0;

    // These return true when the player changed the value.
    virtual bool checkbox(std::string_view label, bool& value) = 0;
    virtual bool slider(std::string_view label, float& value, float minimum, float maximum) = 0;
    virtual bool slider(std::string_view label, int& value, int minimum, int maximum) = 0;

    // A small line graph of recent values, such as frame times.
    virtual void plot(std::string_view label, std::span<const float> values, float minimum,
                      float maximum) = 0;

    virtual void separator() = 0;

    // Puts the next widget beside the last one.
    virtual void same_line() = 0;

    // Returns the frame's triangles, valid until the next begin_frame.
    [[nodiscard]] virtual const ui::DrawList& end_frame() = 0;

    // Whether the pointer is over a window, or typing goes into a field, as of the last frame.
    [[nodiscard]] virtual bool wants_pointer() const = 0;
    [[nodiscard]] virtual bool wants_keyboard() const = 0;
};

} // namespace dream::debug_ui
