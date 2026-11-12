#pragma once

#include "dream/core/profile.h"
#include "dream/core/result.h"

#include <string_view>

namespace dream::profiler {

// Records where a program's time goes, for a tool to show. A backend receives the zones, frames,
// values and messages of the macros in dream/core/profile.h once it is installed with
// core::set_profile_sink, which the runtime does for the backend that the project configuration
// selects:
//
//     [modules.windows]
//     profiler = ["timeline"]
//
// The event functions inherited from core::ProfileSink are called from any thread, at any time,
// and must be cheap: they run inside the code being measured.
class Profiler : public core::ProfileSink {
public:
    static constexpr std::string_view k_module_interface = "profiler";

    // Saves or sends what has been recorded so far, for backends that keep anything. Safe to call
    // while other threads are recording, and more than once. The runtime calls it as the program
    // ends.
    [[nodiscard]] virtual core::Result<void> flush() = 0;
};

} // namespace dream::profiler
