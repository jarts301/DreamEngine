#include "render.h"

#include "layout.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace markup::detail {
namespace {

std::uint32_t pack(Color color, float opacity) {
    const float alpha = std::clamp(color.a * opacity, 0.0f, 1.0f);
    const auto channel = [&](float value) {
        return static_cast<std::uint32_t>(
            std::lround(std::clamp(value * alpha, 0.0f, 1.0f) * 255.0f));
    };
    return channel(color.r) | (channel(color.g) << 8) | (channel(color.b) << 16) |
           (static_cast<std::uint32_t>(std::lround(alpha * 255.0f)) << 24);
}

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

// The outline of a rectangle with rounded corners, clockwise from the top-left corner's arc.
std::vector<Point> outline(const Rect& rect, float radius, int segments) {
    radius = std::min({radius, 0.5f * rect.width, 0.5f * rect.height});
    std::vector<Point> points;
    const Point centers[4] = {{rect.x + radius, rect.y + radius},
                              {rect.x + rect.width - radius, rect.y + radius},
                              {rect.x + rect.width - radius, rect.y + rect.height - radius},
                              {rect.x + radius, rect.y + rect.height - radius}};
    for (int corner = 0; corner < 4; ++corner) {
        const float start = std::numbers::pi_v<float> * (1.0f + 0.5f * static_cast<float>(corner));
        for (int i = 0; i <= segments; ++i) {
            const float angle = start + 0.5f * std::numbers::pi_v<float> * static_cast<float>(i) /
                                            static_cast<float>(segments);
            points.push_back(Point{centers[corner].x + radius * std::cos(angle),
                                   centers[corner].y + radius * std::sin(angle)});
        }
    }
    return points;
}

int segments_for(float radius) {
    return std::clamp(static_cast<int>(radius / 2.0f), 2, 12);
}

} // namespace

void Painter::use(TextureId texture, const std::optional<ClipRect>& clip) {
    const bool same_clip =
        list.commands.empty() ||
        (list.commands.back().clip.has_value() == clip.has_value() &&
         (!clip.has_value() ||
          (list.commands.back().clip->x == clip->x && list.commands.back().clip->y == clip->y &&
           list.commands.back().clip->width == clip->width &&
           list.commands.back().clip->height == clip->height)));
    if (list.commands.empty() || list.commands.back().texture != texture || !same_clip) {
        list.commands.push_back(
            DrawCommand{static_cast<std::uint32_t>(list.indices.size()), 0, texture, clip});
    }
}

void Painter::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                   std::uint32_t color) {
    const auto base = static_cast<std::uint32_t>(list.vertices.size());
    list.vertices.push_back(Vertex{x0, y0, u0, v0, color});
    list.vertices.push_back(Vertex{x1, y0, u1, v0, color});
    list.vertices.push_back(Vertex{x1, y1, u1, v1, color});
    list.vertices.push_back(Vertex{x0, y1, u0, v1, color});
    for (const std::uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u}) {
        list.indices.push_back(base + index);
    }
    list.commands.back().index_count += 6;
}

void Painter::rounded(const Rect& rect, float radius, std::uint32_t color) {
    if (rect.width <= 0.0f || rect.height <= 0.0f || (color >> 24) == 0) {
        return;
    }
    use(0, m_clip);
    if (radius < 0.5f) {
        quad(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height, 0.0f, 0.0f, 0.0f, 0.0f,
             color);
        return;
    }
    const std::vector<Point> points = outline(rect, radius, segments_for(radius));
    const auto center = static_cast<std::uint32_t>(list.vertices.size());
    list.vertices.push_back(
        Vertex{rect.x + 0.5f * rect.width, rect.y + 0.5f * rect.height, 0.0f, 0.0f, color});
    for (const Point& point : points) {
        list.vertices.push_back(Vertex{point.x, point.y, 0.0f, 0.0f, color});
    }
    const auto count = static_cast<std::uint32_t>(points.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        list.indices.push_back(center);
        list.indices.push_back(center + 1 + i);
        list.indices.push_back(center + 1 + (i + 1) % count);
    }
    list.commands.back().index_count += 3 * count;
}

void Painter::border(const Rect& rect, const std::array<float, 4>& widths, float radius,
                     std::uint32_t color) {
    if ((color >> 24) == 0 ||
        (widths[0] <= 0.0f && widths[1] <= 0.0f && widths[2] <= 0.0f && widths[3] <= 0.0f)) {
        return;
    }
    use(0, m_clip);
    const float top = widths[k_top];
    const float right = widths[k_right];
    const float bottom = widths[k_bottom];
    const float left = widths[k_left];
    if (radius < 0.5f) {
        const float x1 = rect.x + rect.width;
        const float y1 = rect.y + rect.height;
        quad(rect.x, rect.y, x1, rect.y + top, 0, 0, 0, 0, color);
        quad(rect.x, y1 - bottom, x1, y1, 0, 0, 0, 0, color);
        quad(rect.x, rect.y + top, rect.x + left, y1 - bottom, 0, 0, 0, 0, color);
        quad(x1 - right, rect.y + top, x1, y1 - bottom, 0, 0, 0, 0, color);
        return;
    }
    // A ring between the outer outline and the inner one, with matching points.
    const int segments = segments_for(radius);
    const Rect inner{rect.x + left, rect.y + top, rect.width - left - right,
                     rect.height - top - bottom};
    const float thickest = std::max({top, right, bottom, left});
    const std::vector<Point> outer_points = outline(rect, radius, segments);
    const std::vector<Point> inner_points =
        outline(inner, std::max(radius - thickest, 0.0f), segments);
    const auto base = static_cast<std::uint32_t>(list.vertices.size());
    for (std::size_t i = 0; i < outer_points.size(); ++i) {
        list.vertices.push_back(Vertex{outer_points[i].x, outer_points[i].y, 0.0f, 0.0f, color});
        list.vertices.push_back(Vertex{inner_points[i].x, inner_points[i].y, 0.0f, 0.0f, color});
    }
    const auto count = static_cast<std::uint32_t>(outer_points.size());
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t next = (i + 1) % count;
        for (const std::uint32_t index : {base + 2 * i, base + 2 * next, base + 2 * next + 1,
                                          base + 2 * i, base + 2 * next + 1, base + 2 * i + 1}) {
            list.indices.push_back(index);
        }
    }
    list.commands.back().index_count += 6 * count;
}

