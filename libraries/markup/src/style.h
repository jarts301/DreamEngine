#pragma once

// Matching selectors and computing every element's style.

#include "dom.h"
#include "parser.h"

#include <string_view>

namespace markup::detail {

// Computes the style of `element` and everything below it. `scale` is pixels per dp.
void compute_styles(Element& element, const StyleSheet& sheet, float scale);

[[nodiscard]] bool matches(const Element& element, const Selector& selector);

[[nodiscard]] bool parse_color(std::string_view text, Color& color);

} // namespace markup::detail
