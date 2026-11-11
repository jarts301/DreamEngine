#pragma once

// Placing every element's box, and every word of text, for a view of a given size.

#include "dom.h"
#include "fonts.h"

#include <functional>
#include <string_view>

namespace markup::detail {

struct LayoutServices {
    Fonts& fonts;
    float scale = 1.0f;
    // Loads an image named by an img element, relative to the document.
    std::function<Image(const Element&)> load_image;
};

// Lays out a document's body, which fills the view.
void layout_document(Element& body, float width, float height, LayoutServices& services);

// The text a text field shows: its value, or dots for a password.
[[nodiscard]] std::string shown_text(const Element& field);

// The font a text element, or a text field, is drawn with.
[[nodiscard]] int font_of(const Style& style, Fonts& fonts);

// Whether an element is drawn as one atomic box in a line of text, like a form control.
[[nodiscard]] bool is_replaced(const Element& element);

// A slider's value from 0 to 1 along its range.
[[nodiscard]] float slider_fraction(const Element& slider);

} // namespace markup::detail
