#include "dream/runtime/crash_reports.h"

#include <format>
#include <string>
#include <string_view>

namespace dream::runtime {
namespace {

core::Error setting_error(std::string_view key, std::string_view problem) {
    return core::Error{core::ErrorCode::InvalidArgument,
                       std::format("Setting 'crash_report.{}' {}", key, problem)};
}

} // namespace

core::Result<CrashReportConfig> CrashReportConfig::from_config(const core::Config& config) {
    CrashReportConfig settings;
#if defined(__ANDROID__)
    settings.enabled = false;
#endif

    for (const auto& [key, value] : {std::pair{"enabled", &settings.enabled},
                                     std::pair{"minidump", &settings.minidump}}) {
        const std::string full_key = std::format("crash_report.{}", key);
        if (config.contains(full_key)) {
            const std::optional<bool> read = config.get_bool(full_key);
            if (!read.has_value()) {
                return setting_error(key, "must be true or false");
            }
            *value = *read;
        }
    }

    if (config.contains("crash_report.directory")) {
        const std::optional<std::string_view> directory =
            config.get_string("crash_report.directory");
        if (!directory.has_value() || directory->empty()) {
            return setting_error("directory", "must be the path of a folder");
        }
        // Configuration text is UTF-8, which a char8_t string keeps on Windows.
        std::u8string utf8;
        for (const char c : *directory) {
            utf8.push_back(static_cast<char8_t>(c));
        }
        settings.directory = std::filesystem::path(utf8);
    }

    if (config.contains("crash_report.simulate")) {
        const std::optional<std::string_view> name = config.get_string("crash_report.simulate");
        if (!name.has_value()) {
            return setting_error("simulate", "must be a string");
        }
        if (*name != "none") {
            settings.simulate = core::crash_kind_from_name(*name);
            if (!settings.simulate.has_value()) {
                return setting_error(
                    "simulate", std::format("has the unknown value '{}'. Use \"none\", "
                                            "\"null_pointer\", \"divide_by_zero\", "
                                            "\"stack_overflow\", \"abort\", \"assert\" or "
                                            "\"terminate\"",
                                            *name));
            }
        }
    }

    if (config.contains("crash_report.simulate_after_frames")) {
        const std::optional<std::int64_t> frames =
            config.get_int("crash_report.simulate_after_frames");
        if (!frames.has_value() || *frames < 0) {
            return setting_error("simulate_after_frames", "must be a whole number, 0 or more");
        }
        settings.simulate_after_frames = static_cast<std::uint64_t>(*frames);
    }
    return settings;
}

} // namespace dream::runtime
