#include "layout.h"

#include "parser.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace markup::detail {

Control control_of(const Element& element) {
    if (element.tag == "textarea") {
        return Control::Text;
    }
    if (element.tag != "input") {
        return Control::None;
    }
    const std::string type = lower(element.attribute("type", "text"));
    if (type == "password") {
        return Control::Password;
    }
    if (type == "range") {
        return Control::Range;
    }
    if (type == "checkbox") {
        return Control::Checkbox;
    }
    if (type == "radio") {
        return Control::Radio;
    }
    if (type == "submit") {
        return Control::Submit;
    }
    if (type == "button") {
        return Control::Button;
    }
    return Control::Text;
}

bool is_replaced(const Element& element) {
    return element.tag == "input" || element.tag == "textarea" || element.tag == "img";
}

std::string shown_text(const Element& field) {
    if (control_of(field) == Control::Password) {
        std::size_t characters = 0;
        for (std::size_t i = 0; i < field.value.size();) {
            (void)next_codepoint(field.value, i);
            ++characters;
        }
        return std::string(characters, '*');
    }
    if (control_of(field) == Control::Submit || control_of(field) == Control::Button) {
        return field.attribute("value");
    }
    return field.value;
}

int font_of(const Style& style, Fonts& fonts) {
    return fonts.select(style.font_family, style.font_weight);
}

namespace {

float number(const std::string& text, float fallback) {
    float value = fallback;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} ? value : fallback;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// How far text of a style reaches above and below its baseline, with half the leading on each
// side, as CSS places it.
struct Extent {
    float above = 0.0f;
    float below = 0.0f;
};

Extent text_extent(const Style& style, Fonts& fonts) {
    const FontMetrics metrics = fonts.metrics(font_of(style, fonts), style.font_size);
    const float content = metrics.ascent - metrics.descent;
    const float half_leading = 0.5f * (style.font_size * style.line_height - content);
    return Extent{metrics.ascent + half_leading, -metrics.descent + half_leading};
}

// One piece of a line of text.
struct Item {
    enum class Kind : std::uint8_t { Word, Space, Atom, Open, Close, Break };
    Kind kind = Kind::Word;
    Element* element = nullptr;
    std::string text;
    float width = 0.0f;
    Extent extent;
    float x = 0.0f;
    int line = -1;
};

struct Line {
    std::size_t first = 0;
    std::size_t end = 0;
    float width = 0.0f;
};

} // namespace

float slider_fraction(const Element& slider) {
    const float low = number(slider.attribute("min", "0"), 0.0f);
    const float high = number(slider.attribute("max", "100"), 100.0f);
    const float value = number(slider.value, low);
    return high > low ? std::clamp((value - low) / (high - low), 0.0f, 1.0f) : 0.0f;
}

namespace {

class Layout {
public:
    explicit Layout(LayoutServices& services) : m_services(services) {}

    void document(Element& body, float width, float height) {
        clear(body);
        const Rect view{0.0f, 0.0f, width, height};
        (void)block(body, 0.0f, 0.0f, width, view, false);
    }

private:
    struct Edges {
        std::array<float, 4> margin{};
        std::array<float, 4> padding{};
        std::array<float, 4> border{};

        [[nodiscard]] float horizontal() const {
            return margin[k_left] + margin[k_right] + padding[k_left] + padding[k_right] +
                   border[k_left] + border[k_right];
        }
        [[nodiscard]] float vertical() const {
            return margin[k_top] + margin[k_bottom] + padding[k_top] + padding[k_bottom] +
                   border[k_top] + border[k_bottom];
        }
    };

    static Edges edges(const Element& element, float reference) {
        Edges result;
        for (std::size_t side = 0; side < 4; ++side) {
            result.margin[side] = element.style.margin[side].resolve(reference);
            result.padding[side] = element.style.padding[side].resolve(reference);
            result.border[side] = element.style.border[side];
        }
        return result;
    }

    float dp(float value) const {
        return value * m_services.scale;
    }

