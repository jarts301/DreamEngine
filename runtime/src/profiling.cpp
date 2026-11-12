#include "dream/runtime/profiling.h"

#include "dream/core/log.h"

#include <utility>

namespace dream::runtime {

core::Result<ProfilerSession> ProfilerSession::start(const core::ModuleProject& project) {
    if (!project.is_configured<profiler::Profiler>()) {
        return ProfilerSession();
    }
#if defined(DREAM_DISABLE_PROFILING)
    DREAM_LOG_INFO("This build has no profiling zones (DREAM_PROFILING=OFF), so the project's "
                   "profiler list is ignored");
    return ProfilerSession();
#else
    core::Result<core::CreatedModule<profiler::Profiler>> created =
        project.create<profiler::Profiler>();
    if (!created.has_value()) {
        return created.error();
    }
    return ProfilerSession(std::move(created.value().instance));
#endif
}

ProfilerSession::ProfilerSession(std::unique_ptr<profiler::Profiler> profiler)
    : m_profiler(std::move(profiler)) {
    if (m_profiler != nullptr) {
        m_previous = core::profile_sink();
        core::set_profile_sink(m_profiler.get());
        m_profiler->set_thread_name("Main");
    }
}

ProfilerSession::ProfilerSession(ProfilerSession&& other) noexcept
    : m_profiler(std::move(other.m_profiler)), m_previous(other.m_previous) {}

ProfilerSession::~ProfilerSession() {
    if (m_profiler == nullptr) {
        return;
    }
    // A sink installed since this one stays installed.
    if (core::profile_sink() == m_profiler.get()) {
        core::set_profile_sink(m_previous);
    }
    if (const core::Result<void> flushed = m_profiler->flush(); !flushed.has_value()) {
        DREAM_LOG_ERROR("{}", flushed.error().message);
    }
}

core::Result<void> ProfilerSession::flush() {
    if (m_profiler == nullptr) {
        return {};
    }
    return m_profiler->flush();
}

} // namespace dream::runtime
