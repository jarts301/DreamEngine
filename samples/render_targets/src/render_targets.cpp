// Render targets, through the graphics interface alone (ADR 0027). Each frame has three passes:
//
// 1. The light's view of the cube, depth only, into a shadow map.
// 2. The cube and the floor, into a float color target and a depth target, darkened where the
//    shadow map has something between them and the light.
// 3. The color target, copied to the window.
//
// The picture is the same on every graphics backend, which the checks compare with one reference.

#include "dream/core/log.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>

using namespace dream;
using core::Vec3;

namespace {

struct Vertex {
    Vec3 position;
    Vec3 color;
};

struct ScreenVertex {
    float position[2];
    float uv[2];
};

struct FrameUniforms {
    core::Mat4 camera_view_projection;
    core::Mat4 light_view_projection;
};

constexpr Vec3 k_red{0.90f, 0.20f, 0.20f};
constexpr Vec3 k_cyan{0.20f, 0.80f, 0.85f};
constexpr Vec3 k_green{0.25f, 0.80f, 0.30f};
constexpr Vec3 k_magenta{0.80f, 0.25f, 0.80f};
constexpr Vec3 k_blue{0.25f, 0.35f, 0.95f};
constexpr Vec3 k_yellow{0.95f, 0.80f, 0.25f};
constexpr Vec3 k_floor{0.75f, 0.75f, 0.70f};

// The cube's faces, then the floor, four corners each, counter-clockwise from outside.
constexpr std::array<Vertex, 28> k_vertices{{
    {{1, -1, 1}, k_red},      {{1, -1, -1}, k_red},      {{1, 1, -1}, k_red},
    {{1, 1, 1}, k_red},       {{-1, -1, -1}, k_cyan},    {{-1, -1, 1}, k_cyan},
    {{-1, 1, 1}, k_cyan},     {{-1, 1, -1}, k_cyan},     {{-1, 1, 1}, k_green},
    {{1, 1, 1}, k_green},     {{1, 1, -1}, k_green},     {{-1, 1, -1}, k_green},
    {{-1, -1, -1}, k_magenta}, {{1, -1, -1}, k_magenta}, {{1, -1, 1}, k_magenta},
    {{-1, -1, 1}, k_magenta}, {{-1, -1, 1}, k_blue},     {{1, -1, 1}, k_blue},
    {{1, 1, 1}, k_blue},      {{-1, 1, 1}, k_blue},      {{1, -1, -1}, k_yellow},
    {{-1, -1, -1}, k_yellow}, {{-1, 1, -1}, k_yellow},   {{1, 1, -1}, k_yellow},
    {{-6, -1, 6}, k_floor},   {{6, -1, 6}, k_floor},     {{6, -1, -6}, k_floor},
    {{-6, -1, -6}, k_floor},
}};

constexpr std::uint32_t k_cube_indices = 36;
constexpr std::uint32_t k_floor_indices = 6;

constexpr std::array<std::uint16_t, k_cube_indices + k_floor_indices> make_indices() {
    std::array<std::uint16_t, k_cube_indices + k_floor_indices> indices{};
    for (std::uint16_t face = 0; face < 7; ++face) {
        const auto first = static_cast<std::uint16_t>(face * 4);
        const std::size_t at = face * std::size_t{6};
        indices[at + 0] = first;
        indices[at + 1] = static_cast<std::uint16_t>(first + 1);
        indices[at + 2] = static_cast<std::uint16_t>(first + 2);
        indices[at + 3] = first;
        indices[at + 4] = static_cast<std::uint16_t>(first + 2);
        indices[at + 5] = static_cast<std::uint16_t>(first + 3);
    }
    return indices;
}

constexpr auto k_indices = make_indices();

// Covers the window, with texture coordinates from the top left.
constexpr std::array<ScreenVertex, 3> k_screen{{
    {{-1.0f, -1.0f}, {0.0f, 1.0f}},
    {{3.0f, -1.0f}, {2.0f, 1.0f}},
    {{-1.0f, 3.0f}, {0.0f, -1.0f}},
}};

constexpr std::uint32_t k_shadow_size = 1024;

class RenderTargetsGame final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        rhi::Device* const device = app.device();
        if (device == nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument, "This game needs a graphics module"};
        }
        m_quit_after_frames = app.config().get_int("demo.quit_after_frames").value_or(0);
        m_spinning = app.config().get_bool("demo.spin").value_or(true);

        // The smallest float format the device can draw into, then the larger one, then 8 bits.
        for (const rhi::TextureFormat format :
             {rhi::TextureFormat::Rg11B10Float, rhi::TextureFormat::Rgba16Float,
              rhi::TextureFormat::Rgba8Unorm}) {
            if (device->supports_render_target(format)) {
                m_color_format = format;
                break;
            }
        }
        DREAM_LOG_INFO("Scene color target: {}",
                       m_color_format == rhi::TextureFormat::Rg11B10Float  ? "R11G11B10 float"
                       : m_color_format == rhi::TextureFormat::Rgba16Float ? "RGBA16 float"
                                                                           : "RGBA8");

        for (const auto& [name, shader] :
             {std::pair{"shadow.vert", &m_shadow_vertex}, std::pair{"scene.vert", &m_scene_vertex},
              std::pair{"composite.vert", &m_composite_vertex}}) {
            core::Result<rhi::ShaderHandle> loaded =
                load_shader(app, *device, name, rhi::ShaderStage::Vertex);
            if (!loaded.has_value()) {
                return loaded.error();
            }
            *shader = loaded.value();
        }
        for (const auto& [name, shader] : {std::pair{"scene.frag", &m_scene_fragment},
                                           std::pair{"composite.frag", &m_composite_fragment}}) {
            core::Result<rhi::ShaderHandle> loaded =
                load_shader(app, *device, name, rhi::ShaderStage::Fragment);
            if (!loaded.has_value()) {
                return loaded.error();
            }
            *shader = loaded.value();
        }

        core::Result<rhi::BufferHandle> vertices =
            device->create_buffer(rhi::BufferUsage::Vertex, std::as_bytes(std::span(k_vertices)));
        if (!vertices.has_value()) {
            return vertices.error();
        }
        m_vertices = vertices.value();
        core::Result<rhi::BufferHandle> indices =
            device->create_buffer(rhi::BufferUsage::Index, std::as_bytes(std::span(k_indices)));
        if (!indices.has_value()) {
            return indices.error();
        }
        m_indices = indices.value();

        core::Result<rhi::TextureHandle> shadow = device->create_texture(
            rhi::TextureDesc{.width = k_shadow_size,
                             .height = k_shadow_size,
                             .format = rhi::TextureFormat::Depth32Float,
                             .render_target = true},
            {});
        if (!shadow.has_value()) {
            return shadow.error();
        }
        m_shadow_map = shadow.value();

        constexpr std::array attributes{
            rhi::VertexAttribute{0, rhi::VertexFormat::Float3, offsetof(Vertex, position)},
            rhi::VertexAttribute{1, rhi::VertexFormat::Float3, offsetof(Vertex, color)},
        };
        // Culling the faces toward the light leaves the back faces in the shadow map, which with
        // the bias keeps lit surfaces from shadowing themselves.
        core::Result<rhi::PipelineHandle> shadow_pipeline = device->create_pipeline(rhi::PipelineDesc{
            .vertex_shader = m_shadow_vertex,
            .vertex_stride = sizeof(Vertex),
            .attributes = attributes,
            .cull = rhi::CullMode::Front,
            .depth_bias = 2.0f,
            .slope_depth_bias = 2.0f,
            .depth_target = rhi::TextureFormat::Depth32Float,
        });
        if (!shadow_pipeline.has_value()) {
            return shadow_pipeline.error();
        }
        m_shadow_pipeline = shadow_pipeline.value();

        core::Result<rhi::PipelineHandle> scene_pipeline = device->create_pipeline(rhi::PipelineDesc{
            .vertex_shader = m_scene_vertex,
            .fragment_shader = m_scene_fragment,
            .vertex_stride = sizeof(Vertex),
            .attributes = attributes,
            .color_target = m_color_format,
            .depth_target = rhi::TextureFormat::Depth32Float,
        });
        if (!scene_pipeline.has_value()) {
            return scene_pipeline.error();
        }
        m_scene_pipeline = scene_pipeline.value();

        constexpr std::array screen_attributes{
            rhi::VertexAttribute{0, rhi::VertexFormat::Float2, offsetof(ScreenVertex, position)},
            rhi::VertexAttribute{1, rhi::VertexFormat::Float2, offsetof(ScreenVertex, uv)},
        };
        core::Result<rhi::PipelineHandle> composite_pipeline =
            device->create_pipeline(rhi::PipelineDesc{
                .vertex_shader = m_composite_vertex,
                .fragment_shader = m_composite_fragment,
                .vertex_stride = sizeof(ScreenVertex),
                .attributes = screen_attributes,
                .depth_test = false,
                .cull = rhi::CullMode::None,
            });
        if (!composite_pipeline.has_value()) {
            return composite_pipeline.error();
        }
        m_composite_pipeline = composite_pipeline.value();
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        if (app.input().was_pressed("toggle_spin")) {
            m_spinning = !m_spinning;
        }
        if (m_spinning) {
            m_angle += static_cast<float>(time.delta_seconds) * 0.5f;
        }
        // Outside the frame, since making textures may wait for the GPU.
        if (auto resized = fit_targets(app); !resized.has_value()) {
            DREAM_LOG_ERROR("Could not make the scene's targets: {}", resized.error().message);
            app.request_quit();
        }
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        if (!m_scene_color.is_valid()) {
            commands.begin_render_pass(rhi::ClearColor{});
            commands.end_render_pass();
            return;
        }
        const platform::SurfaceSize size = app.surface_size();
        const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
        const float distance = 1.0f / std::min(aspect, 1.0f);
        const Vec3 eye{4.5f * distance, 4.0f * distance, 6.0f * distance};
        const core::Mat4 camera = core::perspective(core::radians(45.0f), aspect, 0.1f, 100.0f) *
                                  core::look_at(eye, Vec3{0.0f, -0.5f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
        const core::Mat4 light =
            core::orthographic(-7.0f, 7.0f, -7.0f, 7.0f, 1.0f, 30.0f) *
            core::look_at(Vec3{-6.0f, 10.0f, 4.0f}, Vec3{}, Vec3{0.0f, 1.0f, 0.0f});
        const FrameUniforms uniforms{camera, light};
        const core::Mat4 cube =
            core::rotation(core::quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, m_angle));
        const core::Mat4 floor = core::translation(Vec3{});
        const auto frame_bytes = std::as_bytes(std::span(&uniforms, 1));

        commands.begin_render_pass(rhi::RenderTargets{.depth = m_shadow_map});
        commands.bind_pipeline(m_shadow_pipeline);
        commands.set_uniforms(frame_bytes);
        commands.bind_vertex_buffer(m_vertices);
        commands.bind_index_buffer(m_indices, rhi::IndexFormat::UInt16);
        commands.push_constants(std::as_bytes(std::span(&cube, 1)));
        commands.draw_indexed_range(0, k_cube_indices);
        commands.end_render_pass();

        commands.begin_render_pass(rhi::RenderTargets{
            .color = m_scene_color,
            .depth = m_scene_depth,
            .clear_color = rhi::ClearColor{0.04f, 0.05f, 0.08f, 1.0f},
        });
        commands.bind_pipeline(m_scene_pipeline);
        commands.set_uniforms(frame_bytes);
        commands.bind_texture(rhi::k_shadow_slot, m_shadow_map);
        commands.bind_vertex_buffer(m_vertices);
        commands.bind_index_buffer(m_indices, rhi::IndexFormat::UInt16);
        commands.push_constants(std::as_bytes(std::span(&cube, 1)));
        commands.draw_indexed_range(0, k_cube_indices);
        commands.push_constants(std::as_bytes(std::span(&floor, 1)));
        commands.draw_indexed_range(k_cube_indices, k_floor_indices);
        commands.end_render_pass();

        commands.begin_render_pass(rhi::ClearColor{});
        commands.bind_pipeline(m_composite_pipeline);
        commands.bind_texture(1, m_scene_color);
        commands.draw_vertices(std::as_bytes(std::span(k_screen)), 3);
        commands.end_render_pass();
    }

    void on_stop(runtime::App& app) override {
        if (rhi::Device* const device = app.device()) {
            destroy_targets(*device);
            device->destroy_texture(m_shadow_map);
            for (const rhi::PipelineHandle pipeline :
                 {m_shadow_pipeline, m_scene_pipeline, m_composite_pipeline}) {
                device->destroy_pipeline(pipeline);
            }
            device->destroy_buffer(m_indices);
            device->destroy_buffer(m_vertices);
            for (const rhi::ShaderHandle shader : {m_shadow_vertex, m_scene_vertex, m_scene_fragment,
                                                   m_composite_vertex, m_composite_fragment}) {
                device->destroy_shader(shader);
            }
        }
        const std::string report = std::format("frames={}", m_frames);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    static core::Result<rhi::ShaderHandle> load_shader(runtime::App& app, rhi::Device& device,
                                                       std::string_view name,
                                                       rhi::ShaderStage stage) {
        const auto code = app.files().read(
            std::format("assets:/shaders/{}{}", name, device.shader_file_suffix()));
        if (!code.has_value()) {
            return code.error();
        }
        return device.create_shader(stage, code.value());
    }

    // The scene's targets follow the window's size.
    core::Result<void> fit_targets(runtime::App& app) {
        rhi::Device* const device = app.device();
        const platform::SurfaceSize size = app.surface_size();
        if (device == nullptr || size == m_target_size) {
            return {};
        }
        destroy_targets(*device);
        m_target_size = size;
        if (size.width == 0 || size.height == 0) {
            return {};
        }
        core::Result<rhi::TextureHandle> color = device->create_texture(
            rhi::TextureDesc{.width = size.width, .height = size.height, .format = m_color_format,
                             .render_target = true},
            {});
        if (!color.has_value()) {
            return color.error();
        }
        m_scene_color = color.value();
        core::Result<rhi::TextureHandle> depth = device->create_texture(
            rhi::TextureDesc{.width = size.width,
                             .height = size.height,
                             .format = rhi::TextureFormat::Depth32Float,
                             .render_target = true},
            {});
        if (!depth.has_value()) {
            return depth.error();
        }
        m_scene_depth = depth.value();
        return {};
    }

    void destroy_targets(rhi::Device& device) {
        device.destroy_texture(m_scene_color);
        device.destroy_texture(m_scene_depth);
        m_scene_color = {};
        m_scene_depth = {};
        m_target_size = {};
    }

    rhi::TextureFormat m_color_format = rhi::TextureFormat::Rgba8Unorm;
    rhi::ShaderHandle m_shadow_vertex;
    rhi::ShaderHandle m_scene_vertex;
    rhi::ShaderHandle m_scene_fragment;
    rhi::ShaderHandle m_composite_vertex;
    rhi::ShaderHandle m_composite_fragment;
    rhi::BufferHandle m_vertices;
    rhi::BufferHandle m_indices;
    rhi::TextureHandle m_shadow_map;
    rhi::TextureHandle m_scene_color;
    rhi::TextureHandle m_scene_depth;
    platform::SurfaceSize m_target_size;
    rhi::PipelineHandle m_shadow_pipeline;
    rhi::PipelineHandle m_scene_pipeline;
    rhi::PipelineHandle m_composite_pipeline;
    float m_angle = 0.6f;
    bool m_spinning = true;
    std::int64_t m_quit_after_frames = 0;
    std::int64_t m_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<RenderTargetsGame>();
}