void Painter::text(const Element& owner, const TextRun& run, std::uint32_t color) {
    const Style& style = owner.style;
    const int face = font_of(style, fonts);
    use(atlas, m_clip);
    float pen = run.x;
    const Glyph* previous = nullptr;
    for (std::size_t i = 0; i < run.text.size();) {
        const Glyph& glyph = fonts.glyph(face, style.font_size, next_codepoint(run.text, i));
        if (previous != nullptr) {
            pen += fonts.kerning(*previous, glyph, style.font_size);
        }
        if (glyph.visible) {
            // Whole pixels keep small text sharp.
            const float x = std::round(pen);
            const float y = std::round(run.y);
            quad(x + glyph.x0, y + glyph.y0, x + glyph.x1, y + glyph.y1, glyph.u0, glyph.v0,
                 glyph.u1, glyph.v1, color);
        }
        pen += glyph.advance;
        previous = &glyph;
    }
}

void Painter::element(const Element& element, float opacity) {
    const Style& style = element.style;
    if (style.display == Display::None) {
        return;
    }
    opacity *= style.opacity;

    if (element.is_text()) {
        if (style.visible) {
            const std::uint32_t color = pack(style.color, opacity);
            for (const TextRun& run : element.runs) {
                text(element, run, color);
            }
        }
        return;
    }

    if (style.visible) {
        const std::vector<Rect> single{element.box};
        const std::vector<Rect>& boxes = element.line_boxes.empty() ? single : element.line_boxes;
        for (const Rect& box : boxes) {
            rounded(box, style.border_radius, pack(style.background, opacity));
            border(box, style.border, style.border_radius, pack(style.border_color, opacity));
        }

        if (element.tag == "img" && element.image.texture != 0) {
            const Rect content{element.box.x + style.border[k_left] + element.paddings[k_left],
                               element.box.y + style.border[k_top] + element.paddings[k_top],
                               element.box.width - style.border[k_left] - style.border[k_right] -
                                   element.paddings[k_left] - element.paddings[k_right],
                               element.box.height - style.border[k_top] - style.border[k_bottom] -
                                   element.paddings[k_top] - element.paddings[k_bottom]};
            use(element.image.texture, m_clip);
            quad(content.x, content.y, content.x + content.width, content.y + content.height, 0.0f,
                 0.0f, 1.0f, 1.0f, pack(Color{1.0f, 1.0f, 1.0f, 1.0f}, opacity));
        }

        // A text field's text stays inside its box, with a caret while it has focus.
        if (!element.runs.empty()) {
            const Rect content{element.box.x + style.border[k_left] + element.paddings[k_left],
                               element.box.y + style.border[k_top] + element.paddings[k_top],
                               element.box.width - style.border[k_left] - style.border[k_right] -
                                   element.paddings[k_left] - element.paddings[k_right],
                               element.box.height - style.border[k_top] - style.border[k_bottom] -
                                   element.paddings[k_top] - element.paddings[k_bottom]};
            const std::optional<ClipRect> saved = m_clip;
            m_clip = ClipRect{static_cast<std::int32_t>(std::floor(content.x)),
                              static_cast<std::int32_t>(std::floor(content.y)),
                              static_cast<std::int32_t>(std::ceil(content.width)) + 1,
                              static_cast<std::int32_t>(std::ceil(content.height)) + 1};
            const std::uint32_t color = pack(style.color, opacity);
            for (const TextRun& run : element.runs) {
                text(element, run, color);
            }
            if (element.focus) {
                const std::string shown = shown_text(element);
                const std::size_t caret = std::min(element.caret, element.value.size());
                // A password's dots don't line up with the value's bytes, so count characters.
                std::size_t characters = 0;
                for (std::size_t i = 0; i < caret;) {
                    (void)next_codepoint(element.value, i);
                    ++characters;
                }
                std::size_t prefix = 0;
                for (std::size_t c = 0; c < characters && prefix < shown.size(); ++c) {
                    (void)next_codepoint(shown, prefix);
                }
                const float x = element.runs.front().x +
                                fonts.measure(font_of(style, fonts), style.font_size,
                                              std::string_view(shown).substr(0, prefix));
                use(0, m_clip);
                quad(std::round(x), content.y, std::round(x) + std::max(scale, 1.0f),
                     content.y + content.height, 0.0f, 0.0f, 0.0f, 0.0f, color);
            }
            m_clip = saved;
        }
    }

    for (const auto& child : element.children) {
        this->element(*child, opacity);
    }
}

} // namespace markup::detail
