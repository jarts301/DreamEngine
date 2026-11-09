#pragma once

#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>
#include <utility>

namespace dream::core {

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error, Fatal };

struct LogMessage {
    LogLevel level = LogLevel::Info;
    std::string_view text;
    std::source_location location;
};

// Receives every message at or above the current level. Calls are serialized, so an implementation
// does not need its own locking, but it must not log or assert from inside write().
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogMessage& message) = 0;
};

// The sink is not owned and must be removed before it is destroyed.
void add_log_sink(LogSink& sink);
void remove_log_sink(LogSink& sink);

void set_log_level(LogLevel level) noexcept;
[[nodiscard]] LogLevel log_level() noexcept;

// Controls the built-in output: Logcat on Android, stdout and stderr elsewhere. On by default.
void set_console_log_enabled(bool enabled) noexcept;

void write_log(LogLevel level, const std::source_location& location, std::string_view text);

template <typename... Args>
void log_message(LogLevel level, const std::source_location& location,
                 std::format_string<Args...> format, Args&&... args) {
    if (level >= log_level()) {
        write_log(level, location, std::format(format, std::forward<Args>(args)...));
    }
}

} // namespace dream::core

#define DREAM_LOG_TRACE(...)                                                                       \
    ::dream::core::log_message(::dream::core::LogLevel::Trace, std::source_location::current(),    \
                               __VA_ARGS__)
#define DREAM_LOG_DEBUG(...)                                                                       \
    ::dream::core::log_message(::dream::core::LogLevel::Debug, std::source_location::current(),    \
                               __VA_ARGS__)
#define DREAM_LOG_INFO(...)                                                                        \
    ::dream::core::log_message(::dream::core::LogLevel::Info, std::source_location::current(),     \
                               __VA_ARGS__)
#define DREAM_LOG_WARNING(...)                                                                     \
    ::dream::core::log_message(::dream::core::LogLevel::Warning, std::source_location::current(),  \
                               __VA_ARGS__)
#define DREAM_LOG_ERROR(...)                                                                       \
    ::dream::core::log_message(::dream::core::LogLevel::Error, std::source_location::current(),    \
                               __VA_ARGS__)
#define DREAM_LOG_FATAL(...)                                                                       \
    ::dream::core::log_message(::dream::core::LogLevel::Fatal, std::source_location::current(),    \
                               __VA_ARGS__)
