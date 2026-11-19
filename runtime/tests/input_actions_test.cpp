#include "dream/runtime/input_actions.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <format>
#include <ostream> // Needed for doctest to print std::string_view.
#include <span>
#include <string>
#include <string_view>
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
                                "space", "gamepad:z", "joystick:a"}) {
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

namespace {

using input_device::GamepadAxis;
using input_device::GamepadButton;
using input_device::GamepadState;

InputActions gamepad_actions(std::string_view extra = "") {
    const auto config = core::Config::parse(std::format(R"({}
[input.actions]
jump = ["key:space", "gamepad:south"]
left = ["key:a", "gamepad:left_stick_left", "gamepad:dpad_left"]
right = ["key:d", "gamepad:left_stick_right"]
up = ["gamepad:left_stick_up"]
brake = ["gamepad:left_trigger"]
)",
                                                        extra));
    REQUIRE(config.has_value());
    auto actions = InputActions::from_config(config.value());
    REQUIRE_MESSAGE(actions.has_value(), actions.error().message);
    return std::move(actions).value();
}

GamepadState pad_with(GamepadButton button) {
    GamepadState pad;
    pad.buttons[static_cast<std::size_t>(button)] = true;
    return pad;
}

GamepadState pad_at(float x, float y) {
    GamepadState pad;
    pad.axes[static_cast<std::size_t>(GamepadAxis::LeftX)] = x;
    pad.axes[static_cast<std::size_t>(GamepadAxis::LeftY)] = y;
    return pad;
}

} // namespace

TEST_CASE("InputActions presses and releases gamepad buttons") {
    InputActions actions = gamepad_actions();
    const std::array pressed{pad_with(GamepadButton::South)};
    actions.begin_frame();
    actions.apply(pressed);
    CHECK(actions.was_pressed("jump"));
    CHECK(actions.is_down("jump"));
    CHECK(actions.value("jump") == 1.0f);
    CHECK(actions.last_source() == InputActions::Source::Gamepad);

    actions.begin_frame();
    actions.apply(pressed);
    CHECK_FALSE(actions.was_pressed("jump"));
    CHECK(actions.is_down("jump"));

    actions.begin_frame();
    actions.apply(std::span<const GamepadState>{});
    CHECK(actions.was_released("jump"));
    CHECK_FALSE(actions.is_down("jump"));

    actions.apply(press(Key::Space));
    CHECK(actions.last_source() == InputActions::Source::KeyboardMouse);
}

TEST_CASE("InputActions reads sticks past a round deadzone, as values from 0 to 1") {
    InputActions actions = gamepad_actions();

    // Inside the default deadzone of 0.2: nothing.
    actions.apply(std::array{pad_at(0.15f, 0.1f)});
    CHECK(actions.value("right") == 0.0f);
    CHECK(actions.value("up") == 0.0f);

    // Halfway past the deadzone: 0.5, and down.
    actions.begin_frame();
    actions.apply(std::array{pad_at(0.6f, 0.0f)});
    CHECK(actions.value("right") == doctest::Approx(0.5f));
    CHECK(actions.value("left") == 0.0f);
    CHECK(actions.is_down("right"));
    CHECK(actions.axis("left", "right") == doctest::Approx(0.5f));

    // Back a little, to 0.45: still down, so a stick resting near halfway doesn't flicker.
    actions.begin_frame();
    actions.apply(std::array{pad_at(0.56f, 0.0f)});
    CHECK(actions.is_down("right"));
    CHECK_FALSE(actions.was_released("right"));

    // Fully left and up, diagonally: both directions, and never past 1.
    actions.begin_frame();
    actions.apply(std::array{pad_at(-1.0f, 1.0f)});
    CHECK(actions.was_released("right"));
    CHECK(actions.value("left") == doctest::Approx(0.7071f).epsilon(0.01));
    CHECK(actions.value("up") == doctest::Approx(0.7071f).epsilon(0.01));
}

TEST_CASE("InputActions combines every gamepad and binding") {
    InputActions actions = gamepad_actions();
    GamepadState braking;
    braking.axes[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] = 0.55f;
    // One pad on the d-pad, another braking: both count.
    actions.apply(std::array{pad_with(GamepadButton::DpadLeft), braking});
    CHECK(actions.value("left") == 1.0f);
    CHECK(actions.value("brake") == doctest::Approx(0.5f));
}

TEST_CASE("InputActions takes its deadzones from the configuration") {
    InputActions actions = gamepad_actions("[input]\nstick_deadzone = 0.5\ntrigger_deadzone = 0.0");
    actions.apply(std::array{pad_at(0.4f, 0.0f)});
    CHECK(actions.value("right") == 0.0f);

    const auto bad = core::Config::parse("[input]\nstick_deadzone = 1.5\n[input.actions]\n");
    REQUIRE(bad.has_value());
    CHECK_FALSE(InputActions::from_config(bad.value()).has_value());
}

TEST_CASE("InputActions understands every gamepad binding") {
    for (const char* binding :
         {"gamepad:south", "gamepad:east", "gamepad:west", "gamepad:north", "gamepad:a",
          "gamepad:b", "gamepad:x", "gamepad:y", "gamepad:back", "gamepad:guide", "gamepad:start",
          "gamepad:left_stick_click", "gamepad:right_stick_click", "gamepad:left_shoulder",
          "gamepad:right_shoulder", "gamepad:dpad_up", "gamepad:dpad_down", "gamepad:dpad_left",
          "gamepad:dpad_right", "gamepad:left_trigger", "gamepad:right_trigger",
          "gamepad:left_stick_up", "gamepad:left_stick_down", "gamepad:left_stick_left",
          "gamepad:left_stick_right", "gamepad:right_stick_up", "gamepad:right_stick_down",
          "gamepad:right_stick_left", "gamepad:right_stick_right"}) {
        CHECK_MESSAGE(parse_binding(binding).has_value(), binding);
    }
    CHECK_FALSE(parse_binding("gamepad:z").has_value());
    CHECK_FALSE(parse_binding("gamepad:left_stick").has_value());
}

TEST_CASE("InputActions releases gamepad inputs at once, too") {
    InputActions actions = gamepad_actions();
    actions.apply(std::array{pad_at(1.0f, 0.0f)});
    REQUIRE(actions.is_down("right"));
    actions.begin_frame();
    actions.release_all();
    CHECK(actions.was_released("right"));
    CHECK(actions.value("right") == 0.0f);
}
