#include "crash_report.h"

#include "dream/core/assert.h"
#include "dream/core/crash_handler.h"
#include "dream/core/log.h"
#include "dream/core/process_stats.h"
#include "dream/core/version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

namespace dream::core {
namespace crash {

State g_state;

namespace {

constinit std::atomic<int> g_progress{static_cast<int>(Progress::Idle)};
constinit std::atomic<bool> g_installed{false};
constinit std::atomic<std::uint64_t> g_frame{std::numeric_limits<std::uint64_t>::max()};

// Details: written under a lock, read after a crash without one. A detail being changed at the
// moment of the crash may read half old, half new.
struct Detail {
    char key[32] = {};
    char value[224] = {};
};
std::array<Detail, 16> g_details;
constinit std::atomic<std::size_t> g_detail_count{0};
std::mutex g_details_mutex;

// The last log messages, in a ring. The log calls sinks one at a time, so there is one writer.
struct LogEntry {
    LogLevel level = LogLevel::Info;
    char text[240] = {};
};
constexpr std::size_t k_log_entries = 32;
std::array<LogEntry, k_log_entries> g_log;
constinit std::atomic<std::uint64_t> g_log_count{0};

void copy_text(char* out, std::size_t size, std::string_view text) noexcept {
    const std::size_t length = std::min(text.size(), size - 1);
    std::memcpy(out, text.data(), length);
    out[length] = '\0';
}

class LogRing final : public LogSink {
public:
    void write(const LogMessage& message) override {
        const std::uint64_t index = g_log_count.load(std::memory_order_relaxed);
        LogEntry& entry = g_log[index % k_log_entries];
        entry.level = message.level;
        copy_text(entry.text, sizeof(entry.text), message.text);
        g_log_count.store(index + 1, std::memory_order_release);
    }
};
LogRing g_log_ring;

std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARNING";
    case LogLevel::Error:
        return "ERROR";
    case LogLevel::Fatal:
        return "FATAL";
    }
    return "?";
}

// Kept from being folded into a constant, so the crashes below really happen.
volatile bool g_keep_recursing = true;

int recurse(int depth) {
    volatile char frame[1024];
    frame[0] = static_cast<char>(depth);
    // Adding after the call keeps it from becoming a loop.
    return g_keep_recursing ? recurse(depth + 1) + frame[0] : frame[0];
}

} // namespace

void ReportWriter::text(std::string_view text) noexcept {
    while (!text.empty()) {
        if (m_used == sizeof(m_buffer)) {
            flush();
        }
        const std::size_t count = std::min(text.size(), sizeof(m_buffer) - m_used);
        std::memcpy(m_buffer + m_used, text.data(), count);
        m_used += count;
        text.remove_prefix(count);
    }
}

void ReportWriter::text(const char* text, std::size_t limit) noexcept {
    std::size_t length = 0;
    while (length < limit && text[length] != '\0') {
        ++length;
    }
    this->text(std::string_view(text, length));
}

void ReportWriter::number(std::uint64_t value) noexcept {
    number(value, 1);
}

void ReportWriter::number(std::uint64_t value, int digits) noexcept {
    char reversed[24];
    int count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < 24);
    while (count < digits && count < 24) {
        reversed[count++] = '0';
    }
    char ordered[24];
    for (int i = 0; i < count; ++i) {
        ordered[i] = reversed[count - 1 - i];
    }
    text(std::string_view(ordered, static_cast<std::size_t>(count)));
}

