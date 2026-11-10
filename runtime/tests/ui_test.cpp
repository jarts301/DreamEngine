#include "dream/runtime/ui.h"

#include "dream/runtime/app.h"
#include "dream/runtime/renderer.h"
#include "fake_device.h"
#include "fake_platform.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;

namespace {

using Calls = std::vector<std::string>;

const core::Config k_config = core::Config::parse("[app]\nmax_fps = 0").value();

// A renderer inside an App that is never run, so frames are recorded by hand.
struct UiFixture {
    FakeState platform;
    FakeDeviceState device;
    App app;
    Renderer renderer;

    UiFixture()
        : app(std::make_unique<FakePlatform>(platform), k_config,
              std::make_unique<FakeDevice>(device)) {
        for (const char* name : {"forward", "debug_lines", "ui"}) {
            platform.assets[std::format("shaders/{}.vert.fake", name)] = std::vector<std::byte>(4);
            platform.assets[std::format("shaders/{}.frag.fake", name)] = std::vector<std::byte>(4);
        }
        REQUIRE(renderer.initialize(app).has_value());
    }

    // Records one frame of an empty world and returns the device calls it made.
    Calls frame() {
        device.calls.clear();
        rhi::CommandList* const commands = app.device()->begin_frame().value();
        REQUIRE(commands != nullptr);
        const World world;
        renderer.render(world, *commands, platform::SurfaceSize{800, 600});
        REQUIRE(app.device()->end_frame().has_value());
        return device.calls;
    }
};

// One quad per command.
ui::DrawList quads(std::vector<ui::DrawCommand> commands, bool premultiplied = false) {
    ui::DrawList list;
    list.premultiplied_alpha = premultiplied;
    for (ui::DrawCommand& command : commands) {
        const auto base = static_cast<std::uint32_t>(list.vertices.size());
        list.vertices.resize(list.vertices.size() + 4);
        command.first_index = static_cast<std::uint32_t>(list.indices.size());
        command.index_count = 6;
        for (const std::uint32_t index : {0u, 1u, 2u, 2u, 1u, 3u}) {
            list.indices.push_back(base + index);
        }
        list.commands.push_back(command);
    }
    return list;
}

bool has(const Calls& calls, std::string_view call) {
    return std::find(calls.begin(), calls.end(), call) != calls.end();
}

class FakeDebugUi final : public debug_ui::DebugUi {
public:
    core::Result<void> initialize(ui::Host& given) override {
        host = &given;
        return {};
    }
    bool handle_event(const platform::Event& event) override {
        events.push_back(event);
        return true;
    }
    void begin_frame(const ui::FrameInfo& info) override {
        frame = info;
    }
    bool begin_window(std::string_view, bool*) override {
        return true;
    }
    void end_window() override {}
    void text(std::string_view) override {}
    bool button(std::string_view) override {
        return false;
    }
    bool checkbox(std::string_view, bool&) override {
        return false;
    }
    bool slider(std::string_view, float&, float, float) override {
        return false;
    }
    bool slider(std::string_view, int&, int, int) override {
        return false;
    }
    void plot(std::string_view, std::span<const float>, float, float) override {}
    void separator() override {}
    void same_line() override {}
    const ui::DrawList& end_frame() override {
        return list;
    }
    bool wants_pointer() const override {
        return true;
    }
    bool wants_keyboard() const override {
        return false;
    }

    ui::Host* host = nullptr;
    std::vector<platform::Event> events;
    ui::FrameInfo frame;
    ui::DrawList list = quads({ui::DrawCommand{}});
};

class FakeGameUi final : public ui::GameUi {
public:
    core::Result<void> initialize(ui::Host& given) override {
        host = &given;
        return {};
    }
    core::Result<void> load_font(std::string_view) override {
        return {};
    }
    core::Result<ui::DocumentHandle> load_document(std::string_view) override {
        return ui::DocumentHandle{.index = 0, .generation = 1};
    }
    void show(ui::DocumentHandle) override {}
    void hide(ui::DocumentHandle) override {}
    void close(ui::DocumentHandle) override {}
    bool is_visible(ui::DocumentHandle) const override {
        return true;
    }
    core::Result<void> set_text(ui::DocumentHandle, std::string_view, std::string_view) override {
        return {};
    }
    core::Result<void> set_class(ui::DocumentHandle, std::string_view, std::string_view,
                                 bool) override {
        return {};
    }
    std::optional<std::string> value(ui::DocumentHandle, std::string_view) const override {
        return std::nullopt;
    }
    bool handle_event(const platform::Event& event) override {
        ++events_seen;
        if (const auto* moved = std::get_if<platform::PointerMoved>(&event)) {
            pointer = *moved;
        }
        return false;
    }
    void update(const ui::FrameInfo& info) override {
        frame = info;
        ++updates;
    }
    std::span<const ui::UiEvent> events() const override {
        return pending;
    }
    bool wants_pointer() const override {
        return false;
    }
    bool wants_text() const override {
        return typing;
    }
    const ui::DrawList& render() override {
        return list;
    }

    ui::Host* host = nullptr;
    ui::FrameInfo frame;
    platform::PointerMoved pointer;
    int events_seen = 0;
    int updates = 0;
    bool typing = false;
    std::vector<ui::UiEvent> pending;
    ui::DrawList list = quads({ui::DrawCommand{}}, true);
};

} // namespace

