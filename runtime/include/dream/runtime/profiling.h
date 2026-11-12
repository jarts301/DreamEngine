#pragma once

#include "dream/core/module_project.h"
#include "dream/core/profile.h"
#include "dream/core/result.h"
#include "dream/profiler/profiler.h"

#include <memory>

namespace dream::runtime {

// Makes a profiler receive the engine's profiling events, from the macros in dream/core/profile.h,
// for as long as the session exists. Every game gets one from run_program, started before anything
// else and ended after everything else, with the backend its project configuration lists:
//
//     [modules.windows]
//     profiler = ["timeline"]
//
// Ending the session flushes the profiler, so a trace file is complete once the program has ended.
class ProfilerSession {
public:
    // Creates the profiler the project configuration selects. Without a `profiler` list, or in a
    // build made with DREAM_PROFILING=OFF, the session does nothing.
    [[nodiscard]] static core::Result<ProfilerSession> start(const core::ModuleProject& project);

    // Installs `profiler`, if it isn't null, and names the calling thread "Main".
    explicit ProfilerSession(std::unique_ptr<profiler::Profiler> profiler = nullptr);

    ProfilerSession(ProfilerSession&& other) noexcept;
    ProfilerSession& operator=(ProfilerSession&&) = delete;
    ProfilerSession(const ProfilerSession&) = delete;
    ProfilerSession& operator=(const ProfilerSession&) = delete;

    // Restores the sink installed before, then flushes and logs any failure. Every zone that began
    // while this session was installed must have ended, so threads that profile must have stopped.
    ~ProfilerSession();

    // Saves what has been recorded so far, without waiting for the end.
    [[nodiscard]] core::Result<void> flush();

    // Null when profiling is off.
    [[nodiscard]] profiler::Profiler* profiler() noexcept {
        return m_profiler.get();
    }

private:
    std::unique_ptr<profiler::Profiler> m_profiler;
    core::ProfileSink* m_previous = nullptr;
};

} // namespace dream::runtime
