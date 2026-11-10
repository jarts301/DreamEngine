#pragma once

#include "dream/core/clock.h"
#include "dream/core/config.h"
#include "dream/core/module_project.h"
#include "dream/core/result.h"
#include "dream/platform/platform.h"
#include "dream/runtime/asset_manager.h"
#include "dream/runtime/event_bus.h"
#include "dream/runtime/game.h"
#include "dream/runtime/input_actions.h"
#include "dream/runtime/scene_loader.h"
#include "dream/runtime/virtual_file_system.h"

#include <memory>

namespace dream::runtime {

// Runs a game's main loop and gives it access to the engine.
//
// The window and frame rate come from the project configuration:
//
//     [app]
//     title = "My Game"
//     width = 1280
//     height = 720
//     max_fps = 60     # 0 means no limit. Defaults to 60, or no limit with a graphics device,
//                      # whose vertical sync already sets the pace.
//     fixed_rate = 60  # How many times per second Game::on_fixed_update runs. Defaults to 60.
class App {
public:
    // `config` must outlive this object. `device` is null for a program that draws nothing.
    // `modules`, which must also outlive it, lets engine systems such as physics create their
    // backends. Without it they can still be given one directly.
    App(std::unique_ptr<platform::Platform> platform, const core::Config& config,
        std::unique_ptr<rhi::Device> device = nullptr, const core::ModuleProject* modules = nullptr);

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Returns when the game ends. Call once.
    [[nodiscard]] core::Result<void> run(Game& game);

    // Ends the main loop after the current frame.
    void request_quit() noexcept {
        m_running = false;
    }

    [[nodiscard]] const InputActions& input() const noexcept {
        return m_input;
    }

    [[nodiscard]] VirtualFileSystem& files() noexcept {
        return m_files;
    }

    [[nodiscard]] AssetManager& assets() noexcept {
        return m_assets;
    }

    [[nodiscard]] SceneLoader& scenes() noexcept {
        return m_scenes;
    }

    // Every platform event, such as platform::SurfaceResized, is also published here.
    [[nodiscard]] EventBus& events() noexcept {
        return m_events;
    }

    [[nodiscard]] const core::Config& config() const noexcept {
        return m_config;
    }

    // Null if the project has no graphics module.
    [[nodiscard]] rhi::Device* device() noexcept {
        return m_device.get();
    }

    [[nodiscard]] platform::SurfaceSize surface_size() const noexcept {
        return m_surface_size;
    }

    [[nodiscard]] double fixed_step_seconds() const noexcept {
        return m_fixed_steps.step_seconds();
    }

    // How far this frame is between the last fixed update and the next, from 0 to 1.
    [[nodiscard]] double fixed_step_alpha() const noexcept {
        return m_fixed_steps.alpha();
    }

    // Creates the backend for `Interface` that the project configuration selects for this platform.
    template <core::ModuleInterface Interface>
    [[nodiscard]] core::Result<std::unique_ptr<Interface>> create_module() const {
        if (m_modules == nullptr) {
            return core::Error{core::ErrorCode::NotFound,
                               "This program has no module project to create backends from"};
        }
        core::Result<core::CreatedModule<Interface>> created = m_modules->create<Interface>();
        if (!created.has_value()) {
            return created.error();
        }
        return std::move(created.value().instance);
    }

private:
    [[nodiscard]] bool can_update() const noexcept {
        return !m_paused && m_has_surface;
    }

    void pump_events(Game& game);
    void handle_event(Game& game, const platform::Event& event);
    [[nodiscard]] core::Result<void> attach_surface();
    [[nodiscard]] core::Result<void> render_frame(Game& game);

    // Declared in this order so the device is destroyed before the window it draws to.
    std::unique_ptr<platform::Platform> m_platform;
    std::unique_ptr<rhi::Device> m_device;
    const core::Config& m_config;
    const core::ModuleProject* m_modules;
    VirtualFileSystem m_files;
    AssetManager m_assets;
    SceneLoader m_scenes;
    EventBus m_events;
    InputActions m_input;
    platform::SurfaceSize m_surface_size;
    core::FixedTimestep m_fixed_steps;
    bool m_running = true;
    bool m_paused = false;
    bool m_has_surface = true;
    bool m_surface_attached = false;
};

} // namespace dream::runtime
