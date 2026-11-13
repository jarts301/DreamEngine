#pragma once

#include "dream/core/config.h"
#include "dream/core/crash_handler.h"
#include "dream/core/result.h"

#include <cstdint>
#include <filesystem>
#include <optional>

namespace dream::runtime {

// How a game's crashes are reported, from the project configuration, all optional:
//
//     [crash_report]
//     enabled = true                # Off by default on Android, which profiling doesn't cover.
//     directory = "crash_reports"   # Relative to the folder the game is started from.
//     minidump = true               # Windows: a minidump beside each report.
//     simulate = "none"             # Crash on purpose, to try the reports: "null_pointer",
//                                   # "divide_by_zero", "stack_overflow", "abort", "assert" or
//                                   # "terminate".
//     simulate_after_frames = 300   # The crash happens in the frame after this many.
//
// run_program installs the crash handler with these, and App runs the simulated crash. See
// docs/reports/crash-reports.md.
struct CrashReportConfig {
    bool enabled = true;
    std::filesystem::path directory = "crash_reports";
    bool minidump = true;
    std::optional<core::CrashKind> simulate = std::nullopt;
    std::uint64_t simulate_after_frames = 300;

    [[nodiscard]] static core::Result<CrashReportConfig> from_config(const core::Config& config);
};

} // namespace dream::runtime