TEST_CASE_FIXTURE(UiFixture, "Renderer draws interface layers with their textures and clips") {
    const std::vector<std::byte> pixels(2 * 2 * 4);
    const ui::TextureId texture =
        renderer.ui_host().create_texture(2, 2, pixels, ui::TextureKind::Coverage).value();
    CHECK(texture != 0);

    renderer.set_ui_layer(UiLayer::Game,
                          quads({ui::DrawCommand{.texture = texture,
                                                 .clip = ui::ClipRect{10, 20, 30, 40}},
                                 ui::DrawCommand{}}));
    const Calls calls = frame();
    CHECK(renderer.last_ui_command_count() == 2);
    CHECK(has(calls, std::format("geometry {} 12", 8 * sizeof(ui::Vertex))));
    CHECK(has(calls, "scissor 10 20 30 40"));
    CHECK(has(calls, "draw 0+6"));
    CHECK(has(calls, "draw 6+6"));
    const auto last_scissor = std::find_if(calls.rbegin(), calls.rend(), [](const std::string& call) {
        return call.starts_with("scissor");
    });
    REQUIRE(last_scissor != calls.rend());
    CHECK(*last_scissor == "scissor off");

    // Layers stay until replaced or cleared.
    CHECK(frame() == calls);
    renderer.clear_ui_layer(UiLayer::Game);
    frame();
    CHECK(renderer.last_ui_command_count() == 0);
}

TEST_CASE_FIXTURE(UiFixture, "Renderer releases interface textures only when asked") {
    const std::vector<std::byte> pixels(4);
    const ui::TextureId texture =
        renderer.ui_host().create_texture(1, 1, pixels, ui::TextureKind::Color).value();
    renderer.set_ui_layer(UiLayer::Game, quads({ui::DrawCommand{.texture = texture}}));
    const Calls before = frame();

    renderer.ui_host().release_texture(texture);
    CHECK(frame() == before);
    renderer.release_ui_textures();
    CHECK(has(frame(), "bind_texture 0"));
}

TEST_CASE_FIXTURE(UiFixture, "Renderer reports interface files and images that can't be read") {
    CHECK_FALSE(renderer.ui_host().read_file("assets:/missing.rml").has_value());
    CHECK_FALSE(renderer.ui_host().load_texture("assets:/missing.png", true).has_value());
}

TEST_CASE_FIXTURE(UiFixture, "DebugOverlay passes input on and draws over the game") {
    auto backend = std::make_unique<FakeDebugUi>();
    FakeDebugUi& tools = *backend;
    {
        DebugOverlay overlay;
        REQUIRE(overlay.initialize(app, renderer, std::move(backend)).has_value());
        CHECK(tools.host == &renderer.ui_host());

        renderer.set_ui_layer(UiLayer::Game, quads({ui::DrawCommand{}}));
        platform.display_scale = 2.0f;
        overlay.begin_frame(app, 0.5f).text("hello");
        overlay.end_frame();
        CHECK(tools.frame.display_scale == doctest::Approx(2.0f));
        CHECK(tools.frame.delta_seconds == doctest::Approx(0.5f));
        frame();
        CHECK(renderer.last_ui_command_count() == 2);

        app.events().publish(platform::PointerMoved{1.0f, 2.0f});
        app.events().publish(platform::QuitRequested{});
        CHECK(tools.events.size() == 1);
        CHECK(overlay.wants_pointer());

        overlay.set_visible(false);
        app.events().publish(platform::PointerMoved{1.0f, 2.0f});
        CHECK(tools.events.size() == 1);
        CHECK_FALSE(overlay.wants_pointer());
        overlay.begin_frame(app, 0.5f);
        overlay.end_frame();
        frame();
        CHECK(renderer.last_ui_command_count() == 1);
    }
    // Gone with the overlay.
    renderer.clear_ui_layer(UiLayer::Game);
    frame();
    CHECK(renderer.last_ui_command_count() == 0);
}

TEST_CASE_FIXTURE(UiFixture, "UserInterface publishes events and turns text input on and off") {
    auto backend = std::make_unique<FakeGameUi>();
    FakeGameUi& game_ui = *backend;
    UserInterface interface;
    REQUIRE(interface.initialize(app, renderer, std::move(backend)).has_value());

    std::vector<std::string> heard;
    const Subscription subscription = app.events().subscribe<UiEventPublished>(
        [&](const UiEventPublished& published) { heard.push_back(published.event.element_id); });

    game_ui.pending.push_back(ui::UiEvent{.element_id = "play"});
    game_ui.typing = true;
    interface.update(app, 0.016f);
    CHECK(heard == std::vector<std::string>{"play"});
    CHECK(platform.text_input);
    CHECK(game_ui.updates == 1);

    game_ui.typing = false;
    interface.update(app, 0.016f);
    CHECK_FALSE(platform.text_input);

    app.events().publish(platform::KeyChanged{platform::Key::A, true});
    CHECK(game_ui.events_seen == 1);

    frame();
    CHECK(renderer.last_ui_command_count() == 1);
}

TEST_CASE_FIXTURE(UiFixture, "Interfaces are laid out inside the safe area") {
    platform.safe_area = platform::SurfaceRect{20, 100, 760, 450};
    auto backend = std::make_unique<FakeGameUi>();
    FakeGameUi& game_ui = *backend;
    game_ui.list = quads({ui::DrawCommand{.clip = ui::ClipRect{0, 0, 50, 60}}}, true);

    UserInterface interface;
    REQUIRE(interface.initialize(app, renderer, std::move(backend)).has_value());
    interface.update(app, 0.016f);
    CHECK(game_ui.frame.size == platform::SurfaceSize{760, 450});

    // Pointer positions are given relative to the safe area.
    app.events().publish(platform::PointerMoved{30.0f, 110.0f});
    CHECK(game_ui.pointer.x == doctest::Approx(10.0f));
    CHECK(game_ui.pointer.y == doctest::Approx(10.0f));

    // The scissor moves with the layer.
    CHECK(has(frame(), "scissor 20 100 50 60"));
}
