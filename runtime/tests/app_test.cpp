#include "dream/runtime/app.h"

#include "fake_device.h"
#include "fake_platform.h"

#include <doctest/doctest.h>

#include <functional>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using namespace dream::platform;

namespace {

const core::Config k_config = core::Config::parse(R"(
[app]
max_fps = 0

[input.actions]
jump = ["key:space"]
)")
                                  .value();

using Log = std::vector<std::string>;

class RecordingGame final : public Game {
public:
    core::Result<void> on_start(App&) override {
        log.emplace_back("start");
        return start_result;
    }

    void on_update(App& app, const core::FrameTime&) override {
        log.emplace_back("update");
        if (on_update_hook) {
            on_update_hook(app);
        }
    }

    void on_render(App&, rhi::CommandList&) override {
        log.emplace_back("render");
    }

    void on_stop(App&) override {
        log.emplace_back("stop");
    }
    void on_pause(App&) override {
        log.emplace_back("pause");
    }
    void on_resume(App&) override {
        log.emplace_back("resume");
    }
    void on_surface_lost(App&) override {
        log.emplace_back("surface_lost");
    }
    void on_surface_restored(App&) override {
        log.emplace_back("surface_restored");
    }
    void on_surface_resized(App&, SurfaceSize size) override {
        log.push_back(std::format("resized {}x{}", size.width, size.height));
    }
    void on_low_memory(App&) override {
        log.emplace_back("low_memory");
    }

    Log log;
    core::Result<void> start_result;
    std::function<void(App&)> on_update_hook;
};

struct Run {
    FakeState state;
    RecordingGame game;

    core::Result<void> go(std::deque<Step> script, const core::Config& config = k_config) {
        state.script = std::move(script);
        App app(std::make_unique<FakePlatform>(state), config);
        return app.run(game);
    }
};

struct RenderRun {
    FakeState state;
    FakeDeviceState device;
    RecordingGame game;

    core::Result<void> go(std::deque<Step> script) {
        state.script = std::move(script);
        App app(std::make_unique<FakePlatform>(state), k_config,
                std::make_unique<FakeDevice>(device));
        return app.run(game);
    }
};

} // namespace

TEST_CASE("App updates once per frame until asked to quit") {
    Run run;
    REQUIRE(run.go({k_end_of_frame, k_end_of_frame}).has_value());
    CHECK(run.game.log == Log{"start", "update", "update", "stop"});
}

TEST_CASE("App stops updating while paused and sleeps until the next event") {
    Run run;
    REQUIRE(run.go({k_end_of_frame, AppPaused{}, k_end_of_frame, AppResumed{}, k_end_of_frame})
                .has_value());

    CHECK(run.game.log == Log{"start", "update", "pause", "resume", "update", "stop"});
    CHECK(run.state.wait_calls == 1);
}

TEST_CASE("App waits for both the surface and resume, in either order") {
    Run run;

    SUBCASE("the surface returns first") {
        REQUIRE(run.go({AppPaused{}, SurfaceLost{}, k_end_of_frame, SurfaceRestored{},
                        k_end_of_frame, AppResumed{}, k_end_of_frame})
                    .has_value());
        CHECK(run.game.log == Log{"start", "pause", "surface_lost", "surface_restored", "resume",
                                  "update", "stop"});
    }
    SUBCASE("resume arrives first") {
        REQUIRE(run.go({AppPaused{}, SurfaceLost{}, k_end_of_frame, AppResumed{}, k_end_of_frame,
                        SurfaceRestored{}, k_end_of_frame})
                    .has_value());
        CHECK(run.game.log == Log{"start", "pause", "surface_lost", "resume", "surface_restored",
                                  "update", "stop"});
    }
}

TEST_CASE("App ignores lifecycle events that change nothing") {
    Run run;
    REQUIRE(run.go({AppResumed{}, SurfaceRestored{}, AppPaused{}, AppPaused{}, SurfaceLost{},
                    SurfaceLost{}, k_end_of_frame, AppResumed{}, AppResumed{}, SurfaceRestored{},
                    k_end_of_frame})
                .has_value());

    CHECK(run.game.log == Log{"start", "pause", "surface_lost", "resume", "surface_restored",
                              "update", "stop"});
}

TEST_CASE("App can be closed while paused") {
    Run run;
    REQUIRE(run.go({AppPaused{}, k_end_of_frame, QuitRequested{}}).has_value());
    CHECK(run.game.log == Log{"start", "pause", "stop"});
}

