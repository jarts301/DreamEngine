#include "dream/core/log.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#else
#include <cstdio>
#endif

namespace dream::core {
namespace {

struct LogState {
    std::mutex mutex;
    std::vector<LogSink*> sinks;
    std::atomic<LogLevel> level{LogLevel::Info};
    std::atomic<bool> console_enabled{true};
};

// Deliberately never destroyed, so code running during static destruction can still log.
LogState& state() {
    static LogState* const instance = new LogState;
    return *instance;
}

#if defined(__ANDROID__)

int android_priority(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return ANDROID_LOG_VERBOSE;
    case LogLevel::Debug:
        return ANDROID_LOG_DEBUG;
    case LogLevel::Info:
        return ANDROID_LOG_INFO;
    case LogLevel::Warning:
        return ANDROID_LOG_WARN;
    case LogLevel::Error:
        return ANDROID_LOG_ERROR;
    case LogLevel::Fatal:
        return ANDROID_LOG_FATAL;
    }
    return ANDROID_LOG_INFO;
}

void write_to_console(const LogMessage& message) {
    const std::string text(message.text);
    __android_log_write(android_priority(message.level), "DreamEngine", text.c_str());
}

#else

const char* level_name(LogLevel level) {
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
    return "UNKNOWN";
}

void write_to_console(const LogMessage& message) {
    const bool is_problem = message.level >= LogLevel::Warning;
    std::FILE* const stream = is_problem ? stderr : stdout;
    const std::string line = std::format("[{}] {}\n", level_name(message.level), message.text);
    std::fwrite(line.data(), 1, line.size(), stream);
    if (is_problem) {
        std::fflush(stream);
    }
}

#endif

} // namespace

void add_log_sink(LogSink& sink) {
    LogState& log = state();
    const std::lock_guard lock(log.mutex);
    log.sinks.push_back(&sink);
}

void remove_log_sink(LogSink& sink) {
    LogState& log = state();
    const std::lock_guard lock(log.mutex);
    std::erase(log.sinks, &sink);
}

void set_log_level(LogLevel level) noexcept {
    state().level.store(level, std::memory_order_relaxed);
}

LogLevel log_level() noexcept {
    return state().level.load(std::memory_order_relaxed);
}

void set_console_log_enabled(bool enabled) noexcept {
    state().console_enabled.store(enabled, std::memory_order_relaxed);
}

void write_log(LogLevel level, const std::source_location& location, std::string_view text) {
    const LogMessage message{.level = level, .text = text, .location = location};

    LogState& log = state();
    const std::lock_guard lock(log.mutex);
    if (log.console_enabled.load(std::memory_order_relaxed)) {
        write_to_console(message);
    }
    for (LogSink* const sink : log.sinks) {
        sink->write(message);
    }
}

} // namespace dream::core
