#pragma once

#include "dream/core/math/bounds.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/rhi/rhi.h"
#include "dream/ui/draw_list.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dream::renderer {

// Draws a game's 3D scene, its debug lines and its interface layers, from plain data the runtime
// gathers from the world each frame (ADR 0030). The runtime keeps everything about the world:
// entities, components, scene files and loading models. A renderer only draws, so a project can
// swap how it draws, and with what, by editing its module list.

// The vertices of every model, as runtime models are built.
struct MeshVertex {
    core::Vec3 position;
    core::Vec3 normal;
    core::Vec2 uv;
    // The direction in which `uv.x` grows along the surface, for normal maps, with w = 1 or -1
    // saying which way the bitangent, cross(normal, tangent) * w, points. All zero when unknown,
    // and then normal maps are ignored there (ADR 0033).
    core::Vec4 tangent{};
};

// How a surface's alpha is used, as glTF's alphaMode (ADR 0034).
enum class AlphaMode : std::uint8_t {
    // Alpha is ignored, and the surface hides what is behind it.
    Opaque,
    // Pixels whose alpha is below the cutoff aren't drawn, and the rest are opaque: leaves, fences,
    // cut-out decals. They cast cut-out shadows.
    Mask,
    // Blended over what is behind it by its alpha: glass, water, smoke. Drawn after everything
    // opaque, farthest first, and casting no shadow.
    Blend,
};

// How a surface looks, in glTF's metallic-roughness model (ADR 0033). Factors multiply their
// textures, and a missing texture reads as white, so the factors alone apply.
struct MeshMaterial {
    // Linear. Its alpha is the surface's opacity for the Mask and Blend alpha modes.
    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    // 0 for dielectrics such as wood, stone and plastic; 1 for bare metal. The texture's blue
    // channel.
    float metallic = 0.0f;
    // 0 mirror-smooth, 1 fully matte. The texture's green channel.
    float roughness = 0.5f;
    // How strongly the normal map tilts the surface.
    float normal_scale = 1.0f;
    // How much of the occlusion texture's red channel darkens ambient light.
    float occlusion_strength = 1.0f;
    // Light the surface gives off, linear and in the same units as the sun's intensity.
    core::Vec3 emissive{0.0f, 0.0f, 0.0f};
    // How the base color's alpha, its factor times its texture's, is used.
    AlphaMode alpha_mode = AlphaMode::Opaque;
    // The alpha below which a Mask surface isn't drawn.
    float alpha_cutoff = 0.5f;
    // Both faces are drawn, and lit from the side seen, as for leaves and thin glass. Otherwise
    // only the front faces, counter-clockwise ones, are drawn.
    bool double_sided = false;

    // sRGB colors.
    rhi::TextureHandle base_color_texture{};
    // Tangent-space normals, linear: red +X along the tangent, green +Y along the bitangent.
    rhi::TextureHandle normal_texture{};
    // Linear: roughness in green, metallic in blue. The same texture can also be the occlusion.
    rhi::TextureHandle metallic_roughness_texture{};
    // Linear: ambient occlusion in red.
    rhi::TextureHandle occlusion_texture{};
    // sRGB colors.
    rhi::TextureHandle emissive_texture{};
};

// The vertices of debug lines, two per line.
struct LineVertex {
    core::Vec3 position;
    core::Vec4 color;
};

// A simpler version of a part, in the same buffers: another range of indices over its vertices.
struct MeshLod {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    // How far, at most, it strays from the full part, in model space.
    float error = 0.0f;
};

// One part of one model: a range of its 32-bit indices, drawn with one material.
struct MeshDraw {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    // Model space to world space.
    core::Mat4 model;
    // The material, with its base color already multiplied by the instance's tint.
    MeshMaterial material;
    bool casts_shadows = true;
    // Around the part, in model space, for skipping parts out of view. Empty when unknown, and then
    // the part is always drawn.
    core::Aabb bounds;
    // Simpler versions, each simpler than the last, for when the part is small on screen. The
    // renderer may draw one in place of the full range (ADR 0032).
    std::span<const MeshLod> lods;
};

// The camera, as runtime::camera_view gives it.
struct View {
    core::Mat4 view_projection;
    core::Vec3 eye;
    float z_near = 0.1f;
    float z_far = 100.0f;
};

// The scene's one directional light.
struct Sunlight {
    // The way the light travels.
    core::Vec3 direction{-0.4f, -1.0f, -0.5f};
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.3f;
    float ambient = 0.3f;
    bool casts_shadows = true;
};

// A layer of the game's interface, drawn over the scene in the order given, moved by `offset`
// pixels.
struct UiLayer {
    const ui::DrawList* list = nullptr;
    core::Vec2 offset;
};

// Everything one frame draws. The spans only need to live during render.
struct Frame {
    platform::SurfaceSize size;
    rhi::ClearColor clear_color{0.45f, 0.60f, 0.80f, 1.0f};
    // Without a view, only the interface layers are drawn, over the clear color.
    std::optional<View> view;
    std::optional<Sunlight> sun;
    std::span<const MeshDraw> meshes;
    std::span<const LineVertex> lines;
    std::span<const UiLayer> ui_layers;
    // The texture an interface draw command names, by its id. Invalid draws as white.
    std::function<rhi::TextureHandle(ui::TextureId)> ui_texture;
};

// What one frame drew, for profiling and tests.
struct FrameStats {
    std::uint32_t draws = 0;
    std::uint32_t shadow_casters = 0;
    std::uint32_t lines = 0;
    std::uint32_t ui_commands = 0;
    // Parts skipped because they were out of the camera's view.
    std::uint32_t culled = 0;
    // Parts skipped in shadow cascades they were outside of, once for each cascade.
    std::uint32_t shadow_culled = 0;
    // Draw commands recorded for the scene and its shadows, each drawing one or more parts.
    std::uint32_t draw_calls = 0;
    // Parts drawn with one of their simpler versions.
    std::uint32_t simplified = 0;
    // Parts blended over the scene, which `draws` also counts.
    std::uint32_t blended = 0;
};

// Reads one of the game's files, such as "assets:/shaders/forward.vert.spv".
using FileReader = std::function<core::Result<std::vector<std::byte>>(std::string_view path)>;

class SceneRenderer {
public:
    static constexpr std::string_view k_module_interface = "renderer";

    virtual ~SceneRenderer() = default;

    // Called once, before anything else, with the device it draws with for the rest of its life,
    // which outlives it. A renderer reads its settings from the project configuration it was made
    // with.
    [[nodiscard]] virtual core::Result<void> start(rhi::Device& device) = 0;

    // Loads its shaders and what it makes from them, through `read_file`. Called after start,
    // and again whenever those files change, between frames, for hot reloading. A failure keeps
    // what it had before.
    [[nodiscard]] virtual core::Result<void> load_shaders(const FileReader& read_file) = 0;

    // Records the frame into `commands`: it begins and ends its own render passes, ending with
    // one into the window.
    virtual FrameStats render(rhi::CommandList& commands, const Frame& frame) = 0;

    // A line for the log, saying what the renderer chose for this device, such as its HDR format
    // and shadows.
    [[nodiscard]] virtual std::string_view describe() const = 0;
};

} // namespace dream::renderer
