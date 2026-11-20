#pragma once

#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/rhi/rhi.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/debug_lines.h"
#include "dream/runtime/model.h"
#include "dream/runtime/world.h"
#include "dream/ui/draw_list.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace dream::runtime {

class App;

// Interface layers, drawn over the scene in this order.
enum class UiLayer : std::uint8_t { Game, Debug, Count };

// Looks along its entity's -Z axis, or at `target` if one is set.
struct Camera {
    float vertical_fov_degrees = 60.0f;
    float z_near = 0.1f;
    float z_far = 500.0f;
    std::optional<core::Vec3> target;
};

// A texture from an image file, drawn in the world, such as particles' (ADR 0038). Load it with
// `app.assets().load<TextureAsset>("assets:/textures/smoke.png")` after Renderer::initialize, which
// registers its loader. Colors are sRGB. A cooked KTX2 file is used in place of the PNG when the
// build made one, which it does for images in a "textures" folder.
struct TextureAsset {
    GpuTexture texture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Draws a model at its entity's position. `tint` multiplies the model's own colors.
struct MeshRenderer {
    AssetRef<ModelAsset> model;
    core::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
    // Whether it is drawn into the shadow map, so it shadows what is behind it.
    bool casts_shadows = true;
};

// Sunlight. Only the first one in a world is used.
struct DirectionalLight {
    // The way the light travels.
    core::Vec3 direction{-0.4f, -1.0f, -0.5f};
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.3f;
    float ambient = 0.3f;
    // Whether it casts shadows, where the renderer draws them (ADR 0029).
    bool casts_shadows = true;
};

// A light that shines every way from its entity's position, up to `range` meters (ADR 0037).
struct PointLight {
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    // What a white matte surface facing it 1 m away reflects, in the sun's units; it falls off with
    // the square of the distance.
    float intensity = 1.0f;
    float range = 10.0f;
};

// A light that shines in a cone along its entity's -Z axis, as cameras look (ADR 0037).
struct SpotLight {
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    // From the cone's middle, in degrees: full inside the inner angle, fading out to the outer.
    float inner_angle = 20.0f;
    float outer_angle = 35.0f;
};

// The field of view to use so that `vertical_fov_radians` also fits on a narrow screen. On a
// portrait screen this widens the view, so a scene framed for landscape isn't cropped at the sides.
[[nodiscard]] float fitted_vertical_fov(float vertical_fov_radians, float aspect) noexcept;

// How the world's first camera sees it on a surface of `size`, as Renderer::render draws it.
struct CameraView {
    core::Mat4 view_projection;
    core::Vec3 eye;
    float z_near = 0.1f;
    float z_far = 100.0f;
};

// Nothing without a camera, or before the camera has a WorldTransform, or for an empty surface.
[[nodiscard]] std::optional<CameraView> camera_view(const World& world, platform::SurfaceSize size);

// A line from the camera's eye out through a pixel. `direction` has length 1.
struct CameraRay {
    core::Vec3 origin;
    core::Vec3 direction;
};

// The ray through `pixel`, in pixels from the surface's top left, such as the pointer's position.
// Pass it to Physics3D::cast_ray to find what the player pointed at.
[[nodiscard]] std::optional<CameraRay> camera_ray(const World& world, platform::SurfaceSize size,
                                                  core::Vec2 pixel);

// The renderer module's shaders, loaded as one asset, so hot reloading reloads them all when one
// of their files changes.
struct RendererShaders {};

// Draws a World with the project's renderer module, chosen by its `renderer` list in dream.toml,
// such as `renderer = ["forward", "null"]` (ADR 0030). This class is the runtime's side: it gathers
// the camera, the light, the models, the debug lines and the interface layers from the world each
// frame, and the module draws them. Games call render from Game::on_render, between nothing else:
// the module starts and ends its render passes itself. The module's own settings, such as HDR and
// shadows, are in its documentation and the [renderer] table. When the project also lists a
// `mesh_simplifier`, models get simpler versions as they load, which the [lods] table sets out, for
// the module to draw far away (ADR 0032). When it lists a `tangent_generator`, models with normal
// maps but no tangents get them as they load (ADR 0033); docs/guides/materials.md explains
// materials.
//
// initialize also lets scene files use the components "camera", "mesh" and "directional_light":
//
//     [entities.lamp.mesh]
//     model = "assets:/models/lamp.glb"
//     tint = [1.0, 0.8, 0.6]            # Optional.
//     casts_shadows = true              # Optional.
//
//     [entities.main.camera]
//     vertical_fov = 60.0               # Optional, in degrees.
//     target = [0.0, 1.0, 0.0]          # Optional.
//
//     [entities.sun.directional_light]
//     direction = [-0.4, -1.0, -0.5]    # All optional.
//     color = [1.0, 1.0, 1.0]
//     intensity = 1.3
//     ambient = 0.3
//     casts_shadows = true
//
//     [entities.lamp.point_light]
//     color = [1.0, 0.6, 0.3]           # All optional.
//     intensity = 2.0
//     range = 6.0
//
//     [entities.torch.spot_light]       # Points along the entity's -Z axis.
//     color = [1.0, 1.0, 1.0]           # All optional.
//     intensity = 8.0
//     range = 12.0
//     inner_angle = 20.0                # Degrees.
//     outer_angle = 35.0
//
// Entities with a ParticleEmitter, which runtime::Particles moves on, are drawn too (ADR 0038), and
// 2D sprites and tile maps through a Camera2D, over the 3D scene (ADR 0039, dream/runtime/sprites.h):
// initialize also adds the components "camera_2d", "sprite", "sprite_animator" and "tile_map".
//
// How many point and spot lights are drawn depends on the renderer module: `forward` takes the 16
// nearest, and `forward_plus` 256 (ADR 0037).
class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Creates the configured renderer module. Needs a graphics device, and must be called before
    // any scene that uses these components is loaded. Game::on_start is the place for it.
    [[nodiscard]] core::Result<void> initialize(App& app);

