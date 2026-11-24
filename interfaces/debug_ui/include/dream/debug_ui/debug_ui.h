#pragma once

#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"
#include "dream/ui/draw_list.h"

#include <span>
#include <string>
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

    // Tools such as the editor's panels use these too (ADR 0046). A backend without them gets the
    // defaults here, which show what they can with the widgets above.

    // Places the next window the first time it opens, in pixels from the top left.
    virtual void next_window_placement(core::Vec2 /*position*/, core::Vec2 /*size*/) {}

    // Text in a quieter color, such as a hint.
    virtual void text_disabled(std::string_view text) {
        this->text(text);
    }

    // Shows `text` while the pointer is over the last widget.
    virtual void tooltip(std::string_view /*text*/) {}

    // A row that can be picked. Returns true when clicked.
    virtual bool selectable(std::string_view label, bool selected) {
        return selected ? button(std::string(label) + " *") : button(label);
    }

    // A node of a tree, open or closed, which can be picked. Returns whether it is open, in which
    // case its children follow, then tree_pop. Sets `clicked` when the node itself was clicked.
    virtual bool tree_node(std::string_view label, bool leaf, bool selected, bool& clicked) {
        static_cast<void>(leaf);
        clicked = selectable(label, selected);
        return false;
    }
    virtual void tree_pop() {}

    // A heading over contents that show while it is open. Returns whether it is.
    virtual bool collapsing_header(std::string_view label) {
        separator();
        text(label);
        return true;
    }

    // Each returns true when the value changed. A drag with `minimum` equal to `maximum` has no
    // limits.
    virtual bool drag(std::string_view /*label*/, std::span<float> /*values*/, float /*speed*/,
                      float /*minimum*/ = 0.0f, float /*maximum*/ = 0.0f) {
        return false;
    }
    virtual bool drag(std::string_view /*label*/, int& /*value*/, float /*speed*/) {
        return false;
    }
    // Red, green and blue, from 0 to 1.
    virtual bool color(std::string_view /*label*/, std::span<float> /*rgb*/) {
        return false;
    }
    // Returns true when the text was changed and then Enter pressed or the field left.
    virtual bool input_text(std::string_view /*label*/, std::string& /*value*/) {
        return false;
    }
    // `selected` indexes `items`.
    virtual bool combo(std::string_view /*label*/, int& /*selected*/,
                       std::span<const std::string> /*items*/) {
        return false;
    }

    // Returns the frame's triangles, valid until the next begin_frame.
    [[nodiscard]] virtual const ui::DrawList& end_frame() = 0;

    // Whether the pointer is over a window, or typing goes into a field, as of the last frame.
    [[nodiscard]] virtual bool wants_pointer() const = 0;
    [[nodiscard]] virtual bool wants_keyboard() const = 0;
    // Whether a text field is being typed in, as of the last frame, so the platform should send
    // typed text, as on a phone it shows the keyboard.
    [[nodiscard]] virtual bool wants_text() const {
        return false;
    }
};

} // namespace dream::debug_ui
