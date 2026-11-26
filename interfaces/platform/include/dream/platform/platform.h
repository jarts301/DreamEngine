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
#include <utility>
#include <vector>

namespace dream::platform {

// How a window shows itself on a desktop. Ignored where the window always fills the screen, such
// as on Android.
enum class WindowMode : std::uint8_t {
    // Shown, and given the keyboard, as a game should be.
    Normal,
    // Shown, but never takes the keyboard or comes to the front, so an unattended run, such as a
    // screenshot check, doesn't interrupt whoever is using the computer.
    Background,
    // Never shown. Drawing and screenshots still work where the graphics backend allows it.
    Hidden,
};

struct WindowSettings {
    std::string title;

    // Ignored where the window always fills the screen, such as on Android.
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    WindowMode mode = WindowMode::Normal;
};

enum class FileRoot : std::uint8_t {
    Assets, // Read-only files shipped with the game. Inside the APK on Android.
    User,   // Writable storage that survives between runs.
};

// The operating system, as seen by the engine: a window, its events and file storage.
//
// All functions must be called from the thread that created the Platform.
// An HTTPS request a platform sends for the engine, such as a crash report's upload (ADR 0043).
struct HttpRequest {
    std::string url;
    // Name and value of each header, such as {"Content-Type", "application/json"}.
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::byte> body;
    // How long to wait for the server, for connecting and for its answer each.
    std::uint32_t timeout_seconds = 20;
};

struct HttpResponse {
    // The HTTP status, such as 200.
    int status = 0;
    std::string body;
};

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

    // An optional capability, for hot reloading: a number that changes whenever the file does,
    // such as its modification time, and 0 for a file that doesn't exist. Nothing when the platform
    // can't tell, which is the default, as for files inside an APK or an archive.
    [[nodiscard]] virtual std::optional<std::int64_t> file_version(FileRoot /*root*/,
                                                                   std::string_view /*path*/) {
        return std::nullopt;
    }

    // An optional capability: the folder the program is in, ending with a separator, where a
    // packaged game on PC keeps its asset archives. Nothing where there is none to read files
    // from, which is the default, as on Android, whose assets are inside the APK.
    [[nodiscard]] virtual std::optional<std::string> program_folder() const {
        return std::nullopt;
    }

    // An optional capability: whether the device runs on its battery now, unplugged. Nothing when
    // the platform can't tell, which is the default. It may take a moment, so ask now and then.
    [[nodiscard]] virtual std::optional<bool> on_battery() {
        return std::nullopt;
    }

    // An optional capability: asks the display to refresh at about this many frames a second,
    // which saves power where the screen is faster than the game runs, as on 120 Hz phones; 0
    // lets the display choose again. Does nothing where the platform can't, which is the default.
    virtual void set_frame_rate_hint(float /*frames_per_second*/) {}

    // An optional capability: sends a POST request and waits for the answer, from any thread.
    // Fails where the platform can't, which is the default, and when the network does. An Android
    // game needs its modules to ask for the INTERNET permission (dream_add_module).
    [[nodiscard]] virtual core::Result<HttpResponse> post_http(const HttpRequest& /*request*/) {
        return core::Error{core::ErrorCode::Unknown, "This platform can't send HTTP requests"};
    }
};

} // namespace dream::platform