    // The same with a given module, for tests or a game that makes its own.
    [[nodiscard]] core::Result<void> initialize(App& app,
                                                std::unique_ptr<renderer::SceneRenderer> backend);

    // Draws the world's first camera and light, its models, then the debug lines, which it clears,
    // and the interface layers over them. Without a camera, this draws only the interface layers
    // over the clear color.
    void render(const World& world, rhi::CommandList& commands, platform::SurfaceSize size,
                rhi::ClearColor clear_color = rhi::ClearColor{0.45f, 0.60f, 0.80f, 1.0f});

    // Lines to draw over the next frame, such as Physics3D::debug_draw adds.
    [[nodiscard]] DebugLines& debug_lines() noexcept {
        return m_debug_lines;
    }

    // How many parts of models were drawn in the last call to render.
    [[nodiscard]] std::uint32_t last_draw_count() const noexcept {
        return m_last_stats.draws;
    }

    // How many point and spot lights the last frame was lit with, and how many in view were left
    // out for being more than the renderer module takes (ADR 0037).
    [[nodiscard]] std::uint32_t last_light_count() const noexcept {
        return m_last_stats.lights;
    }
    [[nodiscard]] std::uint32_t last_dropped_light_count() const noexcept {
        return m_last_stats.lights_dropped;
    }

    // How many particles the last frame drew, and how many it left out for being more than the
    // renderer module takes in a frame (ADR 0038).
    [[nodiscard]] std::uint32_t last_particle_count() const noexcept {
        return m_last_stats.particles;
    }
    [[nodiscard]] std::uint32_t last_dropped_particle_count() const noexcept {
        return m_last_stats.particles_dropped;
    }

    // How many sprites the last frame drew, tiles of tile maps included (ADR 0039).
    [[nodiscard]] std::uint32_t last_sprite_count() const noexcept {
        return m_last_stats.sprites;
    }

    // How many parts of models the last call to render skipped, because they were out of view.
    [[nodiscard]] std::uint32_t last_culled_count() const noexcept {
        return m_last_stats.culled;
    }

    // How many debug lines the last call to render drew.
    [[nodiscard]] std::uint32_t last_line_count() const noexcept {
        return m_last_stats.lines;
    }

    // How many parts of models the last call to render drew into shadow maps, once for each
    // cascade.
    [[nodiscard]] std::uint32_t last_shadow_caster_count() const noexcept {
        return m_last_stats.shadow_casters;
    }

    // Draws `list` over the scene in every frame from now on, until replaced or cleared. The list
    // is copied, and moved by `offset` pixels. Textures it names come from ui_host().
    void set_ui_layer(UiLayer layer, const ui::DrawList& list, core::Vec2 offset = {});
    void clear_ui_layer(UiLayer layer);

    // Textures and files for interface backends. Valid after initialize.
    [[nodiscard]] ui::Host& ui_host() noexcept;

    // Destroys textures that interfaces released. It may wait for the GPU, so call it outside
    // on_render; DebugOverlay and UserInterface call it from their updates.
    void release_ui_textures();

    // How many interface draw commands the last call to render drew.
    [[nodiscard]] std::uint32_t last_ui_command_count() const noexcept {
        return m_last_stats.ui_commands;
    }

    // The module drawing, valid after initialize.
    [[nodiscard]] renderer::SceneRenderer* backend() const noexcept {
        return m_backend.get();
    }

private:
    class UiTextures;

    std::unique_ptr<renderer::SceneRenderer> m_backend;
    // Makes simpler versions of models as they load, when the project lists one.
    std::unique_ptr<mesh_simplifier::MeshSimplifier> m_simplifier;
    std::unique_ptr<tangent_generator::TangentGenerator> m_tangents;
    // After the backend, so the asset that reloads its shaders goes first.
    AssetRef<RendererShaders> m_shaders;
    std::unique_ptr<UiTextures> m_ui_textures;
    std::array<ui::DrawList, static_cast<std::size_t>(UiLayer::Count)> m_ui_layers;
    std::array<core::Vec2, static_cast<std::size_t>(UiLayer::Count)> m_ui_offsets{};
    DebugLines m_debug_lines;
    // Reused every frame, so gathering the scene allocates nothing once it has grown.
    std::vector<renderer::MeshDraw> m_meshes;
    std::vector<renderer::Light> m_lights;
    std::vector<renderer::ParticleBatch> m_particles;
    std::vector<renderer::SpriteQuad> m_sprite_quads;
    std::vector<renderer::SpriteBatch> m_sprite_batches;
    renderer::FrameStats m_last_stats;
};

} // namespace dream::runtime
