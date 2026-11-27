// Physics in 3D (docs/adr/0011): the scene file gives entities a `rigid_body` and a `joint`, and
// Physics3D simulates them, moving their Transforms. The game only adds what a scene can't say:
//
// - it tints the balls inside the sensor zone, from the contacts Physics3D publishes;
// - it sweeps a laser across the yard with ray casts, drawn as debug lines;
// - it throws a ball wherever the player clicks or taps, giving it a body from code.
//
// Controls: click, tap, Space or a gamepad's south button throws a ball. R or start begins again,
// F or north shows the physics shapes. Turn and zoom with A/D and W/S, the arrows, the left stick
// and triggers, or by dragging with the right mouse button. Escape quits.

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/runtime/app.h"
#include "dream/runtime/debug_lines.h"
#include "dream/runtime/game.h"
#include "dream/runtime/physics_3d.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"
#include "sample_camera.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>

using namespace dream;
using core::Vec2;
using core::Vec3;
using core::Vec4;

namespace {

constexpr double k_step = 1.0 / 120.0;
constexpr std::size_t k_most_thrown = 30;
constexpr float k_throw_speed = 16.0f;
const Vec4 k_inside_tint{0.3f, 1.0f, 0.4f, 1.0f};

class Physics final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        // Before the scene loads, so its "rigid_body" and "joint" components are known.
        if (auto ready = m_physics.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto ball = app.assets().load<runtime::ModelAsset>("assets:/models/ball.glb");
        if (!ball.has_value()) {
            return ball.error();
        }
        m_ball_model = std::move(ball).value();
        m_contacts = app.events().subscribe<runtime::Contact3D>(
            [this](const runtime::Contact3D& contact) { on_contact(contact); });

        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        m_held_time = config.get_float("demo.time");
        m_app = &app;
        return load_scene();
    }

