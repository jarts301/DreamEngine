#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/ui/game_ui.h"
#include "tests/fake_host.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_ui_markup(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::ui;

namespace {

core::Result<std::unique_ptr<GameUi>> try_make() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_ui_markup(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"markup"};
    auto created = registry.create<GameUi>(preference, config);
    if (!created.has_value()) {
        return created.error();
    }
    return std::move(created.value().instance);
}

// A menu with a button at a known place, a slider and a text field.
struct Fixture {
    tests::FakeHost host;
    std::unique_ptr<GameUi> ui;
    DocumentHandle menu;

    Fixture() {
        // A phone running the tests can point at its own copy of the font.
#if defined(_WIN32)
        const char* const font = nullptr;
#else
        const char* const font = std::getenv("DREAM_TEST_FONT");
#endif
        host.add_disk_file("assets:/fonts/lato.ttf", font != nullptr ? font : DREAM_TEST_FONT);
        host.files["assets:/ui/menu.rml"] = R"(<rml>
<head>
  <link type="text/rcss" href="../styles/menu.rcss"/>
  <title>Menu</title>
</head>
<body>
  <div id="score">0</div>
  <button id="play">Play</button>
  <input id="volume" type="range" min="0" max="1" step="0.1" value="0.5"/>
  <input id="name" type="text" value="Ann"/>
</body>
</rml>)";
        host.files["assets:/styles/menu.rcss"] = R"(
body { font-family: LatoLatin; font-size: 20px; color: #ffffff; width: 100%; height: 100%; }
#score { position: absolute; left: 10px; top: 10px; }
#play { display: block; position: absolute; left: 100px; top: 100px; width: 200px; height: 50px;
        background-color: #336699; }
#volume { position: absolute; left: 100px; top: 200px; width: 200px; height: 20px; }
#name { position: absolute; left: 100px; top: 300px; width: 200px; height: 30px; }
)";
        core::Result<std::unique_ptr<GameUi>> made = try_make();
        REQUIRE(made.has_value());
        ui = std::move(made).value();
        REQUIRE(ui->initialize(host).has_value());
        REQUIRE(ui->load_font("assets:/fonts/lato.ttf").has_value());
        core::Result<DocumentHandle> loaded = ui->load_document("assets:/ui/menu.rml");
        REQUIRE(loaded.has_value());
        menu = loaded.value();
    }

    void frame() {
        ui->update(FrameInfo{.size = {800, 600}, .delta_seconds = 1.0f / 60.0f});
    }

    void click(float x, float y) {
        ui->handle_event(platform::PointerMoved{x, y});
        ui->handle_event(platform::PointerButtonChanged{platform::PointerButton::Left, true});
        ui->handle_event(platform::PointerButtonChanged{platform::PointerButton::Left, false});
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "markup draws a shown document with its font") {
    frame();
    CHECK(ui->render().commands.empty());
    CHECK_FALSE(ui->is_visible(menu));

    ui->show(menu);
    frame();
    const DrawList& list = ui->render();
    CHECK(list.premultiplied_alpha);
    REQUIRE_FALSE(list.commands.empty());
    CHECK_FALSE(host.live.empty());
    for (const DrawCommand& command : list.commands) {
        CHECK(command.first_index + command.index_count <= list.indices.size());
    }
    for (const std::uint32_t index : list.indices) {
        CHECK(index < list.vertices.size());
    }

    ui->hide(menu);
    frame();
    CHECK(ui->render().commands.empty());
}

TEST_CASE_FIXTURE(Fixture, "markup reports clicks and changes by element id") {
    ui->show(menu);
    frame();

    click(150.0f, 120.0f);
    CHECK(ui->wants_pointer());
    frame();
    REQUIRE(ui->events().size() == 1);
    CHECK(ui->events()[0].kind == UiEvent::Kind::Click);
    CHECK(ui->events()[0].element_id == "play");
    CHECK(ui->events()[0].document == menu);

    // Events are only kept until the next update.
    frame();
    CHECK(ui->events().empty());

    // Clicking empty space reaches the body, which has no id.
    ui->handle_event(platform::PointerMoved{700.0f, 500.0f});
    CHECK_FALSE(ui->wants_pointer());
    click(700.0f, 500.0f);
    frame();
    CHECK(ui->events().empty());
}

TEST_CASE_FIXTURE(Fixture, "markup changes text, classes and reads values") {
    ui->show(menu);
    CHECK(ui->set_text(menu, "score", "42 <points>").has_value());
    CHECK(ui->set_class(menu, "play", "highlighted", true).has_value());
    CHECK_FALSE(ui->set_text(menu, "missing", "x").has_value());
    CHECK(std::stof(ui->value(menu, "volume").value()) == doctest::Approx(0.5f));
    CHECK(ui->value(menu, "name") == "Ann");
    CHECK_FALSE(ui->value(menu, "missing").has_value());
    frame();

    // Typing goes into a focused text field.
    click(150.0f, 315.0f);
    frame();
    CHECK(ui->wants_text());
    ui->handle_event(platform::KeyChanged{platform::Key::End, true});
    ui->handle_event(platform::TextEntered{"e"});
    frame();
    CHECK(ui->value(menu, "name") == "Anne");
}

TEST_CASE_FIXTURE(Fixture, "markup allows several interfaces and refuses missing files") {
    core::Result<std::unique_ptr<GameUi>> second = try_make();
    REQUIRE(second.has_value());
    tests::FakeHost other_host;
    CHECK(second.value()->initialize(other_host).has_value());
    CHECK_FALSE(ui->load_document("assets:/ui/missing.rml").has_value());
    CHECK_FALSE(ui->load_font("assets:/fonts/missing.ttf").has_value());

    ui->close(menu);
    ui->close(menu);
    CHECK_FALSE(ui->is_visible(menu));
    CHECK_FALSE(ui->set_text(menu, "score", "1").has_value());
}

TEST_CASE_FIXTURE(Fixture, "markup reports slider and checkbox changes") {
    ui->show(menu);
    frame();
    // The slider spans x = 100 to 300; without styles for its parts, its bar is as wide as it is
    // tall, 20 pixels.
    click(290.0f, 210.0f);
    frame();
    bool changed = false;
    for (const UiEvent& event : ui->events()) {
        changed = changed || (event.kind == UiEvent::Kind::Change && event.element_id == "volume");
    }
    CHECK(changed);
    CHECK(std::stof(ui->value(menu, "volume").value()) == doctest::Approx(1.0f));
}

TEST_CASE_FIXTURE(Fixture, "markup takes taps as clicks") {
    ui->show(menu);
    frame();
    ui->handle_event(platform::PointerMoved{150.0f, 120.0f});
    ui->handle_event(platform::PointerButtonChanged{platform::PointerButton::Touch, true});
    ui->handle_event(platform::PointerButtonChanged{platform::PointerButton::Touch, false});
    frame();
    REQUIRE(ui->events().size() == 1);
    CHECK(ui->events()[0].element_id == "play");
}
