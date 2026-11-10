#pragma once

#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dream::ui {

// What every interface backend, game or debug, hands over to be drawn. Backends never touch the
// graphics API: they describe triangles, and the engine's renderer draws them.

// Chosen by the host. 0 means no texture, which draws plain color.
using TextureId = std::uint64_t;

struct Vertex {
    // In pixels from the top-left corner of the surface.
    core::Vec2 position;
    core::Vec2 uv;

    // RGBA, eight bits each, red in the lowest byte, as sRGB.
    std::uint32_t color = 0xffffffff;
};

// In pixels from the top-left corner of the surface.
struct ClipRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct DrawCommand {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    TextureId texture = 0;
    std::optional<ClipRect> clip;

    // Added to every vertex position.
    core::Vec2 translation;
};

// Triangles in drawing order. Indices refer to `vertices`.
struct DrawList {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<DrawCommand> commands;

    // Whether vertex and texture colors are already multiplied by their alpha.
    bool premultiplied_alpha = false;

    void clear() noexcept {
        vertices.clear();
        indices.clear();
        commands.clear();
    }
};

// Color is for images, stored as sRGB. Coverage is for data such as font masks, used as it is.
enum class TextureKind : std::uint8_t { Color, Coverage };

struct LoadedTexture {
    TextureId id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// The engine services an interface backend uses.
class Host {
public:
    // `path` is a virtual path such as "assets:/ui/menu.rml".
    [[nodiscard]] virtual core::Result<std::vector<std::byte>> read_file(std::string_view path) = 0;

    // `rgba` holds width * height pixels of four bytes, row by row from the top.
    [[nodiscard]] virtual core::Result<TextureId>
    create_texture(std::uint32_t width, std::uint32_t height, std::span<const std::byte> rgba,
                   TextureKind kind) = 0;

    // Decodes a PNG or JPEG file, multiplying colors by alpha if asked.
    [[nodiscard]] virtual core::Result<LoadedTexture> load_texture(std::string_view path,
                                                                   bool premultiply_alpha) = 0;

    // Takes effect once no frame being drawn uses the texture.
    virtual void release_texture(TextureId texture) = 0;

protected:
    ~Host() = default;
};

// What a backend needs to know each frame.
struct FrameInfo {
    platform::SurfaceSize size;
    float delta_seconds = 0.0f;

    // See platform::Platform::display_scale.
    float display_scale = 1.0f;
};

} // namespace dream::ui
