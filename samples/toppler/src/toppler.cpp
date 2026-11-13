// Toppler: a physics puzzle. Each level stands gold crates on platforms, towers and posts, and the
// player knocks every one of them down within the level's throws. A throw lands where the player
// taps or clicks. Clearing a level within its par earns three stars, one throw over two stars, and
// any more one star. Stars and volumes are saved, and each level opens once the one before it is
// cleared.
//
// Controls: a tap or a click throws. Escape (or Back on Android) pauses, or goes back a screen.

#include "dream/core/config.h"
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
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dream;
using core::Vec3;
using core::Vec4;

namespace {

constexpr int k_level_count = 10;
constexpr std::string_view k_progress_path = "user:/progress.toml";

// A ball flies at about this speed, in meters per second, whatever the distance.
constexpr float k_ball_speed = 24.0f;
constexpr float k_ball_radius = 0.3f;

// A gold crate is down once it has fallen this far below where it started, in meters.
constexpr float k_knocked_drop = 0.9f;

// After the last throw, how long the crates have to finish falling before the level is lost.
constexpr double k_settle_seconds = 4.0;

// How long a cleared level is shown before the result, so the last crate is seen to fall.
constexpr double k_cleared_delay_seconds = 1.0;

// A crate to knock down. The scene marks it with an empty [entities.<name>.target] table.
struct Target {
    float start_height = 0.0f;
    bool down = false;
};

// A ball the player threw.
struct Thrown {};

// The level's name and how many throws it allows, from its scene:
//
//     [entities.rules.level]
//     name = "Tower"
//     throws = 3    # Throws allowed.
//     par = 1       # Throws for three stars.
struct LevelRules {
    std::string name;
    int throws = 3;
    int par = 1;
};

core::Result<void> read_target(runtime::ComponentContext& context) {
    if (core::Result<void> known = context.data.check_keys({}); !known.has_value()) {
        return known;
    }
    context.world.registry().emplace<Target>(context.entity);
    return {};
}

core::Result<void> read_level(runtime::ComponentContext& context) {
    const runtime::ComponentData& data = context.data;
    if (core::Result<void> known = data.check_keys({"name", "throws", "par"}); !known.has_value()) {
        return known;
    }
    core::Result<std::string> name = data.text("name");
    if (!name.has_value()) {
        return name.error();
    }
    const core::Result<float> throws = data.number("throws", 3.0f);
    if (!throws.has_value()) {
        return throws.error();
    }
    const core::Result<float> par = data.number("par", 1.0f);
    if (!par.has_value()) {
        return par.error();
    }
    if (throws.value() < 1.0f || par.value() < 1.0f || par.value() > throws.value()) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "level: needs 1 <= par <= throws"};
    }
    context.world.registry().emplace<LevelRules>(
        context.entity, LevelRules{.name = std::move(name).value(),
                                   .throws = static_cast<int>(throws.value()),
                                   .par = static_cast<int>(par.value())});
    return {};
}

int stars_for(int throws_used, int par) {
    if (throws_used <= par) {
        return 3;
    }
    return throws_used == par + 1 ? 2 : 1;
}

std::string level_path(int level) {
    return std::format("assets:/levels/level{:02}.toml", level + 1);
}

float parse_volume(const std::string& text, float fallback) {
    float value = fallback;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return std::clamp(value, 0.0f, 1.0f);
}

// What is saved between runs, as TOML in user storage.
struct Progress {
    std::array<int, k_level_count> stars{};
    float music = 0.6f;
    float effects = 1.0f;

    [[nodiscard]] bool unlocked(int level) const {
        return level == 0 || stars.at(static_cast<std::size_t>(level - 1)) > 0;
    }
};

std::string level_key(int level) {
    return std::format("level{:02}", level + 1);
}

