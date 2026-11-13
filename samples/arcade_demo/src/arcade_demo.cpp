// A small target-practice game: throw balls at towers of crates. It brings the engine's parts
// together: physics moves things, audio plays music and hits, a game interface shows the score and
// a pause menu, and debug tools show what's going on. Which backend does each comes from dream.toml.
// On PC it also saves a profile of each run, with the game's own zones, to open in Perfetto.
//
// Controls: a click, a tap or Space throws a ball. Escape (or Back on Android) or the Menu button
// pauses, and F1 shows the debug tools.

#include "dream/core/log.h"
#include "dream/core/profile.h"
#include "dream/runtime/app.h"
#include "dream/runtime/audio.h"
#include "dream/runtime/game.h"
#include "dream/runtime/physics_3d.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/ui.h"
#include "dream/runtime/world.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>

using namespace dream;
using core::Vec3;
using core::Vec4;

namespace {

// A crate worth a point the first time a ball hits it.
struct Target {
    bool hit = false;
};

// A ball the player threw.
struct Thrown {};

core::Result<void> read_target(runtime::ComponentContext& context) {
    if (core::Result<void> known = context.data.check_keys({}); !known.has_value()) {
        return known;
    }
    context.world.registry().emplace<Target>(context.entity);
    return {};
}

float parse_volume(const std::string& text, float fallback) {
    float value = fallback;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return std::clamp(value, 0.0f, 1.0f);
}

class ArcadeDemo final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        m_scripted = app.config().get_bool("demo.scripted").value_or(false);

