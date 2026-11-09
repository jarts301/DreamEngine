#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"
#include "dream/platform/platform.h"
#include "dream/runtime/game.h"
#include "dream/runtime/input_actions.h"
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
//     max_fps = 60     # 0 means no limit.
class App {
public:
    // `config` must outlive this object.
    App(std::unique_ptr<platform::Platform> platform, const core::Config& config);

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

    [[nodiscard]] const core::Config& config() const noexcept {
        return m_config;
    }

    [[nodiscard]] platform::SurfaceSize surface_size() const noexcept {
        return m_surface_size;
    }

private:
    [[nodiscard]] bool can_update() const noexcept {
        return !m_paused && m_has_surface;
    }

    void pump_events(Game& game);
    void handle_event(Game& game, const platform::Event& event);

    std::unique_ptr<platform::Platform> m_platform;
    const core::Config& m_config;
    VirtualFileSystem m_files;
    InputActions m_input;
    platform::SurfaceSize m_surface_size;
    bool m_running = true;
    bool m_paused = false;
    bool m_has_surface = true;
};

} // namespace dream::runtime
