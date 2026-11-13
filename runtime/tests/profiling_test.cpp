#include "dream/runtime/profiling.h"

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/asset_manager.h"

#include "fake_device.h"
#include "fake_platform.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <string_view>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;

namespace {

using Events = std::vector<std::string>;

// What a RecordingProfiler saw. Kept outside it, because sessions own their profiler.
struct ProfilerLog {
    std::mutex mutex;
    Events events;
    int flushes = 0;
    std::optional<core::Error> flush_error;

    void add(std::string event) {
        const std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }

    [[nodiscard]] std::size_t count(std::string_view event) {
        const std::lock_guard lock(mutex);
        return static_cast<std::size_t>(std::count(events.begin(), events.end(), event));
    }

    // The numbers recorded for a value, in order.
    [[nodiscard]] std::vector<double> values(std::string_view name) {
        const std::string prefix = std::format("value {}=", name);
        const std::lock_guard lock(mutex);
        std::vector<double> found;
        for (const std::string& event : events) {
            if (event.starts_with(prefix)) {
                found.push_back(std::strtod(event.c_str() + prefix.size(), nullptr));
            }
        }
        return found;
    }

    // The position of the first event starting with `prefix`, or the number of events.
    [[nodiscard]] std::size_t find_prefix(std::string_view prefix) {
        const std::lock_guard lock(mutex);
        return static_cast<std::size_t>(
            std::find_if(events.begin(), events.end(),
                         [prefix](const std::string& event) { return event.starts_with(prefix); }) -
            events.begin());
    }

    // The position of the first `event`, or the number of events if there is none.
    [[nodiscard]] std::size_t find(std::string_view event) {
        const std::lock_guard lock(mutex);
        return static_cast<std::size_t>(std::find(events.begin(), events.end(), event) -
                                        events.begin());
    }
};

class RecordingProfiler final : public profiler::Profiler {
public:
    explicit RecordingProfiler(ProfilerLog& log) : m_log(log) {}

    void begin_zone(const core::ProfileSite& site, std::string_view detail) override {
        m_log.add(detail.empty() ? std::format("begin {}", site.name)
                                 : std::format("begin {}: {}", site.name, detail));
    }
    void end_zone(const core::ProfileSite& site) override {
        m_log.add(std::format("end {}", site.name));
    }
    void mark_frame() override {
        m_log.add("frame");
    }
    void set_thread_name(std::string_view name) override {
        m_log.add(std::format("thread {}", name));
    }
    void record_value(const char* name, double value) override {
        m_log.add(std::format("value {}={}", name, value));
    }
    void record_message(std::string_view text) override {
        m_log.add(std::format("message {}", text));
    }
    core::Result<void> flush() override {
        ++m_log.flushes;
        if (m_log.flush_error.has_value()) {
            return *m_log.flush_error;
        }
        return {};
    }

private:
    ProfilerLog& m_log;
};

// The profiler that ModuleProject creates for the "recording" backend.
ProfilerLog* g_module_log = nullptr;

core::Result<std::unique_ptr<profiler::Profiler>> create_recording(const core::Config& config) {
    if (!config.get_bool("profiler.recording.available").value_or(true)) {
        return core::Error{core::ErrorCode::NotFound, "not available"};
    }
    return std::unique_ptr<profiler::Profiler>{std::make_unique<RecordingProfiler>(*g_module_log)};
}

core::Result<void> register_recording(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<profiler::Profiler>{
        .name = "recording",
        .create = &create_recording,
    });
}

core::ModuleProject load_project(std::string_view config_text) {
    core::Result<core::ModuleProject> project = core::ModuleProject::load(
        core::ModuleProjectDefinition{"test", config_text, &register_recording});
    REQUIRE(project.has_value());
    return std::move(project).value();
}

class CaptureLog final : public core::LogSink {
public:
    CaptureLog() {
        core::add_log_sink(*this);
    }
    ~CaptureLog() override {
        core::remove_log_sink(*this);
    }
    CaptureLog(const CaptureLog&) = delete;
    CaptureLog& operator=(const CaptureLog&) = delete;

