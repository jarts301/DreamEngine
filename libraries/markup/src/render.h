#pragma once

// Turning laid-out elements into triangles.

#include "dom.h"
#include "fonts.h"

namespace markup::detail {

class Painter {
public:
    Painter(DrawList& list_, Fonts& fonts_, TextureId atlas_, float scale_)
        : list(list_), fonts(fonts_), atlas(atlas_), scale(scale_) {}

    DrawList& list;
    Fonts& fonts;
    TextureId atlas = 0;
    float scale = 1.0f;

    // Draws an element and everything in it.
    void element(const Element& element, float opacity = 1.0f);

private:
    void use(TextureId texture, const std::optional<ClipRect>& clip);
    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
              std::uint32_t color);
    void rounded(const Rect& rect, float radius, std::uint32_t color);
    void border(const Rect& rect, const std::array<float, 4>& widths, float radius,
                std::uint32_t color);
    void text(const Element& owner, const TextRun& run, std::uint32_t color);

    std::optional<ClipRect> m_clip;
};

} // namespace markup::detail
