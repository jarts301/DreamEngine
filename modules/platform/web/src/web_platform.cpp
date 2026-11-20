// A platform whose window is a web page (ADR 0039): it serves a page that shows the frames the
// software device draws and sends back keyboard, mouse and touch input, so a game runs and is
// played in a browser on a machine with no display, GPU or sound card. Files come from the folder
// the program is in, as on any desktop.
//
// Settings, all optional:
//
//   [platform.web]
//   host = "127.0.0.1"   # "0.0.0.0" to let other machines on the network open the page
//   port = 8080          # 0 picks a free port
//
// The environment variables DREAM_WEB_HOST and DREAM_WEB_PORT override host and port, as the hub
// (tools/hub) sets them for each game it starts.
//   frame_limit = 0      # quit after this many frames, 0 for never, for automated runs
//
// With app.window = "hidden" no page is served at all, for automated runs such as tests.

#include "web_client.h"

#include "dream/core/archive.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/platform/platform.h"
#include "dream/platform/web_canvas.h"

#include "webserve/server.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dream::modules {
namespace {

using namespace platform;

constexpr std::uint16_t k_default_port = 8080;
// How long a request for a new frame waits before answering that there is none yet.
constexpr auto k_frame_wait = std::chrono::milliseconds(1000);
constexpr auto k_wait_slice = std::chrono::milliseconds(100);

// KeyboardEvent.code values, as browsers name physical keys, to the engine's keys.
std::optional<Key> key_from_code(std::string_view code) {
    if (code.size() == 4 && code.starts_with("Key") && code[3] >= 'A' && code[3] <= 'Z') {
        return static_cast<Key>(static_cast<int>(Key::A) + (code[3] - 'A'));
    }
    if (code.size() == 6 && code.starts_with("Digit") && code[5] >= '0' && code[5] <= '9') {
        return static_cast<Key>(static_cast<int>(Key::Digit0) + (code[5] - '0'));
    }
    if (code.size() >= 2 && code.size() <= 3 && code[0] == 'F') {
        const int number = std::atoi(std::string(code.substr(1)).c_str());
        if (number >= 1 && number <= 12) {
            return static_cast<Key>(static_cast<int>(Key::F1) + number - 1);
        }
    }
    static const std::unordered_map<std::string_view, Key> named{
        {"Space", Key::Space},           {"Enter", Key::Enter},
        {"NumpadEnter", Key::Enter},     {"Escape", Key::Escape},
        {"Tab", Key::Tab},               {"Backspace", Key::Backspace},
        {"Delete", Key::Delete},         {"Home", Key::Home},
        {"End", Key::End},               {"ArrowLeft", Key::Left},
        {"ArrowRight", Key::Right},      {"ArrowUp", Key::Up},
        {"ArrowDown", Key::Down},        {"ShiftLeft", Key::LeftShift},
        {"ShiftRight", Key::RightShift}, {"ControlLeft", Key::LeftControl},
        {"ControlRight", Key::RightControl}, {"AltLeft", Key::LeftAlt},
        {"AltRight", Key::RightAlt},     {"BrowserBack", Key::Back},
    };
    const auto found = named.find(code);
    if (found != named.end()) {
        return found->second;
    }
    return std::nullopt;
}

// The folder the program is in, with a trailing slash.
std::string program_folder() {
    std::error_code error;
    const std::filesystem::path program = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        return "./";
    }
    return program.parent_path().string() + "/";
}

// Where a game keeps files between runs: $DREAM_USER_DIR if set, otherwise the XDG data folder,
// ~/.local/share/<organization>/<application>/.
std::string user_folder(std::string_view organization, std::string_view application) {
    if (const char* const forced = std::getenv("DREAM_USER_DIR"); forced != nullptr && *forced) {
        std::string folder = forced;
        return folder.ends_with('/') ? folder : folder + "/";
    }
    std::string base;
    if (const char* const data = std::getenv("XDG_DATA_HOME"); data != nullptr && *data) {
        base = data;
    } else if (const char* const home = std::getenv("HOME"); home != nullptr && *home) {
        base = std::string(home) + "/.local/share";
    } else {
        base = "/tmp";
    }
    return std::format("{}/{}/{}/", base, organization, application);
}

