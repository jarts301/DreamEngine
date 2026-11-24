// The editor (ADR 0046): a program built on the engine that edits a project's scene files. Its
// panels are editor_panel modules, drawn with the debug interface over a scene view, where the
// renderer draws the scene from a camera of the editor's own.
//
//     dream_editor <project folder>       Opens the project's first scene.
//     dream_editor <scene file>           Opens that scene of the project it is in.
//
// A project's dream.toml may say what the editor opens and how Play builds and starts the game:
//
//     [editor]
//     scene = "assets:/scenes/main.toml"   # The default.
//     build = "build/windows-debug"        # The build folder, from the project's; the default.
//     game = "my_game"                     # The game's target; the project folder's name.
//     assets = ["assets", "../shared"]     # Its asset folders, as its build's ASSETS lists them,
//                                          # later ones over earlier ones. ["assets"] by default.
//
// Play saves the scene, builds the game with CMake and starts it. While it runs, each save builds
// its assets again, which its hot reload picks up: models and textures reload by themselves, and a
// game that handles ScenesChanged loads its scene again.

#include "editor_scene.h"
#include "editor_process.h"

#include "dream/core/log.h"
#include "dream/core/math/matrix.h"
#include "dream/core/math/quaternion.h"
#include "dream/editor_panel/editor_panel.h"
#include "dream/platform/event.h"
#include "dream/runtime/animation.h"
#include "dream/runtime/app.h"
#include "dream/runtime/game.h"
#include "dream/runtime/particles.h"
#include "dream/runtime/program.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/ui.h"
#include "dream/runtime/world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace dream;
using core::Vec2;
using core::Vec3;
using core::Vec4;