    static void clear(Element& element) {
        element.box = Rect{};
        element.line_boxes.clear();
        element.runs.clear();
        for (const auto& child : element.children) {
            clear(*child);
        }
    }

    static void translate(Element& element, float dx, float dy) {
        element.box.x += dx;
        element.box.y += dy;
        for (Rect& line : element.line_boxes) {
            line.x += dx;
            line.y += dy;
        }
        for (TextRun& run : element.runs) {
            run.x += dx;
            run.y += dy;
        }
        for (const auto& child : element.children) {
            translate(*child, dx, dy);
        }
    }

    // --- Sizes of content ---

    // The size of a form control or image when the style doesn't give one.
    void intrinsic_size(Element& element, float& width, float& height) {
        const Style& style = element.style;
        const float line = style.font_size * style.line_height;
        if (element.tag == "img") {
            if (!element.image_loaded) {
                element.image = m_services.load_image(element);
                element.image_loaded = true;
            }
            width = number(element.attribute("width"), static_cast<float>(element.image.width));
            height = number(element.attribute("height"), static_cast<float>(element.image.height));
            return;
        }
        switch (control_of(element)) {
        case Control::Range:
            width = dp(100.0f);
            height = dp(16.0f);
            return;
        case Control::Checkbox:
        case Control::Radio:
            width = dp(16.0f);
            height = dp(16.0f);
            return;
        case Control::Submit:
        case Control::Button:
            width = m_services.fonts.measure(font_of(style, m_services.fonts), style.font_size,
                                             shown_text(element));
            height = line;
            return;
        case Control::Text:
        case Control::Password:
            width = dp(150.0f);
            height = element.tag == "textarea" ? 3.0f * line : line;
            return;
        case Control::None:
            break;
        }
        width = 0.0f;
        height = 0.0f;
    }

    // The widest the content box would be if nothing wrapped.
    float max_content(Element& element) {
        if (is_replaced(element)) {
            float width = 0.0f;
            float height = 0.0f;
            intrinsic_size(element, width, height);
            return element.style.width.unit == Length::Unit::Pixels ? element.style.width.value
                                                                    : width;
        }
        float widest = 0.0f;
        float line = 0.0f;
        // A space seen at the end of some text counts once something follows it on the line,
        // whether more text or an inline element.
        float pending_space = 0.0f;
        for (const auto& pointer : element.children) {
            Element& child = *pointer;
            const Style& style = child.style;
            if (style.display == Display::None || style.position == Position::Absolute) {
                continue;
            }
            if (child.is_text()) {
                const int face = font_of(style, m_services.fonts);
                const float space = m_services.fonts.measure(face, style.font_size, " ");
                std::size_t i = 0;
                while (i < child.text.size()) {
                    if (is_space(child.text[i])) {
                        pending_space = line > 0.0f ? space : 0.0f;
                        ++i;
                        continue;
                    }
                    const std::size_t start = i;
                    while (i < child.text.size() && !is_space(child.text[i])) {
                        ++i;
                    }
                    line += pending_space;
                    pending_space = 0.0f;
                    line += m_services.fonts.measure(
                        face, style.font_size,
                        std::string_view(child.text).substr(start, i - start));
                }
                continue;
            }
            if (child.tag == "br") {
                widest = std::max(widest, line);
                line = 0.0f;
                pending_space = 0.0f;
                continue;
            }
            const Edges e = edges(child, 0.0f);
            const float inner =
                style.width.unit == Length::Unit::Pixels ? style.width.value : max_content(child);
            if (style.display == Display::Block) {
                widest = std::max({widest, line, inner + e.horizontal()});
                line = 0.0f;
                pending_space = 0.0f;
            } else {
                line += pending_space + inner + e.horizontal();
                pending_space = 0.0f;
            }
        }
        return std::max(widest, line);
    }

    // --- Block boxes ---

