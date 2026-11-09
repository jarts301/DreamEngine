// A colored cube, drawn through the graphics interface alone. Each face has its own color, so the
// picture shows whether the engine's coordinate conventions hold: +X is red, +Y is green and +Z is
// blue, and those three faces are the ones facing the camera at the start.

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

constexpr Vec3 k_red{0.90f, 0.20f, 0.20f};
constexpr Vec3 k_cyan{0.20f, 0.80f, 0.85f};
constexpr Vec3 k_green{0.25f, 0.80f, 0.30f};
constexpr Vec3 k_magenta{0.80f, 0.25f, 0.80f};
constexpr Vec3 k_blue{0.25f, 0.35f, 0.95f};
constexpr Vec3 k_yellow{0.95f, 0.80f, 0.25f};

// Four corners per face, counter-clockwise as seen from outside the cube.
constexpr std::array<Vertex, 24> k_vertices{{
    // +X
    {{1, -1, 1}, k_red},
    {{1, -1, -1}, k_red},
    {{1, 1, -1}, k_red},
    {{1, 1, 1}, k_red},
    // -X
    {{-1, -1, -1}, k_cyan},
    {{-1, -1, 1}, k_cyan},
    {{-1, 1, 1}, k_cyan},
    {{-1, 1, -1}, k_cyan},
    // +Y
    {{-1, 1, 1}, k_green},
    {{1, 1, 1}, k_green},
    {{1, 1, -1}, k_green},
    {{-1, 1, -1}, k_green},
    // -Y
    {{-1, -1, -1}, k_magenta},
    {{1, -1, -1}, k_magenta},
    {{1, -1, 1}, k_magenta},
    {{-1, -1, 1}, k_magenta},
    // +Z
    {{-1, -1, 1}, k_blue},
    {{1, -1, 1}, k_blue},
    {{1, 1, 1}, k_blue},
    {{-1, 1, 1}, k_blue},
    // -Z
    {{1, -1, -1}, k_yellow},
    {{-1, -1, -1}, k_yellow},
    {{-1, 1, -1}, k_yellow},
    {{1, 1, -1}, k_yellow},
}};

constexpr std::array<std::uint16_t, 36> make_indices() {
    std::array<std::uint16_t, 36> indices{};
    for (std::uint16_t face = 0; face < 6; ++face) {
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

constexpr std::array<std::uint16_t, 36> k_indices = make_indices();

class CubeGame final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        rhi::Device* const device = app.device();
        if (device == nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument, "This game needs a graphics module"};
        }

        m_quit_after_frames = app.config().get_int("cube.quit_after_frames").value_or(0);
        m_spinning = app.config().get_bool("cube.spin").value_or(true);

        core::Result<rhi::ShaderHandle> vertex_shader =
            load_shader(app, *device, "cube.vert", rhi::ShaderStage::Vertex);
        if (!vertex_shader.has_value()) {
            return vertex_shader.error();
        }
        core::Result<rhi::ShaderHandle> fragment_shader =
            load_shader(app, *device, "cube.frag", rhi::ShaderStage::Fragment);
        if (!fragment_shader.has_value()) {
            return fragment_shader.error();
        }
        m_vertex_shader = vertex_shader.value();
        m_fragment_shader = fragment_shader.value();

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

        constexpr std::array attributes{
            rhi::VertexAttribute{0, rhi::VertexFormat::Float3, offsetof(Vertex, position)},
            rhi::VertexAttribute{1, rhi::VertexFormat::Float3, offsetof(Vertex, color)},
        };
        core::Result<rhi::PipelineHandle> pipeline = device->create_pipeline(rhi::PipelineDesc{
            .vertex_shader = m_vertex_shader,
            .fragment_shader = m_fragment_shader,
            .vertex_stride = sizeof(Vertex),
            .attributes = attributes,
        });
        if (!pipeline.has_value()) {
            return pipeline.error();
        }
        m_pipeline = pipeline.value();
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;

        if (app.input().was_pressed("toggle_spin")) {
            m_spinning = !m_spinning;
            DREAM_LOG_INFO("Spinning: {}", m_spinning);
        }
        if (m_spinning) {
            m_angle += static_cast<float>(time.delta_seconds);
        }
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        const platform::SurfaceSize size = app.surface_size();
        const float aspect = size.height == 0 ? 1.0f
                                              : static_cast<float>(size.width) /
                                                    static_cast<float>(size.height);

        // Moves back on a portrait screen, so the cube still fits across the narrow side.
        const float distance = 1.0f / std::min(aspect, 1.0f);
        const Vec3 eye{3.5f * distance, 3.0f * distance, 4.5f * distance};

        const core::Mat4 model =
            core::rotation(core::quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, m_angle));
        const core::Mat4 view = core::look_at(eye, Vec3{}, Vec3{0.0f, 1.0f, 0.0f});
        const core::Mat4 projection = core::perspective(core::radians(45.0f), aspect, 0.1f, 100.0f);
        const core::Mat4 model_view_projection = projection * view * model;

        commands.begin_render_pass(rhi::ClearColor{0.05f, 0.06f, 0.09f, 1.0f});
        commands.bind_pipeline(m_pipeline);
        commands.bind_vertex_buffer(m_vertices);
        commands.bind_index_buffer(m_indices, rhi::IndexFormat::UInt16);
        commands.push_constants(std::as_bytes(std::span(&model_view_projection, 1)));
        commands.draw_indexed(static_cast<std::uint32_t>(k_indices.size()));
        commands.end_render_pass();
    }

    void on_stop(runtime::App& app) override {
        if (rhi::Device* const device = app.device()) {
            device->destroy_pipeline(m_pipeline);
            device->destroy_buffer(m_indices);
            device->destroy_buffer(m_vertices);
            device->destroy_shader(m_fragment_shader);
            device->destroy_shader(m_vertex_shader);
        }
        const std::string report = std::format("frames={}", m_frames);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

    void on_pause(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Paused");
    }

    void on_resume(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Resumed after {} frames", m_frames);
    }

    void on_surface_lost(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Surface lost");
    }

    void on_surface_restored(runtime::App& app) override {
        DREAM_LOG_INFO("Surface restored at {}x{}", app.surface_size().width,
                       app.surface_size().height);
    }

    void on_surface_resized(runtime::App& /*app*/, platform::SurfaceSize size) override {
        DREAM_LOG_INFO("Surface resized to {}x{}", size.width, size.height);
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

    rhi::ShaderHandle m_vertex_shader;
    rhi::ShaderHandle m_fragment_shader;
    rhi::BufferHandle m_vertices;
    rhi::BufferHandle m_indices;
    rhi::PipelineHandle m_pipeline;
    float m_angle = 0.0f;
    bool m_spinning = true;
    std::int64_t m_quit_after_frames = 0;
    std::int64_t m_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<CubeGame>();
}
