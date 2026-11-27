// A small top-down 2D game: a garden made in the Tiled map editor (ADR 0050), where a gardener
// walks among trees, in front of those behind them and behind those in front, as the camera sorts
// their layer by height (ADR 0049). Hedges, a stone wall and the pond are solid, from the map's
// tiles; trees and stones stop the gardener at their trunks.
//
// Controls: walk with WASD, the arrows, a gamepad's stick or d-pad, or by holding a finger or the
// mouse where to walk to. Escape quits.

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/physics_2d.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/sprites.h"
#include "dream/runtime/tile_map_files.h"
#include "dream/runtime/world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dream;
using core::Vec2;
using core::Vec3;

namespace {

constexpr float k_walk_speed = 3.5f;
// The round footprint of a trunk, which the gardener's own, in the scene, bumps into.
constexpr float k_trunk_radius = 0.3f;

class TopDown final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (auto ready = m_physics.initialize(app); !ready.has_value()) {
            return ready;
        }
        core::Result<std::unique_ptr<runtime::World>> world =
            app.scenes().load("assets:/scenes/garden.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_world->update_transforms();
        runtime::Registry& registry = m_world->registry();

        m_hero = m_world->find("gardener");
        m_camera = m_world->find("camera");
        m_garden = m_world->find("garden");
        const runtime::Entity start = m_world->find("start");
        if (m_hero == runtime::k_no_entity || m_camera == runtime::k_no_entity ||
            m_garden == runtime::k_no_entity || start == runtime::k_no_entity) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "The garden needs a gardener, a camera, its map, and a start in "
                               "the map"};
        }
        // The map's "start" point places the gardener.
        const Vec3 spawn =
            core::transform_point(registry.get<runtime::WorldTransform>(start).matrix, Vec3{});
        registry.get<runtime::Transform>(m_hero).position = spawn;

        // Hedges, the wall and the pond, from the map's solid tiles, and a trunk under every tree
        // and stone the map places.
        if (auto colliders = runtime::make_tile_colliders(*m_world, m_garden);
            !colliders.has_value()) {
            return colliders.error();
        }
        std::vector<Vec3> trunks;
        for (const auto [entity, object, transform] :
             registry.view<const runtime::MapObject, const runtime::WorldTransform>().each()) {
            if (object.type == "tree" || object.type == "stone" || object.type == "lamp") {
                trunks.push_back(core::transform_point(transform.matrix, Vec3{}));
            }
        }
        for (const Vec3& foot : trunks) {
            const runtime::Entity trunk = m_world->create_entity();
            registry.get<runtime::Transform>(trunk).position = foot + Vec3{0.0f, 0.2f, 0.0f};
            runtime::RigidBody2D body;
            body.desc.motion = physics2d::MotionType::Static;
            body.desc.shape = physics2d::CircleShape{.radius = k_trunk_radius};
            registry.emplace<runtime::RigidBody2D>(trunk, body);
        }
        m_world->update_transforms();
        m_physics.attach(m_world.get());

        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        m_script = config.get_bool("demo.script").value_or(false);
        // Checks compare a picture, so they hold every animation still, the water on its second
        // frame.
        if (config.get_bool("demo.hold_animation").value_or(false)) {
            m_hold = true;
            for (auto [entity, map] : registry.view<runtime::TileMap>().each()) {
                map.animation_speed = 0.0f;
                map.animation_time = 0.5;
            }
        }
        DREAM_LOG_INFO("The garden has {} trees and stones", trunks.size());
        return {};
    }

    void on_fixed_update(runtime::App& app, double step_seconds) override {
        steer(app);
        m_physics.step(static_cast<float>(step_seconds));
        ++m_steps;
        if (m_script && m_steps >= k_script_steps) {
            app.request_quit();
        }
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        m_physics.update_transforms(static_cast<float>(app.fixed_step_alpha()));
        animate_hero();
        runtime::update_sprite_animators(*m_world, time.delta_seconds);
        follow(app);
        m_world->update_transforms();

        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.19f, 0.36f, 0.22f, 1.0f});
    }

    void on_stop(runtime::App& /*app*/) override {
        m_physics.attach(nullptr);
        const Vec3 hero = m_world != nullptr
                              ? m_world->registry().get<runtime::Transform>(m_hero).position
                              : Vec3{};
        // The scripted walk goes up into the stone wall, whose bottom is at 11: stopped by it, the
        // gardener's middle stays a footprint below.
        const bool blocked = hero.y > 10.0f && hero.y < 11.0f;
        const std::string report =
            std::format("frames={} sprites={} x={:.2f} y={:.2f} blocked={}", m_frames,
                        m_renderer.last_sprite_count(), hero.x, hero.y, blocked ? 1 : 0);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    // A scripted walk for the check: left along the path, then up into the stone wall.
    static constexpr std::int64_t k_script_steps = 240;

    Vec2 read_controls(runtime::App& app) const {
        if (m_script) {
            return m_steps < 45 ? Vec2{-1.0f, 0.0f} : Vec2{0.0f, 1.0f};
        }
        const runtime::InputActions& input = app.input();
        Vec2 move{input.axis("move_left", "move_right"), input.axis("move_down", "move_up")};
        // A finger or the mouse held down: walk toward it.
        if (input.is_down("touch")) {
            const auto pointer = input.pointer_position();
            const std::optional<Vec2> target = runtime::screen_to_world_2d(
                *m_world, app.surface_size(), Vec2{pointer.x, pointer.y});
            if (target.has_value()) {
                const Vec3 hero = m_world->registry().get<runtime::Transform>(m_hero).position;
                const Vec2 toward{target->x - hero.x, target->y - (hero.y + 0.5f)};
                if (core::length(toward) > 0.2f) {
                    move = toward;
                }
            }
        }
        const float length = core::length(move);
        return length > 1.0f ? move / length : move;
    }

    void steer(runtime::App& app) {
        const Vec2 move = read_controls(app);
        m_physics.set_linear_velocity(m_hero, move * k_walk_speed);
        if (core::length(move) > 0.1f) {
            // The clip for the way it walks most: down, up, left or right.
            if (std::abs(move.x) > std::abs(move.y)) {
                m_facing = move.x < 0.0f ? "left" : "right";
            } else {
                m_facing = move.y < 0.0f ? "down" : "up";
            }
            m_walking = true;
        } else {
            m_walking = false;
        }
    }

    void animate_hero() {
        runtime::SpriteAnimator& animator =
            m_world->registry().get<runtime::SpriteAnimator>(m_hero);
        animator.play(m_facing);
        // Standing still: the first frame of the way it faces.
        animator.speed = m_walking && !m_hold ? 1.0f : 0.0f;
        if (!m_walking) {
            animator.time = 0.0f;
        }
    }

    // Keeps the gardener in the middle, without showing past the garden's edges.
    void follow(runtime::App& app) {
        const platform::SurfaceSize size = app.surface_size();
        if (size.width == 0 || size.height == 0) {
            return;
        }
        runtime::Registry& registry = m_world->registry();
        const runtime::Camera2D& camera = registry.get<runtime::Camera2D>(m_camera);
        const Vec3 hero = registry.get<runtime::Transform>(m_hero).position;
        const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
        const float half_height = std::max(camera.height, camera.min_width / aspect) * 0.5f;
        const float half_width = half_height * aspect;
        const auto clamp = [](float value, float half, float extent) {
            return half * 2.0f >= extent ? extent * 0.5f
                                         : std::clamp(value, half, extent - half);
        };
        Vec3& position = registry.get<runtime::Transform>(m_camera).position;
        position.x = clamp(hero.x, half_width, k_garden_width);
        position.y = clamp(hero.y + 0.5f, half_height, k_garden_height);
    }

    static constexpr float k_garden_width = 24.0f;
    static constexpr float k_garden_height = 16.0f;

    runtime::Renderer m_renderer;
    runtime::Physics2D m_physics;
    std::unique_ptr<runtime::World> m_world;
    runtime::Entity m_hero = runtime::k_no_entity;
    runtime::Entity m_camera = runtime::k_no_entity;
    runtime::Entity m_garden = runtime::k_no_entity;
    std::string m_facing = "down";
    bool m_walking = false;
    bool m_hold = false;
    std::int64_t m_steps = 0;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
    bool m_script = false;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<TopDown>();
}