// Nothing saved yet, or a file that can't be read, starts afresh.
Progress read_progress(const runtime::VirtualFileSystem& files) {
    Progress progress;
    const core::Result<std::vector<std::byte>> bytes = files.read(k_progress_path);
    if (!bytes.has_value()) {
        return progress;
    }
    const std::string text(reinterpret_cast<const char*>(bytes.value().data()),
                           bytes.value().size());
    const core::Result<core::Config> saved = core::Config::parse(text);
    if (!saved.has_value()) {
        DREAM_LOG_WARNING("Starting afresh, as the saved progress can't be read: {}",
                          saved.error().message);
        return progress;
    }
    for (int level = 0; level < k_level_count; ++level) {
        const std::optional<std::int64_t> stars = saved.value().get_int("stars." + level_key(level));
        progress.stars.at(static_cast<std::size_t>(level)) =
            static_cast<int>(std::clamp<std::int64_t>(stars.value_or(0), 0, 3));
    }
    progress.music = static_cast<float>(
        std::clamp(saved.value().get_float("settings.music").value_or(0.6), 0.0, 1.0));
    progress.effects = static_cast<float>(
        std::clamp(saved.value().get_float("settings.effects").value_or(1.0), 0.0, 1.0));
    return progress;
}

core::Result<void> write_progress(const runtime::VirtualFileSystem& files,
                                  const Progress& progress) {
    std::string text = "# Toppler's saved progress.\n\n[stars]\n";
    for (int level = 0; level < k_level_count; ++level) {
        text += std::format("{} = {}\n", level_key(level),
                            progress.stars.at(static_cast<std::size_t>(level)));
    }
    text += std::format("\n[settings]\nmusic = {:.2f}\neffects = {:.2f}\n", progress.music,
                        progress.effects);
    return files.write(k_progress_path, std::as_bytes(std::span(text.data(), text.size())));
}

enum class Screen : std::uint8_t { Title, Levels, Playing, Paused, Result };

class Toppler final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        m_scripted = app.config().get_bool("demo.scripted").value_or(false);
        m_script_levels = static_cast<int>(std::clamp<std::int64_t>(
            app.config().get_int("demo.script_levels").value_or(k_level_count), 1,
            k_level_count));

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
        app.scenes().register_component("target", &read_target);
        app.scenes().register_component("level", &read_level);

        if (core::Result<void> loaded = load_interface(); !loaded.has_value()) {
            return loaded;
        }
        if (core::Result<void> loaded = load_sounds(app); !loaded.has_value()) {
            return loaded;
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

        // A scripted run starts afresh, so its result doesn't depend on earlier runs.
        if (!m_scripted) {
            m_progress = read_progress(app.files());
        }
        apply_volumes();

        // The first level stands behind the title screen.
        if (core::Result<void> loaded = load_level(app, 0); !loaded.has_value()) {
            return loaded;
        }
        show(Screen::Title);
        return {};
    }

    void on_fixed_update(runtime::App& /*app*/, double step_seconds) override {
        m_simulated_seconds += step_seconds;
        if (m_screen != Screen::Paused) {
            m_physics.step(static_cast<float>(step_seconds));
        }
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        ++m_frames;
        const auto delta = static_cast<float>(time.delta_seconds);
        m_sound_cooldown = std::max(m_sound_cooldown - delta, 0.0f);

        // The interface sees input first, so its buttons can take a click before the game does.
        m_interface.update(app, delta);
        {
            DREAM_PROFILE_ZONE("Toppler input");
            if (m_scripted) {
                run_script(app);
            } else {
                handle_input(app);
            }
        }

        m_physics.update_transforms(static_cast<float>(app.fixed_step_alpha()));
        m_world->update_transforms();
        if (m_screen == Screen::Playing || m_screen == Screen::Result) {
            check_targets(app);
        }
        update_hud();
        m_audio.update(m_world.get());

        if (m_quit) {
            app.request_quit();
        }
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        m_renderer.render(*m_world, commands, app.surface_size());
    }

    void on_stop(runtime::App& app) override {
        m_physics.attach(nullptr);
        if (!m_scripted) {
            return;
        }
        // Reads the progress back from storage, to check it was saved.
        const Progress saved = read_progress(app.files());
        int total_stars = 0;
        std::string stars;
        for (const int count : saved.stars) {
            total_stars += count;
            stars += std::to_string(count);
        }
        const std::string report = std::format(
            "frames={} levels_played={} cleared={} stars={} saved_stars={} saved_music={:.2f} "
            "menu_opened={}",
            m_frames, m_levels_played, m_levels_cleared, total_stars, stars, saved.music,
            m_menu_opened);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
    }

