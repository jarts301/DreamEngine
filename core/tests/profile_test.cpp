// Tests the profiling macros as they are normally compiled, even in a build made with
// DREAM_PROFILING=OFF. profile_disabled_test.cpp covers the other case. The engine's own zones,
// such as the job system's, follow the build's setting.
#if defined(DREAM_DISABLE_PROFILING)
#define DREAM_TEST_ENGINE_PROFILING 0
#undef DREAM_DISABLE_PROFILING
#else
#define DREAM_TEST_ENGINE_PROFILING 1
#endif

#include "dream/core/profile.h"

#include "dream/core/job_system.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace dream::core;

namespace {

// Writes each event as a line of text, from any thread.
class RecordingSink final : public ProfileSink {
public:
    void begin_zone(const ProfileSite& site, std::string_view detail) override {
        add(detail.empty() ? std::format("begin {}", site.name)
                           : std::format("begin {}: {}", site.name, detail));
        const std::lock_guard lock(m_mutex);
        sites.push_back(&site);
    }
    void end_zone(const ProfileSite& site) override {
        add(std::format("end {}", site.name));
    }
    void mark_frame() override {
        add("frame");
    }
    void set_thread_name(std::string_view name) override {
        add(std::format("thread {}", name));
    }
    void record_value(const char* name, double value) override {
        add(std::format("value {}={}", name, value));
    }
    void record_message(std::string_view text) override {
        add(std::format("message {}", text));
    }

    [[nodiscard]] std::vector<std::string> take() {
        const std::lock_guard lock(m_mutex);
        return std::move(events);
    }

    [[nodiscard]] std::size_t count(std::string_view event) {
        const std::lock_guard lock(m_mutex);
        return static_cast<std::size_t>(std::count(events.begin(), events.end(), event));
    }

    std::vector<std::string> events;
    std::vector<const ProfileSite*> sites;

private:
    void add(std::string event) {
        const std::lock_guard lock(m_mutex);
        events.push_back(std::move(event));
    }

    std::mutex m_mutex;
};

// Installs a sink for one test and removes it afterwards, so tests don't see each other's events.
class SinkFixture {
public:
    SinkFixture() {
        set_profile_sink(&sink);
    }
    ~SinkFixture() {
        set_profile_sink(nullptr);
    }

    RecordingSink sink;
};

using Events = std::vector<std::string>;

void measured_function() {
    DREAM_PROFILE_ZONE("Measured");
}

} // namespace

TEST_CASE("profiling without a sink does nothing") {
    REQUIRE(profile_sink() == nullptr);

    DREAM_PROFILE_ZONE("Unheard");
    DREAM_PROFILE_ZONE_DETAIL("Unheard detail", "text");
    DREAM_PROFILE_FRAME();
    DREAM_PROFILE_THREAD("Unheard thread");
    DREAM_PROFILE_VALUE("Unheard value", 1);
    DREAM_PROFILE_MESSAGE("Unheard {}", 1);
    CHECK(profile_sink() == nullptr);
}

TEST_CASE("set_profile_sink installs and removes the sink") {
    RecordingSink sink;
    set_profile_sink(&sink);
    CHECK(profile_sink() == &sink);
    set_profile_sink(nullptr);
    CHECK(profile_sink() == nullptr);
}

TEST_CASE_FIXTURE(SinkFixture, "zones end in the reverse order they began") {
    {
        DREAM_PROFILE_ZONE("Outer");
        {
            DREAM_PROFILE_ZONE("Inner");
            DREAM_PROFILE_ZONE("Innermost");
        }
        DREAM_PROFILE_ZONE("Second");
    }

    CHECK(sink.take() == Events{"begin Outer", "begin Inner", "begin Innermost", "end Innermost",
                                "end Inner", "begin Second", "end Second", "end Outer"});
}

TEST_CASE_FIXTURE(SinkFixture, "a zone's site knows where it is and is the same every time") {
    measured_function();
    measured_function();

    REQUIRE(sink.sites.size() == 2);
    CHECK(sink.sites[0] == sink.sites[1]);
    CHECK(std::string_view(sink.sites[0]->name) == "Measured");
    CHECK(std::string_view(sink.sites[0]->file).find("profile_test.cpp") != std::string_view::npos);
    const std::uint32_t line = __LINE__ + 2;
    sink.sites.clear();
    DREAM_PROFILE_ZONE("Here");
    REQUIRE(sink.sites.size() == 1);
    CHECK(sink.sites[0]->line == line);
}

TEST_CASE_FIXTURE(SinkFixture, "a zone can describe its occurrence") {
    const std::string path = "assets:/scenes/level.toml";
    {
        DREAM_PROFILE_ZONE_DETAIL("Load scene", path);
    }
    CHECK(sink.take() == Events{"begin Load scene: assets:/scenes/level.toml", "end Load scene"});
}

TEST_CASE_FIXTURE(SinkFixture, "frames, thread names, values and messages reach the sink") {
    const int bodies = 12;
    DREAM_PROFILE_FRAME();
    DREAM_PROFILE_THREAD("Main");
    DREAM_PROFILE_VALUE("Bodies", bodies);
    DREAM_PROFILE_VALUE("Load", 0.5f);
    DREAM_PROFILE_MESSAGE("Loaded {} of {}", 3, "levels");

    CHECK(sink.take() == Events{"frame", "thread Main", "value Bodies=12", "value Load=0.5",
                                "message Loaded 3 of levels"});
}

TEST_CASE("a zone ends on the sink it began with") {
    RecordingSink first;
    RecordingSink second;
    set_profile_sink(&first);
    {
        DREAM_PROFILE_ZONE("Straddling");
        set_profile_sink(&second);
        DREAM_PROFILE_ZONE("Later");
    }
    set_profile_sink(nullptr);

    CHECK(first.take() == Events{"begin Straddling", "end Straddling"});
    CHECK(second.take() == Events{"begin Later", "end Later"});
}

TEST_CASE_FIXTURE(SinkFixture, "threads can profile at the same time") {
    constexpr int k_threads = 4;
    constexpr int k_zones = 200;

    std::vector<std::thread> threads;
    for (int t = 0; t < k_threads; ++t) {
        threads.emplace_back([] {
            for (int i = 0; i < k_zones; ++i) {
                DREAM_PROFILE_ZONE("Work");
                DREAM_PROFILE_VALUE("Index", i);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(sink.count("begin Work") == k_threads * k_zones);
    CHECK(sink.count("end Work") == k_threads * k_zones);
}

#if DREAM_TEST_ENGINE_PROFILING
TEST_CASE_FIXTURE(SinkFixture, "the job system names its workers and measures each job") {
    {
        JobSystem jobs(2);
        JobCounter counter;
        for (int i = 0; i < 8; ++i) {
            jobs.run(counter, [] {});
        }
        jobs.wait(counter);
        // Every job's zone has ended once wait returns.
        CHECK(sink.count("end Job") == 8);
    }

    // Destroying the system joins the workers, so both have started and named themselves.
    CHECK(sink.count("begin Job") == 8);
    CHECK(sink.count("thread Job worker 1") == 1);
    CHECK(sink.count("thread Job worker 2") == 1);
}
#endif