        if (core::Result<void> ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (core::Result<void> ready = m_physics.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (core::Result<void> ready = m_audio.initialize(app); !ready.has_value()) {
            return ready;
        }
        if (core::Result<void> ready = m_interface.initialize(app, m_renderer);
            !ready.has_value()) {
            return ready;
        }
        if (core::Result<void> ready = m_debug.initialize(app, m_renderer); !ready.has_value()) {
            return ready;
        }
        m_debug.set_visible(app.config().get_bool("demo.debug_tools").value_or(false));
        app.scenes().register_component("target", &read_target);

        ui::GameUi& ui = m_interface.ui();
        for (const char* font : {"assets:/fonts/LatoLatin-Regular.ttf",
                                 "assets:/fonts/LatoLatin-Bold.ttf"}) {
            if (core::Result<void> loaded = ui.load_font(font); !loaded.has_value()) {
                return loaded;
            }
        }
        core::Result<ui::DocumentHandle> hud = ui.load_document("assets:/ui/hud.rml");
        if (!hud.has_value()) {
            return hud.error();
        }
        core::Result<ui::DocumentHandle> menu = ui.load_document("assets:/ui/menu.rml");
        if (!menu.has_value()) {
            return menu.error();
        }
        m_hud = hud.value();
        m_menu = menu.value();
        ui.show(m_hud);
        m_audio.set_bus_volume(audio::Bus::Music, 0.6f);

        for (auto [name, sound] : {std::pair{"throw", &m_throw_sound}, std::pair{"hit", &m_hit_sound},
                                   std::pair{"click", &m_click_sound}}) {
            core::Result<runtime::AssetRef<runtime::SoundAsset>> loaded =
                app.assets().load<runtime::SoundAsset>(std::format("assets:/sounds/{}.wav", name));
            if (!loaded.has_value()) {
                return loaded.error();
            }
            *sound = std::move(loaded).value();
        }
        core::Result<runtime::AssetRef<runtime::ModelAsset>> ball =
            app.assets().load<runtime::ModelAsset>("assets:/models/ball.glb");
        if (!ball.has_value()) {
            return ball.error();
        }
        m_ball = std::move(ball).value();

        m_contacts = app.events().subscribe<runtime::Contact3D>(
            [this](const runtime::Contact3D& contact) { on_contact(contact); });
        m_ui_events = app.events().subscribe<runtime::UiEventPublished>(
            [this, &app](const runtime::UiEventPublished& published) {
                on_ui_event(app, published.event);
            });
        return restart(app);
    }

    void on_fixed_update(runtime::App& /*app*/, double step_seconds) override {
        m_simulated_seconds += step_seconds;
        if (!m_paused) {
            m_physics.step(static_cast<float>(step_seconds));
        }
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        const auto delta = static_cast<float>(time.delta_seconds);
        m_frame_times[static_cast<std::size_t>(m_frames) % m_frame_times.size()] = delta * 1000.0f;
        m_sound_cooldown = std::max(m_sound_cooldown - delta, 0.0f);

        // The interface sees input first, so its buttons can take a click before the game does.
        m_interface.update(app, delta);
        {
            DREAM_PROFILE_ZONE("Arcade input");
            if (m_scripted) {
                run_script(app);
            } else {
                handle_input(app);
            }
        }
        DREAM_PROFILE_VALUE("Arcade score", m_score);
        if (m_score != m_shown_score) {
            m_shown_score = m_score;
            (void)m_interface.ui().set_text(m_hud, "score", std::to_string(m_score));
        }

        m_physics.update_transforms(static_cast<float>(app.fixed_step_alpha()));
        m_world->update_transforms();
        m_audio.update(m_world.get());
        {
            DREAM_PROFILE_ZONE("Arcade debug tools");
            draw_debug_tools(app, delta);
        }

        if (m_script_done) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        if (m_show_shapes) {
            m_physics.debug_draw(m_renderer.debug_lines());
        }
        m_renderer.render(*m_world, commands, app.surface_size());
        m_most_ui_commands = std::max(m_most_ui_commands, m_renderer.last_ui_command_count());
    }

    void on_stop(runtime::App& /*app*/) override {
        m_physics.attach(nullptr);
        const std::string report =
            std::format("frames={} score={} hits_heard={} menu_opened={} music_volume={:.2f} "
                        "debug_drawn={} ui_commands={}",
                        m_frames, m_score, m_hits_heard, m_menu_opened,
                        m_audio.bus_volume(audio::Bus::Music), m_debug_drawn ? "yes" : "no",
                        m_most_ui_commands > 0 ? "some" : "none");
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
    core::Result<void> restart(runtime::App& app) {
        core::Result<std::unique_ptr<runtime::World>> world =
            app.scenes().load("assets:/scenes/range.toml");
        if (!world.has_value()) {
            return world.error();
        }
        // The physics leave the old world before it goes.
        m_physics.attach(world.value().get());
        m_world = std::move(world).value();
        m_thrown.clear();
        m_score = 0;
        return {};
    }

    void handle_input(runtime::App& app) {
        if (app.input().was_pressed("menu")) {
            set_paused(!m_paused);
        }
        if (app.input().was_pressed("debug")) {
            m_debug.set_visible(!m_debug.visible());
        }
        if (m_paused || !app.input().was_pressed("throw") || m_interface.wants_pointer() ||
            m_debug.wants_pointer()) {
            return;
        }
        const auto pointer = app.input().pointer_position();
        throw_ball(app.surface_size(), pointer.x, pointer.y);
    }

    // A fixed sequence for automated checks, timed by simulated seconds.
    void run_script(runtime::App& app) {
        const auto at = [this](double seconds) {
            return m_script_seconds < seconds && m_simulated_seconds >= seconds;
        };
        const platform::SurfaceSize size = app.surface_size();
        const float middle_x = 0.5f * static_cast<float>(size.width);
        const float aim_y = 0.6f * static_cast<float>(size.height);

        if (at(0.5) || at(0.9) || at(1.3)) {
            throw_ball(size, middle_x, aim_y);
        }
        if (at(2.5)) {
            m_debug.set_visible(true);
            set_paused(true);
            // As if the player dragged the music slider down.
            on_ui_event(app, ui::UiEvent{.kind = ui::UiEvent::Kind::Change,
                                         .document = m_menu,
                                         .element_id = "music",
                                         .value = "0.25"});
        }
        if (at(3.0)) {
            on_ui_event(app, ui::UiEvent{.document = m_menu, .element_id = "resume"});
        }
        if (at(3.5)) {
            m_script_done = true;
        }
        m_script_seconds = m_simulated_seconds;
    }

    void on_ui_event(runtime::App& app, const ui::UiEvent& event) {
        if (event.kind == ui::UiEvent::Kind::Click) {
            m_audio.play(m_click_sound, audio::PlayDesc{.bus = audio::Bus::Interface});
            if (event.element_id == "menu") {
                set_paused(true);
            } else if (event.element_id == "resume") {
                set_paused(false);
            } else if (event.element_id == "restart") {
                if (core::Result<void> restarted = restart(app); !restarted.has_value()) {
                    DREAM_LOG_ERROR("{}", restarted.error().message);
                }
                set_paused(false);
            }
        } else if (event.kind == ui::UiEvent::Kind::Change) {
            if (event.element_id == "music") {
                m_audio.set_bus_volume(audio::Bus::Music, parse_volume(event.value, 0.6f));
            } else if (event.element_id == "effects") {
                m_audio.set_bus_volume(audio::Bus::Effects, parse_volume(event.value, 1.0f));
            } else if (event.element_id == "debug") {
                m_debug.set_visible(!event.value.empty());
            }
        }
    }

    void set_paused(bool paused) {
        m_paused = paused;
        if (paused) {
            m_interface.ui().show(m_menu);
            ++m_menu_opened;
        } else {
            m_interface.ui().hide(m_menu);
        }
    }

    void throw_ball(platform::SurfaceSize size, float x, float y) {
        const std::optional<runtime::CameraRay> ray = runtime::camera_ray(*m_world, size, {x, y});
        if (!ray.has_value()) {
            return;
        }
        runtime::Registry& registry = m_world->registry();
        const runtime::Entity ball = m_world->create_entity();
        runtime::Transform& transform = registry.get<runtime::Transform>(ball);
        transform.position = ray->origin + ray->direction * 1.5f;
        transform.scale = Vec3{0.3f, 0.3f, 0.3f};
        registry.emplace<runtime::MeshRenderer>(ball, m_ball, Vec4{1.0f, 0.5f, 0.1f, 1.0f});
        registry.emplace<Thrown>(ball);

        runtime::RigidBody3D body;
        body.desc.shape = physics3d::SphereShape{0.3f};
        body.desc.mass = 3.0f;
        body.desc.restitution = 0.3f;
        body.desc.linear_velocity = ray->direction * 20.0f + Vec3{0.0f, 1.5f, 0.0f};
        body.desc.continuous_collision = true;
        registry.emplace<runtime::RigidBody3D>(ball, body);
        m_audio.play(m_throw_sound, audio::PlayDesc{.volume = 0.7f});

        // Keeps the newest few, so throwing forever doesn't slow the game.
        m_thrown.push_back(ball);
        if (m_thrown.size() > 20) {
            m_world->destroy_entity(m_thrown.front());
            m_thrown.pop_front();
        }
    }

    void on_contact(const runtime::Contact3D& contact) {
        if (contact.kind != physics3d::ContactEvent::Kind::Began || m_world == nullptr) {
            return;
        }
        runtime::Registry& registry = m_world->registry();
        const auto is_ball = [&](runtime::Entity entity) {
            return registry.valid(entity) && registry.all_of<Thrown>(entity);
        };
        if (!is_ball(contact.a) && !is_ball(contact.b)) {
            return;
        }
        const runtime::Entity other = is_ball(contact.a) ? contact.b : contact.a;
        if (registry.valid(other)) {
            if (Target* const target = registry.try_get<Target>(other);
                target != nullptr && !target->hit) {
                target->hit = true;
                ++m_score;
            }
        }
        // Many contacts can begin in one step; a short pause keeps them from piling up.
        if (m_sound_cooldown <= 0.0f) {
            m_audio.play_at(m_hit_sound, contact.point,
                            audio::PlayDesc{.min_distance = 4.0f, .max_distance = 40.0f});
            m_sound_cooldown = 0.06f;
            ++m_hits_heard;
        }
    }

    void draw_debug_tools(runtime::App& app, float delta) {
        debug_ui::DebugUi& tools = m_debug.begin_frame(app, delta);
        if (m_debug.visible()) {
            if (tools.begin_window("Debug")) {
                tools.text(std::format("{:.1f} ms per frame", delta * 1000.0f));
                tools.plot("Frame times##plot", m_frame_times, 0.0f, 50.0f);
                tools.text(std::format("Bodies: {}   Voices: {}",
                                       m_physics.backend()->body_count(),
                                       m_audio.device()->voice_count()));
                tools.checkbox("Show shapes", m_show_shapes);
                if (tools.slider("Gravity", m_gravity, -30.0f, 0.0f)) {
                    m_physics.set_gravity(Vec3{0.0f, m_gravity, 0.0f});
                }
                if (tools.button("Restart")) {
                    (void)restart(app);
                }
            }
            tools.end_window();
            m_debug_drawn = true;
        }
        m_debug.end_frame();
    }

    // Declared before the interfaces, which must be destroyed first.
    runtime::Renderer m_renderer;
    runtime::Physics3D m_physics;
    runtime::Audio m_audio;
    runtime::UserInterface m_interface;
    runtime::DebugOverlay m_debug;

    std::unique_ptr<runtime::World> m_world;
    runtime::AssetRef<runtime::ModelAsset> m_ball;
    runtime::AssetRef<runtime::SoundAsset> m_throw_sound;
    runtime::AssetRef<runtime::SoundAsset> m_hit_sound;
    runtime::AssetRef<runtime::SoundAsset> m_click_sound;
    runtime::Subscription m_contacts;
    runtime::Subscription m_ui_events;
    ui::DocumentHandle m_hud;
    ui::DocumentHandle m_menu;
    std::deque<runtime::Entity> m_thrown;
    std::array<float, 120> m_frame_times{};

    double m_simulated_seconds = 0.0;
    double m_script_seconds = 0.0;
    std::int64_t m_frames = 0;
    int m_score = 0;
    int m_shown_score = -1;
    int m_hits_heard = 0;
    int m_menu_opened = 0;
    float m_sound_cooldown = 0.0f;
    float m_gravity = -9.81f;
    std::uint32_t m_most_ui_commands = 0;
    bool m_paused = false;
    bool m_show_shapes = false;
    bool m_debug_drawn = false;
    bool m_scripted = false;
    bool m_script_done = false;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<ArcadeDemo>();
}