    void on_fixed_update(runtime::App& /*app*/, double step_seconds) override {
        if (!m_held) {
            m_physics.step(static_cast<float>(step_seconds));
            m_seconds += step_seconds;
        }
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        const runtime::InputActions& input = app.input();
        if (input.was_pressed("restart")) {
            if (auto loaded = load_scene(); !loaded.has_value()) {
                DREAM_LOG_ERROR("{}", loaded.error().message);
            }
        }
        if (input.was_pressed("shapes")) {
            m_show_shapes = !m_show_shapes;
        }
        if (input.was_pressed("throw") && !m_held) {
            throw_ball(app);
        }
        m_camera.update(app, static_cast<float>(time.delta_seconds));
        m_physics.update_transforms(m_held ? 1.0f : static_cast<float>(app.fixed_step_alpha()));
        m_world->update_transforms();
        if (input.was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        runtime::DebugLines& lines = m_renderer.debug_lines();
        sweep_laser(lines);
        if (m_show_shapes) {
            m_physics.debug_draw(lines);
        }
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.6f, 0.72f, 0.86f, 1.0f});
    }

    void on_stop(runtime::App& /*app*/) override {
        m_physics.attach(nullptr);
        const auto bodies = m_world->registry().view<const runtime::RigidBody3D>().size();
        const std::string report = std::format("frames={} bodies={} inside={}", m_frames, bodies,
                                               m_inside.size());
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    core::Result<void> load_scene() {
        m_physics.attach(nullptr);
        m_inside.clear();
        m_thrown.clear();
        auto world = m_app->scenes().load("assets:/scenes/physics.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_zone = m_world->find("zone");
        m_laser = m_world->find("laser");
        m_world->update_transforms();
        m_physics.attach(m_world.get());
        m_camera.attach(*m_world);
        m_seconds = 0.0;
        m_held = false;
        // Checks compare a picture: they run the simulation in fixed steps for a while, the same
        // every run, and hold it there.
        if (m_held_time.has_value()) {
            for (double simulated = 0.0; simulated < *m_held_time; simulated += k_step) {
                m_physics.step(static_cast<float>(k_step));
                m_seconds += k_step;
            }
            m_held = true;
        }
        return {};
    }

    // Balls change color while inside the sensor zone, and back as they leave.
    void on_contact(const runtime::Contact3D& contact) {
        if (contact.a != m_zone && contact.b != m_zone) {
            return;
        }
        const runtime::Entity other = contact.a == m_zone ? contact.b : contact.a;
        runtime::Registry& registry = m_world->registry();
        if (!registry.valid(other) || !registry.all_of<runtime::MeshRenderer>(other)) {
            return;
        }
        auto& mesh = registry.get<runtime::MeshRenderer>(other);
        if (contact.kind == physics3d::ContactEvent::Kind::Began) {
            m_inside.insert(other);
            mesh.tint = k_inside_tint;
        } else {
            m_inside.erase(other);
            mesh.tint = Vec4{1.0f, 1.0f, 1.0f, 1.0f};
        }
    }

    // A ray from the laser's top, turning back and forth, stopped by the first body it meets.
    void sweep_laser(runtime::DebugLines& lines) const {
        if (m_laser == runtime::k_no_entity) {
            return;
        }
        const Vec3 origin = m_world->registry().get<runtime::Transform>(m_laser).position +
                            Vec3{0.0f, 0.7f, 0.0f};
        const auto angle = static_cast<float>(3.75 + 0.45 * std::sin(m_seconds * 0.8));
        const Vec3 direction = core::normalized(Vec3{std::cos(angle), -0.12f, std::sin(angle)});
        constexpr float k_reach = 25.0f;
        const Vec4 red{1.0f, 0.1f, 0.05f, 1.0f};
        if (const std::optional<runtime::RayHit3D> hit =
                m_physics.cast_ray(origin, direction, k_reach)) {
            lines.line(origin, hit->point, red);
            // A small star where it lands, along the surface's normal and across it.
            const Vec3 normal = hit->normal * 0.3f;
            lines.line(hit->point, hit->point + normal, Vec4{1.0f, 0.9f, 0.2f, 1.0f});
            lines.box(hit->point, core::Quat{}, Vec3{0.06f, 0.06f, 0.06f}, red);
        } else {
            lines.line(origin, origin + direction * k_reach, red);
        }
    }

    // A ball from the camera, toward the pointer, or straight ahead from a gamepad.
    void throw_ball(runtime::App& app) {
        const runtime::InputActions& input = app.input();
        const platform::SurfaceSize size = app.surface_size();
        const auto pointer = input.pointer_position();
        const Vec2 pixel = input.last_source() == runtime::InputActions::Source::Gamepad
                               ? Vec2{static_cast<float>(size.width) * 0.5f,
                                      static_cast<float>(size.height) * 0.5f}
                               : Vec2{pointer.x, pointer.y};
        const std::optional<runtime::CameraRay> ray = runtime::camera_ray(*m_world, size, pixel);
        if (!ray.has_value()) {
            return;
        }
        if (m_thrown.size() >= k_most_thrown) {
            m_world->destroy_entity(m_thrown.front());
            m_thrown.pop_front();
        }
        const runtime::Entity ball = m_world->create_entity();
        constexpr float k_radius = 0.25f;
        runtime::Transform& transform = m_world->registry().get<runtime::Transform>(ball);
        transform.position = ray->origin + ray->direction * 1.0f;
        transform.scale = Vec3{k_radius, k_radius, k_radius};
        m_world->registry().emplace<runtime::MeshRenderer>(
            ball, runtime::MeshRenderer{.model = m_ball_model,
                                        .tint = Vec4{0.95f, 0.6f, 0.1f, 1.0f}});
        runtime::RigidBody3D body;
        body.desc.shape = physics3d::SphereShape{.radius = k_radius};
        body.desc.mass = 2.0f;
        body.desc.restitution = 0.3f;
        body.desc.linear_velocity = ray->direction * k_throw_speed;
        body.desc.continuous_collision = true;
        m_world->registry().emplace<runtime::RigidBody3D>(ball, body);
        m_thrown.push_back(ball);
    }

    runtime::App* m_app = nullptr;
    runtime::Renderer m_renderer;
    runtime::Physics3D m_physics;
    std::unique_ptr<runtime::World> m_world;
    samples::SampleCamera m_camera;
    runtime::AssetRef<runtime::ModelAsset> m_ball_model;
    runtime::Subscription m_contacts;
    runtime::Entity m_zone = runtime::k_no_entity;
    runtime::Entity m_laser = runtime::k_no_entity;
    std::unordered_set<runtime::Entity> m_inside;
    std::deque<runtime::Entity> m_thrown;
    std::optional<double> m_held_time;
    double m_seconds = 0.0;
    bool m_held = false;
    bool m_show_shapes = false;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Physics>();
}
