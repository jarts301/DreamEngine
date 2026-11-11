#include "style.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace markup::detail {
namespace {

std::vector<std::string_view> split_words(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        // Keeps parenthesized groups, such as rgba(1, 2, 3, 4), whole.
        const std::size_t start = i;
        int depth = 0;
        while (i < text.size() &&
               (depth > 0 || std::isspace(static_cast<unsigned char>(text[i])) == 0)) {
            depth += text[i] == '(' ? 1 : (text[i] == ')' ? -1 : 0);
            ++i;
        }
        if (i > start) {
            words.push_back(text.substr(start, i - start));
        }
    }
    return words;
}

bool parse_number(std::string_view text, float& value) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// A length in pixels or percent. `em` is the font size that em units refer to.
bool parse_length(std::string_view text, float scale, float em, Length& length) {
    if (text == "auto") {
        length = Length{};
        return true;
    }
    std::size_t unit = 0;
    while (unit < text.size() && (std::isdigit(static_cast<unsigned char>(text[unit])) != 0 ||
                                  text[unit] == '.' || text[unit] == '-' || text[unit] == '+')) {
        ++unit;
    }
    float value = 0.0f;
    if (!parse_number(text.substr(0, unit), value)) {
        return false;
    }
    const std::string_view suffix = text.substr(unit);
    if (suffix.empty() || suffix == "px") {
        length = Length::pixels(value);
    } else if (suffix == "dp") {
        length = Length::pixels(value * scale);
    } else if (suffix == "%") {
        length = Length{Length::Unit::Percent, value};
    } else if (suffix == "em" || suffix == "rem") {
        length = Length::pixels(value * em);
    } else if (suffix == "pt") {
        length = Length::pixels(value * 4.0f / 3.0f);
    } else {
        return false;
    }
    return true;
}

// Fills top, right, bottom, left from one to four values, as CSS shorthands do.
template <typename Parse> bool parse_sides(std::string_view text, Parse parse) {
    const std::vector<std::string_view> words = split_words(text);
    if (words.empty() || words.size() > 4) {
        return false;
    }
    static constexpr std::array<std::array<int, 4>, 4> k_sources{{
        {0, 0, 0, 0},
        {0, 1, 0, 1},
        {0, 1, 2, 1},
        {0, 1, 2, 3},
    }};
    const auto& source = k_sources[words.size() - 1];
    for (int side = 0; side < 4; ++side) {
        if (!parse(side, words[static_cast<std::size_t>(source[static_cast<std::size_t>(side)])])) {
            return false;
        }
    }
    return true;
}

int side_index(std::string_view name) {
    if (name == "top") {
        return k_top;
    }
    if (name == "right") {
        return k_right;
    }
    if (name == "bottom") {
        return k_bottom;
    }
    if (name == "left") {
        return k_left;
    }
    return -1;
}

