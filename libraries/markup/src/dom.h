#pragma once

// The document tree, with each element's computed style and layout.

#include "markup/markup.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace markup::detail {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

// A length as written: pixels (dp already scaled), a percentage of the containing block, or auto.
struct Length {
    enum class Unit : std::uint8_t { Auto, Pixels, Percent };
    Unit unit = Unit::Auto;
    float value = 0.0f;

    [[nodiscard]] bool is_auto() const {
        return unit == Unit::Auto;
    }
    // Against a reference size, such as the containing block's width; auto gives `fallback`.
    [[nodiscard]] float resolve(float reference, float fallback = 0.0f) const {
        switch (unit) {
        case Unit::Pixels:
            return value;
        case Unit::Percent:
            return value * 0.01f * reference;
        case Unit::Auto:
            break;
        }
        return fallback;
    }
    [[nodiscard]] static Length pixels(float value) {
        return Length{Unit::Pixels, value};
    }
};

enum class Display : std::uint8_t { None, Block, Inline, InlineBlock };
enum class Position : std::uint8_t { Static, Relative, Absolute };
enum class TextAlign : std::uint8_t { Left, Center, Right };

// Sides in CSS order: top, right, bottom, left.
using Sides = std::array<Length, 4>;
constexpr int k_top = 0;
constexpr int k_right = 1;
constexpr int k_bottom = 2;
constexpr int k_left = 3;

struct Style {
    Display display = Display::Inline;
    Position position = Position::Static;
    Length left;
    Length top;
    Length right;
    Length bottom;
    Length width;
    Length height;
    Sides margin{Length::pixels(0.0f), Length::pixels(0.0f), Length::pixels(0.0f),
                 Length::pixels(0.0f)};
    Sides padding{Length::pixels(0.0f), Length::pixels(0.0f), Length::pixels(0.0f),
                  Length::pixels(0.0f)};
    std::array<float, 4> border{0.0f, 0.0f, 0.0f, 0.0f};
    Color border_color{0.0f, 0.0f, 0.0f, 1.0f};
    float border_radius = 0.0f;
    Color background;
    float opacity = 1.0f;

    // Inherited.
    Color color{1.0f, 1.0f, 1.0f, 1.0f};
    std::string font_family;
    float font_size = 12.0f;
    int font_weight = 400;
    TextAlign text_align = TextAlign::Left;
    float line_height = 1.2f;
    bool visible = true;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    [[nodiscard]] bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

// A word of text placed on a line, with its baseline at `y`.
struct TextRun {
    float x = 0.0f;
    float y = 0.0f;
    std::string text;
};

struct Element {
    // Lower case, or empty for text.
    std::string tag;
    std::string text;
    std::map<std::string, std::string> attributes;
    std::string id;
    std::vector<std::string> classes;
    Element* parent = nullptr;
    std::vector<std::unique_ptr<Element>> children;

    // Parts a form control makes for itself, such as a slider's bar, which style sheets can style
    // but documents don't contain.
    bool internal = false;

    // Interaction state.
    bool hover = false;
    bool active = false;
    bool focus = false;
    bool checked = false;

    // A form control's current value, and where a text field's caret is, in bytes.
    std::string value;
    std::size_t caret = 0;

    Style style;

    // Layout: the border box, and for inline elements one box per line they appear on.
    Rect box;
    std::array<float, 4> margins{};
    std::array<float, 4> paddings{};
    std::vector<Rect> line_boxes;
    std::vector<TextRun> runs;
    // Where an image is, for img elements.
    Image image;
    bool image_loaded = false;

    [[nodiscard]] bool is_text() const {
        return tag.empty();
    }
    [[nodiscard]] bool has_class(std::string_view name) const {
        for (const std::string& c : classes) {
            if (c == name) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] std::string attribute(const std::string& name,
                                        const std::string& fallback = {}) const {
        const auto found = attributes.find(name);
        return found == attributes.end() ? fallback : found->second;
    }
    Element* add_child(std::unique_ptr<Element> child) {
        child->parent = this;
        children.push_back(std::move(child));
        return children.back().get();
    }
};

// What kind of form control an element is, if any.
enum class Control : std::uint8_t { None, Text, Password, Range, Checkbox, Radio, Submit, Button };

[[nodiscard]] Control control_of(const Element& element);

} // namespace markup::detail
