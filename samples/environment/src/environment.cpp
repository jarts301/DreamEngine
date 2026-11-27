// Environment lighting and bloom (docs/guides/environment.md): the scene's `environment` lights
// everything from an HDR sky and is drawn behind it, and bloom spreads what is brighter than white.
// The game changes the look by changing the components: the sky's brightness and turn, and the
// sun's color, height and strength, for day, dusk and night. At night the lamps carry the scene.
//
// Controls: turn and zoom with A/D and W/S, the arrows, a gamepad's left stick and triggers, or by
// dragging with a finger or the mouse. Space, or a gamepad's south button, changes the time of day.
// Escape quits.

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/environment.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"
#include "sample_camera.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <string_view>

using namespace dream;
using core::Vec3;

namespace {

struct Look {
    std::string_view name;
    float sky_intensity;
    float sky_rotation_degrees;
    Vec3 sun_direction;
    Vec3 sun_color;
    float sun_intensity;
};

// From the table "How bright" in the environment guide.
constexpr std::array<Look, 3> k_looks{{
    {"day", 0.8f, 0.0f, Vec3{-0.5f, -1.0f, -0.6f}, Vec3{1.0f, 0.95f, 0.85f}, 1.3f},
    {"dusk", 0.3f, 150.0f, Vec3{0.8f, -0.25f, -0.5f}, Vec3{1.0f, 0.55f, 0.3f}, 0.8f},
    {"night", 0.05f, 150.0f, Vec3{0.3f, -1.0f, -0.4f}, Vec3{0.6f, 0.7f, 1.0f}, 0.12f},
}};

class Environment final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto world = app.scenes().load("assets:/scenes/environment.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_camera.attach(*m_world);
        runtime::Registry& registry = m_world->registry();
        m_sun = registry.view<runtime::DirectionalLight>().front();
        m_sky = registry.view<runtime::EnvironmentLight>().front();
        if (m_sun == runtime::k_no_entity || m_sky == runtime::k_no_entity) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "The scene needs a sun and an environment"};
        }
        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        m_look = static_cast<std::size_t>(config.get_int("demo.look").value_or(0)) % k_looks.size();
        apply_look();
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        if (app.input().was_pressed("next_look")) {
            m_look = (m_look + 1) % k_looks.size();
            apply_look();
        }
        m_camera.update(app, static_cast<float>(time.delta_seconds));
        m_world->update_transforms();
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size());
    }

    void on_stop(runtime::App& /*app*/) override {
        const std::string report = std::format("frames={} draws={} look={}", m_frames,
                                               m_renderer.last_draw_count(), k_looks[m_look].name);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    void apply_look() {
        const Look& look = k_looks[m_look];
        runtime::Registry& registry = m_world->registry();
        auto& sky = registry.get<runtime::EnvironmentLight>(m_sky);
        sky.intensity = look.sky_intensity;
        sky.rotation_degrees = look.sky_rotation_degrees;
        auto& sun = registry.get<runtime::DirectionalLight>(m_sun);
        sun.direction = look.sun_direction;
        sun.color = look.sun_color;
        sun.intensity = look.sun_intensity;
        DREAM_LOG_INFO("Look: {}", look.name);
    }

    runtime::Renderer m_renderer;
    std::unique_ptr<runtime::World> m_world;
    samples::SampleCamera m_camera;
    runtime::Entity m_sun = runtime::k_no_entity;
    runtime::Entity m_sky = runtime::k_no_entity;
    std::size_t m_look = 0;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Environment>();
}
