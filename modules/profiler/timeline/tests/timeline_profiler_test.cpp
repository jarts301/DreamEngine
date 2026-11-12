#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_profiler_timeline(core::ModuleRegistry& registry);
}

using namespace dream;

namespace {

namespace fs = std::filesystem;

// An empty folder of this test program's own, so parallel test programs don't collide.
fs::path fresh_folder(std::string_view name) {
    const fs::path folder = fs::temp_directory_path() / "dream_profiler_timeline_tests" / name;
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder);
    return folder;
}

// The path as TOML text. Forward slashes need no escaping, and Windows accepts them.
std::string toml_path(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

core::Result<core::CreatedModule<profiler::Profiler>> create(std::string_view config_text) {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_profiler_timeline(registry).has_value());
    const core::Config config = core::Config::parse(config_text).value();
    const std::vector<std::string> preference{"timeline"};
    return registry.create<profiler::Profiler>(preference, config);
}

std::unique_ptr<profiler::Profiler> create_writing_to(const fs::path& path,
                                                      std::string_view extra_settings = {}) {
    auto created = create(std::format("[app]\ntitle = \"Profiled Game\"\n\n[profiler.timeline]\n"
                                      "path = \"{}\"\n{}",
                                      toml_path(path), extra_settings));
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // namespace

std::unique_ptr<profiler::Profiler> make_profiler() {
    return create_writing_to(fresh_folder("conformance") / "trace.json");
}

TEST_CASE("timeline writes a Chrome trace, named after the game, where its settings say") {
    const fs::path path = fresh_folder("writes") / "trace.json";
    const auto profiler = create_writing_to(path);
    {
        const conformance::Installed installed(*profiler);
        DREAM_PROFILE_THREAD("Main");
        {
            DREAM_PROFILE_ZONE("Frame");
            DREAM_PROFILE_ZONE_DETAIL("Load scene", "assets:/scenes/level.toml");
            DREAM_PROFILE_VALUE("Bodies", 20);
            DREAM_PROFILE_MESSAGE("Level {} loaded", 1);
        }
        DREAM_PROFILE_FRAME();
    }
    REQUIRE(profiler->flush().has_value());

    const std::string trace = read_file(path);
    CHECK(trace.starts_with("{\"traceEvents\":["));
    CHECK(trace.ends_with("}\n"));
    CHECK(contains(trace, R"("name":"process_name","args":{"name":"Profiled Game"})"));
    CHECK(contains(trace, R"("name":"thread_name","args":{"name":"Main"})"));
    CHECK(contains(trace, R"("name":"Frame","ts":)"));
    CHECK(contains(trace, R"("name":"Load scene")"));
    CHECK(contains(trace, R"("source":"timeline_profiler_test.cpp:)"));
    CHECK(contains(trace, R"("detail":"assets:/scenes/level.toml")"));
    CHECK(contains(trace, R"("name":"Bodies")"));
    CHECK(contains(trace, R"("name":"Level 1 loaded")"));
    CHECK(contains(trace, R"("frames":1)"));
    CHECK_FALSE(fs::exists(fs::path(path) += ".tmp"));
}

TEST_CASE("timeline replaces its earlier trace with everything recorded so far") {
    const fs::path path = fresh_folder("replaces") / "trace.json";
    const auto profiler = create_writing_to(path);
    static constexpr core::ProfileSite k_first{"First", "", 0};
    static constexpr core::ProfileSite k_second{"Second", "", 0};

    profiler->begin_zone(k_first, {});
    profiler->end_zone(k_first);
    REQUIRE(profiler->flush().has_value());
    CHECK_FALSE(contains(read_file(path), R"("name":"Second")"));

    profiler->begin_zone(k_second, {});
    profiler->end_zone(k_second);
    REQUIRE(profiler->flush().has_value());
    const std::string trace = read_file(path);
    CHECK(contains(trace, R"("name":"First")"));
    CHECK(contains(trace, R"("name":"Second")"));
}

TEST_CASE("timeline creates the folders its path needs") {
    const fs::path path = fresh_folder("folders") / "profiles" / "today" / "trace.json";
    const auto profiler = create_writing_to(path);
    REQUIRE(profiler->flush().has_value());
    CHECK(fs::is_regular_file(path));
}

TEST_CASE("timeline records only the frames its settings ask for") {
    const fs::path path = fresh_folder("window") / "trace.json";
    const auto profiler = create_writing_to(path, "start_frame = 1\nframes = 2\n");
    static constexpr core::ProfileSite k_work{"Work", "", 0};

    for (int frame = 0; frame < 4; ++frame) {
        profiler->begin_zone(k_work, std::format("frame {}", frame));
        profiler->end_zone(k_work);
        profiler->mark_frame();
    }
    REQUIRE(profiler->flush().has_value());

    const std::string trace = read_file(path);
    CHECK_FALSE(contains(trace, R"("detail":"frame 0")"));
    CHECK(contains(trace, R"("detail":"frame 1")"));
    CHECK(contains(trace, R"("detail":"frame 2")"));
    CHECK_FALSE(contains(trace, R"("detail":"frame 3")"));
    CHECK(contains(trace, R"("frames":4)"));
}

TEST_CASE("timeline keeps at most max_events, and says how many it dropped") {
    const fs::path path = fresh_folder("limit") / "trace.json";
    const auto profiler = create_writing_to(path, "max_events = 1\n");
    for (int i = 0; i < 1100; ++i) {
        profiler->record_value("Index", i);
    }
    REQUIRE(profiler->flush().has_value());
    // The limit is rounded up to the recorder's block of 1024 events.
    CHECK(contains(read_file(path), R"("recorded_events":1024,"dropped_events":76)"));
}

TEST_CASE("timeline refuses settings it can't use") {
    const auto rejects = [](std::string_view settings, std::string_view key) {
        const auto created = create(std::format("[profiler.timeline]\n{}\n", settings));
        REQUIRE_FALSE(created.has_value());
        CHECK_MESSAGE(contains(created.error().message, key), created.error().message);
    };
    rejects("path = \"\"", "path");
    rejects("path = 3", "path");
    rejects("max_events = 0", "max_events");
    rejects("max_events = \"many\"", "max_events");
    rejects("start_frame = -1", "start_frame");
    rejects("frames = -2", "frames");
    rejects("frames = 1.5", "frames");
}

TEST_CASE("timeline reports a trace it can't save, and leaves nothing behind") {
    const fs::path folder = fresh_folder("unwritable");

    SUBCASE("the folder it needs is a file") {
        const fs::path blocker = folder / "blocker";
        std::ofstream(blocker) << "not a folder";
        const auto profiler = create_writing_to(blocker / "trace.json");
        const core::Result<void> flushed = profiler->flush();
        REQUIRE_FALSE(flushed.has_value());
        CHECK(flushed.error().code == core::ErrorCode::IoError);
    }

    SUBCASE("a folder is in the way of the file") {
        const fs::path in_the_way = folder / "trace.json";
        fs::create_directories(in_the_way);
        const auto profiler = create_writing_to(in_the_way);
        const core::Result<void> flushed = profiler->flush();
        REQUIRE_FALSE(flushed.has_value());
        CHECK(flushed.error().code == core::ErrorCode::IoError);
        CHECK(fs::is_directory(in_the_way));
        CHECK_FALSE(fs::exists(folder / "trace.json.tmp"));
    }
}