    // Places a box with its margin edge's top-left at (x, y), and returns its margin box height.
    float block(Element& element, float x, float y, float available, const Rect& containing,
                bool shrink, std::optional<float> forced_width = std::nullopt,
                std::optional<float> forced_height = std::nullopt) {
        const Style& style = element.style;
        Edges e = edges(element, containing.width);
        float width = 0.0f;
        if (forced_width.has_value()) {
            width = *forced_width;
        } else if (!style.width.is_auto()) {
            width = std::max(style.width.resolve(containing.width), 0.0f);
        } else if (shrink || is_replaced(element)) {
            width =
                std::clamp(max_content(element), 0.0f, std::max(available - e.horizontal(), 0.0f));
        } else {
            width = std::max(available - e.horizontal(), 0.0f);
        }
        // Auto side margins center a block of known width.
        if (!shrink && !style.width.is_auto() && style.margin[k_left].is_auto() &&
            style.margin[k_right].is_auto()) {
            const float room = std::max(available - width - e.horizontal(), 0.0f);
            e.margin[k_left] = e.margin[k_right] = 0.5f * room;
        }
        element.margins = e.margin;
        element.paddings = e.padding;

        const float content_x = x + e.margin[k_left] + e.border[k_left] + e.padding[k_left];
        const float content_y = y + e.margin[k_top] + e.border[k_top] + e.padding[k_top];
        const bool positioned = style.position != Position::Static || element.tag == "body";
        if (positioned) {
            m_absolute.emplace_back();
        }

        const std::optional<float> fixed_height =
            forced_height.has_value()
                ? forced_height
                : (style.height.is_auto() ? std::nullopt
                                          : std::optional<float>(std::max(
                                                style.height.resolve(containing.height), 0.0f)));
        float content_height = 0.0f;
        if (is_replaced(element)) {
            float intrinsic_width = 0.0f;
            intrinsic_size(element, intrinsic_width, content_height);
        } else {
            const Rect inner{content_x, content_y, width, fixed_height.value_or(0.0f)};
            content_height = flow(element, content_x, content_y, width, inner);
        }
        const float height = fixed_height.value_or(content_height);

        element.box = Rect{
            x + e.margin[k_left], y + e.margin[k_top],
            width + e.padding[k_left] + e.padding[k_right] + e.border[k_left] + e.border[k_right],
            height + e.padding[k_top] + e.padding[k_bottom] + e.border[k_top] + e.border[k_bottom]};
        if (is_replaced(element)) {
            place_control(element, Rect{content_x, content_y, width, height});
        }

        if (positioned) {
            std::vector<Element*> absolute = std::move(m_absolute.back());
            m_absolute.pop_back();
            // As in CSS, a body that isn't positioned leaves the view as the containing block, so
            // a body that is only as tall as its content doesn't squeeze what is positioned in it.
            const Rect padding_box =
                element.tag == "body" && style.position == Position::Static
                    ? containing
                    : Rect{element.box.x + e.border[k_left], element.box.y + e.border[k_top],
                           width + e.padding[k_left] + e.padding[k_right],
                           height + e.padding[k_top] + e.padding[k_bottom]};
            for (Element* child : absolute) {
                place_absolute(*child, padding_box);
            }
        }
        if (style.position == Position::Relative) {
            const float dx = !style.left.is_auto()    ? style.left.resolve(containing.width)
                             : !style.right.is_auto() ? -style.right.resolve(containing.width)
                                                      : 0.0f;
            const float dy = !style.top.is_auto()      ? style.top.resolve(containing.height)
                             : !style.bottom.is_auto() ? -style.bottom.resolve(containing.height)
                                                       : 0.0f;
            translate(element, dx, dy);
        }
        return element.box.height + e.margin[k_top] + e.margin[k_bottom];
    }

