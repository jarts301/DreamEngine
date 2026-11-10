// A scene file drawn by the engine's renderer. The game only says what spins: loading models,
// lighting and drawing all come from the engine.

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"

#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <utility>

using namespace dream;
using core::Vec3;

namespace {

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

class RenderDemo final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        m_quit_after_frames = app.config().get_int("demo.quit_after_frames").value_or(0);
        m_spinning = app.config().get_bool("demo.spin").value_or(true);

        if (core::Result<void> ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        app.scenes().register_component("spin", &read_spin);

        core::Result<std::unique_ptr<runtime::World>> world =
            app.scenes().load("assets:/scenes/yard.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;

        if (app.input().was_pressed("toggle_spin")) {
            m_spinning = !m_spinning;
            DREAM_LOG_INFO("Spinning: {}", m_spinning);
        }
        if (m_spinning) {
            const Vec3 axis{0.0f, 1.0f, 0.0f};
            for (const auto [entity, transform, spin] :
                 m_world->registry().view<runtime::Transform, Spin>().each()) {
                const auto step = static_cast<float>(time.delta_seconds) * spin.speed;
                transform.rotation =
                    core::normalized(core::quat_from_axis_angle(axis, step) * transform.rotation);
            }
        }
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

    void on_pause(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Paused");
    }

    void on_resume(runtime::App& /*app*/) override {
        DREAM_LOG_INFO("Resumed after {} frames", m_frames);
    }

private:
    runtime::Renderer m_renderer;
    std::unique_ptr<runtime::World> m_world;
    std::int64_t m_quit_after_frames = 0;
    std::int64_t m_frames = 0;
    bool m_spinning = true;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<RenderDemo>();
}
