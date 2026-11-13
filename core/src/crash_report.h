#pragma once

// What the platform crash handlers share. Everything here runs after a crash, so nothing may
// allocate or take a lock: the heap or a lock may be what broke. Text goes through a fixed buffer
// straight to the file.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dream::core::crash {

// Writes text through a fixed buffer to `sink`, which takes raw bytes.
class ReportWriter {
public:
    using Sink = void (*)(void* target, const char* data, std::size_t size);

    ReportWriter(Sink sink, void* target) noexcept : m_sink(sink), m_target(target) {}
    ~ReportWriter() {
        flush();
    }

    ReportWriter(const ReportWriter&) = delete;
    ReportWriter& operator=(const ReportWriter&) = delete;

    void text(std::string_view text) noexcept;
    // Stops at the first zero byte, for text in fixed arrays.
    void text(const char* text, std::size_t limit) noexcept;
    void number(std::uint64_t value) noexcept;
    // Zero-padded to `digits`.
    void number(std::uint64_t value, int digits) noexcept;
    void hex(std::uint64_t value) noexcept;
    void flush() noexcept;

private:
    Sink m_sink;
    void* m_target;
    char m_buffer[2048];
    std::size_t m_used = 0;
};

// Text composed in fixed storage, such as a crash's reason. Stops quietly when full.
class FixedText {
public:
    void append(std::string_view text) noexcept {
        for (const char c : text) {
            if (m_size + 1 >= sizeof(m_data)) {
                return;
            }
            m_data[m_size++] = c;
        }
    }

    void append_hex(std::uint64_t value) noexcept {
        char digits[18] = {'0', 'x'};
        for (int i = 0; i < 16; ++i) {
            const auto nibble = static_cast<unsigned>((value >> (60 - 4 * i)) & 0xF);
            digits[2 + i] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
        append(std::string_view(digits, sizeof(digits)));
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(m_data, m_size);
    }

private:
    char m_data[320] = {};
    std::size_t m_size = 0;
};

// Turns seconds since 1970 into a UTC date and time.
struct DateTime {
    int year = 1970;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
};
[[nodiscard]] DateTime to_date_time(std::int64_t seconds) noexcept;

// Fills `out` with "<directory>/crash-YYYYMMDD-HHMMSS-<process>" and returns its length, or zero
// if it doesn't fit. The caller adds the extension.
std::size_t report_stem(char* out, std::size_t size, std::int64_t seconds,
                        std::uint64_t process) noexcept;

// The report's sections, in order.
void write_header(ReportWriter& writer, std::string_view reason, std::uint64_t process,
                  std::uint64_t thread, std::int64_t seconds) noexcept;
void write_details(ReportWriter& writer) noexcept;
void write_heap(ReportWriter& writer) noexcept;
void write_recent_log(ReportWriter& writer) noexcept;

// Settings copied into fixed storage at install, for use after a crash.
struct State {
    char directory[1024] = {};
    char application[128] = {};
    bool minidump = true;
};
extern State g_state;

// Whether a report is being written or has been, so a crash while reporting, or a second crash
// in another thread, doesn't start another. Each handler calls begin_report first.
enum class Progress : int { Idle, Reporting, Reported };

// Returns true if this call should write the report. If another thread is writing one, this
// waits for the process to end. If one was written already, it returns false and the crash can
// carry on to end the program.
[[nodiscard]] bool begin_report() noexcept;
void end_report() noexcept;
void reset_progress() noexcept;

// The platform's part of CrashHandler.
[[nodiscard]] bool install_platform() noexcept;
void uninstall_platform() noexcept;

// Keeps the last log messages for the report.
void start_log_capture();
void stop_log_capture();

} // namespace dream::core::crash
