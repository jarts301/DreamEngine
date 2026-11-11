#pragma once

// TrueType fonts, with glyphs rasterized on demand into one shared texture atlas.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace markup::detail {

// Decodes the UTF-8 character at `i` and moves past it. Malformed bytes read as U+FFFD.
[[nodiscard]] std::uint32_t next_codepoint(std::string_view text, std::size_t& i);

struct FontMetrics {
    // In pixels. Descent is below the baseline, so negative.
    float ascent = 0.0f;
    float descent = 0.0f;
    float line_gap = 0.0f;
};

// A glyph's quad relative to the pen on the baseline, and where it is in the atlas.
struct Glyph {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float advance = 0.0f;
    int face = 0;
    int index = 0;
    bool visible = false;
};

class Fonts {
public:
    Fonts();
    ~Fonts();

    Fonts(const Fonts&) = delete;
    Fonts& operator=(const Fonts&) = delete;

    // Keeps the file's bytes. Fails, with a reason, if they aren't a font.
    [[nodiscard]] bool add(std::vector<std::byte> data, std::string& error);
    [[nodiscard]] bool empty() const;

    // The loaded face that best fits a family name (any case, spaces ignored) and weight. The
    // first face loaded stands in for families that aren't loaded.
    [[nodiscard]] int select(std::string_view family, int weight) const;

    [[nodiscard]] FontMetrics metrics(int face, float size) const;

    // Rasterizes the glyph into the atlas the first time it is needed. Characters a face lacks
    // come from the first face.
    [[nodiscard]] const Glyph& glyph(int face, float size, std::uint32_t codepoint);

    // Extra space between two glyphs of the same face.
    [[nodiscard]] float kerning(const Glyph& left, const Glyph& right, float size) const;

    // Width of a run of text on one line.
    [[nodiscard]] float measure(int face, float size, std::string_view text);

    // The atlas as one coverage value per pixel, and whether it changed since it was last taken.
    [[nodiscard]] int atlas_size() const;
    [[nodiscard]] const std::vector<std::uint8_t>& atlas() const;
    [[nodiscard]] bool atlas_changed() const;
    void atlas_taken();

    // Call between layouts: if the atlas filled up, it is emptied so glyphs can be rasterized
    // again.
    void trim();

private:
    struct Face;
    std::vector<std::unique_ptr<Face>> m_faces;
    std::unordered_map<std::uint64_t, Glyph> m_glyphs;
    std::vector<std::uint8_t> m_atlas;
    int m_shelf_x = 1;
    int m_shelf_y = 1;
    int m_shelf_height = 0;
    bool m_changed = false;
    bool m_full = false;
    Glyph m_missing;
};

} // namespace markup::detail