void ReportWriter::hex(std::uint64_t value) noexcept {
    char digits[18] = {'0', 'x'};
    for (int i = 0; i < 16; ++i) {
        const auto nibble = static_cast<unsigned>((value >> (60 - 4 * i)) & 0xF);
        digits[2 + i] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    text(std::string_view(digits, sizeof(digits)));
}

void ReportWriter::flush() noexcept {
    if (m_used > 0) {
        m_sink(m_target, m_buffer, m_used);
        m_used = 0;
    }
}

DateTime to_date_time(std::int64_t seconds) noexcept {
    // Days since 1970 to a civil date, from Howard Hinnant's date algorithms.
    std::int64_t days = seconds / 86400;
    std::int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t month_index = (5 * day_of_year + 2) / 153;
    const std::int64_t day = day_of_year - (153 * month_index + 2) / 5 + 1;
    const std::int64_t month = month_index < 10 ? month_index + 3 : month_index - 9;
    const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    return DateTime{
        .year = static_cast<int>(year),
        .month = static_cast<int>(month),
        .day = static_cast<int>(day),
        .hour = static_cast<int>(rest / 3600),
        .minute = static_cast<int>(rest % 3600 / 60),
        .second = static_cast<int>(rest % 60),
    };
}

std::size_t report_stem(char* out, std::size_t size, std::int64_t seconds,
                        std::uint64_t process) noexcept {
    struct Target {
        char* out;
        std::size_t size;
        std::size_t used;
        bool overflow;
    } target{out, size, 0, false};
    {
        ReportWriter writer(
            [](void* raw, const char* data, std::size_t count) {
                auto& into = *static_cast<Target*>(raw);
                if (into.used + count >= into.size) {
                    into.overflow = true;
                    return;
                }
                std::memcpy(into.out + into.used, data, count);
                into.used += count;
            },
            &target);
        const DateTime time = to_date_time(seconds);
        writer.text(g_state.directory, sizeof(g_state.directory));
        writer.text("/crash-");
        writer.number(static_cast<std::uint64_t>(time.year), 4);
        writer.number(static_cast<std::uint64_t>(time.month), 2);
        writer.number(static_cast<std::uint64_t>(time.day), 2);
        writer.text("-");
        writer.number(static_cast<std::uint64_t>(time.hour), 2);
        writer.number(static_cast<std::uint64_t>(time.minute), 2);
        writer.number(static_cast<std::uint64_t>(time.second), 2);
        writer.text("-");
        writer.number(process);
    }
    if (target.overflow) {
        return 0;
    }
    out[target.used] = '\0';
    return target.used;
}

void write_header(ReportWriter& writer, std::string_view reason, std::uint64_t process,
                  std::uint64_t thread, std::int64_t seconds) noexcept {
    const Version version = engine_version();
    const DateTime time = to_date_time(seconds);
    writer.text("=== Dream Engine crash report ===\nApplication: ");
    writer.text(g_state.application, sizeof(g_state.application));
    writer.text("\nEngine: ");
    writer.number(version.major_number);
    writer.text(".");
    writer.number(version.minor_number);
    writer.text(".");
    writer.number(version.patch_number);
    writer.text("\nTime: ");
    writer.number(static_cast<std::uint64_t>(time.year), 4);
    writer.text("-");
    writer.number(static_cast<std::uint64_t>(time.month), 2);
    writer.text("-");
    writer.number(static_cast<std::uint64_t>(time.day), 2);
    writer.text(" ");
    writer.number(static_cast<std::uint64_t>(time.hour), 2);
    writer.text(":");
    writer.number(static_cast<std::uint64_t>(time.minute), 2);
    writer.text(":");
    writer.number(static_cast<std::uint64_t>(time.second), 2);
    writer.text(" UTC\nProcess: ");
    writer.number(process);
    writer.text(", thread ");
    writer.number(thread);
    writer.text("\nReason: ");
    writer.text(reason);
    writer.text("\nFrame: ");
    const std::uint64_t frame = g_frame.load(std::memory_order_relaxed);
    if (frame == std::numeric_limits<std::uint64_t>::max()) {
        writer.text("none, the main loop hadn't started");
    } else {
        writer.number(frame);
    }
    writer.text("\n");
}

void write_details(ReportWriter& writer) noexcept {
    const std::size_t count = std::min(g_detail_count.load(std::memory_order_acquire),
                                       g_details.size());
    if (count == 0) {
        return;
    }
    writer.text("\nDetails:\n");
    for (std::size_t i = 0; i < count; ++i) {
        writer.text("  ");
        writer.text(g_details[i].key, sizeof(g_details[i].key));
        writer.text(": ");
        writer.text(g_details[i].value, sizeof(g_details[i].value));
        writer.text("\n");
    }
}

void write_heap(ReportWriter& writer) noexcept {
    if (!is_heap_counted()) {
        return;
    }
    const HeapStats heap = heap_stats();
    writer.text("  C++ heap in use: ");
    writer.number(heap.live_bytes / 1024);
    writer.text(" KB, in ");
    writer.number(heap.allocations - heap.frees);
    writer.text(" allocations\n");
}

void write_recent_log(ReportWriter& writer) noexcept {
    const std::uint64_t count = g_log_count.load(std::memory_order_acquire);
    writer.text("\nRecent log, oldest first:\n");
    if (count == 0) {
        writer.text("  (nothing was logged)\n");
        return;
    }
    const std::uint64_t first = count > k_log_entries ? count - k_log_entries : 0;
    for (std::uint64_t index = first; index < count; ++index) {
        const LogEntry& entry = g_log[index % k_log_entries];
        writer.text("  [");
        writer.text(level_name(entry.level));
        writer.text("] ");
        writer.text(entry.text, sizeof(entry.text));
        writer.text("\n");
    }
}

bool begin_report() noexcept {
    int expected = static_cast<int>(Progress::Idle);
    if (g_progress.compare_exchange_strong(expected, static_cast<int>(Progress::Reporting))) {
        return true;
    }
    if (expected == static_cast<int>(Progress::Reported)) {
        return false;
    }
    // Another thread is writing the report and will end the process.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void end_report() noexcept {
    g_progress.store(static_cast<int>(Progress::Reported), std::memory_order_release);
}

void reset_progress() noexcept {
    g_progress.store(static_cast<int>(Progress::Idle), std::memory_order_release);
}

void start_log_capture() {
    add_log_sink(g_log_ring);
}

void stop_log_capture() {
    remove_log_sink(g_log_ring);
}

#if !defined(_WIN32) && !(defined(__linux__) && !defined(__ANDROID__))
// Crash reports are for PC for now.
bool install_platform() noexcept {
    return false;
}

void uninstall_platform() noexcept {}
#endif

} // namespace crash