void apply(Style& style, const Declaration& declaration, float scale, float parent_font_size) {
    const std::string& name = declaration.property;
    const std::string_view value = declaration.value;
    const float em = style.font_size;

    if (name == "display") {
        if (value == "none") {
            style.display = Display::None;
        } else if (value == "inline") {
            style.display = Display::Inline;
        } else if (value == "inline-block") {
            style.display = Display::InlineBlock;
        } else {
            style.display = Display::Block;
        }
    } else if (name == "position") {
        style.position = value == "absolute" || value == "fixed"
                             ? Position::Absolute
                             : (value == "relative" ? Position::Relative : Position::Static);
    } else if (name == "left" || name == "top" || name == "right" || name == "bottom" ||
               name == "width" || name == "height") {
        Length* const target = name == "left"     ? &style.left
                               : name == "top"    ? &style.top
                               : name == "right"  ? &style.right
                               : name == "bottom" ? &style.bottom
                               : name == "width"  ? &style.width
                                                  : &style.height;
        Length length;
        if (parse_length(value, scale, em, length)) {
            *target = length;
        }
    } else if (name == "margin" || name == "padding") {
        Sides& sides = name == "margin" ? style.margin : style.padding;
        Sides parsed = sides;
        if (parse_sides(value, [&](int side, std::string_view word) {
                return parse_length(word, scale, em, parsed[static_cast<std::size_t>(side)]);
            })) {
            sides = parsed;
        }
    } else if (name.starts_with("margin-") || name.starts_with("padding-")) {
        const bool margin = name.starts_with("margin-");
        const int side = side_index(std::string_view(name).substr(margin ? 7 : 8));
        Length length;
        if (side >= 0 && parse_length(value, scale, em, length)) {
            (margin ? style.margin : style.padding)[static_cast<std::size_t>(side)] = length;
        }
    } else if (name == "border" || (name.starts_with("border-") && name != "border-radius")) {
        // border: 2dp #fff, border-width: 1px 2px, border-left: 1px solid red, border-top-color.
        std::string_view rest = std::string_view(name).substr(6);
        int only = -1;
        if (!rest.empty()) {
            rest.remove_prefix(1);
            const std::size_t dash = rest.find('-');
            only = side_index(rest.substr(0, dash));
            rest = only >= 0 ? (dash == std::string_view::npos ? std::string_view{}
                                                               : rest.substr(dash + 1))
                             : rest;
        }
        const auto set_width = [&](int side, float width) {
            if (only < 0 || only == side) {
                style.border[static_cast<std::size_t>(side)] = width;
            }
        };
        if (rest == "width" && only < 0) {
            (void)parse_sides(value, [&](int side, std::string_view word) {
                Length length;
                if (!parse_length(word, scale, em, length)) {
                    return false;
                }
                style.border[static_cast<std::size_t>(side)] = length.resolve(0.0f);
                return true;
            });
        } else if (rest == "color") {
            Color color;
            if (parse_color(split_words(value).empty() ? value : split_words(value).front(),
                            color)) {
                style.border_color = color;
            }
        } else {
            for (const std::string_view word : split_words(value)) {
                Length length;
                Color color;
                if (word == "none" || word == "hidden") {
                    for (int side = 0; side < 4; ++side) {
                        set_width(side, 0.0f);
                    }
                } else if (parse_length(word, scale, em, length)) {
                    for (int side = 0; side < 4; ++side) {
                        set_width(side, length.resolve(0.0f));
                    }
                } else if (parse_color(word, color)) {
                    style.border_color = color;
                }
            }
        }
    } else if (name == "border-radius") {
        Length length;
        const std::vector<std::string_view> words = split_words(value);
        if (!words.empty() && parse_length(words.front(), scale, em, length)) {
            style.border_radius = length.resolve(0.0f);
        }
    } else if (name == "background-color" || name == "background") {
        for (const std::string_view word : split_words(value)) {
            Color color;
            if (parse_color(word, color)) {
                style.background = color;
            }
        }
    } else if (name == "color") {
        Color color;
        if (parse_color(value, color)) {
            style.color = color;
        }
    } else if (name == "opacity") {
        float opacity = 1.0f;
        if (parse_number(value, opacity)) {
            style.opacity = std::clamp(opacity, 0.0f, 1.0f);
        }
    } else if (name == "visibility") {
        style.visible = value != "hidden" && value != "collapse";
    } else if (name == "font-family") {
        std::string family(value.substr(0, value.find(',')));
        std::erase_if(family, [](char c) { return c == '"' || c == '\'' || c == ' '; });
        style.font_family = lower(family);
    } else if (name == "font-size") {
        Length length;
        if (parse_length(value, scale, parent_font_size, length)) {
            style.font_size = std::max(length.resolve(parent_font_size, parent_font_size), 1.0f);
        }
    } else if (name == "font-weight") {
        float weight = 400.0f;
        if (value == "bold" || value == "bolder") {
            style.font_weight = 700;
        } else if (value == "normal" || value == "lighter") {
            style.font_weight = 400;
        } else if (parse_number(value, weight)) {
            style.font_weight = static_cast<int>(weight);
        }
    } else if (name == "font") {
        // Only the size and family are taken from the shorthand.
        for (const std::string_view word : split_words(value)) {
            Length length;
            if (word == "bold") {
                style.font_weight = 700;
            } else if (parse_length(word, scale, parent_font_size, length)) {
                style.font_size =
                    std::max(length.resolve(parent_font_size, parent_font_size), 1.0f);
            }
        }
    } else if (name == "text-align") {
        style.text_align = value == "center"  ? TextAlign::Center
                           : value == "right" ? TextAlign::Right
                                              : TextAlign::Left;
    } else if (name == "line-height") {
        float number = 0.0f;
        Length length;
        if (value == "normal") {
            style.line_height = 1.2f;
        } else if (parse_number(value, number)) {
            style.line_height = std::max(number, 0.0f);
        } else if (parse_length(value, scale, em, length)) {
            style.line_height = length.resolve(100.0f) / std::max(style.font_size, 1.0f);
        }
    }
}