core::Error io_error(std::string message) {
    return core::Error{core::ErrorCode::IoError, std::move(message)};
}

struct Settings {
    std::string organization;
    std::string application;
    std::string host = "127.0.0.1";
    std::uint16_t port = k_default_port;
    std::int64_t frame_limit = 0;
};

class WebPlatform final : public Platform, private WebCanvas {
public:
    explicit WebPlatform(Settings settings)
        : m_settings(std::move(settings)), m_program_folder(program_folder()),
          m_user_folder(user_folder(m_settings.organization, m_settings.application)) {}

    ~WebPlatform() override {
        m_server.stop();
    }

    WebPlatform(const WebPlatform&) = delete;
    WebPlatform& operator=(const WebPlatform&) = delete;

    core::Result<void> create_window(const WindowSettings& settings) override {
        m_size = SurfaceSize{settings.width, settings.height};
        m_title = settings.title;
        if (settings.mode == WindowMode::Hidden) {
            DREAM_LOG_INFO("The window is hidden, so no web page is served");
            return {};
        }
        const std::optional<std::string> error = m_server.start(
            m_settings.host, m_settings.port,
            [this](const webserve::Request& request) { return handle(request); });
        if (error.has_value()) {
            return io_error(std::format("The web platform could not start: {}. Set another port "
                                        "with [platform.web] port or DREAM_WEB_PORT",
                                        *error));
        }
        const std::string shown_host =
            m_settings.host == "0.0.0.0" ? std::string("<this machine's address>") : m_settings.host;
        DREAM_LOG_INFO("Open http://{}:{}/ in a web browser to see and play {}", shown_host,
                       m_server.port(), m_title);
        return {};
    }

    SurfaceSize surface_size() const override {
        return m_size;
    }

    NativeWindow native_window() const override {
        if (!serving()) {
            return std::monostate{};
        }
        return WebWindow{.canvas = static_cast<WebCanvas*>(const_cast<WebPlatform*>(this))};
    }

    float display_scale() const override {
        return 1.0f;
    }

    void set_text_input(bool enabled) override {
        const std::lock_guard lock(m_events_mutex);
        m_text_input = enabled;
    }

    SurfaceRect safe_area() const override {
        return SurfaceRect{0, 0, m_size.width, m_size.height};
    }

    std::optional<Event> poll_event() override {
        const std::lock_guard lock(m_events_mutex);
        if (m_events.empty()) {
            // The main loop reads events until there are none, so each "none" is one frame.
            if (m_settings.frame_limit > 0 && ++m_frames >= m_settings.frame_limit) {
                return QuitRequested{};
            }
            return std::nullopt;
        }
        Event event = std::move(m_events.front());
        m_events.pop_front();
        return event;
    }

    Event wait_event() override {
        std::unique_lock lock(m_events_mutex);
        m_event_arrived.wait(lock, [this] { return !m_events.empty(); });
        Event event = std::move(m_events.front());
        m_events.pop_front();
        return event;
    }

    core::Result<std::vector<std::byte>> read_file(FileRoot root, std::string_view path) override {
        if (root == FileRoot::Assets) {
            if (core::ArchiveReader* const archive = asset_archive()) {
                return archive->read(path);
            }
        }
        const std::string full_path = resolve(root, path);
        std::ifstream file(full_path, std::ios::binary | std::ios::ate);
        if (!file) {
            return core::Error{core::ErrorCode::NotFound, std::format("Could not read '{}'", path)};
        }
        const std::streamsize size = file.tellg();
        file.seekg(0);
        std::vector<std::byte> contents(static_cast<std::size_t>(std::max<std::streamsize>(size, 0)));
        if (!file.read(reinterpret_cast<char*>(contents.data()), size)) {
            return io_error(std::format("Could not read '{}'", path));
        }
        return contents;
    }

