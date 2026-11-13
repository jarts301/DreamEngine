#include "dream/runtime/crash_reports.h"

#include "dream/runtime/app.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <string_view>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;

namespace {

core::Result<CrashReportConfig> read(std::string_view text) {
    return CrashReportConfig::from_config(core::Config::parse(text).value());
}

class IdleGame final : public Game {
public:
    void on_update(App& /*app*/, const core::FrameTime& /*time*/) override {
        ++frames;
    }
    int frames = 0;
};

} // namespace

TEST_CASE("Crash report settings have defaults, and read every key") {
    const core::Result<CrashReportConfig> defaults = read("");
    REQUIRE(defaults.has_value());
#if defined(__ANDROID__)
    // Off by default there, since crash reports aren't caught on Android.
    CHECK_FALSE(defaults.value().enabled);
#else
    CHECK(defaults.value().enabled);
#endif
    CHECK(defaults.value().directory == std::filesystem::path("crash_reports"));
    CHECK(defaults.value().minidump);
    CHECK_FALSE(defaults.value().simulate.has_value());
    CHECK(defaults.value().simulate_after_frames == 300);

    const core::Result<CrashReportConfig> none = read("[crash_report]\nsimulate = \"none\"\n");
    REQUIRE(none.has_value());
    CHECK_FALSE(none.value().simulate.has_value());

    const core::Result<CrashReportConfig> set = read(R"(
[crash_report]
enabled = false
directory = "reports/crashes"
minidump = false
simulate = "stack_overflow"
simulate_after_frames = 12
)");
    REQUIRE(set.has_value());
    CHECK_FALSE(set.value().enabled);
    CHECK(set.value().directory == std::filesystem::path("reports/crashes"));
    CHECK_FALSE(set.value().minidump);
    CHECK(set.value().simulate == core::CrashKind::StackOverflow);
    CHECK(set.value().simulate_after_frames == 12);
}

TEST_CASE("Crash report settings refuse values they can't use") {
    const auto rejects = [](std::string_view line, std::string_view key) {
        const core::Result<CrashReportConfig> settings =
            read(std::string("[crash_report]\n") + std::string(line));
        REQUIRE_FALSE(settings.has_value());
        CHECK_MESSAGE(settings.error().message.find(key) != std::string::npos,
                      settings.error().message);
    };
    rejects("simulate = \"segfault\"", "simulate");
    rejects("simulate = 3", "simulate");
    rejects("simulate_after_frames = -1", "simulate_after_frames");
    rejects("directory = \"\"", "directory");
    rejects("enabled = \"yes\"", "enabled");
    rejects("minidump = 0", "minidump");
}

TEST_CASE("The App refuses a simulated crash it doesn't know, before the game starts") {
    FakeState state;
    state.script = {k_end_of_frame};
    IdleGame game;
    const core::Config config =
        core::Config::parse("[crash_report]\nsimulate = \"explode\"\n").value();
    App app(std::make_unique<FakePlatform>(state), config);
    const core::Result<void> result = app.run(game);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("crash_report.simulate") != std::string::npos);
    CHECK(game.frames == 0);
}

TEST_CASE("The App doesn't crash before the frame it was told to") {
    FakeState state;
    state.script = {k_end_of_frame, k_end_of_frame, k_end_of_frame};
    IdleGame game;
    const core::Config config =
        core::Config::parse("[crash_report]\nsimulate = \"abort\"\nsimulate_after_frames = 3\n")
            .value();
    App app(std::make_unique<FakePlatform>(state), config);
    // Three frames run, numbered 0 to 2, and the crash would be in frame 3.
    REQUIRE(app.run(game).has_value());
    CHECK(game.frames == 3);
}
