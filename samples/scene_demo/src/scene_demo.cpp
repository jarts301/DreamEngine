// Scenes described in TOML files, built from components, using shared assets. A tap, click or the
// space bar swaps to the other scene.

#include "gpu_assets.h"

#include "dream/core/log.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/world.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <utility>

using namespace dream;
using core::Vec3;
using runtime::AssetRef;
using runtime::ComponentContext;

namespace {

constexpr const char* k_first_scene = "assets:/scenes/orbit.toml";
constexpr const char* k_second_scene = "assets:/scenes/row.toml";

// Components that this game adds to the ones the engine provides.

struct Spin {
    float speed = 1.0f;
};

struct Solid {
    AssetRef<PipelineAsset> pipeline;
    Vec3 tint{1.0f, 1.0f, 1.0f};
};

struct Camera {
    Vec3 target;
    float field_of_view_degrees = 45.0f;
};

// An event, so the input code doesn't need to know what a scene swap involves.
struct SwitchSceneRequested {};

struct Vertex {
    Vec3 position;
    Vec3 normal;
};

struct DrawConstants {
    core::Mat4 model_view_projection;
    core::Vec4 tint;
};

constexpr std::array k_vertex_attributes{
    rhi::VertexAttribute{0, rhi::VertexFormat::Float3, offsetof(Vertex, position)},
    rhi::VertexAttribute{1, rhi::VertexFormat::Float3, offsetof(Vertex, normal)},
};

struct Face {
    Vec3 normal;
    Vec3 u;
    Vec3 v;
};

// `u` cross `v` points along `normal`, so corners in this order wind counter-clockwise from outside.
constexpr std::array<Face, 6> k_faces{{
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
    {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
    {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
}};

constexpr std::array<Vertex, 24> make_vertices() {
    std::array<Vertex, 24> vertices{};
    std::size_t at = 0;
    for (const Face& face : k_faces) {
        vertices[at++] = {face.normal - face.u - face.v, face.normal};
        vertices[at++] = {face.normal + face.u - face.v, face.normal};
        vertices[at++] = {face.normal + face.u + face.v, face.normal};
        vertices[at++] = {face.normal - face.u + face.v, face.normal};
    }
    return vertices;
}

constexpr std::array<std::uint16_t, 36> make_indices() {
    std::array<std::uint16_t, 36> indices{};
    for (std::size_t face = 0; face < 6; ++face) {
        const auto first = static_cast<std::uint16_t>(face * 4);
        for (std::size_t corner = 0; corner < 6; ++corner) {
            constexpr std::array<std::uint16_t, 6> k_pattern{0, 1, 2, 0, 2, 3};
            indices[face * 6 + corner] = static_cast<std::uint16_t>(first + k_pattern[corner]);
        }
    }
    return indices;
}

constexpr std::array<Vertex, 24> k_vertices = make_vertices();
constexpr std::array<std::uint16_t, 36> k_indices = make_indices();

core::Result<void> read_spin(ComponentContext& context) {
    const core::Result<float> speed = context.data.number("speed", 1.0f);
    if (!speed.has_value()) {
        return speed.error();
    }
    context.world.registry().emplace<Spin>(context.entity, speed.value());
    return {};
}

core::Result<void> read_solid(ComponentContext& context) {
    const core::Result<std::string> path = context.data.text("pipeline");
    if (!path.has_value()) {
        return path.error();
    }
    const core::Result<Vec3> tint = context.data.vec3("tint", Vec3{1.0f, 1.0f, 1.0f});
    if (!tint.has_value()) {
        return tint.error();
    }
    core::Result<AssetRef<PipelineAsset>> pipeline =
        context.assets.load<PipelineAsset>(path.value());
    if (!pipeline.has_value()) {
        return pipeline.error();
    }
    context.world.registry().emplace<Solid>(context.entity, std::move(pipeline).value(),
                                            tint.value());
    return {};
}

core::Result<void> read_camera(ComponentContext& context) {
    const core::Result<Vec3> target = context.data.vec3("target", Vec3{});
    if (!target.has_value()) {
        return target.error();
    }
    const core::Result<float> field_of_view = context.data.number("field_of_view", 45.0f);
    if (!field_of_view.has_value()) {
        return field_of_view.error();
    }
    context.world.registry().emplace<Camera>(context.entity, target.value(), field_of_view.value());
    return {};
}

class SceneDemo final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        rhi::Device* const device = app.device();
        if (device == nullptr) {
            return core::Error{core::ErrorCode::InvalidArgument, "This game needs a graphics module"};
        }

        m_quit_after_frames = app.config().get_int("demo.quit_after_frames").value_or(0);
        m_switch_every_frames = app.config().get_int("demo.switch_every_frames").value_or(0);

        app.assets().register_loader<ShaderAsset>(
            [device](runtime::AssetLoadContext& context) { return load_shader(*device, context); });
        app.assets().register_loader<PipelineAsset>(
            [device, this](runtime::AssetLoadContext& context) {
                ++m_pipeline_loads;
                DREAM_LOG_INFO("Loading pipeline {}", context.path);
                return load_pipeline(*device, context, sizeof(Vertex), k_vertex_attributes);
            });

        app.scenes().register_component("spin", &read_spin);
        app.scenes().register_component("solid", &read_solid);
        app.scenes().register_component("camera", &read_camera);

        const auto vertices =
            device->create_buffer(rhi::BufferUsage::Vertex, std::as_bytes(std::span(k_vertices)));
        if (!vertices.has_value()) {
            return vertices.error();
        }
        m_vertices = vertices.value();

        const auto indices =
            device->create_buffer(rhi::BufferUsage::Index, std::as_bytes(std::span(k_indices)));
        if (!indices.has_value()) {
            return indices.error();
        }
        m_indices = indices.value();

        m_switch_listener = app.events().subscribe<SwitchSceneRequested>(
            [this, &app](const SwitchSceneRequested&) { switch_scene(app); });

        return open_scene(app, k_first_scene);
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;

        if (app.input().was_pressed("switch_scene") ||
            (m_switch_every_frames > 0 && m_frames % m_switch_every_frames == 0)) {
            app.events().enqueue(SwitchSceneRequested{});
        }

        Registry& registry = m_world->registry();
        const auto angle_axis = Vec3{0.0f, 1.0f, 0.0f};
        for (const auto [entity, transform, spin] : registry.view<runtime::Transform, Spin>().each()) {
            const auto step = static_cast<float>(time.delta_seconds) * spin.speed;
            transform.rotation =
                core::normalized(core::quat_from_axis_angle(angle_axis, step) * transform.rotation);
        }
        m_world->update_transforms();

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

        Registry& registry = m_world->registry();
        core::Mat4 view_projection;
        const auto cameras = registry.view<const runtime::Transform, const Camera>();
        if (const runtime::Entity first = cameras.front(); first != runtime::k_no_entity) {
            const auto& transform = cameras.get<const runtime::Transform>(first);
            const auto& camera = cameras.get<const Camera>(first);

            // Moves back on a portrait screen, so the scene still fits across the narrow side.
            const Vec3 eye = camera.target + (transform.position - camera.target) *
                                                 (1.0f / std::min(aspect, 1.0f));
            view_projection = core::perspective(core::radians(camera.field_of_view_degrees), aspect,
                                                0.1f, 200.0f) *
                              core::look_at(eye, camera.target, Vec3{0.0f, 1.0f, 0.0f});
        }

        commands.begin_render_pass(rhi::ClearColor{0.02f, 0.02f, 0.05f, 1.0f});
        commands.bind_vertex_buffer(m_vertices);
        commands.bind_index_buffer(m_indices, rhi::IndexFormat::UInt16);
        for (const auto [entity, world, solid] :
             registry.view<const runtime::WorldTransform, const Solid>().each()) {
            const DrawConstants constants{
                view_projection * world.matrix,
                core::Vec4{solid.tint.x, solid.tint.y, solid.tint.z, 1.0f},
            };
            commands.bind_pipeline(solid.pipeline->pipeline.get());
            commands.push_constants(std::as_bytes(std::span(&constants, 1)));
            commands.draw_indexed(static_cast<std::uint32_t>(k_indices.size()));
        }
        commands.end_render_pass();
    }

    void on_stop(runtime::App& app) override {
        const std::string report =
            std::format("frames={} scene={} switches={} pipeline_loads={}", m_frames, m_scene_path,
                        m_switches, m_pipeline_loads);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());

        m_world.reset();
        if (rhi::Device* const device = app.device()) {
            device->destroy_buffer(m_indices);
            device->destroy_buffer(m_vertices);
        }
    }

