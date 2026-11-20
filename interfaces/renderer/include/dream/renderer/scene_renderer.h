#pragma once

#include "dream/core/math/bounds.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/rhi/rhi.h"
#include "dream/ui/draw_list.h"

#include <array>
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
    // For skinned parts (ADR 0036): up to four joints, as indices into the part's joint matrices,
    // and how much each moves the vertex, out of 255, adding up to 255. Unused otherwise.
    std::array<std::uint8_t, 4> joints{};
    std::array<std::uint8_t, 4> weights{};
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
    // For a skinned part (ADR 0036), each joint's skinning matrix, from the part's vertices to the
    // model's space as the skeleton is posed this frame, which the vertices' joints index. At most
    // k_max_joints. Empty for a part that isn't skinned. `bounds` then holds the posed part.
    std::span<const core::Mat4> joint_matrices;
};

// The most joints one skinned part can use: 256 joints of 48 bytes fill the 12 KB of the per-draw
// block the forward renderer sends them in.
inline constexpr std::size_t k_max_joints = 256;

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

// A light other than the sun (ADR 0037): a point that shines every way, or a spot that shines in a
// cone. Neither casts shadows.
enum class LightType : std::uint8_t { Point, Spot };

struct Light {
    LightType type = LightType::Point;
    core::Vec3 position;
    // Where a spot points. Unit length.
    core::Vec3 direction{0.0f, 0.0f, -1.0f};
    // Linear. With `intensity`, what a white matte surface facing it 1 m away reflects, in the
    // sun's units, falling off with the square of the distance.
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    // How far it reaches, in meters: its light fades smoothly to nothing there.
    float range = 10.0f;
    // A spot's cone, as angles from its direction, in radians: full inside the inner one, fading
    // to nothing at the outer one.
    float inner_angle = 0.35f;
    float outer_angle = 0.6f;
};

// How a batch of particles combines with what is behind it (ADR 0038).
enum class ParticleBlend : std::uint8_t {
    // Over what is behind them by their opacity: smoke, dust, leaves. Drawn farthest first.
    Alpha,
    // Adding their light to what is behind them: fire, sparks, magic. They never darken, and their
    // order doesn't matter.
    Additive,
};

// One particle, drawn as a square facing the camera.
struct ParticleQuad {
    // Its middle, in world space.
    core::Vec3 position;
    // Across, in meters.
    float size = 0.0f;
    // Linear, multiplying the texture, with its opacity in w. Not premultiplied.
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    // In radians, turning it around the view direction.
    float rotation = 0.0f;
};

// Particles drawn with one texture and one way of blending, such as one emitter's.
struct ParticleBatch {
    std::span<const ParticleQuad> particles;
    // sRGB colors with straight alpha. Invalid draws each particle as a soft round dot.
    rhi::TextureHandle texture;
    ParticleBlend blend = ParticleBlend::Alpha;
};

// How a sprite's texture is read (ADR 0039).
enum class SpriteFilter : std::uint8_t {
    // Each screen pixel takes the nearest texel, so pixel art stays crisp at any size.
    Pixel,
    // Blended between texels, with mipmaps, for painted or high-resolution art.
    Smooth,
};

// One sprite: a rectangle of a texture, placed in the 2D world's XY plane, +Y up.
struct SpriteQuad {
    // Where its pivot is, in world units.
    core::Vec2 position;
    // Across and up, in world units.
    core::Vec2 size{1.0f, 1.0f};
    // The point it is placed and turned by, from {0, 0} at its bottom left to {1, 1} at its top
    // right.
    core::Vec2 pivot{0.5f, 0.5f};
    // In radians, counter-clockwise.
    float rotation = 0.0f;
    // The part of the texture it shows, as texture coordinates from the top left: left, top,
    // right, bottom. A rectangle given right to left, or bottom to top, flips the sprite.
    core::Vec4 uv{0.0f, 0.0f, 1.0f, 1.0f};
    // Linear, multiplying the texture, with its opacity in w.
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
};

// Sprites drawn with one texture, in the order given.
struct SpriteBatch {
    std::span<const SpriteQuad> sprites;
    // sRGB colors with straight alpha. Invalid draws white rectangles.
    rhi::TextureHandle texture;
    // In pixels, for the Pixel filter.
    std::uint32_t texture_width = 1;
    std::uint32_t texture_height = 1;
    SpriteFilter filter = SpriteFilter::Pixel;
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
    // Point and spot lights. A renderer may draw only some, such as the nearest, when there are
    // more than it takes.
    std::span<const Light> lights;
    std::span<const MeshDraw> meshes;
    // Drawn over the scene's surfaces, blended ones included, and hidden behind opaque ones.
    std::span<const ParticleBatch> particles;
    // The 2D camera, from the 2D world's XY plane to clip space (ADR 0039). Without one, no sprites
    // are drawn.
    std::optional<core::Mat4> sprite_view;
    // Drawn over the 3D scene, if there is one, batch after batch and each in its order, without
    // depth: later ones cover earlier ones. Debug lines use the 2D camera when there is no 3D one.
    std::span<const SpriteBatch> sprites;
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
    // Parts skinned by their joints, which `draws` also counts.
    std::uint32_t skinned = 0;
    // Point and spot lights in view that the frame was lit with, and those left out for being
    // more than the renderer takes.
    std::uint32_t lights = 0;
    std::uint32_t lights_dropped = 0;
    // Particles drawn, and those left out for being more than the renderer takes in a frame.
    std::uint32_t particles = 0;
    std::uint32_t particles_dropped = 0;
    // Sprites drawn, tiles of tile maps included.
    std::uint32_t sprites = 0;
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