Result<std::unique_ptr<CrashHandler>> CrashHandler::install(const CrashReportSettings& settings) {
    if (crash::g_installed.exchange(true)) {
        return Error{ErrorCode::InvalidArgument, "A crash handler is already installed"};
    }
    const auto fail = [](ErrorCode code, std::string message) {
        crash::g_installed.store(false);
        return Error{code, std::move(message)};
    };

    std::error_code error;
    std::filesystem::create_directories(settings.directory, error);
    std::filesystem::path directory = std::filesystem::absolute(settings.directory, error);
    if (error || !std::filesystem::is_directory(directory, error)) {
        return fail(ErrorCode::IoError,
                    std::format("Crash reports can't be written to '{}': {}",
                                settings.directory.string(), error.message()));
    }
    const std::u8string utf8 = directory.generic_u8string();
    // Room for the file name after it.
    if (utf8.size() + 64 > sizeof(crash::g_state.directory)) {
        return fail(ErrorCode::InvalidArgument, "The crash report folder's path is too long");
    }
    crash::copy_text(crash::g_state.directory, sizeof(crash::g_state.directory),
                     std::string_view(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    crash::copy_text(crash::g_state.application, sizeof(crash::g_state.application),
                     settings.application);
    crash::g_state.minidump = settings.minidump;

    crash::reset_progress();
    crash::start_log_capture();
    if (!crash::install_platform()) {
        crash::stop_log_capture();
        return fail(ErrorCode::NotFound, "Crash reports aren't available on this platform");
    }
    return std::unique_ptr<CrashHandler>(new CrashHandler(std::move(directory)));
}

CrashHandler::~CrashHandler() {
    crash::uninstall_platform();
    crash::stop_log_capture();
    crash::g_installed.store(false);
}

void set_crash_detail(std::string_view key, std::string_view value) {
    using namespace crash;
    const std::lock_guard lock(g_details_mutex);
    const std::size_t count = g_detail_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        if (std::string_view(g_details[i].key) == key.substr(0, sizeof(g_details[i].key) - 1)) {
            copy_text(g_details[i].value, sizeof(g_details[i].value), value);
            return;
        }
    }
    if (count < g_details.size()) {
        copy_text(g_details[count].key, sizeof(g_details[count].key), key);
        copy_text(g_details[count].value, sizeof(g_details[count].value), value);
        g_detail_count.store(count + 1, std::memory_order_release);
    }
}

void set_crash_frame(std::uint64_t frame) noexcept {
    crash::g_frame.store(frame, std::memory_order_relaxed);
}

namespace {

constexpr std::array<std::pair<CrashKind, std::string_view>, 6> k_kind_names{{
    {CrashKind::NullPointer, "null_pointer"},
    {CrashKind::DivideByZero, "divide_by_zero"},
    {CrashKind::StackOverflow, "stack_overflow"},
    {CrashKind::Abort, "abort"},
    {CrashKind::Assert, "assert"},
    {CrashKind::Terminate, "terminate"},
}};

} // namespace

std::optional<CrashKind> crash_kind_from_name(std::string_view name) noexcept {
    for (const auto& [kind, kind_name] : k_kind_names) {
        if (kind_name == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view crash_kind_name(CrashKind kind) noexcept {
    for (const auto& [known, name] : k_kind_names) {
        if (known == kind) {
            return name;
        }
    }
    return "unknown";
}

void simulate_crash(CrashKind kind) {
    switch (kind) {
    case CrashKind::NullPointer: {
        // The pointer is read from a volatile, so the compiler can't tell it is null, and the
        // write is volatile too, so it can't be dropped as unused before the abort() below.
        volatile int* volatile pointer = nullptr;
        *pointer = 1;
        break;
    }
    case CrashKind::DivideByZero: {
        // Both from volatiles, or the compiler turns 1 / x into comparisons, with no division.
        volatile int numerator = 1;
        volatile int zero = 0;
        volatile int result = numerator / zero;
        static_cast<void>(result);
        break;
    }
    case CrashKind::StackOverflow:
        static_cast<void>(crash::recurse(0));
        break;
    case CrashKind::Abort:
        std::abort();
    case CrashKind::Assert:
        DREAM_VERIFY(kind != CrashKind::Assert, "A simulated assertion failure");
        break;
    case CrashKind::Terminate:
        std::terminate();
    }
    // Reached only if the crash above didn't happen, such as a division by zero on a processor
    // that doesn't trap it, or an assert handler that returns.
    std::abort();
}

} // namespace dream::core
