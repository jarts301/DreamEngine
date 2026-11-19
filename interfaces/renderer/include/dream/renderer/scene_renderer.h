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
};

// The vertices of debug lines, two per line.
struct LineVertex {
    core::Vec3 position;
    core::Vec4 color;
};

// One part of one model: a range of its 32-bit indices, drawn with one material.
struct MeshDraw {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    // Model space to world space.
    core::Mat4 model;
    // The material's color, times the instance's tint. Linear.
    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    // Invalid for none, which draws as white.
    rhi::TextureHandle texture;
    bool casts_shadows = true;
    // Around the part, in model space, for skipping parts out of view. Empty when unknown, and then
    // the part is always drawn.
    core::Aabb bounds;
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
