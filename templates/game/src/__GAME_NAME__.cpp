// __GAME_TITLE__: loads a scene file, and spins whatever has the scene's "spin" component. The
// engine loads the models, lights them and draws them; the game says what moves.

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/runtime/app.h"
#include "dream/runtime/event_bus.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/scene_loader.h"
#include "dream/runtime/world.h"

#include <cstdint>
#include <memory>
#include <utility>

using namespace dream;

namespace {

// A component of the game's own, which scene files give as [entities.<name>.spin].
struct Spin {
    // Radians per second around the up axis.
    float speed = 1.0f;
};

core::Result<void> read_spin(runtime::ComponentContext& context) {
    if (core::Result<void> known = context.data.check_keys({"speed"}); !known.has_value()) {
        return known;
    }
    runtime::SettingReader read(context.data);
    Spin spin;
    read.number("speed", spin.speed);
    if (core::Result<void> done = read.result(); !done.has_value()) {
        return done;
    }
    context.world.registry().emplace<Spin>(context.entity, spin);
    return {};
}

class StarterGame final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        m_quit_after_frames = app.config().get_int("game.quit_after_frames").value_or(0);

        if (core::Result<void> ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        app.scenes().register_component("spin", &read_spin);
        // The scene is loaded again when its file changes, as when the editor saves it while the
        // game runs (hot reload, in debug builds).
        m_scene_changed = app.events().subscribe<runtime::ScenesChanged>(
            [this](const runtime::ScenesChanged&) { m_reload = true; });
        return load_scene(app);
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        if (m_reload) {
            m_reload = false;
            if (const core::Result<void> loaded = load_scene(app); !loaded.has_value()) {
                // The last scene that loaded stays, so a mistake in the file doesn't end the game.
                DREAM_LOG_WARNING("The scene didn't load again: {}", loaded.error().message);
            } else {
                DREAM_LOG_INFO("Loaded the scene again");
            }
        }
        if (app.input().was_pressed("toggle_spin")) {
            m_spinning = !m_spinning;
        }
        if (m_spinning) {
            const core::Vec3 up{0.0f, 1.0f, 0.0f};
            for (const auto [entity, transform, spin] :
                 m_world->registry().view<runtime::Transform, const Spin>().each()) {
                const auto angle = static_cast<float>(time.delta_seconds) * spin.speed;
                transform.rotation =
                    core::normalized(core::quat_from_axis_angle(up, angle) * transform.rotation);
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
        DREAM_LOG_INFO("__GAME_TITLE__ ran {} frames", m_frames);
    }

private:
    core::Result<void> load_scene(runtime::App& app) {
        core::Result<std::unique_ptr<runtime::World>> world =
            app.scenes().load("assets:/scenes/main.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        return {};
    }

    runtime::Renderer m_renderer;
    runtime::Subscription m_scene_changed;
    bool m_reload = false;
    std::unique_ptr<runtime::World> m_world;
    std::int64_t m_quit_after_frames = 0;
    std::int64_t m_frames = 0;
    bool m_spinning = true;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<StarterGame>();
}