    // The parts of a control that depend on its content box, such as a slider's bar.
    void place_control(Element& element, const Rect& content) {
        const Control control = control_of(element);
        if (control == Control::Range) {
            for (const auto& pointer : element.children) {
                Element& part = *pointer;
                const Edges e = edges(part, content.width);
                const Style& style = part.style;
                if (part.tag == "slidertrack") {
                    const float height = style.height.resolve(content.height, content.height);
                    part.box =
                        Rect{content.x + e.margin[k_left], content.y + e.margin[k_top],
                             std::max(content.width - e.margin[k_left] - e.margin[k_right], 0.0f),
                             height};
                } else if (part.tag == "sliderbar") {
                    const float width = style.width.resolve(content.width, content.height);
                    const float height = style.height.resolve(content.height, content.height);
                    const float travel = std::max(content.width - width, 0.0f);
                    part.box = Rect{content.x + travel * slider_fraction(element),
                                    content.y + e.margin[k_top], width, height};
                }
            }
        } else if (control == Control::Text || control == Control::Password ||
                   control == Control::Submit || control == Control::Button) {
            const Extent extent = text_extent(element.style, m_services.fonts);
            float x = content.x;
            if (control == Control::Submit || control == Control::Button) {
                const float text_width =
                    m_services.fonts.measure(font_of(element.style, m_services.fonts),
                                             element.style.font_size, shown_text(element));
                x += std::max(0.5f * (content.width - text_width), 0.0f);
            }
            element.runs.push_back(TextRun{x, content.y + extent.above, shown_text(element)});
        }
    }

    void place_absolute(Element& element, const Rect& containing) {
        const Style& style = element.style;
        const Edges e = edges(element, containing.width);
        const bool has_left = !style.left.is_auto();
        const bool has_right = !style.right.is_auto();
        const bool has_top = !style.top.is_auto();
        const bool has_bottom = !style.bottom.is_auto();
        const float left = style.left.resolve(containing.width);
        const float right = style.right.resolve(containing.width);
        const float top = style.top.resolve(containing.height);
        const float bottom = style.bottom.resolve(containing.height);

        float width = 0.0f;
        if (!style.width.is_auto()) {
            width = std::max(style.width.resolve(containing.width), 0.0f);
        } else if (has_left && has_right) {
            width = std::max(containing.width - left - right - e.horizontal(), 0.0f);
        } else {
            const float room = containing.width - (has_left ? left : 0.0f) -
                               (has_right ? right : 0.0f) - e.horizontal();
            width = std::clamp(max_content(element), 0.0f, std::max(room, 0.0f));
        }
        std::optional<float> height;
        if (style.height.is_auto() && has_top && has_bottom) {
            height = std::max(containing.height - top - bottom - e.vertical(), 0.0f);
        }

        const float outer_width = width + e.horizontal();
        const float x = has_left    ? containing.x + left
                        : has_right ? containing.x + containing.width - right - outer_width
                                    : containing.x;
        const float y = containing.y + (has_top ? top : 0.0f);
        const float outer_height =
            block(element, x, y, outer_width, containing, false, width, height);
        if (!has_top && has_bottom) {
            translate(element, 0.0f, containing.y + containing.height - bottom - outer_height - y);
        }
    }

    // --- Flow ---

    // Lays out an element's children inside its content box and returns their height.
    float flow(Element& element, float x, float y, float width, const Rect& containing) {
        float cursor = y;
        std::vector<Element*> run;
        const auto flush = [&] {
            if (!run.empty()) {
                cursor += lines(element, run, x, cursor, width, containing);
                run.clear();
            }
        };
        for (const auto& pointer : element.children) {
            Element& child = *pointer;
            if (child.internal) {
                continue;
            }
            const Style& style = child.style;
            if (style.display == Display::None) {
                continue;
            }
            if (style.position == Position::Absolute) {
                if (!m_absolute.empty()) {
                    m_absolute.back().push_back(&child);
                }
                continue;
            }
            if (!child.is_text() && style.display == Display::Block) {
                flush();
                cursor += block(child, x, cursor, width, containing, false);
            } else {
                run.push_back(&child);
            }
        }
        flush();
        return cursor - y;
    }