    void write(const core::LogMessage& message) override {
        if (message.level >= core::LogLevel::Error) {
            errors.emplace_back(message.text);
        }
    }

    std::vector<std::string> errors;
};

const core::Config k_app_config = core::Config::parse(R"(
[app]
max_fps = 0
)")
                                      .value();

class ScriptedGame final : public Game {
public:
    void on_update(App& /*app*/, const core::FrameTime& /*time*/) override {
        ++updates;
    }
    int updates = 0;
};

} // namespace

TEST_CASE("A profiler session installs its profiler and names the main thread") {
    ProfilerLog log;
    REQUIRE(core::profile_sink() == nullptr);
    {
        const ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        CHECK(core::profile_sink() != nullptr);
        CHECK(log.events == Events{"thread Main"});
        CHECK(log.flushes == 0);
    }
    CHECK(core::profile_sink() == nullptr);
    CHECK(log.flushes == 1);
}

TEST_CASE("A profiler session puts back the sink it replaced, unless another replaced it") {
    ProfilerLog log;
    RecordingProfiler before(log);
    RecordingProfiler replacement(log);
    core::set_profile_sink(&before);
    {
        const ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        CHECK(core::profile_sink() != &before);
    }
    CHECK(core::profile_sink() == &before);

    {
        const ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        // Someone else takes over while the session runs, and keeps the place.
        core::set_profile_sink(&replacement);
    }
    CHECK(core::profile_sink() == &replacement);
    core::set_profile_sink(nullptr);
}

TEST_CASE("A profiler session without a profiler does nothing") {
    ProfilerSession session;
    CHECK(session.profiler() == nullptr);
    CHECK(core::profile_sink() == nullptr);
    CHECK(session.flush().has_value());
}

TEST_CASE("A profiler session can flush early, and reports a failed flush at the end") {
    ProfilerLog log;
    CaptureLog errors;
    {
        ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        CHECK(session.flush().has_value());
        CHECK(log.flushes == 1);

        log.flush_error = core::Error{core::ErrorCode::IoError, "disk full"};
        CHECK_FALSE(session.flush().has_value());
    }
    CHECK(log.flushes == 3);
    CHECK(errors.errors == std::vector<std::string>{"disk full"});
}

TEST_CASE("A moved profiler session keeps the profiler installed until it ends") {
    ProfilerLog log;
    {
        std::optional<ProfilerSession> moved;
        {
            ProfilerSession original(std::make_unique<RecordingProfiler>(log));
            moved.emplace(std::move(original));
        }
        CHECK(core::profile_sink() != nullptr);
        CHECK(log.flushes == 0);
    }
    CHECK(core::profile_sink() == nullptr);
    CHECK(log.flushes == 1);
}

TEST_CASE("A project without a profiler list starts a session that does nothing") {
    const core::ModuleProject project = load_project("[modules.test]\n");
    core::Result<ProfilerSession> session = ProfilerSession::start(project);
    REQUIRE(session.has_value());
    CHECK(session.value().profiler() == nullptr);
    CHECK(core::profile_sink() == nullptr);
}

#if !defined(DREAM_DISABLE_PROFILING)

TEST_CASE("A project's profiler list chooses the backend the session installs") {
    ProfilerLog log;
    g_module_log = &log;
    const core::ModuleProject project =
        load_project("[modules.test]\nprofiler = [\"recording\"]\n");
    {
        core::Result<ProfilerSession> session = ProfilerSession::start(project);
        REQUIRE(session.has_value());
        CHECK(session.value().profiler() != nullptr);
        CHECK(core::profile_sink() == session.value().profiler());
    }
    CHECK(core::profile_sink() == nullptr);
    CHECK(log.events == Events{"thread Main"});
    CHECK(log.flushes == 1);
    g_module_log = nullptr;
}

