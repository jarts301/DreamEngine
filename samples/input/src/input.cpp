// Input (docs/guides/input.md): the game reads named actions, never keys or buttons, so the same
// code takes a keyboard, a gamepad, the mouse or a finger. Sticks and triggers are analog: value()
// says how far they are pushed, so the ball rolls slowly with a stick pushed a little, and boosts
// as far as the trigger is pulled. A panel, made with the `ui` module, shows each action's state.
//
// Escape, or a gamepad's back button, quits.

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/ui.h"
#include "dream/runtime/world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

using namespace dream;
using core::Vec2;
using core::Vec3;

namespace {

constexpr std::array<std::string_view, 8> k_actions{
    "move_left", "move_right", "move_up", "move_down", "jump", "boost", "brake", "touch"};

constexpr float k_speed = 3.0f;
constexpr float k_arena = 4.4f;
constexpr float k_radius = 0.5f;
constexpr float k_gravity = 18.0f;

std::string_view source_name(runtime::InputActions::Source source) {
    switch (source) {
    case runtime::InputActions::Source::KeyboardMouse:
        return "keyboard and mouse";
    case runtime::InputActions::Source::Touch:
        return "touch";
    case runtime::InputActions::Source::Gamepad:
        return "gamepad";
    case runtime::InputActions::Source::None:
        break;
    }
    return "nothing yet";
}

class Input final : public runtime::Game {
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
        auto panel = ui.load_document("assets:/ui/input.rml");
        if (!panel.has_value()) {
            return panel.error();
        }
        m_panel = panel.value();
        ui.show(m_panel);

        auto world = app.scenes().load("assets:/scenes/arena.toml");
        if (!world.has_value()) {
            return world.error();
        }
        m_world = std::move(world).value();
        m_ball = m_world->find("ball");
        if (m_ball == runtime::k_no_entity) {
            return core::Error{core::ErrorCode::InvalidArgument, "The arena needs a ball"};
        }
        m_quit_after_frames = app.config().get_int("demo.quit_after_frames").value_or(0);
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        const auto seconds = static_cast<float>(time.delta_seconds);
        // The panel sees input first; the ball doesn't follow a finger on it.
        m_interface.update(app, seconds);
        move_ball(app, seconds);
        show_actions(app);
        m_world->update_transforms();
        if (app.input().was_pressed("quit") ||
            (m_quit_after_frames > 0 && m_frames >= m_quit_after_frames)) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size(),
                          rhi::ClearColor{0.2f, 0.24f, 0.3f, 1.0f});
    }

    void on_stop(runtime::App& app) override {
        const std::string report =
            std::format("frames={} actions={} source={}", m_frames, k_actions.size(),
                        app.input().last_source() == runtime::InputActions::Source::None ? "none"
                                                                                         : "some");
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    void move_ball(runtime::App& app, float seconds) {
        const runtime::InputActions& input = app.input();
        Vec2 move{input.axis("move_left", "move_right"), input.axis("move_down", "move_up")};
        // Held where to go: toward that point on the ground.
        if (input.is_down("touch") && !m_interface.wants_pointer()) {
            const auto pointer = input.pointer_position();
            if (const auto ray = runtime::camera_ray(*m_world, app.surface_size(),
                                                     Vec2{pointer.x, pointer.y});
                ray.has_value() && ray->direction.y < -0.01f) {
                const float along = -ray->origin.y / ray->direction.y;
                const Vec3 ground = ray->origin + ray->direction * along;
                const Vec3 ball = m_world->registry().get<runtime::Transform>(m_ball).position;
                const Vec2 toward{ground.x - ball.x, ball.z - ground.z};
                if (core::length(toward) > 0.2f) {
                    move = toward;
                }
            }
        }
        if (const float length = core::length(move); length > 1.0f) {
            move = move / length;
        }
        // Analog: half speed with the brake fully held, twice with the boost.
        const float speed = k_speed * (1.0f + input.value("boost")) * (1.0f - 0.5f * input.value("brake"));
        Vec3& position = m_world->registry().get<runtime::Transform>(m_ball).position;
        position.x = std::clamp(position.x + move.x * speed * seconds, -k_arena, k_arena);
        position.z = std::clamp(position.z - move.y * speed * seconds, -k_arena, k_arena);

        if (input.was_pressed("jump") && position.y <= k_radius) {
            m_rising = 7.0f;
        }
        m_rising -= k_gravity * seconds;
        position.y = std::max(position.y + m_rising * seconds, k_radius);
    }

    // Each action's strength on its slider, and a highlight while it is down.
    void show_actions(runtime::App& app) {
        const runtime::InputActions& input = app.input();
        ui::GameUi& ui = m_interface.ui();
        for (std::size_t index = 0; index < k_actions.size(); ++index) {
            const std::string_view action = k_actions[index];
            const float value = input.value(action);
            if (value != m_shown[index]) {
                m_shown[index] = value;
                (void)ui.set_value(m_panel, action, std::format("{:.2f}", value));
                (void)ui.set_class(m_panel, std::format("row_{}", action), "down",
                                   input.is_down(action));
            }
        }
        if (input.last_source() != m_shown_source) {
            m_shown_source = input.last_source();
            (void)ui.set_text(m_panel, "source", source_name(m_shown_source));
        }
        const auto pointer = input.pointer_position();
        if (pointer.x != m_shown_pointer.x || pointer.y != m_shown_pointer.y) {
            m_shown_pointer = Vec2{pointer.x, pointer.y};
            (void)ui.set_text(m_panel, "pointer", std::format("{:.0f}, {:.0f}", pointer.x, pointer.y));
        }
    }

    runtime::Renderer m_renderer;
    runtime::UserInterface m_interface;
    ui::DocumentHandle m_panel;
    std::unique_ptr<runtime::World> m_world;
    runtime::Entity m_ball = runtime::k_no_entity;
    float m_rising = 0.0f;
    std::array<float, k_actions.size()> m_shown{};
    runtime::InputActions::Source m_shown_source = runtime::InputActions::Source::None;
    Vec2 m_shown_pointer{-1.0f, -1.0f};
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Input>();
}
