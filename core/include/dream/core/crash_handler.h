#pragma once

#include "dream/core/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Crash reports: when the program crashes, a text report is written, and on Windows a minidump,
// so the crash can be understood afterwards. See docs/adr/0019-crash-reports.md and
// docs/reports/crash-reports.md.

namespace dream::core {

struct CrashReportSettings {
    // Where reports go. Created if missing.
    std::filesystem::path directory = "crash_reports";
    // Named in each report, such as the game's title.
    std::string application = "Dream Engine";
    // Windows only: also write a minidump, which a debugger such as Visual Studio opens at the
    // crash.
    bool minidump = true;
};

// Catches the ways a program can crash and writes a report for each, then lets the crash end
// the program as it would have:
//
// - Windows: unhandled exceptions, such as access violations, stack overflows, division by zero
//   and the breakpoint a failed DREAM_VERIFY stops at; abort(); std::terminate; pure virtual calls;
//   invalid parameters to C runtime functions.
// - Linux: SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT; std::terminate.
//
// Only one may be installed at a time. It must be uninstalled, by destroying it, on the thread that
// installed it.
class CrashHandler {
public:
    // Fails if the directory can't be created, on a platform without crash reports, or while
    // another handler is installed.
    [[nodiscard]] static Result<std::unique_ptr<CrashHandler>>
    install(const CrashReportSettings& settings);

    ~CrashHandler();

    CrashHandler(const CrashHandler&) = delete;
    CrashHandler& operator=(const CrashHandler&) = delete;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept {
        return m_directory;
    }

private:
    explicit CrashHandler(std::filesystem::path directory) : m_directory(std::move(directory)) {}

    std::filesystem::path m_directory;
};

// A line for the reports, such as which graphics backend is in use. Setting a key again replaces
// its value. There is room for 16 keys, and long values are cut short.
void set_crash_detail(std::string_view key, std::string_view value);

// The frame the main loop is on, for the reports. Cheap enough to call every frame.
void set_crash_frame(std::uint64_t frame) noexcept;

// Ways to crash on purpose, to try the crash reports.
enum class CrashKind : std::uint8_t {
    NullPointer,
    DivideByZero,
    StackOverflow,
    Abort,
    Assert,
    Terminate,
};

// The names used in configuration: "null_pointer", "divide_by_zero", "stack_overflow", "abort",
// "assert" and "terminate".
[[nodiscard]] std::optional<CrashKind> crash_kind_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view crash_kind_name(CrashKind kind) noexcept;

// Crashes the program the chosen way. On a processor that doesn't trap a division by zero, that
// kind ends with abort() instead.
[[noreturn]] void simulate_crash(CrashKind kind);

} // namespace dream::core