namespace {

constexpr float k_panel_width = 380.0f;
constexpr float k_toolbar_height = 96.0f;
constexpr float k_margin = 8.0f;
// Above any camera a scene would give, so the editor's draws the view.
constexpr std::int32_t k_editor_camera_priority = 1 << 30;

#ifdef _WIN32
constexpr std::string_view k_program_suffix = ".exe";
constexpr std::string_view k_default_build = "build/windows-debug";
#else
constexpr std::string_view k_program_suffix = "";
constexpr std::string_view k_default_build = "build/linux-debug";
#endif

// Where a project is and how to build and run its game.
struct Project {
    fs::path folder;
    fs::path scene;
    fs::path build;
    std::string game;
    // In the order the build merges them, so later ones win.
    std::vector<fs::path> assets;
};

std::optional<std::string> read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The project from the command line: its folder, or a scene file inside one.
core::Result<Project> find_project(std::string_view argument) {
    std::error_code error;
    fs::path given = fs::absolute(fs::path(std::string(argument)), error);
    std::optional<fs::path> scene;
    if (fs::is_regular_file(given, error) && given.extension() == ".toml" &&
        given.filename() != "dream.toml") {
        scene = given;
        given = given.parent_path();
    }
    fs::path folder = given;
    while (!fs::exists(folder / "dream.toml", error)) {
        if (!folder.has_parent_path() || folder.parent_path() == folder) {
            return core::Error{core::ErrorCode::NotFound,
                               std::format("No dream.toml in '{}' or above it", given.string())};
        }
        folder = folder.parent_path();
    }

    Project project{.folder = folder,
                    .scene = {},
                    .build = folder / std::string(k_default_build),
                    .game = folder.filename().string()};
    const std::optional<std::string> text = read_text(folder / "dream.toml");
    const core::Result<core::Config> config =
        core::Config::parse(text.value_or(std::string()));
    if (!config.has_value()) {
        return core::Error{config.error().code,
                           std::format("{}: {}", (folder / "dream.toml").string(),
                                       config.error().message)};
    }
    std::string scene_path(
        config.value().get_string("editor.scene").value_or("assets:/scenes/main.toml"));
    if (scene_path.starts_with("assets:/")) {
        scene_path = "assets/" + scene_path.substr(8);
    }
    project.scene = scene.value_or(folder / scene_path);
    if (const auto build = config.value().get_string("editor.build")) {
        project.build = folder / std::string(*build);
    }
    if (const auto game = config.value().get_string("editor.game")) {
        project.game = *game;
    }
    if (const auto folders = config.value().get_string_list("editor.assets")) {
        for (const std::string& assets : *folders) {
            project.assets.push_back((folder / assets).lexically_normal());
        }
    } else {
        project.assets.push_back(folder / "assets");
    }
    return project;
}

// Whether a ray meets a box, and how far along it.
std::optional<float> ray_hits(const runtime::CameraRay& ray, const core::Aabb& box) {
    float near = 0.0f;
    float far = std::numeric_limits<float>::max();
    const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float direction[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    const float low[3] = {box.min.x, box.min.y, box.min.z};
    const float high[3] = {box.max.x, box.max.y, box.max.z};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1.0e-8f) {
            if (origin[axis] < low[axis] || origin[axis] > high[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float enter = (low[axis] - origin[axis]) / direction[axis];
        float leave = (high[axis] - origin[axis]) / direction[axis];
        if (enter > leave) {
            std::swap(enter, leave);
        }
        near = std::max(near, enter);
        far = std::min(far, leave);
        if (near > far) {
            return std::nullopt;
        }
    }
    return near;
}

Vec3 position_of(const runtime::World& world, runtime::Entity entity) {
    const auto* transform = world.registry().try_get<runtime::WorldTransform>(entity);
    return transform == nullptr ? Vec3{} : core::transform_point(transform->matrix, Vec3{});
}

// The boxes around an entity's model, in the world, or none for an entity without one.
std::vector<core::Aabb> boxes_of(const runtime::World& world, runtime::Entity entity) {
    std::vector<core::Aabb> boxes;
    const auto* mesh = world.registry().try_get<runtime::MeshRenderer>(entity);
    const auto* transform = world.registry().try_get<runtime::WorldTransform>(entity);
    if (mesh == nullptr || transform == nullptr || !mesh->model) {
        return boxes;
    }
    for (const runtime::ModelPart& part : mesh->model->parts) {
        if (!part.bounds.empty()) {
            boxes.push_back(core::transformed(transform->matrix, part.bounds));
        }
    }
    return boxes;
}

class Editor final : public runtime::Game {
public:
    core::Result<void> on_start(runtime::App& app) override {
        if (core::Result<void> ready = m_renderer.initialize(app); !ready.has_value()) {
            return ready;
        }
        // Optional, as in games: listed, their components draw in the view too.
        if (app.has_module<particles::ParticleSystem>()) {
            if (core::Result<void> ready = m_particles.initialize(app); !ready.has_value()) {
                return ready;
            }
        }
        if (app.has_module<animation::AnimationSystem>()) {
            if (core::Result<void> ready = m_animation.initialize(app); !ready.has_value()) {
                return ready;
            }
        }
        if (core::Result<void> ready = m_overlay.initialize(app, m_renderer); !ready.has_value()) {
            return ready;
        }
        m_panels = app.create_modules<editor_panel::EditorPanel>();
        // For checks that run the editor and end by themselves.
        m_quit_after_frames = app.config().get_int("editor.quit_after_frames").value_or(0);
        // A game's own components can't be built here, but they stay in the scene, editable.
        app.scenes().set_skip_unknown_components(true);
        m_scene = std::make_unique<editor::EditorScene>(app.scenes());

        m_subscriptions.push_back(app.events().subscribe<platform::PointerWheel>(
            [this](const platform::PointerWheel& wheel) { m_wheel += wheel.y; }));

        const std::span<const std::string> arguments = runtime::program_arguments();
        if (arguments.empty()) {
            m_status = "Start the editor with a project folder or a scene file: "
                       "dream_editor <folder>";
            return {};
        }
        core::Result<Project> project = find_project(arguments.front());
        if (!project.has_value()) {
            m_status = project.error().message;
            return {};
        }
        m_project = std::move(project).value();
        // The project's own assets, as its scene names them, read before the editor's: the last
        // added is read first, so later folders win, as in the game's build.
        for (const fs::path& assets : m_project->assets) {
            app.files().add_asset_folder(assets);
        }
        if (const core::Result<void> opened = m_scene->open(m_project->scene);
            !opened.has_value()) {
            m_status = opened.error().message;
        } else {
            m_status = std::format("Opened {}", m_project->scene.string());
            start_from_scene_camera(app);
            // For checks, which show an entity selected.
            if (const auto selected = app.config().get_string("editor.select")) {
                m_scene->select(*selected);
            }
            // For a check of Play: it plays, moves the selected entity up and saves while the
            // game runs, then quits.
            if (app.config().get_bool("editor.check_play").value_or(false)) {
                m_check_play = true;
                play();
            }
        }
        return {};
    }

    void on_update(runtime::App& app, const core::FrameTime& time) override {
        const auto seconds = static_cast<float>(time.delta_seconds);
        if (++m_frames == m_quit_after_frames) {
            app.request_quit();
        }
        debug_ui::DebugUi& ui = m_overlay.begin_frame(app, seconds);
        draw_toolbar(ui, app);
        draw_panels(ui, app);
        m_overlay.end_frame();

        if (!m_overlay.wants_keyboard()) {
            shortcuts(app);
        }
        if (!m_overlay.wants_pointer()) {
            move_camera(app);
            if (app.input().was_pressed("select")) {
                pick(app);
            }
        }
        m_wheel = 0.0f;
        m_last_pointer = Vec2{app.input().pointer_position().x, app.input().pointer_position().y};

        watch_file(seconds);
        watch_play();
        if (m_check_play) {
            check_play(app, seconds);
        }

        runtime::World* world = m_scene->world();
        if (world == nullptr) {
            return;
        }
        if (m_camera_version != m_scene->world_version() || m_camera == runtime::k_no_entity) {
            add_camera(*world);
        }
        place_camera(*world);
        world->update_transforms();
        if (m_particles.system() != nullptr) {
            m_particles.update(*world, time.delta_seconds);
        }
        if (m_animation.system() != nullptr) {
            m_animation.update(*world, time.delta_seconds);
        }
        draw_markers(*world);
    }

    void on_render(runtime::App& app, rhi::CommandList& commands) override {
        if (runtime::World* world = m_scene->world()) {
            m_renderer.render(*world, commands, app.surface_size());
        } else {
            m_renderer.render(m_empty, commands, app.surface_size());
        }
    }

    void on_stop(runtime::App& /*app*/) override {
        const std::string report = std::format(
            "editor frames={} entities={} problem={}{}", m_frames, m_scene->entities().size(),
            m_scene->problem().empty() ? std::string("none") : m_scene->problem(),
            m_check_report.empty() ? std::string() : " play: " + m_check_report);
        DREAM_LOG_INFO("{}", report);
        std::puts(report.c_str());
        // Unsaved changes aren't lost with the window: they are kept beside the scene.
        if (m_scene->is_open() && m_scene->unsaved()) {
            const fs::path backup = m_scene->file().string() + ".unsaved";
            std::ofstream out(backup, std::ios::binary | std::ios::trunc);
            const std::string text = m_scene->text();
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (out) {
                DREAM_LOG_WARNING("The scene had unsaved changes, kept in '{}'", backup.string());
            }
        }
        m_game.stop();
        m_subscriptions.clear();
    }

private:
    void draw_toolbar(debug_ui::DebugUi& ui, runtime::App& app) {
        const auto width = static_cast<float>(app.surface_size().width);
        ui.next_window_placement(Vec2{k_margin, k_margin},
                                 Vec2{std::max(width - 2.0f * k_margin, 200.0f), k_toolbar_height});
        if (ui.begin_window("Scene")) {
            const std::string name =
                m_scene->is_open() ? m_scene->file().filename().string() : std::string("No scene");
            ui.text(std::format("{}{}", name, m_scene->unsaved() ? " (unsaved)" : ""));
            if (m_scene->is_open()) {
                ui.same_line();
                if (ui.button("Save")) {
                    save();
                }
                ui.tooltip("Ctrl+S");
                ui.same_line();
                if (ui.button("Undo") && m_scene->can_undo()) {
                    m_scene->undo();
                }
                ui.tooltip("Ctrl+Z");
                ui.same_line();
                if (ui.button("Redo") && m_scene->can_redo()) {
                    m_scene->redo();
                }
                ui.tooltip("Ctrl+Y");
                ui.same_line();
                if (ui.button("Revert")) {
                    revert();
                }
                ui.tooltip("Reads the scene file again, dropping unsaved changes");
                ui.same_line();
                draw_play(ui);
            }
            ui.text_disabled(m_status);
            // Shown here too, as with nothing selected the inspector doesn't say it.
            if (const std::string problem = m_scene->problem(); !problem.empty()) {
                ui.text(std::format("The scene can't be shown as it is: {}", problem));
            }
        }
        ui.end_window();
    }

    void draw_play(debug_ui::DebugUi& ui) {
        if (!m_project.has_value()) {
            return;
        }
        if (m_play == Play::Idle) {
            if (ui.button("Play")) {
                play();
            }
            ui.tooltip(std::format("Saves, builds {} in {}, and starts it", m_project->game,
                                   m_project->build.string()));
        } else if (ui.button("Stop")) {
            m_game.stop();
            m_play = Play::Idle;
            m_status = "Stopped";
        }
    }

    void draw_panels(debug_ui::DebugUi& ui, runtime::App& app) {
        const auto width = static_cast<float>(app.surface_size().width);
        const auto height = static_cast<float>(app.surface_size().height);
        const float top = k_toolbar_height + 2.0f * k_margin;
        std::size_t left_count = 0;
        std::size_t right_count = 0;
        for (const auto& panel : m_panels) {
            (panel->side() == editor_panel::EditorPanel::Side::Left ? left_count : right_count)++;
        }
        std::size_t left = 0;
        std::size_t right = 0;
        for (const auto& panel : m_panels) {
            const bool on_left = panel->side() == editor_panel::EditorPanel::Side::Left;
            const std::size_t count = on_left ? left_count : right_count;
            std::size_t& index = on_left ? left : right;
            const float each = (height - top - k_margin) / static_cast<float>(count);
            ui.next_window_placement(
                Vec2{on_left ? k_margin : width - k_panel_width - k_margin,
                     top + each * static_cast<float>(index)},
                Vec2{k_panel_width, each - k_margin});
            ++index;
            if (ui.begin_window(panel->title()) && m_scene->is_open()) {
                panel->draw(ui, *m_scene);
            }
            ui.end_window();
        }
    }

    void shortcuts(runtime::App& app) {
        const runtime::InputActions& input = app.input();
        const bool control = input.is_down("control");
        if (control && input.was_pressed("save")) {
            save();
        } else if (control && input.was_pressed("undo") && m_scene->can_undo()) {
            m_scene->undo();
        } else if (control && input.was_pressed("redo") && m_scene->can_redo()) {
            m_scene->redo();
        } else if (input.was_pressed("delete") && !m_scene->selected().empty()) {
            m_scene->remove_entity(m_scene->selected());
        } else if (input.was_pressed("focus")) {
            focus_selected();
        }
    }

    void save() {
        if (const core::Result<void> saved = m_scene->save(); !saved.has_value()) {
            m_status = saved.error().message;
            return;
        }
        m_status = std::format("Saved {}", m_scene->file().string());
        // A running game picks the change up when its assets are built again.
        if (m_play == Play::Running) {
            m_restage = true;
        }
    }

    void revert() {
        if (const core::Result<void> reverted = m_scene->reload(); !reverted.has_value()) {
            m_status = reverted.error().message;
        } else {
            m_status = "Read the scene file again";
        }
    }

    // Another program, such as a text editor or git, changed the file: it is read again, unless
    // there are changes here, which win until saved or reverted.
    void watch_file(float seconds) {
        m_file_check -= seconds;
        if (m_file_check > 0.0f || !m_scene->is_open()) {
            return;
        }
        m_file_check = 1.0f;
        if (m_scene->changed_on_disk()) {
            if (m_scene->unsaved()) {
                m_status = "The file changed on disk; Save keeps these changes, Revert takes those";
            } else {
                revert();
            }
        }
    }

    void play() {
        if (m_scene->unsaved()) {
            save();
        }
        const fs::path log = m_project->build / "editor_play.log";
        if (!m_build.run("cmake", {"--build", m_project->build.string(), "--target", m_project->game},
                         m_project->folder, log)) {
            m_status = std::format("CMake can't be started to build {}: is it on the PATH?",
                                   m_project->game);
            return;
        }
        m_play = Play::Building;
        m_status = std::format("Building {}...", m_project->game);
    }

    // The game's program: in <build>/<game>/ for a game in its own repository, or deeper, as the
    // engine's samples are in <build>/samples/<sample>/<game>/.
    [[nodiscard]] fs::path find_program() const {
        const std::string file = m_project->game + std::string(k_program_suffix);
        const fs::path usual = m_project->build / m_project->game / file;
        std::error_code error;
        if (fs::exists(usual, error)) {
            return usual;
        }
        for (fs::recursive_directory_iterator it(m_project->build, error), end;
             !error && it != end; it.increment(error)) {
            if (it.depth() > 4) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->path().filename() == file &&
                it->path().parent_path().filename() == m_project->game) {
                return it->path();
            }
        }
        return usual;
    }

    void watch_play() {
        if (m_play == Play::Building && !m_build.running()) {
            if (m_build.exit_code() != 0) {
                m_play = Play::Idle;
                m_status = std::format("The build failed; see {}",
                                       (m_project->build / "editor_play.log").string());
                return;
            }
            const fs::path program = find_program();
            const fs::path folder = program.parent_path();
            // What the game prints goes beside the build, to read when it misbehaves.
            if (!m_game.start(program, folder, m_project->build / "editor_game.log")) {
                m_play = Play::Idle;
                m_status = std::format("{} can't be started", program.string());
                return;
            }
            m_play = Play::Running;
            m_status = std::format("{} is running: saved changes reach it as it runs; its "
                                   "output is in {}",
                                   m_project->game,
                                   (m_project->build / "editor_game.log").string());
        } else if (m_play == Play::Running) {
            if (!m_game.running()) {
                m_play = Play::Idle;
                m_status = std::format("{} ended", m_project->game);
            } else if (m_restage && !m_build.running()) {
                m_restage = false;
                static_cast<void>(m_build.run(
                    "cmake",
                    {"--build", m_project->build.string(), "--target", m_project->game + "_assets"},
                    m_project->folder, m_project->build / "editor_assets.log"));
            }
        }
    }

    // The steps of the Play check, a few seconds apart, so the game has time to start and to
    // reload.
    void check_play(runtime::App& app, float seconds) {
        m_check_time += seconds;
        if (m_play == Play::Idle || m_check_time > 900.0f) {
            m_check_report = m_check_saved ? "ended after saving" : "ended before saving";
            app.request_quit();
            m_check_play = false;
            return;
        }
        if (m_play != Play::Running) {
            m_check_time = 0.0f;
            return;
        }
        if (!m_check_saved && m_check_time > 4.0f) {
            const std::string entity = m_scene->selected();
            m_scene->set_setting(entity, "transform", "position",
                                 std::vector<double>{0.0, 2.5, 0.0});
            save();
            m_check_saved = true;
            m_check_time = 0.0f;
        } else if (m_check_saved && !m_restage && !m_build.running() && m_check_time > 6.0f) {
            m_check_report = std::format("saved while running, assets build exit={}",
                                         m_build.exit_code());
            app.request_quit();
            m_check_play = false;
        }
    }

    // The editor's camera starts where the scene's own looks from, when it has one.
    void start_from_scene_camera(runtime::App& app) {
        runtime::World* world = m_scene->world();
        if (world == nullptr) {
            return;
        }
        const runtime::Entity camera = runtime::active_camera(*world);
        if (camera == runtime::k_no_entity) {
            return;
        }
        const Vec3 eye = position_of(*world, camera);
        const auto& lens = world->registry().get<runtime::Camera>(camera);
        m_target = lens.target.value_or(eye + Vec3{0.0f, 0.0f, -5.0f});
        const Vec3 offset = eye - m_target;
        m_distance = std::max(core::length(offset), 0.5f);
        m_yaw = std::atan2(offset.x, offset.z);
        m_pitch = std::asin(std::clamp(offset.y / m_distance, -1.0f, 1.0f));
        static_cast<void>(app);
    }

    void add_camera(runtime::World& world) {
        m_camera = world.create_entity();
        world.registry().emplace<runtime::Camera>(
            m_camera, runtime::Camera{.vertical_fov_degrees = 50.0f,
                                      .z_near = 0.05f,
                                      .z_far = 1000.0f,
                                      .target = m_target,
                                      .priority = k_editor_camera_priority});
        m_camera_version = m_scene->world_version();
    }

    void place_camera(runtime::World& world) {
        const Vec3 offset{std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch),
                          std::cos(m_pitch) * std::cos(m_yaw)};
        world.registry().get<runtime::Transform>(m_camera).position = m_target + offset * m_distance;
        world.registry().get<runtime::Camera>(m_camera).target = m_target;
    }

    // Right drag turns around the target, middle drag moves it, and the wheel goes nearer.
    void move_camera(runtime::App& app) {
        const Vec2 pointer{app.input().pointer_position().x, app.input().pointer_position().y};
        const Vec2 moved = pointer - m_last_pointer;
        if (app.input().is_down("orbit")) {
            m_yaw -= moved.x * 0.008f;
            m_pitch = std::clamp(m_pitch + moved.y * 0.008f, -1.5f, 1.5f);
        }
        if (app.input().is_down("pan")) {
            const Vec3 forward{-std::cos(m_pitch) * std::sin(m_yaw), -std::sin(m_pitch),
                               -std::cos(m_pitch) * std::cos(m_yaw)};
            const Vec3 right = core::normalized(core::cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
            const Vec3 up = core::cross(right, forward);
            const float scale = m_distance * 0.0015f;
            m_target = m_target - right * (moved.x * scale) + up * (moved.y * scale);
        }
        if (m_wheel != 0.0f) {
            m_distance = std::clamp(m_distance * std::pow(0.88f, m_wheel), 0.2f, 2000.0f);
        }
    }

    // Selects what is under the pointer: the nearest model, or the marker of an entity without
    // one, such as a light.
    void pick(runtime::App& app) {
        runtime::World* world = m_scene->world();
        if (world == nullptr) {
            return;
        }
        const std::optional<runtime::CameraRay> ray = runtime::camera_ray(
            *world, app.surface_size(),
            Vec2{app.input().pointer_position().x, app.input().pointer_position().y});
        if (!ray.has_value()) {
            return;
        }
        std::string nearest;
        float nearest_distance = std::numeric_limits<float>::max();
        for (const auto [entity, name] : world->registry().view<const runtime::Name>().each()) {
            std::vector<core::Aabb> boxes = boxes_of(*world, entity);
            if (boxes.empty()) {
                const Vec3 at = position_of(*world, entity);
                boxes.push_back(core::Aabb{at - Vec3{0.25f, 0.25f, 0.25f},
                                           at + Vec3{0.25f, 0.25f, 0.25f}});
            }
            for (const core::Aabb& box : boxes) {
                if (const std::optional<float> hit = ray_hits(*ray, box);
                    hit.has_value() && *hit < nearest_distance) {
                    nearest_distance = *hit;
                    nearest = name.value;
                }
            }
        }
        m_scene->select(nearest);
    }

    void focus_selected() {
        runtime::World* world = m_scene->world();
        const runtime::Entity entity =
            world == nullptr ? runtime::k_no_entity : m_scene->entity_in_world(m_scene->selected());
        if (entity == runtime::k_no_entity) {
            return;
        }
        core::Aabb all;
        for (const core::Aabb& box : boxes_of(*world, entity)) {
            all.min = Vec3{std::min(all.min.x, box.min.x), std::min(all.min.y, box.min.y),
                           std::min(all.min.z, box.min.z)};
            all.max = Vec3{std::max(all.max.x, box.max.x), std::max(all.max.y, box.max.y),
                           std::max(all.max.z, box.max.z)};
        }
        if (all.empty()) {
            m_target = position_of(*world, entity);
            m_distance = 4.0f;
        } else {
            m_target = (all.min + all.max) * 0.5f;
            m_distance = std::max(core::length(all.max - all.min) * 1.5f, 1.0f);
        }
    }

    // Small crosses where entities without a model are, such as lights, and boxes around the
    // selected one.
    void draw_markers(const runtime::World& world) {
        runtime::DebugLines& lines = m_renderer.debug_lines();
        const runtime::Entity selected = m_scene->entity_in_world(m_scene->selected());
        for (const auto [entity, name] : world.registry().view<const runtime::Name>().each()) {
            const std::vector<core::Aabb> boxes = boxes_of(world, entity);
            const bool is_selected = entity == selected;
            const Vec4 color = is_selected ? Vec4{1.0f, 0.75f, 0.1f, 1.0f}
                                           : Vec4{0.6f, 0.8f, 1.0f, 1.0f};
            if (boxes.empty()) {
                lines.cross(position_of(world, entity), is_selected ? 0.5f : 0.3f, color);
            } else if (is_selected) {
                for (const core::Aabb& box : boxes) {
                    lines.box((box.min + box.max) * 0.5f, core::Quat{}, (box.max - box.min) * 0.5f,
                              color);
                }
            }
        }
    }

    enum class Play { Idle, Building, Running };

    runtime::Renderer m_renderer;
    runtime::Particles m_particles;
    runtime::Animation m_animation;
    runtime::DebugOverlay m_overlay;
    std::vector<std::unique_ptr<editor_panel::EditorPanel>> m_panels;
    std::unique_ptr<editor::EditorScene> m_scene;
    std::optional<Project> m_project;
    std::vector<runtime::Subscription> m_subscriptions;
    runtime::World m_empty;
    std::string m_status;
    std::int64_t m_frames = 0;
    std::int64_t m_quit_after_frames = 0;

    runtime::Entity m_camera = runtime::k_no_entity;
    std::uint64_t m_camera_version = 0;
    Vec3 m_target;
    float m_distance = 10.0f;
    float m_yaw = 0.0f;
    float m_pitch = 0.5f;
    float m_wheel = 0.0f;
    Vec2 m_last_pointer;
    float m_file_check = 1.0f;

    editor::Process m_build;
    editor::Process m_game;
    Play m_play = Play::Idle;
    bool m_restage = false;
    bool m_check_play = false;
    bool m_check_saved = false;
    float m_check_time = 0.0f;
    std::string m_check_report;
};

} // namespace

std::unique_ptr<runtime::Game> runtime::create_game() {
    return std::make_unique<Editor>();
}
