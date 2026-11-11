#include "fonts.h"

#include "parser.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// stb_truetype's functions are made static, so another copy of it in the same program can't clash.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace markup::detail {
namespace {

constexpr int k_atlas_size = 1024;

std::uint32_t read_u16(const unsigned char* p) {
    return (std::uint32_t{p[0]} << 8) | p[1];
}

std::uint32_t read_u32(const unsigned char* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) |
           p[3];
}

// The weight a font records in its OS/2 table, 400 if it doesn't say.
int font_weight(const std::vector<unsigned char>& data, int start) {
    if (static_cast<std::size_t>(start) + 12 > data.size()) {
        return 400;
    }
    const std::uint32_t tables = read_u16(data.data() + start + 4);
    for (std::uint32_t i = 0; i < tables; ++i) {
        const std::size_t record = static_cast<std::size_t>(start) + 12 + 16 * i;
        if (record + 16 > data.size()) {
            break;
        }
        if (std::memcmp(data.data() + record, "OS/2", 4) == 0) {
            const std::uint32_t offset = read_u32(data.data() + record + 8);
            if (offset + 6 <= data.size()) {
                return static_cast<int>(read_u16(data.data() + offset + 4));
            }
        }
    }
    return 400;
}

// The family name, from the Windows name record (UTF-16) or else the Mac one (Roman).
std::string family_name(const stbtt_fontinfo& info) {
    int length = 0;
    const char* name = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT,
                                               STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
    std::string family;
    if (name != nullptr) {
        for (int i = 0; i + 1 < length; i += 2) {
            const auto high = static_cast<unsigned char>(name[i]);
            const auto low = static_cast<unsigned char>(name[i + 1]);
            if (high == 0 && low < 0x80) {
                family += static_cast<char>(low);
            }
        }
    } else {
        name = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN,
                                       STBTT_MAC_LANG_ENGLISH, 1);
        if (name != nullptr) {
            family.assign(name, static_cast<std::size_t>(length));
        }
    }
    std::erase(family, ' ');
    return lower(family);
}

std::uint64_t glyph_key(int face, int size, std::uint32_t codepoint) {
    return (std::uint64_t(static_cast<std::uint32_t>(face)) << 48) |
           (std::uint64_t(static_cast<std::uint32_t>(size)) << 32) | codepoint;
}

int pixel_size(float size) {
    return std::max(1, static_cast<int>(std::lround(size)));
}

} // namespace

struct Fonts::Face {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    std::string family;
    int weight = 400;
};

std::uint32_t next_codepoint(std::string_view text, std::size_t& i) {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char first = byte(i);
    int extra = 0;
    std::uint32_t code = first;
    if (first >= 0xf0) {
        extra = 3;
        code = first & 0x07u;
    } else if (first >= 0xe0) {
        extra = 2;
        code = first & 0x0fu;
    } else if (first >= 0xc0) {
        extra = 1;
        code = first & 0x1fu;
    } else if (first >= 0x80) {
        ++i;
        return 0xfffd;
    }
    ++i;
    for (int k = 0; k < extra; ++k) {
        if (i >= text.size() || (byte(i) & 0xc0u) != 0x80u) {
            return 0xfffd;
        }
        code = (code << 6) | (byte(i) & 0x3fu);
        ++i;
    }
    return code;
}

Fonts::Fonts() : m_atlas(static_cast<std::size_t>(k_atlas_size) * k_atlas_size, 0) {}

Fonts::~Fonts() = default;

bool Fonts::add(std::vector<std::byte> bytes, std::string& error) {
    auto face = std::make_unique<Face>();
    face->data.resize(bytes.size());
    std::memcpy(face->data.data(), bytes.data(), bytes.size());
    const int start = stbtt_GetFontOffsetForIndex(face->data.data(), 0);
    if (bytes.size() < 12 || start < 0 ||
        stbtt_InitFont(&face->info, face->data.data(), start) == 0) {
        error = "The file is not a TrueType or OpenType font";
        return false;
    }
    face->family = family_name(face->info);
    face->weight = font_weight(face->data, start);
    m_faces.push_back(std::move(face));
    return true;
}

bool Fonts::empty() const {
    return m_faces.empty();
}

int Fonts::select(std::string_view family, int weight) const {
    int best = 0;
    int best_score = -1;
    for (std::size_t i = 0; i < m_faces.size(); ++i) {
        const Face& face = *m_faces[i];
        // The family matters most, then how close the weight is.
        const int score =
            (face.family == family ? 10000 : 0) + 1000 - std::abs(face.weight - weight);
        if (score > best_score) {
            best_score = score;
            best = static_cast<int>(i);
        }
    }
    // Without the family, the first face stands in, in the nearest weight of its family.
    if (best_score < 10000 && !m_faces.empty()) {
        const std::string& fallback = m_faces.front()->family;
        best_score = -1;
        for (std::size_t i = 0; i < m_faces.size(); ++i) {
            const Face& face = *m_faces[i];
            if (face.family == fallback && 1000 - std::abs(face.weight - weight) > best_score) {
                best_score = 1000 - std::abs(face.weight - weight);
                best = static_cast<int>(i);
            }
        }
    }
    return best;
}