    void collect(Element& element, std::vector<Item>& items, float width, const Rect& containing) {
        const Style& style = element.style;
        if (style.display == Display::None) {
            return;
        }
        if (style.position == Position::Absolute) {
            if (!m_absolute.empty()) {
                m_absolute.back().push_back(&element);
            }
            return;
        }
        if (element.is_text()) {
            const int face = font_of(style, m_services.fonts);
            const Extent extent = text_extent(style, m_services.fonts);
            const float space = m_services.fonts.measure(face, style.font_size, " ");
            std::size_t i = 0;
            while (i < element.text.size()) {
                if (is_space(element.text[i])) {
                    while (i < element.text.size() && is_space(element.text[i])) {
                        ++i;
                    }
                    if (items.empty() || items.back().kind != Item::Kind::Space) {
                        items.push_back(Item{.kind = Item::Kind::Space,
                                             .element = &element,
                                             .width = space,
                                             .extent = extent});
                    }
                    continue;
                }
                const std::size_t start = i;
                while (i < element.text.size() && !is_space(element.text[i])) {
                    ++i;
                }
                std::string word = element.text.substr(start, i - start);
                const float measured = m_services.fonts.measure(face, style.font_size, word);
                items.push_back(Item{.kind = Item::Kind::Word,
                                     .element = &element,
                                     .text = std::move(word),
                                     .width = measured,
                                     .extent = extent});
            }
            return;
        }
        if (element.tag == "br") {
            items.push_back(Item{.kind = Item::Kind::Break,
                                 .element = &element,
                                 .extent = text_extent(style, m_services.fonts)});
            return;
        }
        if (style.display == Display::Inline && !is_replaced(element)) {
            const Edges e = edges(element, containing.width);
            element.margins = e.margin;
            element.paddings = e.padding;
            const Extent extent = text_extent(style, m_services.fonts);
            items.push_back(Item{.kind = Item::Kind::Open,
                                 .element = &element,
                                 .width = e.margin[k_left] + e.border[k_left] + e.padding[k_left],
                                 .extent = extent});
            for (const auto& child : element.children) {
                if (!child->internal) {
                    collect(*child, items, width, containing);
                }
            }
            items.push_back(
                Item{.kind = Item::Kind::Close,
                     .element = &element,
                     .width = e.margin[k_right] + e.border[k_right] + e.padding[k_right],
                     .extent = extent});
            return;
        }
        // Inline blocks and controls are laid out now, and moved into place with their line.
        const float height = block(element, 0.0f, 0.0f, width, containing, true);
        const Edges e = edges(element, containing.width);
        items.push_back(Item{.kind = Item::Kind::Atom,
                             .element = &element,
                             .width = element.box.width + e.margin[k_left] + e.margin[k_right],
                             .extent = Extent{height, 0.0f}});
    }