TEST_CASE("A session can't start when no listed profiler is usable") {
    ProfilerLog log;
    g_module_log = &log;
    const core::ModuleProject project = load_project(
        "[modules.test]\nprofiler = [\"recording\"]\n\n[profiler.recording]\navailable = false\n");
    const core::Result<ProfilerSession> session = ProfilerSession::start(project);
    REQUIRE_FALSE(session.has_value());
    CHECK(session.error().message.find("not available") != std::string::npos);
    CHECK(core::profile_sink() == nullptr);
    g_module_log = nullptr;
}

TEST_CASE("The main loop measures its parts and marks each frame after it ends") {
    ProfilerLog log;
    FakeState state;
    state.script = {k_end_of_frame, k_end_of_frame};
    ScriptedGame game;
    {
        const ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        App app(std::make_unique<FakePlatform>(state), k_app_config);
        REQUIRE(app.run(game).has_value());
    }
    REQUIRE(game.updates == 2);

    CHECK(log.count("begin Start") == 1);
    CHECK(log.count("begin Frame") == 2);
    CHECK(log.count("end Frame") == 2);
    CHECK(log.count("begin Update") == 2);
    CHECK(log.count("begin Queued events") == 2);
    CHECK(log.count("frame") == 2);
    CHECK(log.count("begin Stop") == 1);
    // No graphics device, so nothing to render.
    CHECK(log.count("begin Render") == 0);

    // Every zone ended, and each frame is marked only once its zone has ended.
    CHECK(log.count("begin Platform events") == log.count("end Platform events"));
    CHECK(log.find("end Frame") < log.find("frame"));
    CHECK(log.find("end Start") < log.find("begin Frame"));
    CHECK(log.find("end Frame") < log.find("begin Stop"));
}

TEST_CASE("The main loop measures waiting for, rendering and presenting each frame") {
    ProfilerLog log;
    FakeState state;
    state.script = {k_end_of_frame};
    FakeDeviceState device;
    ScriptedGame game;
    {
        const ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        App app(std::make_unique<FakePlatform>(state), k_app_config,
                std::make_unique<FakeDevice>(device));
        REQUIRE(app.run(game).has_value());
    }

    const std::size_t begin_gpu = log.find("begin Begin GPU frame");
    const std::size_t render = log.find("begin Render");
    const std::size_t present = log.find("begin Submit and present");
    const std::size_t frame_end = log.find("end Frame");
    CHECK(begin_gpu < render);
    CHECK(render < present);
    CHECK(present < frame_end);
    CHECK(frame_end < log.events.size());
    CHECK(log.count("end Submit and present") == 1);
}

TEST_CASE("The main loop records frame timing, the heap, and memory from the system and GPU") {
    ProfilerLog log;
    FakeState state;
    state.script = {k_end_of_frame, k_end_of_frame, k_end_of_frame};
    FakeDeviceState device;
    device.memory = rhi::MemoryUsage{.used_bytes = 256 << 20, .budget_bytes = 1024 << 20};
    ScriptedGame game;
    const core::Config config = core::Config::parse("[app]\nmax_fps = 0\n").value();
    {
        const ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        App app(std::make_unique<FakePlatform>(state), config,
                std::make_unique<FakeDevice>(device));
        REQUIRE(app.run(game).has_value());
    }

    // Every frame.
    const std::vector<double> frame_times = log.values("Frame time (ms)");
    const std::vector<double> work_times = log.values("Frame CPU work (ms)");
    REQUIRE(frame_times.size() == 3);
    REQUIRE(work_times.size() == 3);
    for (std::size_t frame = 0; frame < frame_times.size(); ++frame) {
        CHECK(work_times[frame] >= 0.0);
        CHECK(work_times[frame] <= frame_times[frame]);
    }
    CHECK(log.values("Fixed steps per frame").size() == 3);
    // The test program counts its heap, as profiled games do. The first frame has no earlier one
    // to compare with.
    CHECK(log.values("Heap allocations per frame").size() == 2);
    CHECK(log.values("Heap allocated per frame (KB)").size() == 2);
    CHECK(log.values("Heap in use (MB)").size() == 3);

    // Sampled, starting with the first frame.
    CHECK(log.values("GPU memory (MB)").front() == 256.0);
    CHECK(log.values("GPU memory budget (MB)").front() == 1024.0);
    CHECK(log.count("begin Profiler sampling") >= 1);
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    REQUIRE_FALSE(log.values("Memory resident (MB)").empty());
    CHECK(log.values("Memory resident (MB)").front() > 0.0);
    CHECK(log.values("Memory private (MB)").front() > 0.0);
#endif

    // A frame's values come after its zone and before its mark, so a profiler recording a range
    // of frames keeps them with their frame.
    const std::size_t first_value = log.find_prefix("value Frame time (ms)=");
    CHECK(log.find("end Frame") < first_value);
    CHECK(first_value < log.find("frame"));
}

