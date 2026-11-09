#include "dream/runtime/input_actions.h"

#include <doctest/doctest.h>

#include <format>
#include <ostream> // Needed for doctest to print std::string_view.
#include <string>
#include <utility>

using namespace dream;
using namespace dream::platform;
using dream::runtime::InputActions;

namespace {

InputActions make_actions() {
    const auto config = core::Config::parse(R"(
[input.actions]
jump = ["key:space", "mouse:left", "touch"]
fire = ["key:f"]
)");
    REQUIRE(config.has_value());
    auto actions = InputActions::from_config(config.value());
    REQUIRE(actions.has_value());
    return std::move(actions).value();
}

KeyChanged press(Key key) {
    return KeyChanged{key, true};
}

KeyChanged release(Key key) {
    return KeyChanged{key, false};
}

core::Result<InputActions> parse_binding(const std::string& binding) {
    const auto config =
        core::Config::parse(std::format("[input.actions]\ntest = [\"{}\"]", binding));
    REQUIRE(config.has_value());
    return InputActions::from_config(config.value());
}

} // namespace

TEST_CASE("InputActions tracks a press across frames") {
    InputActions actions = make_actions();

    actions.begin_frame();
    actions.apply(press(Key::Space));
    CHECK(actions.is_down("jump"));
    CHECK(actions.was_pressed("jump"));
    CHECK_FALSE(actions.was_released("jump"));

    actions.begin_frame();
    CHECK(actions.is_down("jump"));
    CHECK_FALSE(actions.was_pressed("jump"));

    actions.begin_frame();
    actions.apply(release(Key::Space));
    CHECK_FALSE(actions.is_down("jump"));
    CHECK(actions.was_released("jump"));
}

TEST_CASE("InputActions reports a press and release within one frame") {
    InputActions actions = make_actions();

    actions.begin_frame();
    actions.apply(press(Key::Space));
    actions.apply(release(Key::Space));

    CHECK_FALSE(actions.is_down("jump"));
    CHECK(actions.was_pressed("jump"));
    CHECK(actions.was_released("jump"));
}

TEST_CASE("InputActions stays active while any binding is held") {
    InputActions actions = make_actions();

    actions.apply(press(Key::Space));
    actions.apply(PointerButtonChanged{PointerButton::Touch, true});
    actions.apply(release(Key::Space));
    CHECK(actions.is_down("jump"));

    actions.apply(PointerButtonChanged{PointerButton::Touch, false});
    CHECK_FALSE(actions.is_down("jump"));
}

TEST_CASE("InputActions keeps actions separate") {
    InputActions actions = make_actions();

    actions.apply(press(Key::F));

    CHECK(actions.is_down("fire"));
    CHECK_FALSE(actions.is_down("jump"));
}

TEST_CASE("InputActions ignores a repeated press") {
    InputActions actions = make_actions();

    actions.apply(press(Key::Space));
    actions.begin_frame();
    actions.apply(press(Key::Space));

    CHECK_FALSE(actions.was_pressed("jump"));
}

TEST_CASE("InputActions releases everything at once") {
    InputActions actions = make_actions();

    actions.apply(press(Key::Space));
    actions.apply(press(Key::F));
    actions.begin_frame();
    actions.release_all();

    CHECK_FALSE(actions.is_down("jump"));
    CHECK(actions.was_released("jump"));
    CHECK_FALSE(actions.is_down("fire"));
}

TEST_CASE("InputActions remembers where the pointer is") {
    InputActions actions = make_actions();

    actions.apply(PointerMoved{12.0f, 34.0f});

    CHECK(actions.pointer_position().x == doctest::Approx(12.0f));
    CHECK(actions.pointer_position().y == doctest::Approx(34.0f));
}

TEST_CASE("InputActions understands every form of key name") {
    const auto expect_binding = [](const char* binding, Key key) {
        auto parsed = parse_binding(binding);
        REQUIRE(parsed.has_value());
        InputActions actions = std::move(parsed).value();
        actions.apply(press(key));
        CHECK_MESSAGE(actions.is_down("test"), binding);
    };

    expect_binding("key:a", Key::A);
    expect_binding("key:z", Key::Z);
    expect_binding("key:0", Key::Digit0);
    expect_binding("key:9", Key::Digit9);
    expect_binding("key:f1", Key::F1);
    expect_binding("key:f9", Key::F9);
    expect_binding("key:f10", Key::F10);
    expect_binding("key:f12", Key::F12);
    expect_binding("key:left_shift", Key::LeftShift);
    expect_binding("key:back", Key::Back);
}

TEST_CASE("InputActions rejects unknown bindings") {
    for (const char* binding : {"key:", "key:nope", "key:f13", "key:f0", "key:A", "mouse:side",
                                "space", "gamepad:a"}) {
        CAPTURE(binding);
        const auto parsed = parse_binding(binding);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().message.find(binding) != std::string::npos);
    }
}

TEST_CASE("InputActions rejects an action that isn't a list") {
    const auto config = core::Config::parse("[input.actions]\njump = \"key:space\"");
    REQUIRE(config.has_value());

    const auto actions = InputActions::from_config(config.value());
    REQUIRE_FALSE(actions.has_value());
    CHECK(actions.error().message.find("jump") != std::string::npos);
}

TEST_CASE("InputActions works without any configured actions") {
    const auto config = core::Config::parse("");
    REQUIRE(config.has_value());
    CHECK(InputActions::from_config(config.value()).has_value());
}
