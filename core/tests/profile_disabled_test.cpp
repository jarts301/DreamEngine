// Tests the profiling macros as a build made with DREAM_PROFILING=OFF compiles them, in any build.
#if !defined(DREAM_DISABLE_PROFILING)
#define DREAM_DISABLE_PROFILING
#endif

#include "dream/core/profile.h"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

using namespace dream::core;

namespace {

class CountingSink final : public ProfileSink {
public:
    void begin_zone(const ProfileSite&, std::string_view) override {
        ++events;
    }
    void end_zone(const ProfileSite&) override {
        ++events;
    }
    void mark_frame() override {
        ++events;
    }
    void set_thread_name(std::string_view) override {
        ++events;
    }
    void record_value(const char*, double) override {
        ++events;
    }
    void record_message(std::string_view) override {
        ++events;
    }

    int events = 0;
};

} // namespace

TEST_CASE("disabled profiling macros record nothing and evaluate nothing") {
    CountingSink sink;
    set_profile_sink(&sink);

    int evaluated = 0;
    const auto text = [&evaluated] {
        ++evaluated;
        return std::string("text");
    };
    {
        DREAM_PROFILE_ZONE("Zone");
        DREAM_PROFILE_ZONE_DETAIL("Zone with detail", text());
        DREAM_PROFILE_FRAME();
        DREAM_PROFILE_THREAD(text());
        DREAM_PROFILE_VALUE("Value", ++evaluated);
        DREAM_PROFILE_MESSAGE("Message {}", text());
    }

    // A variable used only by profiling must not warn as unused, which -Werror would make fatal.
    const int only_profiled = 3;
    DREAM_PROFILE_VALUE("Only profiled", only_profiled);

    set_profile_sink(nullptr);
    CHECK(sink.events == 0);
    CHECK(evaluated == 0);
}
