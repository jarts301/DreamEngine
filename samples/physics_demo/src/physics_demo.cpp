// Physics in 3D and 2D. Scenes give entities bodies and joints; the game only shoots balls, drops
// shapes, turns a paddle and counts what crosses a sensor. The backends come from dream.toml.
//
// Controls: shoot or drop with Space, a click or a tap. Tab switches between the 3D and 2D scenes,
// and D shows the physics shapes. On a touch screen, tap the top-left corner to switch and the
// top-right corner for the shapes.

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/physics_2d.h"
#include "dream/runtime/physics_3d.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace dream;
using core::Quat;
using core::Vec2;
using core::Vec3;
using core::Vec4;

namespace {

// Turns a kinematic body's Transform, which its rigid body then follows.
struct Spin {
    float speed = 1.0f;
};

core::Result<void> read_spin(runtime::ComponentContext& context) {
    const core::Result<float> speed = context.data.number("speed", 1.0f);
    if (!speed.has_value()) {
        return speed.error();
    }
    context.world.registry().emplace<Spin>(context.entity, speed.value());
    return {};
}

struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// The ray from a world's camera through a pixel.
std::optional<Ray> pointer_ray(const runtime::World& world, platform::SurfaceSize size, float x,
                               float y) {
    const runtime::Registry& registry = world.registry();
    const runtime::Entity entity = registry.view<const runtime::Camera>().front();
    if (entity == runtime::k_no_entity || size.width == 0 || size.height == 0) {
        return std::nullopt;
    }
    const runtime::Camera& camera = registry.get<const runtime::Camera>(entity);
    const Vec3 eye = registry.get<const runtime::Transform>(entity).position;
    const Vec3 forward = core::normalized(camera.target.value_or(eye - Vec3{0.0f, 0.0f, 1.0f}) - eye);
    const Vec3 right = core::normalized(core::cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 up = core::cross(right, forward);

    const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
    const float fov =
        runtime::fitted_vertical_fov(core::radians(camera.vertical_fov_degrees), aspect);
    const float half_height = std::tan(fov * 0.5f);
    const float ndc_x = 2.0f * x / static_cast<float>(size.width) - 1.0f;
    const float ndc_y = 1.0f - 2.0f * y / static_cast<float>(size.height);
    return Ray{eye, core::normalized(forward + right * (ndc_x * half_height * aspect) +
                                     up * (ndc_y * half_height))};
}

class PhysicsDemo final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        m_scripted = config.get_bool("demo.scripted").value_or(false);
        m_show_shapes = config.get_bool("demo.show_shapes").value_or(false);
        m_flat = config.get_string("demo.mode").value_or("3d") == "2d";

        if (core::Result<void> ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (core::Result<void> ready = m_physics_3d.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (core::Result<void> ready = m_physics_2d.initialize(app); !ready.has_value()) {
            return ready;
        }
        app.scenes().register_component("spin", &read_spin);

        core::Result<std::unique_ptr<runtime::World>> playground =
            app.scenes().load("assets:/scenes/playground.toml");
        if (!playground.has_value()) {
            return playground.error();
        }
        core::Result<std::unique_ptr<runtime::World>> flatland =
            app.scenes().load("assets:/scenes/flatland.toml");
        if (!flatland.has_value()) {
            return flatland.error();
        }
        m_playground = std::move(playground).value();
        m_flatland = std::move(flatland).value();
        m_physics_3d.attach(m_playground.get());
        m_physics_2d.attach(m_flatland.get());

        core::Result<runtime::AssetRef<runtime::ModelAsset>> ball =
            app.assets().load<runtime::ModelAsset>("assets:/models/ball.glb");
        core::Result<runtime::AssetRef<runtime::ModelAsset>> crate =
            app.assets().load<runtime::ModelAsset>("assets:/models/crate.glb");
        if (!ball.has_value() || !crate.has_value()) {
            return ball.has_value() ? crate.error() : ball.error();
        }
        m_ball = std::move(ball).value();
        m_crate = std::move(crate).value();

        m_zone = m_playground->find("zone");
        m_contacts_3d = app.events().subscribe<runtime::Contact3D>(
            [this](const runtime::Contact3D& contact) {
                if (contact.kind == physics3d::ContactEvent::Kind::Began &&
                    (contact.a == m_zone || contact.b == m_zone)) {
                    ++m_zone_entries;
                    DREAM_LOG_INFO("Something entered the zone ({} so far)", m_zone_entries);
                }
            });
        m_contacts_2d = app.events().subscribe<runtime::Contact2D>(
            [this](const runtime::Contact2D& contact) {
                if (contact.kind == physics2d::ContactEvent::Kind::Began) {
                    ++m_contacts_2d_count;
                }
            });

        DREAM_LOG_INFO("Showing the {} scene", m_flat ? "2D" : "3D");
        return {};
    }

    void on_fixed_update(runtime::App& /*app*/, double step_seconds) override {
        m_simulated_seconds += step_seconds;
        const auto step = static_cast<float>(step_seconds);
        if (m_flat) {
            m_physics_2d.step(step);
            return;
        }
        for (const auto [entity, transform, spin] :
             m_playground->registry().view<runtime::Transform, Spin>().each()) {
            transform.rotation = core::normalized(
                core::quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, spin.speed * step) *
                transform.rotation);
        }
        m_physics_3d.step(step);
    }

    void on_update(runtime::App& app, const core::FrameTime& /*time*/) override {
        ++m_frames;
        if (m_scripted) {
            run_script(app);
        } else {
            handle_input(app);
        }

        const auto alpha = static_cast<float>(app.fixed_step_alpha());
        if (m_flat) {
            m_physics_2d.update_transforms(alpha);
            m_flatland->update_transforms();
        } else {
            m_physics_3d.update_transforms(alpha);
            m_playground->update_transforms();
        }

        if (app.input().was_pressed("quit") || m_script_done ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        if (m_show_shapes) {
            if (m_flat) {
                m_physics_2d.debug_draw(m_renderer.debug_lines());
            } else {
                m_physics_3d.debug_draw(m_renderer.debug_lines());
            }
        }
        m_renderer.render(m_flat ? *m_flatland : *m_playground, commands, app.surface_size());
        m_lines = std::max(m_lines, m_renderer.last_line_count());
    }

    void on_stop(runtime::App& /*app*/) override {
        // The 3D scene is detached first, so its results are read while it still exists.
        const std::string report = std::format(
            "frames={} bodies3d={} bodies2d={} pyramid={} pendulum={} zone={} stack2d={} "
            "seesaw={} contacts2d={} lines={}",
            m_frames, m_physics_3d.backend()->body_count(), m_physics_2d.backend()->body_count(),
            m_pyramid, m_pendulum, m_zone_entries, m_stack_2d, m_seesaw, m_contacts_2d_count,
            m_lines > 0 ? "drawn" : "none");
        m_physics_3d.attach(nullptr);
        m_physics_2d.attach(nullptr);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

    void on_pause(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Paused");
    }

    void on_resume(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Resumed after {} frames", m_frames);
    }

private:
    void handle_input(runtime::App& app) {
        if (app.input().was_pressed("switch")) {
            switch_scene();
        }
        if (app.input().was_pressed("shapes")) {
            m_show_shapes = !m_show_shapes;
        }
        if (!app.input().was_pressed("shoot")) {
            return;
        }

        const runtime::InputActions::PointerPosition pointer = app.input().pointer_position();
        const platform::SurfaceSize size = app.surface_size();
        // The top strip of a touch screen holds the two switches.
        if (pointer.y < 0.12f * static_cast<float>(size.height)) {
            if (pointer.x < 0.5f * static_cast<float>(size.width)) {
                switch_scene();
            } else {
                m_show_shapes = !m_show_shapes;
            }
            return;
        }
        if (m_flat) {
            drop_at_pointer(size, pointer.x, pointer.y);
        } else {
            shoot(size, pointer.x, pointer.y);
        }
    }

    // A fixed sequence for automated checks, timed by simulated seconds so that it plays out the
    // same at any frame rate.
    void run_script(runtime::App& app) {
        const platform::SurfaceSize size = app.surface_size();
        const float middle_x = 0.5f * static_cast<float>(size.width);
        const float middle_y = 0.5f * static_cast<float>(size.height);
        const auto at = [this](double seconds) {
            return m_script_seconds < seconds && m_simulated_seconds >= seconds;
        };

        if (at(1.0)) {
            measure_3d();
        }
        if (at(1.1) || at(1.4)) {
            shoot(size, middle_x, middle_y);
        }
        if (at(2.0)) {
            m_show_shapes = true;
        }
        if (at(2.5)) {
            switch_scene();
        }
        if (at(2.6)) {
            drop(Vec2{6.0f, 4.0f}, true);
        }
        if (at(4.2)) {
            measure_2d();
            m_script_done = true;
        }
        m_script_seconds = m_simulated_seconds;
    }

    void switch_scene() {
        m_flat = !m_flat;
        DREAM_LOG_INFO("Showing the {} scene", m_flat ? "2D" : "3D");
    }

    void shoot(platform::SurfaceSize size, float x, float y) {
        const std::optional<Ray> ray = pointer_ray(*m_playground, size, x, y);
        if (!ray.has_value()) {
            return;
        }
        runtime::Registry& registry = m_playground->registry();
        const runtime::Entity ball = m_playground->create_entity();
        runtime::Transform& transform = registry.get<runtime::Transform>(ball);
        transform.position = ray->origin + ray->direction;
        transform.scale = Vec3{0.35f, 0.35f, 0.35f};
        registry.emplace<runtime::MeshRenderer>(ball, m_ball, Vec4{1.0f, 0.55f, 0.1f, 1.0f});

        runtime::RigidBody3D body;
        body.desc.shape = physics3d::SphereShape{0.35f};
        body.desc.mass = 2.0f;
        body.desc.restitution = 0.3f;
        body.desc.linear_velocity = ray->direction * 22.0f;
        body.desc.continuous_collision = true;
        registry.emplace<runtime::RigidBody3D>(ball, body);
        keep_recent(m_shots, *m_playground, ball);
    }

    void drop_at_pointer(platform::SurfaceSize size, float x, float y) {
        const std::optional<Ray> ray = pointer_ray(*m_flatland, size, x, y);
        if (!ray.has_value() || std::abs(ray->direction.z) < 1e-4f) {
            return;
        }
        // Where the ray crosses the plane z = 0, dropped from near the top of the view.
        const float t = -ray->origin.z / ray->direction.z;
        const float drop_x = ray->origin.x + ray->direction.x * t;
        drop(Vec2{std::clamp(drop_x, -11.0f, 11.0f), 9.5f}, (m_drops++ % 2) == 0);
    }

    void drop(Vec2 position, bool round) {
        runtime::Registry& registry = m_flatland->registry();
        const runtime::Entity shape = m_flatland->create_entity();
        runtime::Transform& transform = registry.get<runtime::Transform>(shape);
        transform.position = Vec3{position.x, position.y, 0.0f};
        transform.scale = round ? Vec3{0.45f, 0.45f, 0.45f} : Vec3{0.4f, 0.4f, 0.4f};
        registry.emplace<runtime::MeshRenderer>(shape, round ? m_ball : m_crate,
                                                Vec4{1.0f, 0.55f, 0.1f, 1.0f});

        runtime::RigidBody2D body;
        body.desc.shape = round ? physics2d::Shape{physics2d::CircleShape{0.45f}}
                                : physics2d::Shape{physics2d::BoxShape{Vec2{0.4f, 0.4f}}};
        body.desc.mass = 4.0f;
        body.desc.restitution = 0.2f;
        registry.emplace<runtime::RigidBody2D>(shape, body);
        keep_recent(m_drops_kept, *m_flatland, shape);
    }

    // Removes the oldest of the game's own objects, so tapping forever doesn't slow the game down.
    static void keep_recent(std::deque<runtime::Entity>& kept, runtime::World& world,
                            runtime::Entity added) {
        constexpr std::size_t k_most = 30;
        kept.push_back(added);
        if (kept.size() > k_most) {
            world.destroy_entity(kept.front());
            kept.pop_front();
        }
    }

    void measure_3d() {
        const std::optional<physics3d::BodyState> top =
            m_physics_3d.body_state(m_playground->find("crate_top"));
        m_pyramid = top.has_value() && std::abs(top->position.y - 3.5f) < 0.1f &&
                            std::abs(top->position.x) < 0.1f
                        ? "standing"
                        : "fallen";

        // The pendulum's distance from its pivot, which the joint keeps at 3 m.
        const std::optional<physics3d::BodyState> bob =
            m_physics_3d.body_state(m_playground->find("pendulum"));
        const float length =
            bob.has_value() ? core::length(bob->position - Vec3{-7.0f, 5.5f, -2.0f}) : 0.0f;
        m_pendulum = std::abs(length - 3.0f) < 0.05f ? "held" : std::format("{:.2f}", length);

        // A ray straight down onto the pyramid finds its top crate.
        const std::optional<runtime::RayHit3D> hit = m_physics_3d.cast_ray(
            Vec3{0.0f, 10.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
        const runtime::Entity found = hit.has_value() ? hit->entity : runtime::k_no_entity;
        DREAM_LOG_INFO("3D: pyramid {}, pendulum {}, ray down hits {}", m_pyramid, m_pendulum,
                       found == m_playground->find("crate_top") ? "the top crate" : "something else");
        if (found != m_playground->find("crate_top")) {
            m_pyramid = "unreachable";
        }
    }

    void measure_2d() {
        const std::optional<physics2d::BodyState> top =
            m_physics_2d.body_state(m_flatland->find("box_3"));
        m_stack_2d = top.has_value() && std::abs(top->position.y - 2.5f) < 0.1f &&
                             std::abs(top->position.x + 5.0f) < 0.1f
                         ? "standing"
                         : "fallen";
        const std::optional<physics2d::BodyState> plank =
            m_physics_2d.body_state(m_flatland->find("plank"));
        const float degrees = plank.has_value() ? plank->angle * 180.0f / core::k_pi : 0.0f;
        m_seesaw = degrees < -15.0f ? "tilted" : std::format("{:.1f}", degrees);
        DREAM_LOG_INFO("2D: stack {}, seesaw {}", m_stack_2d, m_seesaw);
    }

    runtime::Renderer m_renderer;
    runtime::Physics3D m_physics_3d;
    runtime::Physics2D m_physics_2d;
    std::unique_ptr<runtime::World> m_playground;
    std::unique_ptr<runtime::World> m_flatland;
    runtime::AssetRef<runtime::ModelAsset> m_ball;
    runtime::AssetRef<runtime::ModelAsset> m_crate;
    runtime::Subscription m_contacts_3d;
    runtime::Subscription m_contacts_2d;
    std::deque<runtime::Entity> m_shots;
    std::deque<runtime::Entity> m_drops_kept;
    runtime::Entity m_zone = runtime::k_no_entity;

    double m_simulated_seconds = 0.0;
    double m_script_seconds = 0.0;
    std::int64_t m_quit_after_frames = 0;
    std::int64_t m_frames = 0;
    int m_zone_entries = 0;
    int m_contacts_2d_count = 0;
    int m_drops = 0;
    std::uint32_t m_lines = 0;
    std::string m_pyramid = "unmeasured";
    std::string m_pendulum = "unmeasured";
    std::string m_stack_2d = "unmeasured";
    std::string m_seesaw = "unmeasured";
    bool m_scripted = false;
    bool m_script_done = false;
    bool m_show_shapes = false;
    bool m_flat = false;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<PhysicsDemo>();
}
