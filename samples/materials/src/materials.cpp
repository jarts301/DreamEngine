// Materials (docs/guides/materials.md): every surface here is a glTF material, from the models'
// files, lit by the sun and the sky. The game only loads the scene and turns the camera: how a
// surface looks is all in its model.
//
// Controls: turn and zoom with A/D and W/S, the arrows, a gamepad's left stick and triggers, or by
// dragging with a finger or the mouse. Escape quits.

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"
#include "sample_camera.h"

#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <string>

using namespace dream;

namespace {

class Materials final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        auto world = app.scenes().load("assets:/scenes/materials.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_camera.attach(*m_world);
        m_quit_after_frames = app.config().get_int("demo.quit_after_frames").value_or(0);
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
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
        const std::string report =
            std::format("frames={} draws={}", m_frames, m_renderer.last_draw_count());
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    runtime::Renderer m_renderer;
    std::unique_ptr<runtime::World> m_world;
    samples::SampleCamera m_camera;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Materials>();
}