TEST_CASE("App ends after the frame in which the game asks to quit") {
    Run run;
    run.game.on_update_hook = [](App& app) { app.request_quit(); };

    REQUIRE(run.go({k_end_of_frame, k_end_of_frame, k_end_of_frame}).has_value());
    CHECK(run.game.log == Log{"start", "update", "stop"});
}

TEST_CASE("App reports a failed start without running the game") {
    Run run;
    run.game.start_result = core::Error{core::ErrorCode::Unknown, "no save data"};

    const core::Result<void> result = run.go({k_end_of_frame});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "no save data");
    CHECK(run.game.log == Log{"start"});
}

TEST_CASE("App reports resizes and tracks the surface size") {
    Run run;
    SurfaceSize seen;
    run.game.on_update_hook = [&](App& app) { seen = app.surface_size(); };

    REQUIRE(run.go({SurfaceResized{{800, 600}}, SurfaceResized{{800, 600}}, k_end_of_frame})
                .has_value());

    CHECK(run.game.log == Log{"start", "resized 800x600", "update", "stop"});
    CHECK(seen == SurfaceSize{800, 600});
}

TEST_CASE("App re-reads the surface size when the surface returns") {
    Run run;
    SurfaceSize seen;
    run.game.on_update_hook = [&](App& app) { seen = app.surface_size(); };
    run.state.size = SurfaceSize{1080, 2400};

    REQUIRE(run.go({SurfaceLost{}, k_end_of_frame, SurfaceRestored{}, k_end_of_frame}).has_value());
    CHECK(seen == SurfaceSize{1080, 2400});
}

TEST_CASE("App forwards low memory warnings") {
    Run run;
    REQUIRE(run.go({LowMemory{}, k_end_of_frame}).has_value());
    CHECK(run.game.log == Log{"start", "low_memory", "update", "stop"});
}

TEST_CASE("App feeds input events to the actions") {
    Run run;
    std::vector<bool> down;
    run.game.on_update_hook = [&](App& app) { down.push_back(app.input().is_down("jump")); };

    REQUIRE(run.go({KeyChanged{Key::Space, true}, k_end_of_frame, k_end_of_frame,
                    KeyChanged{Key::Space, false}, k_end_of_frame})
                .has_value());

    CHECK(down == std::vector<bool>{true, true, false});
}

TEST_CASE("App releases held input when paused") {
    Run run;
    std::vector<bool> down;
    run.game.on_update_hook = [&](App& app) { down.push_back(app.input().is_down("jump")); };

    REQUIRE(run.go({KeyChanged{Key::Space, true}, k_end_of_frame, AppPaused{}, k_end_of_frame,
                    AppResumed{}, k_end_of_frame})
                .has_value());

    CHECK(down == std::vector<bool>{true, false});
}

TEST_CASE("App creates the window from the configuration") {
    Run run;

    SUBCASE("configured values") {
        const auto config = core::Config::parse(R"(
[app]
title = "Rocks"
width = 800
height = 600
max_fps = 0
)");
        REQUIRE(run.go({}, config.value()).has_value());
        REQUIRE(run.state.window.has_value());
        CHECK(run.state.window->title == "Rocks");
        CHECK(run.state.window->width == 800);
        CHECK(run.state.window->height == 600);
    }
    SUBCASE("default values") {
        REQUIRE(run.go({}).has_value());
        REQUIRE(run.state.window.has_value());
        CHECK(run.state.window->title == "Dream Engine");
        CHECK(run.state.window->width == 1280);
        CHECK(run.state.window->height == 720);
    }
}

TEST_CASE("App rejects invalid settings before starting the game") {
    Run run;
    const char* text = "";

    SUBCASE("zero width") {
        text = "[app]\nwidth = 0";
    }
    SUBCASE("negative height") {
        text = "[app]\nheight = -1";
    }
    SUBCASE("negative frame rate") {
        text = "[app]\nmax_fps = -30";
    }
    SUBCASE("unknown binding") {
        text = "[input.actions]\njump = [\"key:nope\"]";
    }

    const auto config = core::Config::parse(text);
    REQUIRE(config.has_value());

    CHECK_FALSE(run.go({}, config.value()).has_value());
    CHECK(run.game.log.empty());
    CHECK_FALSE(run.state.window.has_value());
}

