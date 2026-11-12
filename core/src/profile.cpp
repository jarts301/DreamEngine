#include "dream/core/profile.h"

namespace dream::core {

namespace detail {

// Read with acquire and written with release, so a thread that sees a sink also sees it fully
// constructed.
std::atomic<ProfileSink*> g_profile_sink{nullptr};

} // namespace detail

void set_profile_sink(ProfileSink* sink) noexcept {
    detail::g_profile_sink.store(sink, std::memory_order_release);
}

} // namespace dream::core