FontMetrics Fonts::metrics(int face, float size) const {
    if (m_faces.empty()) {
        return FontMetrics{0.8f * size, -0.2f * size, 0.0f};
    }
    const stbtt_fontinfo& info = m_faces[static_cast<std::size_t>(face)]->info;
    int ascent = 0;
    int descent = 0;
    int gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    const float scale =
        stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(pixel_size(size)));
    return FontMetrics{static_cast<float>(ascent) * scale, static_cast<float>(descent) * scale,
                       static_cast<float>(gap) * scale};
}

const Glyph& Fonts::glyph(int face_index, float size, std::uint32_t codepoint) {
    if (m_faces.empty()) {
        m_missing.advance = 0.5f * size;
        return m_missing;
    }
    const int pixels = pixel_size(size);
    const std::uint64_t key = glyph_key(face_index, pixels, codepoint);
    if (const auto found = m_glyphs.find(key); found != m_glyphs.end()) {
        return found->second;
    }

    int used = face_index;
    int index = stbtt_FindGlyphIndex(&m_faces[static_cast<std::size_t>(used)]->info,
                                     static_cast<int>(codepoint));
    if (index == 0 && used != 0) {
        used = 0;
        index = stbtt_FindGlyphIndex(&m_faces[0]->info, static_cast<int>(codepoint));
    }
    const stbtt_fontinfo& info = m_faces[static_cast<std::size_t>(used)]->info;
    const float scale = stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(pixels));

    Glyph glyph;
    glyph.face = used;
    glyph.index = index;
    int advance = 0;
    int bearing = 0;
    stbtt_GetGlyphHMetrics(&info, index, &advance, &bearing);
    glyph.advance = static_cast<float>(advance) * scale;

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBox(&info, index, scale, scale, &x0, &y0, &x1, &y1);
    const int width = x1 - x0;
    const int height = y1 - y0;
    if (width > 0 && height > 0 && !m_full) {
        // Shelf packing, with a pixel of space around each glyph so sampling doesn't bleed.
        if (m_shelf_x + width + 1 > k_atlas_size) {
            m_shelf_x = 1;
            m_shelf_y += m_shelf_height + 1;
            m_shelf_height = 0;
        }
        if (m_shelf_y + height + 1 > k_atlas_size) {
            m_full = true;
        } else {
            stbtt_MakeGlyphBitmap(
                &info,
                m_atlas.data() + static_cast<std::size_t>(m_shelf_y) * k_atlas_size + m_shelf_x,
                width, height, k_atlas_size, scale, scale, index);
            const float texel = 1.0f / static_cast<float>(k_atlas_size);
            glyph.x0 = static_cast<float>(x0);
            glyph.y0 = static_cast<float>(y0);
            glyph.x1 = static_cast<float>(x1);
            glyph.y1 = static_cast<float>(y1);
            glyph.u0 = static_cast<float>(m_shelf_x) * texel;
            glyph.v0 = static_cast<float>(m_shelf_y) * texel;
            glyph.u1 = static_cast<float>(m_shelf_x + width) * texel;
            glyph.v1 = static_cast<float>(m_shelf_y + height) * texel;
            glyph.visible = true;
            m_shelf_x += width + 1;
            m_shelf_height = std::max(m_shelf_height, height);
            m_changed = true;
        }
    }
    return m_glyphs.emplace(key, glyph).first->second;
}

float Fonts::kerning(const Glyph& left, const Glyph& right, float size) const {
    if (m_faces.empty() || left.face != right.face) {
        return 0.0f;
    }
    const stbtt_fontinfo& info = m_faces[static_cast<std::size_t>(left.face)]->info;
    const float scale =
        stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(pixel_size(size)));
    return static_cast<float>(stbtt_GetGlyphKernAdvance(&info, left.index, right.index)) * scale;
}

float Fonts::measure(int face, float size, std::string_view text) {
    float width = 0.0f;
    const Glyph* previous = nullptr;
    for (std::size_t i = 0; i < text.size();) {
        const Glyph& current = glyph(face, size, next_codepoint(text, i));
        if (previous != nullptr) {
            width += kerning(*previous, current, size);
        }
        width += current.advance;
        previous = &current;
    }
    return width;
}

int Fonts::atlas_size() const {
    return k_atlas_size;
}

const std::vector<std::uint8_t>& Fonts::atlas() const {
    return m_atlas;
}

bool Fonts::atlas_changed() const {
    return m_changed;
}

void Fonts::atlas_taken() {
    m_changed = false;
}

void Fonts::trim() {
    if (!m_full) {
        return;
    }
    std::fill(m_atlas.begin(), m_atlas.end(), std::uint8_t{0});
    m_glyphs.clear();
    m_shelf_x = 1;
    m_shelf_y = 1;
    m_shelf_height = 0;
    m_full = false;
    m_changed = true;
}

} // namespace markup::detail