    // Modification times, in nanoseconds. Assets inside an archive have none.
    std::optional<std::int64_t> file_version(FileRoot root, std::string_view path) override {
        if (root == FileRoot::Assets && asset_archive() != nullptr) {
            return std::nullopt;
        }
        struct stat info {};
        if (::stat(resolve(root, path).c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
            return 0;
        }
        const std::int64_t version =
            std::int64_t{info.st_mtim.tv_sec} * 1'000'000'000 + info.st_mtim.tv_nsec;
        // Never 0, which means a missing file.
        return version != 0 ? version : 1;
    }

    core::Result<void> write_user_file(std::string_view path,
                                       std::span<const std::byte> data) override {
        const std::string target = resolve(FileRoot::User, path);
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(target).parent_path(), error);
        if (error) {
            return io_error(std::format("Could not create the folder for '{}': {}", path,
                                        error.message()));
        }
        // Writing to a temporary name first means a crash can't leave a half-written file.
        const std::string temporary = target + ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(data.data()),
                       static_cast<std::streamsize>(data.size()));
            if (!file) {
                return io_error(std::format("Could not write '{}'", path));
            }
        }
        std::filesystem::rename(temporary, target, error);
        if (error) {
            return io_error(std::format("Could not replace '{}': {}", path, error.message()));
        }
        return {};
    }

