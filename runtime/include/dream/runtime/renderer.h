#pragma once

#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/platform/event.h"
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

// Draws a model at its entity's position. `tint` multiplies the model's own colors.
struct MeshRenderer {
    AssetRef<ModelAsset> model;
    core::Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
};

// Sunlight. Only the first one in a world is used.
struct DirectionalLight {
    // The way the light travels.
    core::Vec3 direction{-0.4f, -1.0f, -0.5f};
    core::Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.3f;
    float ambient = 0.3f;
};

// The field of view to use so that `vertical_fov_radians` also fits on a narrow screen. On a
// portrait screen this widens the view, so a scene framed for landscape isn't cropped at the sides.
[[nodiscard]] float fitted_vertical_fov(float vertical_fov_radians, float aspect) noexcept;

// Draws a World: opaque models, one directional light and a camera. Games call render from
// Game::on_render, between nothing else: it starts and ends the render pass itself.
//
// initialize also lets scene files use the components "camera", "mesh" and "directional_light":
//
//     [entities.lamp.mesh]
//     model = "assets:/models/lamp.glb"
//     tint = [1.0, 0.8, 0.6]            # Optional.
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
//
// The game needs the RENDERER option on dream_add_game, for the shaders.
class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Needs a graphics device, and must be called before any scene that uses these components is
    // loaded. Game::on_start is the place for it.
    [[nodiscard]] core::Result<void> initialize(App& app);

    // Draws the world's first camera and light, then the debug lines, which it clears. Without a
    // camera, this only clears the screen.
    void render(const World& world, rhi::CommandList& commands, platform::SurfaceSize size,
                rhi::ClearColor clear_color = rhi::ClearColor{0.45f, 0.60f, 0.80f, 1.0f});

    // Lines to draw over the next frame, such as Physics3D::debug_draw adds.
    [[nodiscard]] DebugLines& debug_lines() noexcept {
        return m_debug_lines;
    }

    // How many parts of models were drawn in the last call to render.
    [[nodiscard]] std::uint32_t last_draw_count() const noexcept {
        return m_last_draw_count;
    }

    // How many debug lines the last call to render drew.
    [[nodiscard]] std::uint32_t last_line_count() const noexcept {
        return m_last_line_count;
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
        return m_last_ui_command_count;
    }

private:
    class UiTextures;

    void draw_debug_lines(rhi::CommandList& commands);
    void draw_ui(rhi::CommandList& commands, platform::SurfaceSize size);

    std::optional<GpuShader> m_vertex_shader;
    std::optional<GpuShader> m_fragment_shader;
    std::optional<GpuPipeline> m_pipeline;
    std::optional<GpuShader> m_line_vertex_shader;
    std::optional<GpuShader> m_line_fragment_shader;
    std::optional<GpuPipeline> m_line_pipeline;
    std::optional<GpuShader> m_ui_vertex_shader;
    std::optional<GpuShader> m_ui_fragment_shader;
    std::optional<GpuPipeline> m_ui_pipeline;
    std::optional<GpuPipeline> m_ui_premultiplied_pipeline;
    std::unique_ptr<UiTextures> m_ui_textures;
    std::array<ui::DrawList, static_cast<std::size_t>(UiLayer::Count)> m_ui_layers;
    std::array<core::Vec2, static_cast<std::size_t>(UiLayer::Count)> m_ui_offsets{};
    DebugLines m_debug_lines;
    std::uint32_t m_last_draw_count = 0;
    std::uint32_t m_last_line_count = 0;
    std::uint32_t m_last_ui_command_count = 0;
};

} // namespace dream::runtime