private:
    core::Result<void> load_interface() {
        ui::GameUi& ui = m_interface.ui();
        for (const char* font : {"assets:/fonts/LatoLatin-Regular.ttf",
                                 "assets:/fonts/LatoLatin-Bold.ttf"}) {
            if (core::Result<void> loaded = ui.load_font(font); !loaded.has_value()) {
                return loaded;
            }
        }
        for (auto [name, document] :
             {std::pair{"title", &m_title}, std::pair{"levels", &m_levels},
              std::pair{"hud", &m_hud}, std::pair{"menu", &m_menu},
              std::pair{"result", &m_result}}) {
            core::Result<ui::DocumentHandle> loaded =
                ui.load_document(std::format("assets:/ui/{}.rml", name));
            if (!loaded.has_value()) {
                return loaded.error();
            }
            *document = loaded.value();
        }
#if defined(__ANDROID__)
        // Android apps are left with Back or Home rather than quit.
        (void)ui.set_class(m_title, "quit", "hidden", true);
#endif
        return {};
    }

    core::Result<void> load_sounds(runtime::App& app) {
        for (auto [name, sound] :
             {std::pair{"throw", &m_throw_sound}, std::pair{"hit", &m_hit_sound},
              std::pair{"click", &m_click_sound}, std::pair{"win", &m_win_sound},
              std::pair{"lose", &m_lose_sound}}) {
            core::Result<runtime::AssetRef<runtime::SoundAsset>> loaded =
                app.assets().load<runtime::SoundAsset>(std::format("assets:/sounds/{}.wav", name));
            if (!loaded.has_value()) {
                return loaded.error();
            }
            *sound = std::move(loaded).value();
        }
        return {};
    }

    void apply_volumes() {
        m_audio.set_bus_volume(audio::Bus::Music, m_progress.music);
        m_audio.set_bus_volume(audio::Bus::Effects, m_progress.effects);
        ui::GameUi& ui = m_interface.ui();
        (void)ui.set_value(m_menu, "music", std::format("{:.2f}", m_progress.music));
        (void)ui.set_value(m_menu, "effects", std::format("{:.2f}", m_progress.effects));
    }

    void save_progress(runtime::App& app) {
        if (core::Result<void> saved = write_progress(app.files(), m_progress);
            !saved.has_value()) {
            DREAM_LOG_ERROR("Could not save progress: {}", saved.error().message);
        }
    }

    core::Result<void> load_level(runtime::App& app, int level) {
        core::Result<std::unique_ptr<runtime::World>> world =
            app.scenes().load(level_path(level));
        if (!world.has_value()) {
            return world.error();
        }
        runtime::Registry& registry = world.value()->registry();
        const runtime::Entity rules_entity = registry.view<const LevelRules>().front();
        if (rules_entity == runtime::k_no_entity) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               level_path(level) + " has no [entities.<name>.level] table"};
        }
        for (const auto [entity, target, transform] :
             registry.view<Target, const runtime::Transform>().each()) {
            target.start_height = transform.position.y;
        }

        // The physics leave the old world before it goes.
        m_physics.attach(world.value().get());
        m_world = std::move(world).value();
        m_rules = registry.get<const LevelRules>(rules_entity);
        m_level = level;
        m_throws_left = m_rules.throws;
        m_throws_used = 0;
        m_targets_left = static_cast<int>(registry.view<const Target>().size());
        m_finished = false;
        m_finish_at.reset();
        m_shown_targets = -1;
        m_shown_throws = -1;
        (void)m_interface.ui().set_text(m_hud, "level_name",
                                        std::format("{}. {}", level + 1, m_rules.name));
        return {};
    }

    void start_level(runtime::App& app, int level) {
        if (core::Result<void> loaded = load_level(app, level); !loaded.has_value()) {
            DREAM_LOG_ERROR("{}", loaded.error().message);
            show(Screen::Levels);
            return;
        }
        ++m_levels_played;
        show(Screen::Playing);
    }

    void show(Screen screen) {
        ui::GameUi& ui = m_interface.ui();
        if (m_screen == Screen::Paused && screen != Screen::Paused && m_settings_changed) {
            m_save_requested = true;
        }
        m_screen = screen;
        for (const ui::DocumentHandle document : {m_title, m_levels, m_hud, m_menu, m_result}) {
            ui.hide(document);
        }
        switch (screen) {
        case Screen::Title:
            ui.show(m_title);
            break;
        case Screen::Levels:
            refresh_levels();
            ui.show(m_levels);
            break;
        case Screen::Playing:
            ui.show(m_hud);
            break;
        case Screen::Paused:
            ++m_menu_opened;
            ui.show(m_hud);
            ui.show(m_menu);
            break;
        case Screen::Result:
            ui.show(m_hud);
            ui.show(m_result);
            break;
        }
    }

    void refresh_levels() {
        ui::GameUi& ui = m_interface.ui();
        for (int level = 0; level < k_level_count; ++level) {
            const int number = level + 1;
            (void)ui.set_class(m_levels, std::format("level{}", number), "locked",
                               !m_progress.unlocked(level));
            for (int star = 1; star <= 3; ++star) {
                (void)ui.set_class(m_levels, std::format("star{}_{}", number, star), "on",
                                   m_progress.stars.at(static_cast<std::size_t>(level)) >= star);
            }
        }
    }

    void update_hud() {
        ui::GameUi& ui = m_interface.ui();
        if (m_targets_left != m_shown_targets) {
            m_shown_targets = m_targets_left;
            (void)ui.set_text(m_hud, "targets", std::to_string(m_targets_left));
        }
        if (m_throws_left != m_shown_throws) {
            m_shown_throws = m_throws_left;
            (void)ui.set_text(m_hud, "throws", std::to_string(m_throws_left));
        }
    }

    void handle_input(runtime::App& app) {
        if (m_save_requested) {
            m_save_requested = false;
            m_settings_changed = false;
            save_progress(app);
        }
        if (app.input().was_pressed("menu")) {
            go_back(app);
        }
        if (m_screen != Screen::Playing || !app.input().was_pressed("throw") ||
            m_interface.wants_pointer()) {
            return;
        }
        const auto pointer = app.input().pointer_position();
        throw_at_pixel(app.surface_size(), {pointer.x, pointer.y});
    }

    // The ball lands on whatever is under the pixel, or far off if nothing is.
    void throw_at_pixel(platform::SurfaceSize size, core::Vec2 pixel) {
        const std::optional<runtime::CameraRay> ray = runtime::camera_ray(*m_world, size, pixel);
        if (!ray.has_value()) {
            return;
        }
        const std::optional<runtime::RayHit3D> hit =
            m_physics.cast_ray(ray->origin, ray->direction, 100.0f);
        throw_at(hit.has_value() ? hit->point : ray->origin + ray->direction * 40.0f);
    }

    // Escape or Back: one screen back, or out of the game from the title.
    void go_back(runtime::App& /*app*/) {
        switch (m_screen) {
        case Screen::Title:
            m_quit = true;
            break;
        case Screen::Levels:
            show(Screen::Title);
            break;
        case Screen::Playing:
            show(Screen::Paused);
            break;
        case Screen::Paused:
            show(Screen::Playing);
            break;
        case Screen::Result:
            show(Screen::Levels);
            break;
        }
    }

    void on_ui_event(runtime::App& app, const ui::UiEvent& event) {
        if (event.kind == ui::UiEvent::Kind::Change && event.document == m_menu) {
            if (event.element_id == "music") {
                m_progress.music = parse_volume(event.value, m_progress.music);
            } else if (event.element_id == "effects") {
                m_progress.effects = parse_volume(event.value, m_progress.effects);
            }
            m_audio.set_bus_volume(audio::Bus::Music, m_progress.music);
            m_audio.set_bus_volume(audio::Bus::Effects, m_progress.effects);
            m_settings_changed = true;
            return;
        }
        if (event.kind != ui::UiEvent::Kind::Click) {
            return;
        }
        m_audio.play(m_click_sound, audio::PlayDesc{.bus = audio::Bus::Interface});
        const std::string_view id = event.element_id;
        if (event.document == m_title) {
            if (id == "play") {
                show(Screen::Levels);
            } else if (id == "quit") {
                m_quit = true;
            }
        } else if (event.document == m_levels) {
            if (id == "back") {
                show(Screen::Title);
            } else if (id.starts_with("level")) {
                int number = 0;
                std::from_chars(id.data() + 5, id.data() + id.size(), number);
                if (number >= 1 && number <= k_level_count && m_progress.unlocked(number - 1)) {
                    start_level(app, number - 1);
                }
            }
        } else if (event.document == m_hud) {
            if (id == "menu") {
                show(Screen::Paused);
            }
        } else if (event.document == m_menu) {
            if (id == "resume") {
                show(Screen::Playing);
            } else if (id == "restart") {
                show(Screen::Playing);
                start_level(app, m_level);
            } else if (id == "levels") {
                show(Screen::Levels);
            }
        } else if (event.document == m_result) {
            if (id == "next" && m_level + 1 < k_level_count) {
                start_level(app, m_level + 1);
            } else if (id == "retry") {
                start_level(app, m_level);
            } else if (id == "levels") {
                show(Screen::Levels);
            }
        }
    }

    // Throws a ball that lands on `point`, flying a curve that takes the same time for every
    // distance at the ball's speed.
    void throw_at(Vec3 point) {
        if (m_throws_left <= 0 || m_finished) {
            return;
        }
        // Only the eye is needed, which doesn't depend on the surface's size.
        const std::optional<runtime::CameraView> camera =
            runtime::camera_view(*m_world, platform::SurfaceSize{1, 1});
        if (!camera.has_value()) {
            return;
        }
        // It starts a little ahead of the camera, so it doesn't fill the screen.
        const Vec3 start = camera->eye + core::normalized(point - camera->eye) * 3.0f;
        const Vec3 travel = point - start;
        const float seconds = std::clamp(core::length(travel) / k_ball_speed, 0.2f, 1.5f);
        const Vec3 gravity = m_physics.backend()->gravity();
        const Vec3 velocity = travel * (1.0f / seconds) - gravity * (0.5f * seconds);

        runtime::Registry& registry = m_world->registry();
        const runtime::Entity ball = m_world->create_entity();
        runtime::Transform& transform = registry.get<runtime::Transform>(ball);
        transform.position = start;
        transform.scale = Vec3{k_ball_radius, k_ball_radius, k_ball_radius};
        registry.emplace<runtime::MeshRenderer>(ball, m_ball, Vec4{0.95f, 0.35f, 0.25f, 1.0f});
        registry.emplace<Thrown>(ball);

        runtime::RigidBody3D body;
        body.desc.shape = physics3d::SphereShape{k_ball_radius};
        body.desc.mass = 3.0f;
        body.desc.restitution = 0.2f;
        body.desc.linear_velocity = velocity;
        body.desc.continuous_collision = true;
        registry.emplace<runtime::RigidBody3D>(ball, body);
        m_audio.play(m_throw_sound, audio::PlayDesc{.volume = 0.7f});

        --m_throws_left;
        ++m_throws_used;
        m_last_throw_at = m_simulated_seconds;
    }

    void check_targets(runtime::App& app) {
        runtime::Registry& registry = m_world->registry();
        int standing = 0;
        for (const auto [entity, target, transform] :
             registry.view<Target, const runtime::Transform>().each()) {
            if (!target.down && (transform.position.y < target.start_height - k_knocked_drop ||
                                 transform.position.y < -2.0f)) {
                target.down = true;
            }
            if (!target.down) {
                ++standing;
            }
        }
        m_targets_left = standing;
        if (m_finished || m_screen != Screen::Playing) {
            return;
        }
        if (standing == 0) {
            if (!m_finish_at.has_value()) {
                m_finish_at = m_simulated_seconds + k_cleared_delay_seconds;
            } else if (m_simulated_seconds >= *m_finish_at) {
                finish(app, true);
            }
        } else if (m_throws_left == 0 && m_simulated_seconds - m_last_throw_at > k_settle_seconds) {
            finish(app, false);
        }
    }

    void finish(runtime::App& app, bool cleared) {
        m_finished = true;
        ui::GameUi& ui = m_interface.ui();
        int stars = 0;
        if (cleared) {
            stars = stars_for(m_throws_used, m_rules.par);
            int& best = m_progress.stars.at(static_cast<std::size_t>(m_level));
            best = std::max(best, stars);
            ++m_levels_cleared;
            save_progress(app);
            m_audio.play(m_win_sound, audio::PlayDesc{.bus = audio::Bus::Interface});
            (void)ui.set_text(m_result, "result_title", "Cleared!");
            (void)ui.set_text(
                m_result, "result_detail",
                std::format("{} {}, par {}.", m_throws_used, m_throws_used == 1 ? "throw" : "throws",
                            m_rules.par));
        } else {
            m_audio.play(m_lose_sound, audio::PlayDesc{.bus = audio::Bus::Interface});
            (void)ui.set_text(m_result, "result_title", "Out of throws");
            (void)ui.set_text(m_result, "result_detail",
                              std::format("{} gold {} still standing.", m_targets_left,
                                          m_targets_left == 1 ? "crate" : "crates"));
        }
        for (int star = 1; star <= 3; ++star) {
            (void)ui.set_class(m_result, std::format("result_star{}", star), "on", stars >= star);
        }
        const bool has_next = cleared && m_level + 1 < k_level_count;
        (void)ui.set_class(m_result, "next", "hidden", !has_next);
        DREAM_LOG_INFO("Level {} {} with {} of {} throws: {} stars, {} gold left", m_level + 1,
                       cleared ? "cleared" : "lost", m_throws_used, m_rules.throws, stars,
                       m_targets_left);
        m_last_result_stars = stars;
        show(Screen::Result);
    }

    void on_contact(const runtime::Contact3D& contact) {
        if (contact.kind != physics3d::ContactEvent::Kind::Began || m_world == nullptr) {
            return;
        }
        const runtime::Registry& registry = m_world->registry();
        const auto is_ball = [&](runtime::Entity entity) {
            return registry.valid(entity) && registry.all_of<Thrown>(entity);
        };
        if ((!is_ball(contact.a) && !is_ball(contact.b)) || m_sound_cooldown > 0.0f) {
            return;
        }
        // Many contacts can begin in one step; a short pause keeps them from piling up.
        m_audio.play_at(m_hit_sound, contact.point,
                        audio::PlayDesc{.min_distance = 4.0f, .max_distance = 40.0f});
        m_sound_cooldown = 0.06f;
    }

    // Plays through the levels for automated checks, timed by simulated seconds: opens the level
    // select, starts the first level, turns the music down from the pause menu, tries each level
    // again once if it is lost, and throws at the
    // first gold crate still standing every 2.5 seconds, once the last throw's crates have had time to
    // fall, going on to the next level after each one cleared.
    void run_script(runtime::App& app) {
        if (m_save_requested) {
            m_save_requested = false;
            m_settings_changed = false;
            save_progress(app);
        }
        const double now = m_simulated_seconds;
        if (now < m_script_next) {
            return;
        }
        m_script_next = now + (m_screen == Screen::Playing ? 2.5 : 1.0);
        const auto click = [&](ui::DocumentHandle document, std::string id) {
            on_ui_event(app, ui::UiEvent{.document = document, .element_id = std::move(id)});
        };
        switch (m_screen) {
        case Screen::Title:
            click(m_title, "play");
            break;
        case Screen::Levels:
            if (m_levels_played == 0) {
                click(m_levels, "level1");
            } else {
                m_quit = true;
            }
            break;
        case Screen::Paused:
            on_ui_event(app, ui::UiEvent{.kind = ui::UiEvent::Kind::Change,
                                         .document = m_menu,
                                         .element_id = "music",
                                         .value = "0.3"});
            click(m_menu, "resume");
            break;
        case Screen::Playing:
            if (m_menu_opened == 0) {
                click(m_hud, "menu");
            } else if (m_throws_left > 0 && !m_finish_at.has_value()) {
                tap_standing_target(app.surface_size());
            }
            break;
        case Screen::Result:
            if (m_last_result_stars > 0 && m_level + 1 < m_script_levels) {
                m_script_retries = 0;
                click(m_result, "next");
            } else if (m_last_result_stars == 0 && m_script_retries < 1) {
                ++m_script_retries;
                click(m_result, "retry");
            } else {
                click(m_result, "levels");
            }
            break;
        }
    }

    // Taps where the first gold crate still standing is on screen, as a player would.
    void tap_standing_target(platform::SurfaceSize size) {
        const std::optional<runtime::CameraView> camera = runtime::camera_view(*m_world, size);
        if (!camera.has_value()) {
            return;
        }
        runtime::Registry& registry = m_world->registry();
        for (const auto [entity, target, transform] :
             registry.view<const Target, const runtime::Transform>().each()) {
            if (target.down) {
                continue;
            }
            const Vec4 clip = camera->view_projection * Vec4{transform.position.x,
                                                             transform.position.y,
                                                             transform.position.z, 1.0f};
            const float x = (clip.x / clip.w + 1.0f) * 0.5f * static_cast<float>(size.width);
            const float y = (1.0f - clip.y / clip.w) * 0.5f * static_cast<float>(size.height);
            throw_at_pixel(size, {x, y});
            return;
        }
    }

    // Declared before the interfaces, which must be destroyed first.
    runtime::Renderer m_renderer;
    runtime::Physics3D m_physics;
    runtime::Audio m_audio;
    runtime::UserInterface m_interface;

    std::unique_ptr<runtime::World> m_world;
    runtime::AssetRef<runtime::ModelAsset> m_ball;
    runtime::AssetRef<runtime::SoundAsset> m_throw_sound;
    runtime::AssetRef<runtime::SoundAsset> m_hit_sound;
    runtime::AssetRef<runtime::SoundAsset> m_click_sound;
    runtime::AssetRef<runtime::SoundAsset> m_win_sound;
    runtime::AssetRef<runtime::SoundAsset> m_lose_sound;
    runtime::Subscription m_contacts;
    runtime::Subscription m_ui_events;
    ui::DocumentHandle m_title;
    ui::DocumentHandle m_levels;
    ui::DocumentHandle m_hud;
    ui::DocumentHandle m_menu;
    ui::DocumentHandle m_result;

    Progress m_progress;
    LevelRules m_rules;
    Screen m_screen = Screen::Title;
    int m_level = 0;
    int m_throws_left = 0;
    int m_throws_used = 0;
    int m_targets_left = 0;
    int m_shown_targets = -1;
    int m_shown_throws = -1;
    int m_last_result_stars = 0;
    bool m_finished = false;
    std::optional<double> m_finish_at;
    double m_last_throw_at = 0.0;
    double m_simulated_seconds = 0.0;
    float m_sound_cooldown = 0.0f;
    bool m_settings_changed = false;
    bool m_save_requested = false;
    bool m_quit = false;

    // For the scripted run and its report.
    bool m_scripted = false;
    int m_script_levels = k_level_count;
    int m_script_retries = 0;
    double m_script_next = 0.5;
    std::int64_t m_frames = 0;
    int m_levels_played = 0;
    int m_levels_cleared = 0;
    int m_menu_opened = 0;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Toppler>();
}
