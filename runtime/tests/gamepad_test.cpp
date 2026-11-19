#include "dream/core/module_project.h"
#include "dream/input_device/input_device.h"
#include "dream/runtime/app.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using input_device::GamepadButton;
using input_device::GamepadState;

namespace {

// What the scripted gamepad reports, frame by frame, and what it was asked.
struct ScriptedPads {
    std::vector<std::vector<GamepadState>> frames;
    std::size_t updates = 0;
    std::vector<std::uint32_t> rumbled;
};

ScriptedPads* g_pads = nullptr;

class ScriptedInputDevice final : public input_device::InputDevice {
public:
    void update() override {
        m_current = g_pads->updates < g_pads->frames.size() ? g_pads->frames[g_pads->updates]
                                                            : std::vector<GamepadState>{};
        ++g_pads->updates;
    }
    std::span<const GamepadState> gamepads() const override {
        return m_current;
    }
    bool rumble(std::uint32_t id, float, float, std::uint32_t) override {
        g_pads->rumbled.push_back(id);
        return true;
    }

private:
    std::vector<GamepadState> m_current;
};

core::Result<void> register_scripted(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<input_device::InputDevice>{
        .name = "scripted",
        .create = [](const core::Config&) -> core::Result<std::unique_ptr<input_device::InputDevice>> {
            return std::unique_ptr<input_device::InputDevice>{
                std::make_unique<ScriptedInputDevice>()};
        },
    });
}

constexpr std::string_view k_project = R"(
[app]
max_fps = 0

[input.actions]
jump = ["key:space", "gamepad:south"]

[modules.test]
input_device = ["scripted"]
)";

GamepadState pressing_south(std::uint32_t id) {
    GamepadState pad{.id = id};
    pad.buttons[static_cast<std::size_t>(GamepadButton::South)] = true;
    return pad;
}

// Records, each frame, whether jump is down and how many gamepads there are, and quits after the
// script.
class PadGame final : public Game {
public:
    void on_update(App& app, const core::FrameTime&) override {
        jumps.push_back(app.input().is_down("jump"));
        pads.push_back(app.gamepads().size());
        if (!app.gamepads().empty()) {
            app.rumble(app.gamepads()[0].id, 1.0f, 1.0f, 50);
        }
        if (jumps.size() == 3) {
            app.request_quit();
        }
    }
    std::vector<bool> jumps;
    std::vector<std::size_t> pads;
};

} // namespace

TEST_CASE("The App reads the project's gamepads every frame and feeds them to the actions") {
    ScriptedPads script;
    script.frames = {{}, {pressing_south(7)}, {}};
    g_pads = &script;

    core::Result<core::ModuleProject> project = core::ModuleProject::load(
        core::ModuleProjectDefinition{"test", k_project, &register_scripted});
    REQUIRE(project.has_value());
    FakeState platform;
    // The fake platform asks to quit when its script ends, so it runs more frames than the game.
    platform.script = {k_end_of_frame, k_end_of_frame, k_end_of_frame, k_end_of_frame,
                       k_end_of_frame};
    App app(std::make_unique<FakePlatform>(platform), project.value().config(), nullptr,
            &project.value());
    PadGame game;
    REQUIRE(app.run(game).has_value());

    CHECK(script.updates == 3);
    CHECK(game.jumps == std::vector<bool>{false, true, false});
    CHECK(game.pads == std::vector<std::size_t>{0, 1, 0});
    CHECK(script.rumbled == std::vector<std::uint32_t>{7});
    g_pads = nullptr;
}

TEST_CASE("Without an input_device module, the App has no gamepads") {
    const core::Config config = core::Config::parse("[app]\nmax_fps = 0\n").value();
    FakeState platform;
    App app(std::make_unique<FakePlatform>(platform), config);
    CHECK(app.gamepads().empty());
    CHECK_FALSE(app.rumble(1, 1.0f, 1.0f, 10));
}