bool matches_compound(const Element& element, const Compound& compound) {
    if (element.is_text()) {
        return false;
    }
    if (!compound.tag.empty() && compound.tag != element.tag) {
        return false;
    }
    if (!compound.id.empty() && compound.id != element.id) {
        return false;
    }
    for (const std::string& name : compound.classes) {
        if (!element.has_class(name)) {
            return false;
        }
    }
    for (const std::string& pseudo : compound.pseudo_classes) {
        const bool state = pseudo == "hover"     ? element.hover
                           : pseudo == "active"  ? element.active
                           : pseudo == "focus"   ? element.focus
                           : pseudo == "checked" ? element.checked
                                                 : false;
        if (!state) {
            return false;
        }
    }
    for (const auto& [name, value] : compound.attributes) {
        const auto found = element.attributes.find(name);
        if (found == element.attributes.end() || (!value.empty() && found->second != value)) {
            return false;
        }
    }
    return true;
}

// Whether compounds [0, index] match, with compound `index` at `element`.
bool matches_from(const Element& element, const Selector& selector, std::size_t index) {
    if (!matches_compound(element, selector.compounds[index])) {
        return false;
    }
    if (index == 0) {
        return true;
    }
    const char combinator = selector.combinators[index - 1];
    for (const Element* ancestor = element.parent; ancestor != nullptr;
         ancestor = ancestor->parent) {
        if (matches_from(*ancestor, selector, index - 1)) {
            return true;
        }
        if (combinator == '>') {
            return false;
        }
    }
    return false;
}

void compute(Element& element, const StyleSheet& sheet, float scale, const Style* parent) {
    Style style;
    if (parent != nullptr) {
        style.color = parent->color;
        style.font_family = parent->font_family;
        style.font_size = parent->font_size;
        style.font_weight = parent->font_weight;
        style.text_align = parent->text_align;
        style.line_height = parent->line_height;
        style.visible = parent->visible;
    }
    if (element.is_text()) {
        style.display = Display::Inline;
        element.style = style;
        return;
    }
    if (element.tag == "body") {
        style.display = Display::Block;
    }

    // Declarations in cascade order: by specificity, then by position in the sheet, then those
    // of the style attribute.
    struct Matched {
        int specificity;
        std::size_t order;
        const Declaration* declaration;
    };
    std::vector<Matched> matched;
    std::size_t order = 0;
    for (const Rule& rule : sheet.rules) {
        int best = -1;
        for (const Selector& selector : rule.selectors) {
            if (selector.specificity > best &&
                matches_from(element, selector, selector.compounds.size() - 1)) {
                best = selector.specificity;
            }
        }
        for (const Declaration& declaration : rule.declarations) {
            if (best >= 0) {
                matched.push_back(Matched{best, order, &declaration});
            }
            ++order;
        }
    }
    std::stable_sort(matched.begin(), matched.end(), [](const Matched& l, const Matched& r) {
        return l.specificity != r.specificity ? l.specificity < r.specificity : l.order < r.order;
    });
    const std::vector<Declaration> inline_style = parse_declarations(element.attribute("style"));
    const float parent_font_size = parent != nullptr ? parent->font_size : style.font_size;

    // Font sizes first, because em units in the other properties depend on them.
    for (int pass = 0; pass < 2; ++pass) {
        const auto apply_if = [&](const Declaration& declaration) {
            const bool font = declaration.property == "font-size" || declaration.property == "font";
            if (font == (pass == 0)) {
                apply(style, declaration, scale, parent_font_size);
            }
        };
        for (const Matched& m : matched) {
            apply_if(*m.declaration);
        }
        for (const Declaration& declaration : inline_style) {
            apply_if(declaration);
        }
    }
    element.style = style;

    for (const std::unique_ptr<Element>& child : element.children) {
        compute(*child, sheet, scale, &element.style);
    }
}

struct Named {
    std::string_view name;
    std::uint32_t rgb;
};