    void on_pause(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Paused");
    }

    void on_resume(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Resumed");
    }

private:
    using Registry = runtime::Registry;

    core::Result<void> open_scene(runtime::App& app, const char* path) {
        // The new scene loads before the old one goes away, so assets they share aren't reloaded.
        core::Result<std::unique_ptr<runtime::World>> world = app.scenes().load(path);
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_scene_path = path;
        DREAM_LOG_INFO("Opened {}", path);
        return {};
    }

    void switch_scene(runtime::App& app) {
        const char* const next = m_scene_path == k_first_scene ? k_second_scene : k_first_scene;
        if (const core::Result<void> opened = open_scene(app, next); !opened.has_value()) {
            DREAM_LOG_ERROR("{}", opened.error().message);
            return;
        }
        ++m_switches;
    }

    // Declared first so it is released last, after the listener that uses it.
    std::unique_ptr<runtime::World> m_world;
    runtime::Subscription m_switch_listener;
    rhi::BufferHandle m_vertices;
    rhi::BufferHandle m_indices;
    std::string m_scene_path;
    std::int64_t m_quit_after_frames = 0;
    std::int64_t m_switch_every_frames = 0;
    std::int64_t m_frames = 0;
    int m_switches = 0;
    int m_pipeline_loads = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<SceneDemo>();
}
