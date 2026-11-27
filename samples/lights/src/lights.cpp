// Lights (docs/guides/lighting.md): the scene is a dark courtyard, and the game adds its lights as
// entities with a PointLight or SpotLight and a Transform, then moves them every frame. 240 point
// lights in rings and four spot lights are more than `forward` draws, so dream.toml lists
// `forward_plus`, which sorts up to 256 into clusters.
//
// Controls: turn and zoom with A/D and W/S, the arrows, a gamepad's left stick and triggers, or by
// dragging with a finger or the mouse. Space, or a gamepad's south button, stops the lights. Escape
// quits.

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"
#include "sample_camera.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

using namespace dream;
using core::Vec3;

namespace {

constexpr int k_rings = 8;
constexpr int k_lights_per_ring = 30;
constexpr float k_tau = 2.0f * std::numbers::pi_v<float>;

// A bright color around the color wheel, from 0 to 1.
Vec3 hue(float turn) {
    const auto channel = [turn](float offset) {
        return 0.5f + 0.5f * std::cos(k_tau * (turn - offset));
    };
    return Vec3{channel(0.0f), channel(1.0f / 3.0f), channel(2.0f / 3.0f)};
}

class Lights final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto world = app.scenes().load("assets:/scenes/lights.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_camera.attach(*m_world);
        runtime::Registry& registry = m_world->registry();

        // Rings of small lights, each ring wider and turning the other way.
        for (int ring = 0; ring < k_rings; ++ring) {
            for (int index = 0; index < k_lights_per_ring; ++index) {
                const runtime::Entity entity = m_world->create_entity();
                const float along = static_cast<float>(index) / k_lights_per_ring;
                registry.emplace<runtime::PointLight>(
                    entity, runtime::PointLight{.color = hue(along + 0.1f * ring),
                                                .intensity = 0.5f,
                                                .range = 0.8f});
                m_orbits.push_back(Orbit{.entity = entity,
                                         .radius = 1.4f + 0.8f * static_cast<float>(ring),
                                         .angle = k_tau * along,
                                         .speed = (ring % 2 == 0 ? 0.25f : -0.2f),
                                         .height = 0.25f + 0.1f * static_cast<float>(ring % 3)});
            }
        }
        // Spot lights high up, each sweeping a circle on the ground.
        for (int index = 0; index < 4; ++index) {
            const runtime::Entity entity = m_world->create_entity();
            registry.emplace<runtime::SpotLight>(
                entity, runtime::SpotLight{.color = Vec3{1.0f, 0.92f, 0.8f},
                                           .intensity = 250.0f,
                                           .range = 16.0f,
                                           .inner_angle = 10.0f,
                                           .outer_angle = 13.0f});
            m_spots.push_back(entity);
        }

        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        // Checks compare a picture, so they place the lights at one moment and stop them.
        if (const std::optional<double> held = config.get_float("demo.time")) {
            m_time = *held;
            m_moving = false;
        }
        place_lights();
        DREAM_LOG_INFO("{} point lights and {} spot lights", m_orbits.size(), m_spots.size());
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        if (app.input().was_pressed("pause")) {
            m_moving = !m_moving;
        }
        if (m_moving) {
            m_time += time.delta_seconds;
        }
        place_lights();
        m_camera.update(app, static_cast<float>(time.delta_seconds));
        m_world->update_transforms();
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        // A night sky behind the courtyard.
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.01f, 0.012f, 0.03f, 1.0f});
    }

    void on_stop(runtime::App& /*app*/) override {
        const std::string report = std::format(
            "frames={} draws={} lights={} dropped={}", m_frames, m_renderer.last_draw_count(),
            m_renderer.last_light_count(), m_renderer.last_dropped_light_count());
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    struct Orbit {
        runtime::Entity entity;
        float radius;
        float angle;
        float speed;
        float height;
    };

    void place_lights() {
        runtime::Registry& registry = m_world->registry();
        const auto time = static_cast<float>(m_time);
        for (const Orbit& orbit : m_orbits) {
            const float angle = orbit.angle + orbit.speed * time;
            registry.get<runtime::Transform>(orbit.entity).position =
                Vec3{std::sin(angle) * orbit.radius, orbit.height, std::cos(angle) * orbit.radius};
        }
        // Spot lights shine along their -Z axis: each leans 30 degrees from straight down, and
        // turns around the vertical.
        for (std::size_t index = 0; index < m_spots.size(); ++index) {
            const float turn = 0.6f * time + k_tau * static_cast<float>(index) / 4.0f;
            runtime::Transform& transform = registry.get<runtime::Transform>(m_spots[index]);
            transform.position = Vec3{0.0f, 9.0f, 0.0f};
            transform.rotation =
                core::quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, turn) *
                core::quat_from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, -(90.0f - 30.0f) * k_tau / 360.0f);
        }
    }

    runtime::Renderer m_renderer;
    std::unique_ptr<runtime::World> m_world;
    samples::SampleCamera m_camera;
    std::vector<Orbit> m_orbits;
    std::vector<runtime::Entity> m_spots;
    double m_time = 0.0;
    bool m_moving = true;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Lights>();
}