private:
    bool serving() const {
        return m_server.port() != 0 && !m_server.stopping();
    }

    std::string resolve(FileRoot root, std::string_view path) const {
        if (root == FileRoot::User) {
            return m_user_folder + std::string(path);
        }
        return std::format("{}assets/{}", m_program_folder, path);
    }

    // Looked for once, at the first asset read: a packaged game reads assets.pak next to the
    // program in place of an assets folder.
    core::ArchiveReader* asset_archive() {
        if (!m_archive_checked) {
            m_archive_checked = true;
            core::Result<core::ArchiveReader> archive =
                core::ArchiveReader::open(m_program_folder + "assets.pak");
            if (archive.has_value()) {
                DREAM_LOG_INFO("Reading assets from assets.pak, {} files",
                               archive.value().file_count());
                m_archive.emplace(std::move(archive).value());
            } else if (archive.error().code != core::ErrorCode::NotFound) {
                DREAM_LOG_ERROR("{}; reading the assets folder instead", archive.error().message);
            }
        }
        return m_archive.has_value() ? &*m_archive : nullptr;
    }

    // WebCanvas: called by the device on the game's thread.
    void present(SurfaceSize size, std::span<const std::byte> rgba) override {
        {
            const std::lock_guard lock(m_frame_mutex);
            m_frame_size = size;
            m_frame.assign(rgba.begin(), rgba.end());
            ++m_frame_id;
        }
        m_frame_ready.notify_all();
    }

    void push_event(Event event) {
        {
            const std::lock_guard lock(m_events_mutex);
            m_events.push_back(std::move(event));
        }
        m_event_arrived.notify_one();
    }

    // Called on the server's threads.
    webserve::Response handle(const webserve::Request& request) {
        if (request.path == "/" || request.path == "/index.html") {
            return webserve::Response{.content_type = "text/html; charset=utf-8",
                                      .body = std::string(web::k_client_page)};
        }
        if (request.path == "/frame") {
            return frame(request);
        }
        if (request.path == "/input" && request.method == "POST") {
            input(request.body);
            return webserve::Response{.status = 204};
        }
        if (request.path == "/status") {
            std::uint64_t frame_id = 0;
            {
                const std::lock_guard lock(m_frame_mutex);
                frame_id = m_frame_id;
            }
            return webserve::Response{
                .content_type = "application/json",
                .body = std::format(R"({{"title":"{}","width":{},"height":{},"frame":{}}})",
                                    webserve::json_escape(m_title), m_size.width, m_size.height,
                                    frame_id)};
        }
        if (request.path == "/quit" && request.method == "POST") {
            DREAM_LOG_INFO("The web page asked the game to stop");
            push_event(QuitRequested{});
            return webserve::Response{.status = 204};
        }
        return webserve::Response{.status = 404, .body = "Not found"};
    }

    // The newest frame, once there is one newer than ?after=, as raw RGBA pixels.
    webserve::Response frame(const webserve::Request& request) {
        const std::uint64_t after = std::strtoull(
            webserve::query_value(request.query, "after").value_or("0").c_str(), nullptr, 10);
        std::unique_lock lock(m_frame_mutex);
        const auto deadline = std::chrono::steady_clock::now() + k_frame_wait;
        while (m_frame_id <= after && !m_server.stopping() &&
               std::chrono::steady_clock::now() < deadline) {
            m_frame_ready.wait_for(lock, k_wait_slice);
        }
        if (m_frame_id <= after) {
            return webserve::Response{.status = 204};
        }
        webserve::Response response{
            .content_type = "application/octet-stream",
            .body = std::string(reinterpret_cast<const char*>(m_frame.data()), m_frame.size())};
        response.headers = {{"X-Frame", std::to_string(m_frame_id)},
                            {"X-Width", std::to_string(m_frame_size.width)},
                            {"X-Height", std::to_string(m_frame_size.height)}};
        return response;
    }

    void input(std::string_view body) {
        while (!body.empty()) {
            const std::size_t end = body.find('\n');
            const std::string line(body.substr(0, end));
            body.remove_prefix(end == std::string_view::npos ? body.size() : end + 1);
            if (line.size() < 3 || line[1] != ' ') {
                continue;
            }
            const std::string_view arguments = std::string_view(line).substr(2);
            const std::size_t space = arguments.find(' ');
            const std::string first(arguments.substr(0, space));
            const std::string second(space == std::string_view::npos ? std::string_view()
                                                                     : arguments.substr(space + 1));
            switch (line[0]) {
            case 'k':
                if (const std::optional<Key> key = key_from_code(first)) {
                    push_event(KeyChanged{.key = *key, .pressed = second == "1"});
                }
                break;
            case 'm':
                push_event(PointerMoved{.x = std::strtof(first.c_str(), nullptr),
                                        .y = std::strtof(second.c_str(), nullptr)});
                break;
            case 'b': {
                const int button = std::atoi(first.c_str());
                if (button >= 0 && button < static_cast<int>(PointerButton::Count)) {
                    push_event(PointerButtonChanged{.button = static_cast<PointerButton>(button),
                                                    .pressed = second == "1"});
                }
                break;
            }
            case 'w':
                push_event(PointerWheel{.x = std::strtof(first.c_str(), nullptr),
                                        .y = std::strtof(second.c_str(), nullptr)});
                break;
            case 't': {
                bool enabled = false;
                {
                    const std::lock_guard lock(m_events_mutex);
                    enabled = m_text_input;
                }
                if (enabled) {
                    push_event(TextEntered{.text = webserve::url_decode(arguments)});
                }
                break;
            }
            default:
                break;
            }
        }
    }

    Settings m_settings;
    std::string m_program_folder;
    std::string m_user_folder;
    std::string m_title;
    SurfaceSize m_size;
    std::optional<core::ArchiveReader> m_archive;
    bool m_archive_checked = false;

    std::mutex m_events_mutex;
    std::condition_variable m_event_arrived;
    std::deque<Event> m_events;
    bool m_text_input = false;
    std::int64_t m_frames = 0;

    std::mutex m_frame_mutex;
    std::condition_variable m_frame_ready;
    std::vector<std::byte> m_frame;
    SurfaceSize m_frame_size;
    std::uint64_t m_frame_id = 0;

    // Last, so it stops before anything its handlers use is destroyed.
    webserve::Server m_server;
};

core::Result<std::unique_ptr<Platform>> create(const core::Config& config) {
    Settings settings{
        .organization = std::string(config.get_string("app.organization").value_or("DreamEngine")),
        .application = std::string(config.get_string("app.name").value_or("DreamGame")),
        .host = std::string(config.get_string("platform.web.host").value_or("127.0.0.1")),
    };
    std::int64_t port = config.get_int("platform.web.port").value_or(k_default_port);
    if (const char* const forced = std::getenv("DREAM_WEB_PORT"); forced != nullptr && *forced) {
        port = std::atoll(forced);
    }
    if (const char* const forced = std::getenv("DREAM_WEB_HOST"); forced != nullptr && *forced) {
        settings.host = forced;
    }
    if (port < 0 || port > 65535) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "Setting 'platform.web.port' must be from 0 to 65535"};
    }
    settings.port = static_cast<std::uint16_t>(port);
    settings.frame_limit = config.get_int("platform.web.frame_limit").value_or(0);
    return std::unique_ptr<Platform>{std::make_unique<WebPlatform>(std::move(settings))};
}

} // namespace

[[nodiscard]] core::Result<void> register_platform_web(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Platform>{.name = "web", .create = &create});
}

} // namespace dream::modules
