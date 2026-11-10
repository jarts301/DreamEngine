#pragma once

#include "dream/core/result.h"
#include "dream/platform/event.h"
#include "dream/platform/native_window.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::platform {

struct WindowSettings {
    std::string title;

    // Ignored where the window always fills the screen, such as on Android.
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
};

enum class FileRoot : std::uint8_t {
    Assets, // Read-only files shipped with the game. Inside the APK on Android.
    User,   // Writable storage that survives between runs.
};

// The operating system, as seen by the engine: a window, its events and file storage.
//
// All functions must be called from the thread that created the Platform.
class Platform {
public:
    static constexpr std::string_view k_module_interface = "platform";

    virtual ~Platform() = default;

    // Call once, before polling for events.
    [[nodiscard]] virtual core::Result<void> create_window(const WindowSettings& settings) = 0;

    [[nodiscard]] virtual SurfaceSize surface_size() const = 0;

    // Changes when a surface is lost and restored, so fetch it again after SurfaceRestored.
    [[nodiscard]] virtual NativeWindow native_window() const = 0;

    // How much larger than usual the system draws its own text and controls, such as 2 on many
    // phones. Interfaces multiply their sizes by this, so they stay readable on dense screens.
    [[nodiscard]] virtual float display_scale() const = 0;

    // While on, typing arrives as TextEntered events, and touch screens show their keyboard.
    virtual void set_text_input(bool enabled) = 0;

    // The part of the surface that nothing covers, such as a phone's status bar, camera cutout or
    // rounded corners. Interfaces keep their contents inside it. The whole surface where nothing
    // covers it.
    [[nodiscard]] virtual SurfaceRect safe_area() const = 0;

    // Returns the next pending event, or nothing if there are none.
    [[nodiscard]] virtual std::optional<Event> poll_event() = 0;

    // Sleeps until an event arrives. Used while paused, to avoid draining the battery.
    [[nodiscard]] virtual Event wait_event() = 0;

    // `path` is relative to `root`, uses forward slashes, and has no "." or ".." parts.
    [[nodiscard]] virtual core::Result<std::vector<std::byte>>
    read_file(FileRoot root, std::string_view path) = 0;

    // Writes into FileRoot::User, creating parent directories and replacing any existing file.
    [[nodiscard]] virtual core::Result<void> write_user_file(std::string_view path,
                                                             std::span<const std::byte> data) = 0;
};

} // namespace dream::platform