TEST_CASE("Frame CPU work leaves out waiting for the frame limiter, and CPU use is sampled") {
    ProfilerLog log;
    FakeState state;
    state.script = {k_end_of_frame, k_end_of_frame, k_end_of_frame};
    ScriptedGame game;
    // A 10 ms frame, almost all of it asleep, sampled every time.
    const core::Config config =
        core::Config::parse("[app]\nmax_fps = 100\n\n[profiler]\nsample_rate = 1000\n").value();
    {
        const ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        App app(std::make_unique<FakePlatform>(state), config);
        REQUIRE(app.run(game).has_value());
    }

    const std::vector<double> frame_times = log.values("Frame time (ms)");
    const std::vector<double> work_times = log.values("Frame CPU work (ms)");
    REQUIRE(frame_times.size() == 3);
    REQUIRE(work_times.size() == 3);
    for (std::size_t frame = 0; frame < frame_times.size(); ++frame) {
        CHECK(frame_times[frame] >= 9.0);
        CHECK(work_times[frame] + 5.0 < frame_times[frame]);
    }

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))
    // Each sample after the first measures the CPU use since the one before.
    const std::vector<double> cpu = log.values("CPU usage (%)");
    CHECK(cpu.size() == 2);
    for (const double percent : cpu) {
        CHECK(percent >= 0.0);
    }
#endif
}

TEST_CASE("The profiler's sample rate must be sensible") {
    for (const std::string_view rate : {"0", "1001", "-3"}) {
        FakeState state;
        state.script = {k_end_of_frame};
        ScriptedGame game;
        const core::Config config =
            core::Config::parse(std::format("[profiler]\nsample_rate = {}\n", rate)).value();
        App app(std::make_unique<FakePlatform>(state), config);
        const core::Result<void> result = app.run(game);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("profiler.sample_rate") != std::string::npos);
    }
}

TEST_CASE("Loading an asset is measured, with its path") {
    ProfilerLog log;
    FakeState state;
    state.assets.emplace("notes.txt", std::vector<std::byte>{std::byte{'h'}, std::byte{'i'}});
    FakePlatform platform(state);
    VirtualFileSystem files(platform);
    AssetManager assets(files);
    assets.register_loader<std::string>(
        [](AssetLoadContext& context) -> core::Result<std::string> {
            const auto bytes = context.files.read(context.path);
            if (!bytes.has_value()) {
                return bytes.error();
            }
            return std::string(reinterpret_cast<const char*>(bytes.value().data()),
                               bytes.value().size());
        });

    {
        const ProfilerSession session(std::make_unique<RecordingProfiler>(log));
        const auto loaded = assets.load<std::string>("assets:/notes.txt");
        REQUIRE(loaded.has_value());
        // Still loaded, because the first reference is held, so not measured again.
        REQUIRE(assets.load<std::string>("assets:/notes.txt").has_value());
    }
    CHECK(log.count("begin Load asset: assets:/notes.txt") == 1);
    CHECK(log.count("end Load asset") == 1);
}

#endif
