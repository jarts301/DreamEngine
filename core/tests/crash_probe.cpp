// Crashes on purpose, the way its first argument names, with a crash handler writing to the folder
// its second argument names. cmake/tests/check_crash_report.cmake runs it and reads the report.
//
// Usage: dream_crash_probe <kind> <folder>

#include "dream/core/crash_handler.h"
#include "dream/core/log.h"

#include <cstdio>
#include <optional>

int main(int argc, char** argv) {
    using namespace dream::core;
    if (argc != 3) {
        std::fputs("Usage: dream_crash_probe <kind> <folder>\n", stderr);
        return 2;
    }
    const std::optional<CrashKind> kind = crash_kind_from_name(argv[1]);
    if (!kind.has_value()) {
        std::fprintf(stderr, "Unknown kind of crash: %s\n", argv[1]);
        return 2;
    }

    const auto handler = CrashHandler::install(
        CrashReportSettings{.directory = argv[2], .application = "Crash probe"});
    if (!handler.has_value()) {
        std::fprintf(stderr, "%s\n", handler.error().message.c_str());
        return 3;
    }
    set_crash_detail("Probe", "about to crash");
    set_crash_frame(42);
    DREAM_LOG_INFO("The probe is about to crash with {}", crash_kind_name(*kind));
    simulate_crash(*kind);
}