TEST_CASE("App attaches the surface before starting and renders after updating") {
    RenderRun run;
    REQUIRE(run.go({k_end_of_frame}).has_value());

    CHECK(run.device.calls == Log{"attach 640x480", "begin_frame", "end_frame"});
    CHECK(run.game.log == Log{"start", "update", "render", "stop"});
}

TEST_CASE("App gives up the surface in the background and takes the new one on return") {
    RenderRun run;
    REQUIRE(run.go({AppPaused{}, SurfaceLost{}, k_end_of_frame, SurfaceRestored{}, AppResumed{},
                    k_end_of_frame})
                .has_value());

    CHECK(run.device.calls == Log{"attach 640x480", "detach", "attach 640x480", "begin_frame",
                                  "end_frame"});
    CHECK(run.game.log == Log{"start", "pause", "surface_lost", "surface_restored", "resume",
                              "update", "render", "stop"});
}

TEST_CASE("App passes the current native window to the device") {
    RenderRun run;
    run.state.native_window = AndroidWindow{&run};

    REQUIRE(run.go({}).has_value());

    const auto* const window = std::get_if<AndroidWindow>(&run.device.last_window);
    REQUIRE(window != nullptr);
    CHECK(window->window == &run);
}

TEST_CASE("App fails if the first surface can't be attached") {
    RenderRun run;
    run.device.attach_failures_left = 1;

    const core::Result<void> result = run.go({k_end_of_frame});

    REQUIRE_FALSE(result.has_value());
    CHECK(run.game.log.empty());
}

TEST_CASE("App keeps retrying a surface that isn't ready after returning") {
    // Fails only attaches made after the game has started, so startup itself succeeds.
    struct FailNextAttach final : Game {
        FakeDeviceState& device;
        explicit FailNextAttach(FakeDeviceState& state) : device(state) {}
        core::Result<void> on_start(App&) override {
            device.attach_failures_left = 1;
            return {};
        }
        void on_update(App&, const core::FrameTime&) override {}
    };

    FakeState state;
    FakeDeviceState device;
    state.script = {SurfaceLost{}, k_end_of_frame, SurfaceRestored{}, k_end_of_frame,
                    k_end_of_frame};
    App app(std::make_unique<FakePlatform>(state), k_config, std::make_unique<FakeDevice>(device));
    FailNextAttach game{device};

    REQUIRE(app.run(game).has_value());
    CHECK(device.calls == Log{"attach 640x480", "detach", "attach_failed", "attach 640x480",
                              "begin_frame", "end_frame"});
}

TEST_CASE("App skips drawing when the device has no frame available") {
    RenderRun run;
    run.device.frames_available = false;

    REQUIRE(run.go({k_end_of_frame}).has_value());

    CHECK(run.device.calls == Log{"attach 640x480", "begin_frame"});
    CHECK(run.game.log == Log{"start", "update", "stop"});
}

TEST_CASE("App ends with the error when the device fails") {
    RenderRun run;
    run.device.begin_frame_error = core::Error{core::ErrorCode::Unknown, "device lost"};

    const core::Result<void> result = run.go({k_end_of_frame, k_end_of_frame});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message == "device lost");
    CHECK(run.game.log == Log{"start", "update", "stop"});
}

TEST_CASE("App tells the device about resizes") {
    RenderRun run;
    REQUIRE(run.go({SurfaceResized{{800, 600}}, k_end_of_frame}).has_value());

    CHECK(run.device.calls == Log{"attach 640x480", "resize 800x600", "begin_frame", "end_frame"});
}

TEST_CASE("App draws nothing while paused") {
    RenderRun run;
    REQUIRE(run.go({AppPaused{}, k_end_of_frame, QuitRequested{}}).has_value());

    CHECK(run.device.calls == Log{"attach 640x480"});
}

TEST_CASE("App limits the frame rate when asked") {
    const auto config = core::Config::parse("[app]\nmax_fps = 100");
    REQUIRE(config.has_value());

    Run run;
    const core::Stopwatch stopwatch;
    REQUIRE(run.go({k_end_of_frame, k_end_of_frame, k_end_of_frame, k_end_of_frame, k_end_of_frame},
                   config.value())
                .has_value());

    // Five frames at 10 ms each. Only the lower bound is reliable on a busy machine.
    CHECK(stopwatch.elapsed_seconds() >= 0.04);
}
