// A game's interface (docs/adr/0012): a title menu, a settings screen and a HUD, each an RML
// document styled by RCSS and drawn by the `ui` module over the 3D scene. The game shows and hides
// documents, changes text and classes to update them, and reacts to the player's clicks and changes
// through the events the interface publishes.
//
// Escape, Android's back gesture or a gamepad's start button pauses the game.

#include "dream/core/log.h"
#include "dream/core/math/quaternion.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/ui.h"
#include "dream/runtime/world.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace dream;

namespace {

constexpr int k_health_cells = 10;

class Interface final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (auto ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (auto ready = m_interface.initialize(app, m_renderer); !ready.has_value()) {
            return ready;
        }
        ui::GameUi& ui = m_interface.ui();
        for (const char* font :
             {"assets:/fonts/LatoLatin-Regular.ttf", "assets:/fonts/LatoLatin-Bold.ttf"}) {
            if (auto loaded = ui.load_font(font); !loaded.has_value()) {
                return loaded;
            }
        }
        for (auto [path, document] : {std::pair{"assets:/ui/title.rml", &m_title},
                                      std::pair{"assets:/ui/settings.rml", &m_settings},
                                      std::pair{"assets:/ui/hud.rml", &m_hud}}) {
            auto loaded = ui.load_document(path);
            if (!loaded.has_value()) {
                return loaded.error();
            }
            *document = loaded.value();
            ++m_documents;
        }

        auto world = app.scenes().load("assets:/scenes/backdrop.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_crates = {m_world->find("crate_left"), m_world->find("crate_right")};

        // The interface publishes what the player did on the App's event bus.
        m_events = app.events().subscribe<runtime::UiEventPublished>(
            [this, &app](const runtime::UiEventPublished& published) {
                on_ui_event(app, published.event);
            });

        const core::Config& config = app.config();
        m_quit_after_frames = config.get_int("demo.quit_after_frames").value_or(0);
        // Checks compare a picture: a moment into a game, with the settings open over it.
        if (config.get_bool("demo.hold").value_or(false)) {
            m_hold = true;
            m_seconds = 47.5;
        }
        if (config.get_string("demo.screen").value_or("title") == "settings") {
            start_playing();
            open_settings();
        } else {
            ui.show(m_title);
            m_screen = "title";
        }
        update_hud();
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        const auto seconds = static_cast<float>(time.delta_seconds);
        // The interface sees input first, and publishes its events during this call.
        m_interface.update(app, seconds);
        if (app.input().was_pressed("pause") && m_screen == "game") {
            pause();
        }
        if (m_screen == "game" && !m_hold) {
            m_seconds += time.delta_seconds;
            update_hud();
        }
        if (!m_hold) {
            for (runtime::Entity crate : m_crates) {
                if (crate != runtime::k_no_entity) {
                    auto& rotation = m_world->registry().get<runtime::Transform>(crate).rotation;
                    rotation = core::normalized(
                        core::quat_from_axis_angle(core::Vec3{0.0f, 1.0f, 0.0f}, 0.4f * seconds) *
                        rotation);
                }
            }
        }
        m_world->update_transforms();
        if (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.35f, 0.3f, 0.4f, 1.0f});
    }

    void on_stop(runtime::App& /*app*/) override {
        const std::string report =
            std::format("frames={} screen={} documents={}", m_frames, m_screen, m_documents);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    void on_ui_event(runtime::App& app, const ui::UiEvent& event) {
        ui::GameUi& ui = m_interface.ui();
        if (event.kind == ui::UiEvent::Kind::Click) {
            if (event.element_id == "play") {
                start_playing();
            } else if (event.element_id == "settings") {
                open_settings();
            } else if (event.element_id == "back") {
                ui.hide(m_settings);
                ui.show(m_title);
                m_screen = "title";
            } else if (event.element_id == "quit") {
                app.request_quit();
            } else if (event.element_id == "pause") {
                pause();
            }
        } else if (event.kind == ui::UiEvent::Kind::Change) {
            if (event.element_id == "music" || event.element_id == "effects") {
                // A slider's value comes as text, from 0 to 1.
                const double value = std::strtod(event.value.c_str(), nullptr);
                (void)ui.set_text(m_settings, std::format("{}_amount", event.element_id),
                                  std::format("{:.0f}%", value * 100.0));
            } else if (event.element_id == "show_timer") {
                // A checkbox's value is empty when it is off.
                m_show_timer = !event.value.empty();
                (void)ui.set_class(m_hud, "timer_box", "hidden", !m_show_timer);
            }
        }
    }

    void start_playing() {
        ui::GameUi& ui = m_interface.ui();
        ui.hide(m_title);
        ui.show(m_hud);
        // Once a game is under way, the title menu resumes it.
        (void)ui.set_text(m_title, "play", "Resume");
        m_screen = "game";
    }

    void open_settings() {
        ui::GameUi& ui = m_interface.ui();
        ui.hide(m_title);
        ui.show(m_settings);
        m_screen = "settings";
    }

    void pause() {
        m_interface.ui().show(m_title);
        m_screen = "title";
    }

    // Health rises and falls, as if the player were hit and healed; score and time count up.
    void update_hud() {
        ui::GameUi& ui = m_interface.ui();
        const auto health = static_cast<int>(std::lround(6.0 + 4.0 * std::sin(m_seconds * 0.4)));
        if (health != m_shown_health) {
            m_shown_health = health;
            for (int cell = 0; cell < k_health_cells; ++cell) {
                const std::string id = std::format("cell{}", cell);
                (void)ui.set_class(m_hud, id, "on", cell < health);
                (void)ui.set_class(m_hud, id, "low", health <= 3);
            }
        }
        const auto whole_seconds = static_cast<int>(m_seconds);
        if (whole_seconds != m_shown_seconds) {
            m_shown_seconds = whole_seconds;
            (void)ui.set_text(m_hud, "timer",
                              std::format("{}:{:02}", whole_seconds / 60, whole_seconds % 60));
            (void)ui.set_text(m_hud, "score", std::to_string(whole_seconds * 25));
        }
    }

    runtime::Renderer m_renderer;
    runtime::UserInterface m_interface;
    ui::DocumentHandle m_title;
    ui::DocumentHandle m_settings;
    ui::DocumentHandle m_hud;
    runtime::Subscription m_events;
    std::unique_ptr<runtime::World> m_world;
    std::vector<runtime::Entity> m_crates;
    std::string m_screen;
    double m_seconds = 0.0;
    int m_shown_health = -1;
    int m_shown_seconds = -1;
    int m_documents = 0;
    bool m_show_timer = true;
    bool m_hold = false;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Interface>();
}
