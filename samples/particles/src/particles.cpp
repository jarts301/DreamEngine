// Particles (docs/guides/particles.md): every effect is a `particle_emitter` in the scene file,
// moved on by Particles::update and drawn by the renderer. The explosion is set off from code, with
// Particles::burst, every few seconds or when asked.
//
// Controls: turn and zoom with A/D and W/S, the arrows, a gamepad's left stick and triggers, or by
// dragging with a finger or the mouse. Space, or a gamepad's south button, sets off an explosion.
// Escape quits.

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/particles.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"
#include "sample_camera.h"

#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <string>

using namespace dream;

namespace {

constexpr std::uint32_t k_explosion_particles = 120;
constexpr double k_seconds_between_explosions = 3.0;

class ParticlesSample final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        // The renderer first: it loads the textures the emitters name.
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (auto ready = m_particles.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto world = app.scenes().load("assets:/scenes/particles.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_camera.attach(*m_world);
        m_explosion = m_world->find("explosion");
        if (m_explosion == runtime::k_no_entity) {
            return core::Error{core::ErrorCode::InvalidArgument, "The scene needs an explosion"};
        }
        m_world->update_transforms();

        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        if (const std::optional<double> held = config.get_float("demo.particle_time")) {
            // Checks compare a picture: run every effect in fixed steps, set off one explosion a
            // moment before the end, and hold them all still.
            m_particles.warm_up(*m_world, *held - 0.15);
            m_particles.burst(*m_world, m_explosion, k_explosion_particles);
            m_particles.warm_up(*m_world, 0.15);
            for (auto [entity, emitter] :
                 m_world->registry().view<runtime::ParticleEmitter>().each()) {
                emitter.time_scale = 0.0f;
            }
            m_held = true;
        } else {
            // Already under way when the scene appears, the snow on the ground.
            m_particles.warm_up(*m_world, 8.0);
        }
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        m_camera.update(app, static_cast<float>(time.delta_seconds));
        m_world->update_transforms();
        if (!m_held) {
            m_until_explosion -= time.delta_seconds;
            if (app.input().was_pressed("explode") || m_until_explosion <= 0.0) {
                m_particles.burst(*m_world, m_explosion, k_explosion_particles);
                m_until_explosion = k_seconds_between_explosions;
            }
        }
        m_particles.update(*m_world, time.delta_seconds);
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.16f, 0.13f, 0.2f, 1.0f});
    }

    void on_stop(runtime::App& /*app*/) override {
        const std::string report =
            std::format("frames={} draws={} particles={}", m_frames, m_renderer.last_draw_count(),
                        m_renderer.last_particle_count());
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    runtime::Renderer m_renderer;
    runtime::Particles m_particles;
    std::unique_ptr<runtime::World> m_world;
    samples::SampleCamera m_camera;
    runtime::Entity m_explosion = runtime::k_no_entity;
    double m_until_explosion = 1.0;
    bool m_held = false;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<ParticlesSample>();
}
