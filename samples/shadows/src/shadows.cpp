// Shadows (docs/adr/0029, 0041): the sun is the scene's DirectionalLight, and anything with
// `casts_shadows` left on shadows what is behind it. The game turns the sun around the sky, low
// over the horizon, so long shadows sweep across the avenue. Near shadows are sharp and far ones
// still there: dream.toml asks for four cascades reaching 60 m.
//
// Controls: turn and zoom with A/D and W/S, the arrows, a gamepad's left stick and triggers, or by
// dragging with a finger or the mouse. Space, or a gamepad's south button, stops the sun. Escape
// quits.

#include "dream/core/log.h"
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
#include <optional>
#include <string>

using namespace dream;
using core::Vec3;

namespace {

class Shadows final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto world = app.scenes().load("assets:/scenes/shadows.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_camera.attach(*m_world);
        m_sun = m_world->registry().view<runtime::DirectionalLight>().front();
        if (m_sun == runtime::k_no_entity) {
            return core::Error{core::ErrorCode::InvalidArgument, "The avenue needs a sun"};
        }
        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        // Checks compare a picture, so they hold the sun at one moment.
        if (const std::optional<double> held = config.get_float("demo.time")) {
            m_time = *held;
            m_moving = false;
        }
        place_sun();
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
        place_sun();
        m_camera.update(app, static_cast<float>(time.delta_seconds));
        m_world->update_transforms();
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.62f, 0.72f, 0.85f, 1.0f});
    }

    void on_stop(runtime::App& /*app*/) override {
        const std::string report =
            std::format("frames={} draws={} casters={}", m_frames, m_renderer.last_draw_count(),
                        m_renderer.last_shadow_caster_count());
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    // The sun goes around once a minute, 25 degrees above the horizon.
    void place_sun() {
        const auto angle = static_cast<float>(m_time) * 0.105f + 0.75f;
        constexpr float k_height = 0.47f; // tan(25 degrees)
        m_world->registry().get<runtime::DirectionalLight>(m_sun).direction =
            core::normalized(Vec3{std::cos(angle), -k_height, std::sin(angle)});
    }

    runtime::Renderer m_renderer;
    std::unique_ptr<runtime::World> m_world;
    samples::SampleCamera m_camera;
    runtime::Entity m_sun = runtime::k_no_entity;
    double m_time = 0.0;
    bool m_moving = true;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Shadows>();
}
