// A small 2D platformer: a tile map level with colliders made from its tiles, an animated hero that
// runs and jumps with 2D physics, coins to collect, and a camera that follows (ADR 0039).
//
// Controls: move with A and D, the arrows, a gamepad's stick or d-pad, or by holding the left or
// right third of a touch screen; jump with Space, W, a gamepad's south button, or a tap in the
// middle. Escape quits.

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/physics_2d.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/sprites.h"
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

// Marks a coin, which the hero collects by touching it.
struct Coin {};

core::Result<void> read_coin(runtime::ComponentContext& context) {
    if (auto known = context.data.check_keys({}); !known.has_value()) {
        return known;
    }
    context.world.registry().emplace<Coin>(context.entity);
    return {};
}

constexpr float k_run_speed = 6.0f;
constexpr float k_jump_speed = 10.5f;
// How far the hero's feet are below its middle: the capsule's half height and radius.
constexpr float k_feet = 0.45f;
// Below this the hero has fallen out of the level.
constexpr float k_fall_limit = -4.0f;

class Platformer final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (auto ready = m_physics.initialize(app); !ready.has_value()) {
            return ready;
        }
        app.scenes().register_component("coin", &read_coin);

        core::Result<std::unique_ptr<runtime::World>> world =
            app.scenes().load("assets:/scenes/level.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_world->update_transforms();

        m_hero = m_world->find("hero");
        m_level = m_world->find("level");
        m_camera = m_world->find("camera");
        if (m_hero == runtime::k_no_entity || m_level == runtime::k_no_entity ||
            m_camera == runtime::k_no_entity) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "The level needs a hero, a level map and a camera"};
        }
        m_spawn = m_world->registry().get<runtime::Transform>(m_hero).position;
        if (auto colliders = runtime::make_tile_colliders(*m_world, m_level);
            !colliders.has_value()) {
            return colliders.error();
        }
        m_physics.attach(m_world.get());
        m_coins_left = static_cast<int>(m_world->registry().view<Coin>().size());

        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        m_script = config.get_bool("demo.script").value_or(false);
        // Checks compare a picture, so they hold every animation still.
        if (config.get_bool("demo.hold_animation").value_or(false)) {
            for (auto [entity, animator] : m_world->registry().view<runtime::SpriteAnimator>().each()) {
                animator.speed = 0.0f;
            }
        }
        DREAM_LOG_INFO("{} coins to collect", m_coins_left);
        return {};
    }

    void on_fixed_update(runtime::App& app, double step_seconds) override {
        const float step = static_cast<float>(step_seconds);
        steer(app);
        m_physics.step(step);
        ++m_steps;
        collect_coins();
        if (m_script && m_steps >= k_script_steps) {
            app.request_quit();
        }
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        m_physics.update_transforms(static_cast<float>(app.fixed_step_alpha()));
        m_world->update_transforms();
        animate_hero();
        runtime::update_sprite_animators(*m_world, time.delta_seconds);
        follow(app, static_cast<float>(time.delta_seconds));
        m_world->update_transforms();

        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.36f, 0.62f, 0.92f, 1.0f});
    }

    void on_stop(runtime::App& /*app*/) override {
        m_physics.attach(nullptr);
        const Vec3 hero = m_world != nullptr
                              ? m_world->registry().get<runtime::Transform>(m_hero).position
                              : Vec3{};
        const std::string report =
            std::format("frames={} sprites={} coins={} x={:.0f}", m_frames,
                        m_renderer.last_sprite_count(), m_collected, hero.x);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    // A scripted run for the check: right along the ground, a jump over the first pit, and on.
    static constexpr std::int64_t k_script_steps = 300;

    struct Controls {
        float move = 0.0f;
        bool jump = false;
    };

    Controls read_controls(runtime::App& app) const {
        Controls controls;
        if (m_script) {
            controls.move = 1.0f;
            controls.jump = m_steps == 20 || m_steps == 150 || m_steps == 200;
            return controls;
        }
        const runtime::InputActions& input = app.input();
        controls.move = input.axis("move_left", "move_right");
        controls.jump = input.was_pressed("jump");
        // Touch: hold the left or right third to move, tap the middle to jump.
        if (input.is_down("touch")) {
            const auto pointer = input.pointer_position();
            const float width = static_cast<float>(app.surface_size().width);
            if (pointer.x < width / 3.0f) {
                controls.move = -1.0f;
            } else if (pointer.x > width * 2.0f / 3.0f) {
                controls.move = 1.0f;
            } else if (input.was_pressed("touch")) {
                controls.jump = true;
            }
        }
        return controls;
    }

    [[nodiscard]] bool on_ground() const {
        const runtime::Transform& hero = m_world->registry().get<runtime::Transform>(m_hero);
        // Two short rays down from just under the feet, at either side.
        for (const float side : {-0.2f, 0.2f}) {
            const Vec2 origin{hero.position.x + side, hero.position.y - k_feet - 0.01f};
            if (m_physics.cast_ray(origin, Vec2{0.0f, -1.0f}, 0.08f).has_value()) {
                return true;
            }
        }
        return false;
    }

    void steer(runtime::App& app) {
        const std::optional<physics2d::BodyState> state = m_physics.body_state(m_hero);
        if (!state.has_value()) {
            return;
        }
        // Jumps pressed between fixed steps count at the next one.
        const Controls controls = read_controls(app);
        m_jump_wanted = m_jump_wanted || controls.jump;
        Vec2 velocity{controls.move * k_run_speed, state->linear_velocity.y};
        if (m_jump_wanted && on_ground()) {
            velocity.y = k_jump_speed;
        }
        m_jump_wanted = false;
        m_physics.set_linear_velocity(m_hero, velocity);
        if (controls.move != 0.0f) {
            m_facing_left = controls.move < 0.0f;
        }
        // Out of the level: back to the start.
        if (state->position.y < k_fall_limit) {
            m_physics.teleport(m_hero, Vec2{m_spawn.x, m_spawn.y}, 0.0f);
            m_physics.set_linear_velocity(m_hero, Vec2{});
        }
    }

    void collect_coins() {
        const Vec3 hero = m_world->registry().get<runtime::Transform>(m_hero).position;
        std::vector<runtime::Entity> taken;
        for (const auto [entity, transform] :
             m_world->registry().view<const Coin, const runtime::Transform>().each()) {
            const float dx = transform.position.x - hero.x;
            const float dy = transform.position.y - hero.y;
            if (dx * dx + dy * dy < 0.6f * 0.6f) {
                taken.push_back(entity);
            }
        }
        for (const runtime::Entity coin : taken) {
            m_world->destroy_entity(coin);
            ++m_collected;
            --m_coins_left;
            DREAM_LOG_INFO("Coin! {} collected, {} left", m_collected, m_coins_left);
        }
    }

    void animate_hero() {
        runtime::Registry& registry = m_world->registry();
        runtime::SpriteRenderer& sprite = registry.get<runtime::SpriteRenderer>(m_hero);
        runtime::SpriteAnimator& animator = registry.get<runtime::SpriteAnimator>(m_hero);
        sprite.flip_x = m_facing_left;
        const std::optional<physics2d::BodyState> state = m_physics.body_state(m_hero);
        if (!state.has_value() || animator.speed == 0.0f) {
            return;
        }
        if (!on_ground()) {
            animator.play(state->linear_velocity.y > 0.0f ? "jump" : "fall");
        } else if (std::abs(state->linear_velocity.x) > 0.1f) {
            animator.play("run");
        } else {
            animator.play("idle");
        }
    }

    // Keeps the hero in view, ahead of it a little, without showing past the level's ends.
    void follow(runtime::App& app, float seconds) {
        const platform::SurfaceSize size = app.surface_size();
        if (size.width == 0 || size.height == 0) {
            return;
        }
        runtime::Registry& registry = m_world->registry();
        const runtime::Camera2D& camera = registry.get<runtime::Camera2D>(m_camera);
        const runtime::TileMap& map = registry.get<runtime::TileMap>(m_level);
        const Vec3 hero = registry.get<runtime::Transform>(m_hero).position;
        Vec3& position = registry.get<runtime::Transform>(m_camera).position;
        const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
        const float view_height = std::max(camera.height, camera.min_width / aspect);
        const float half_width = view_height * 0.5f * aspect;
        const float level_width = static_cast<float>(map.columns) * map.tile_size.x;
        const float wanted_x = std::clamp(hero.x, half_width, std::max(half_width,
                                                                        level_width - half_width));
        const float wanted_y = std::max(view_height * 0.5f, hero.y + 2.0f);
        // Eases toward where it wants to be, quickly enough to keep up with a run.
        const float ease = 1.0f - std::exp(-6.0f * seconds);
        position.x += (wanted_x - position.x) * (m_frames <= 1 ? 1.0f : ease);
        position.y += (wanted_y - position.y) * (m_frames <= 1 ? 1.0f : ease);
    }

    runtime::Renderer m_renderer;
    runtime::Physics2D m_physics;
    std::unique_ptr<runtime::World> m_world;
    runtime::Entity m_hero = runtime::k_no_entity;
    runtime::Entity m_level = runtime::k_no_entity;
    runtime::Entity m_camera = runtime::k_no_entity;
    Vec3 m_spawn;
    bool m_facing_left = false;
    bool m_jump_wanted = false;
    int m_coins_left = 0;
    int m_collected = 0;
    std::int64_t m_steps = 0;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
    bool m_script = false;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Platformer>();
}
