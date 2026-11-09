#include "dream/core/log.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace dream::core;

namespace {

class CaptureSink final : public LogSink {
public:
    void write(const LogMessage& message) override {
        texts.emplace_back(message.text);
        levels.push_back(message.level);
        last_file = message.location.file_name();
    }

    std::vector<std::string> texts;
    std::vector<LogLevel> levels;
    std::string last_file;
};

// Captures messages without printing them, and restores the global settings afterwards.
class LogFixture {
public:
    LogFixture() : m_previous_level(log_level()) {
        set_console_log_enabled(false);
        add_log_sink(sink);
    }
    ~LogFixture() {
        remove_log_sink(sink);
        set_console_log_enabled(true);
        set_log_level(m_previous_level);
    }

    CaptureSink sink;

private:
    LogLevel m_previous_level;
};

} // namespace

TEST_CASE_FIXTURE(LogFixture, "messages are formatted and delivered with their source location") {
    set_log_level(LogLevel::Info);

    DREAM_LOG_INFO("value {} and {}", 42, "text");

    REQUIRE(sink.texts.size() == 1);
    CHECK(sink.texts[0] == "value 42 and text");
    CHECK(sink.levels[0] == LogLevel::Info);
    CHECK(sink.last_file.find("log_test.cpp") != std::string::npos);
}

TEST_CASE_FIXTURE(LogFixture, "messages below the current level are dropped") {
    set_log_level(LogLevel::Warning);

    DREAM_LOG_INFO("dropped");
    DREAM_LOG_WARNING("kept");
    DREAM_LOG_ERROR("also kept");

    REQUIRE(sink.texts.size() == 2);
    CHECK(sink.texts[0] == "kept");
    CHECK(sink.texts[1] == "also kept");
}

TEST_CASE_FIXTURE(LogFixture, "a removed sink receives nothing") {
    set_log_level(LogLevel::Info);

    remove_log_sink(sink);
    DREAM_LOG_INFO("not seen");

    CHECK(sink.texts.empty());
}