    // Lays out a run of inline content in lines and returns their total height.
    float lines(Element& container, const std::vector<Element*>& run, float x, float y, float width,
                const Rect& containing) {
        std::vector<Item> items;
        for (Element* element : run) {
            collect(*element, items, width, containing);
        }

        // Breaks the items into lines, dropping spaces where lines start and end.
        std::vector<Line> result;
        Line line;
        float pen = 0.0f;
        bool has_content = false;
        const auto end_line = [&](std::size_t end) {
            line.end = end;
            std::size_t last = end;
            while (last > line.first && items[last - 1].kind == Item::Kind::Space) {
                items[last - 1].line = -1;
                --last;
            }
            line.width = 0.0f;
            for (std::size_t i = line.first; i < last; ++i) {
                if (items[i].line >= 0) {
                    line.width = std::max(line.width, items[i].x + items[i].width);
                }
            }
            if (has_content) {
                result.push_back(line);
            }
            line = Line{.first = end};
            pen = 0.0f;
            has_content = false;
        };
        for (std::size_t i = 0; i < items.size(); ++i) {
            Item& item = items[i];
            const auto line_index = static_cast<int>(result.size());
            switch (item.kind) {
            case Item::Kind::Break:
                item.line = line_index;
                has_content = true;
                end_line(i + 1);
                continue;
            case Item::Kind::Space:
                if (pen == 0.0f && !has_content) {
                    continue;
                }
                break;
            case Item::Kind::Word:
            case Item::Kind::Atom:
                if (has_content && pen + item.width > width + 0.01f) {
                    end_line(i);
                }
                break;
            case Item::Kind::Open:
            case Item::Kind::Close:
                break;
            }
            item.line = static_cast<int>(result.size());
            item.x = pen;
            pen += item.width;
            has_content = true;
        }
        end_line(items.size());

        // Places the lines one below the other, aligned, with a shared baseline each.
        const Extent strut = text_extent(container.style, m_services.fonts);
        const float align = container.style.text_align == TextAlign::Center  ? 0.5f
                            : container.style.text_align == TextAlign::Right ? 1.0f
                                                                             : 0.0f;
        float top = y;
        std::vector<float> baselines(result.size(), 0.0f);
        for (std::size_t index = 0; index < result.size(); ++index) {
            const Line& current = result[index];
            Extent extent = strut;
            for (std::size_t i = current.first; i < current.end; ++i) {
                if (items[i].line == static_cast<int>(index)) {
                    extent.above = std::max(extent.above, items[i].extent.above);
                    extent.below = std::max(extent.below, items[i].extent.below);
                }
            }
            const float baseline = top + extent.above;
            baselines[index] = baseline;
            const float shift = x + std::max(width - current.width, 0.0f) * align;
            for (std::size_t i = current.first; i < current.end; ++i) {
                Item& item = items[i];
                if (item.line != static_cast<int>(index)) {
                    continue;
                }
                item.x += shift;
                if (item.kind == Item::Kind::Word) {
                    item.element->runs.push_back(TextRun{item.x, baseline, item.text});
                } else if (item.kind == Item::Kind::Atom) {
                    translate(*item.element, item.x, baseline - item.extent.above);
                }
            }
            top = baseline + extent.below;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].kind == Item::Kind::Open && items[i].line >= 0) {
                inline_boxes(items, i, baselines);
            }
        }
        return top - y;
    }

    // The boxes of an inline element: one per line its content is on, around its font's height.
    void inline_boxes(const std::vector<Item>& items, std::size_t open,
                      const std::vector<float>& baselines) {
        Element& element = *items[open].element;
        std::size_t close = open + 1;
        while (close < items.size() &&
               !(items[close].element == &element && items[close].kind == Item::Kind::Close)) {
            ++close;
        }
        close = std::min(close, items.size() - 1);

        const FontMetrics metrics = m_services.fonts.metrics(
            font_of(element.style, m_services.fonts), element.style.font_size);
        const std::array<float, 4>& border = element.style.border;
        const float above = metrics.ascent + element.paddings[k_top] + border[k_top];
        const float height = metrics.ascent - metrics.descent + element.paddings[k_top] +
                             element.paddings[k_bottom] + border[k_top] + border[k_bottom];
        element.line_boxes.clear();
        int current_line = -1;
        for (std::size_t i = open; i <= close; ++i) {
            const Item& item = items[i];
            if (item.line < 0) {
                continue;
            }
            const float left = item.x + (i == open ? element.margins[k_left] : 0.0f);
            const float right =
                item.x + item.width - (i == close ? element.margins[k_right] : 0.0f);
            if (item.line != current_line) {
                current_line = item.line;
                element.line_boxes.push_back(Rect{
                    left, baselines[static_cast<std::size_t>(item.line)] - above, 0.0f, height});
            }
            Rect& box = element.line_boxes.back();
            box.width = std::max(box.width, right - box.x);
        }
        if (element.line_boxes.empty()) {
            return;
        }
        Rect all = element.line_boxes.front();
        for (const Rect& r : element.line_boxes) {
            const float right = std::max(all.x + all.width, r.x + r.width);
            const float bottom = std::max(all.y + all.height, r.y + r.height);
            all.x = std::min(all.x, r.x);
            all.y = std::min(all.y, r.y);
            all.width = right - all.x;
            all.height = bottom - all.y;
        }
        element.box = all;
    }

    LayoutServices& m_services;
    std::vector<std::vector<Element*>> m_absolute;
};

} // namespace

void layout_document(Element& body, float width, float height, LayoutServices& services) {
    Layout layout(services);
    layout.document(body, width, height);
}

} // namespace markup::detail
