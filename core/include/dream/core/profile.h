#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

// Profiling instrumentation that any layer can use: zones that measure a scope, frame marks, values
// over time and messages. Events go to one installed ProfileSink, which the runtime sets to the
// `profiler` backend the project configuration selects. With no sink installed, a zone costs one
// atomic load and a branch, and a message isn't formatted. See docs/adr/0015-profiling.md.
//
//     void Physics::step() {
//         DREAM_PROFILE_ZONE("Physics step");
//         DREAM_PROFILE_VALUE("Bodies", body_count);
//     }
//
// Building with the CMake option DREAM_PROFILING=OFF defines DREAM_DISABLE_PROFILING, and every
// macro then compiles to nothing.

namespace dream::core {

// A place in the code that a zone measures. The macros create each one as a static constant, so a
// sink may keep pointers to it and to its strings for the rest of the program.
struct ProfileSite {
    const char* name = "";
    const char* file = "";
    std::uint32_t line = 0;
};

// Receives profiling events. The `profiler` interface derives from this; Core only routes events to
// it, as it routes log messages to a LogSink.
//
// Every function may be called from any thread at the same time, so implementations do their own
// synchronization. They must not use the profiling macros themselves.
class ProfileSink {
public:
    virtual ~ProfileSink() = default;

    // A zone begins on the calling thread. `detail` describes this occurrence, such as the file
    // being loaded, and is valid only during the call. It is often empty.
    virtual void begin_zone(const ProfileSite& site, std::string_view detail) = 0;

    // The innermost zone open on the calling thread ends. Zones on a thread end in the reverse
    // order they began, and `site` is the one the zone began with.
    virtual void end_zone(const ProfileSite& site) = 0;

    // The main loop finished a frame.
    virtual void mark_frame() = 0;

    // Names the calling thread, such as "Main" or "Job worker 2".
    virtual void set_thread_name(std::string_view name) = 0;

    // A sample of a value drawn as a graph over time. `name` has static storage.
    virtual void record_value(const char* name, double value) = 0;

    // A moment worth noting, on the calling thread.
    virtual void record_message(std::string_view text) = 0;
};

namespace detail {

extern std::atomic<ProfileSink*> g_profile_sink;

} // namespace detail

// Installs the sink that receives profiling events, replacing any other, or removes it with null.
// The sink is not owned. A zone ends on the sink it began with, so a sink must stay alive until
// every zone that began while it was installed has ended. Threads should be named after it is
// installed, because names given earlier are not passed on.
void set_profile_sink(ProfileSink* sink) noexcept;

[[nodiscard]] inline ProfileSink* profile_sink() noexcept {
    return detail::g_profile_sink.load(std::memory_order_acquire);
}

// Measures its own lifetime as a zone. Use DREAM_PROFILE_ZONE rather than this directly.
class ProfileZone {
public:
    explicit ProfileZone(const ProfileSite& site, std::string_view detail = {})
        : m_site(&site), m_sink(profile_sink()) {
        if (m_sink != nullptr) {
            m_sink->begin_zone(site, detail);
        }
    }

    ~ProfileZone() {
        if (m_sink != nullptr) {
            m_sink->end_zone(*m_site);
        }
    }

    ProfileZone(const ProfileZone&) = delete;
    ProfileZone& operator=(const ProfileZone&) = delete;

private:
    const ProfileSite* m_site;
    ProfileSink* m_sink;
};

inline void profile_frame() {
    if (ProfileSink* const sink = profile_sink()) {
        sink->mark_frame();
    }
}

inline void profile_thread_name(std::string_view name) {
    if (ProfileSink* const sink = profile_sink()) {
        sink->set_thread_name(name);
    }
}

// `name` must have static storage, such as a string literal.
inline void profile_value(const char* name, double value) {
    if (ProfileSink* const sink = profile_sink()) {
        sink->record_value(name, value);
    }
}

// The text is formatted only when a sink is installed.
template <typename... Args>
void profile_message(std::format_string<Args...> format, Args&&... args) {
    if (ProfileSink* const sink = profile_sink()) {
        sink->record_message(std::format(format, std::forward<Args>(args)...));
    }
}

} // namespace dream::core

#define DREAM_PROFILE_CONCAT_INNER(a, b) a##b
#define DREAM_PROFILE_CONCAT(a, b) DREAM_PROFILE_CONCAT_INNER(a, b)

// Each zone needs its own variable names. MSVC's Edit and Continue mode makes __LINE__ unusable for
// that, and __COUNTER__ is an extension that Clang's -Wpedantic may warn about, so each compiler
// gets the one that works for it.
#if defined(_MSC_VER)
#define DREAM_PROFILE_UNIQUE(prefix) DREAM_PROFILE_CONCAT(prefix, __COUNTER__)
#else
#define DREAM_PROFILE_UNIQUE(prefix) DREAM_PROFILE_CONCAT(prefix, __LINE__)
#endif

#if !defined(DREAM_DISABLE_PROFILING)

#define DREAM_PROFILE_ZONE_WITH_SITE(site, name, detail)                                           \
    static const ::dream::core::ProfileSite site{"" name, __FILE__,                                \
                                                 static_cast<::std::uint32_t>(__LINE__)};          \
    const ::dream::core::ProfileZone DREAM_PROFILE_CONCAT(site, _zone)(site, detail)

// Measures the rest of the enclosing scope. `name` must be a string literal.
#define DREAM_PROFILE_ZONE(name)                                                                   \
    DREAM_PROFILE_ZONE_WITH_SITE(DREAM_PROFILE_UNIQUE(dream_profile_site_), name,                  \
                                 ::std::string_view{})

// The same, with text describing this occurrence, such as a file name. The zone's name must still
// be a string literal, so that every occurrence groups together.
#define DREAM_PROFILE_ZONE_DETAIL(name, detail)                                                    \
    DREAM_PROFILE_ZONE_WITH_SITE(DREAM_PROFILE_UNIQUE(dream_profile_site_), name, detail)

// Marks the end of a frame. The runtime's main loop already does this.
#define DREAM_PROFILE_FRAME() ::dream::core::profile_frame()

// Names the calling thread.
#define DREAM_PROFILE_THREAD(name) ::dream::core::profile_thread_name(name)

// Records a number to draw as a graph over time. `name` must be a string literal.
#define DREAM_PROFILE_VALUE(name, value)                                                           \
    ::dream::core::profile_value("" name, static_cast<double>(value))

// Records a moment, with text formatted like DREAM_LOG_INFO's.
#define DREAM_PROFILE_MESSAGE(...) ::dream::core::profile_message(__VA_ARGS__)

#else

// The arguments stay in an unevaluated operand, as with DREAM_ASSERT: nothing runs, and variables
// used only for profiling don't cause unused-variable warnings.
#define DREAM_PROFILE_ZONE(name) static_cast<void>(sizeof("" name))
#define DREAM_PROFILE_ZONE_DETAIL(name, detail)                                                    \
    static_cast<void>(sizeof("" name) + sizeof(::std::string_view{detail}))
#define DREAM_PROFILE_FRAME() static_cast<void>(0)
#define DREAM_PROFILE_THREAD(name)                                                                 \
    static_cast<void>(sizeof((::dream::core::profile_thread_name(name), 0)))
#define DREAM_PROFILE_VALUE(name, value)                                                           \
    static_cast<void>(                                                                             \
        sizeof((::dream::core::profile_value("" name, static_cast<double>(value)), 0)))
#define DREAM_PROFILE_MESSAGE(...)                                                                 \
    static_cast<void>(sizeof((::dream::core::profile_message(__VA_ARGS__), 0)))

#endif
