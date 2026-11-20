#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/platform/platform.h"
#include "dream/platform/web_canvas.h"

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <variant>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_platform_web(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::platform;

namespace {

std::unique_ptr<Platform> make_platform(std::string_view config_text) {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_platform_web(registry).has_value());
    const std::vector<std::string> preference{"web"};
    auto created =
        registry.create<Platform>(preference, core::Config::parse(config_text).value());
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

std::string http_exchange(std::uint16_t port, const std::string& request) {
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(socket >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    REQUIRE(::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::send(socket, request.data(), request.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(request.size()));
    std::string reply;
    std::array<char, 65536> chunk{};
    while (true) {
        const ssize_t received = ::recv(socket, chunk.data(), chunk.size(), 0);
        if (received <= 0) {
            break;
        }
        reply.append(chunk.data(), static_cast<std::size_t>(received));
    }
    ::close(socket);
    return reply;
}

// A port unlikely to be in use, different for each run of the tests.
std::uint16_t test_port() {
    return static_cast<std::uint16_t>(20000 + (::getpid() % 20000));
}

} // namespace

TEST_CASE("A hidden web window serves nothing and has no canvas") {
    std::unique_ptr<Platform> platform = make_platform("platform.web.frame_limit = 2");
    REQUIRE(platform->create_window(WindowSettings{.title = "Test", .width = 32, .height = 16,
                                                   .mode = WindowMode::Hidden})
                .has_value());
    CHECK(platform->surface_size() == SurfaceSize{32, 16});
    CHECK(std::holds_alternative<std::monostate>(platform->native_window()));
    CHECK_FALSE(platform->poll_event().has_value());
    // The frame limit asks to quit.
    CHECK(std::holds_alternative<QuitRequested>(*platform->poll_event()));
}

TEST_CASE("The web platform keeps user files in the folder DREAM_USER_DIR names") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("dream_web_test_" + std::to_string(::getpid()));
    ::setenv("DREAM_USER_DIR", folder.c_str(), 1);
    std::unique_ptr<Platform> platform = make_platform("");
    const std::array<std::byte, 3> data{std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE(platform->write_user_file("saves/slot.bin", data).has_value());
    const core::Result<std::vector<std::byte>> read = platform->read_file(FileRoot::User, "saves/slot.bin");
    REQUIRE(read.has_value());
    CHECK(read.value().size() == 3);
    CHECK(platform->file_version(FileRoot::User, "saves/slot.bin").value_or(0) > 0);
    CHECK(platform->file_version(FileRoot::User, "missing.bin").value_or(1) == 0);
    CHECK_FALSE(platform->read_file(FileRoot::Assets, "missing.txt").has_value());
    ::unsetenv("DREAM_USER_DIR");
    std::filesystem::remove_all(folder);
}

TEST_CASE("The web page shows presented frames and turns its input into events") {
    const std::uint16_t port = test_port();
    std::unique_ptr<Platform> platform =
        make_platform("platform.web.port = " + std::to_string(port));
    REQUIRE(platform->create_window(WindowSettings{.title = "Web \"Test\"", .width = 2, .height = 1})
                .has_value());

    const std::string page = http_exchange(port, "GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK(page.starts_with("HTTP/1.1 200 OK"));
    CHECK(page.find("<canvas") != std::string::npos);

    const std::string status = http_exchange(port, "GET /status HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK(status.find(R"("title":"Web \"Test\"")") != std::string::npos);

    // The device presents through the canvas the window points at.
    const NativeWindow window = platform->native_window();
    const auto* const web = std::get_if<WebWindow>(&window);
    REQUIRE(web != nullptr);
    auto* const canvas = static_cast<WebCanvas*>(web->canvas);
    const std::array<std::byte, 8> pixels{std::byte{'r'}, std::byte{'g'}, std::byte{'b'},
                                          std::byte{'a'}, std::byte{'R'}, std::byte{'G'},
                                          std::byte{'B'}, std::byte{'A'}};
    canvas->present(SurfaceSize{2, 1}, pixels);
    const std::string frame =
        http_exchange(port, "GET /frame?after=0 HTTP/1.1\r\nConnection: close\r\n\r\n");
    CHECK(frame.find("X-Frame: 1\r\n") != std::string::npos);
    CHECK(frame.find("X-Width: 2\r\n") != std::string::npos);
    CHECK(frame.ends_with("\r\n\r\nrgbaRGBA"));

    const std::string body = "k KeyW 1\nm 1.5 0.5\nb 0 1\nw 0 -2\nt %C3%A9\nk Unknown 1";
    platform->set_text_input(true);
    const std::string posted = http_exchange(
        port, "POST /input HTTP/1.1\r\nConnection: close\r\nContent-Length: " +
                  std::to_string(body.size()) + "\r\n\r\n" + body);
    CHECK(posted.starts_with("HTTP/1.1 204"));

    const auto key = std::get<KeyChanged>(*platform->poll_event());
    CHECK(key.key == Key::W);
    CHECK(key.pressed);
    const auto moved = std::get<PointerMoved>(*platform->poll_event());
    CHECK(moved.x == doctest::Approx(1.5f));
    CHECK(std::get<PointerButtonChanged>(*platform->poll_event()).pressed);
    CHECK(std::get<PointerWheel>(*platform->poll_event()).y == doctest::Approx(-2.0f));
    CHECK(std::get<TextEntered>(*platform->poll_event()).text == "\xC3\xA9");
    CHECK_FALSE(platform->poll_event().has_value());

    http_exchange(port, "POST /quit HTTP/1.1\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
    CHECK(std::holds_alternative<QuitRequested>(*platform->poll_event()));
}