constexpr Named k_named_colors[] = {
    {"black", 0x000000},   {"white", 0xffffff},   {"red", 0xff0000},    {"green", 0x008000},
    {"blue", 0x0000ff},    {"yellow", 0xffff00},  {"cyan", 0x00ffff},   {"aqua", 0x00ffff},
    {"magenta", 0xff00ff}, {"fuchsia", 0xff00ff}, {"gray", 0x808080},   {"grey", 0x808080},
    {"silver", 0xc0c0c0},  {"maroon", 0x800000},  {"olive", 0x808000},  {"lime", 0x00ff00},
    {"teal", 0x008080},    {"navy", 0x000080},    {"purple", 0x800080}, {"orange", 0xffa500},
};

} // namespace

bool matches(const Element& element, const Selector& selector) {
    return !selector.compounds.empty() &&
           matches_from(element, selector, selector.compounds.size() - 1);
}

bool parse_color(std::string_view text, Color& color) {
    const std::string name = lower(text);
    if (name == "transparent") {
        color = Color{};
        return true;
    }
    for (const Named& named : k_named_colors) {
        if (named.name == name) {
            color = Color{static_cast<float>((named.rgb >> 16) & 0xff) / 255.0f,
                          static_cast<float>((named.rgb >> 8) & 0xff) / 255.0f,
                          static_cast<float>(named.rgb & 0xff) / 255.0f, 1.0f};
            return true;
        }
    }
    if (!name.empty() && name[0] == '#') {
        const std::string_view hex = std::string_view(name).substr(1);
        std::uint32_t bits = 0;
        const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), bits, 16);
        if (result.ec != std::errc{} || result.ptr != hex.data() + hex.size()) {
            return false;
        }
        const auto digit = [&](int shift) {
            return static_cast<float>((bits >> shift) & 0xf) / 15.0f;
        };
        const auto pair = [&](int shift) {
            return static_cast<float>((bits >> shift) & 0xff) / 255.0f;
        };
        switch (hex.size()) {
        case 3:
            color = Color{digit(8), digit(4), digit(0), 1.0f};
            return true;
        case 4:
            color = Color{digit(12), digit(8), digit(4), digit(0)};
            return true;
        case 6:
            color = Color{pair(16), pair(8), pair(0), 1.0f};
            return true;
        case 8:
            color = Color{pair(24), pair(16), pair(8), pair(0)};
            return true;
        default:
            return false;
        }
    }
    if (name.starts_with("rgb")) {
        const std::size_t open = name.find('(');
        const std::size_t close = name.find(')');
        if (open == std::string::npos || close == std::string::npos) {
            return false;
        }
        std::vector<float> parts;
        std::string_view list = std::string_view(name).substr(open + 1, close - open - 1);
        while (!list.empty()) {
            const std::size_t comma = std::min(list.find(','), list.size());
            std::string_view part = list.substr(0, comma);
            while (!part.empty() && part.front() == ' ') {
                part.remove_prefix(1);
            }
            while (!part.empty() && part.back() == ' ') {
                part.remove_suffix(1);
            }
            const bool percent = !part.empty() && part.back() == '%';
            float number = 0.0f;
            if (!parse_number(percent ? part.substr(0, part.size() - 1) : part, number)) {
                return false;
            }
            parts.push_back(percent ? number * 2.55f : number);
            list = comma < list.size() ? list.substr(comma + 1) : std::string_view{};
        }
        if (parts.size() < 3) {
            return false;
        }
        // CSS writes alpha from 0 to 1, and RmlUi from 0 to 255; a fraction means the former.
        float alpha = parts.size() > 3 ? parts[3] : 255.0f;
        if (parts.size() > 3 && alpha <= 1.0f && name.find('.') != std::string::npos) {
            alpha *= 255.0f;
        }
        color = Color{
            std::clamp(parts[0] / 255.0f, 0.0f, 1.0f), std::clamp(parts[1] / 255.0f, 0.0f, 1.0f),
            std::clamp(parts[2] / 255.0f, 0.0f, 1.0f), std::clamp(alpha / 255.0f, 0.0f, 1.0f)};
        return true;
    }
    return false;
}

void compute_styles(Element& element, const StyleSheet& sheet, float scale) {
    compute(element, sheet, scale, element.parent != nullptr ? &element.parent->style : nullptr);
}

} // namespace markup::detail
