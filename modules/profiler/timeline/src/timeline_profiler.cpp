#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/profiler/profiler.h"

#include <timeline/timeline.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace dream::modules {
namespace {

using profiler::Profiler;

constexpr std::int64_t k_default_max_events = 1'000'000;

// Configuration text is UTF-8. Building the path from char8_t keeps non-ASCII names intact on
// Windows, where a char string would be read in the system's code page.
std::filesystem::path path_from_utf8(std::string_view text) {
    std::u8string converted;
    converted.reserve(text.size());
    for (const char c : text) {
        converted.push_back(static_cast<char8_t>(c));
    }
    return std::filesystem::path(converted);
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

core::Error setting_error(std::string_view key, std::string_view problem) {
    return core::Error{core::ErrorCode::InvalidArgument,
                       std::format("Setting 'profiler.timeline.{}' {}", key, problem)};
}

core::Result<std::int64_t> read_whole_number(const core::Config& config, std::string_view key,
                                             std::int64_t fallback, std::int64_t minimum) {
    const std::string full_key = std::format("profiler.timeline.{}", key);
    if (!config.contains(full_key)) {
        return fallback;
    }
    const std::optional<std::int64_t> value = config.get_int(full_key);
    if (!value.has_value()) {
        return setting_error(key, "must be a whole number");
    }
    if (*value < minimum) {
        return setting_error(key, std::format("must be at least {}", minimum));
    }
    return *value;
}

// Records into a timeline::Recorder and writes it, on flush, as a Chrome trace file that
// https://ui.perfetto.dev and chrome://tracing open.
class TimelineProfiler final : public Profiler {
public:
    TimelineProfiler(timeline::Settings settings, std::filesystem::path path,
                     std::string process_name, std::int64_t max_events)
        : m_recorder(std::move(settings)), m_path(std::move(path)),
          m_process_name(std::move(process_name)), m_max_events(max_events) {}

    void begin_zone(const core::ProfileSite& site, std::string_view detail) override {
        m_recorder.begin_zone(site.name, site.file, site.line, detail);
    }

    void end_zone(const core::ProfileSite& /*site*/) override {
        m_recorder.end_zone();
    }

    void mark_frame() override {
        m_recorder.mark_frame();
    }

    void set_thread_name(std::string_view name) override {
        m_recorder.set_thread_name(name);
    }

    void record_value(const char* name, double value) override {
        m_recorder.record_value(name, value);
    }

    void record_message(std::string_view text) override {
        m_recorder.record_message(text);
    }

    core::Result<void> flush() override {
        // Two flushes at once would write the same temporary file.
        const std::lock_guard lock(m_flush_mutex);

        std::error_code error;
        const std::filesystem::path folder = m_path.parent_path();
        if (!folder.empty()) {
            std::filesystem::create_directories(folder, error);
            if (error) {
                return io_error(std::format("Could not create the folder '{}': {}",
                                            path_to_utf8(folder), error.message()));
            }
        }

        // Written beside the file, then renamed over it, so a failed write never leaves half a
        // trace where a whole one was.
        std::filesystem::path temporary = m_path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                return io_error(std::format("Could not open '{}'", path_to_utf8(temporary)));
            }
            const bool written = m_recorder.write_json(
                [&file](std::string_view piece) {
                    file.write(piece.data(), static_cast<std::streamsize>(piece.size()));
                    return static_cast<bool>(file);
                },
                m_process_name);
            file.close();
            if (!written || !file) {
                std::filesystem::remove(temporary, error);
                return io_error(std::format("Could not write '{}'", path_to_utf8(temporary)));
            }
        }
        std::filesystem::rename(temporary, m_path, error);
        if (error) {
            const std::string reason = error.message();
            std::filesystem::remove(temporary, error);
            return io_error(
                std::format("Could not replace '{}': {}", path_to_utf8(m_path), reason));
        }

        const timeline::Stats stats = m_recorder.stats();
        const std::filesystem::path shown = std::filesystem::absolute(m_path, error);
        DREAM_LOG_INFO("Profile saved to '{}' (events: {}, frames: {}). Open it at "
                       "https://ui.perfetto.dev.",
                       path_to_utf8(error ? m_path : shown), stats.recorded_events, stats.frames);
        if (stats.dropped_events > 0) {
            DREAM_LOG_WARNING("The profile is missing {} events beyond the limit of {}. Raise "
                              "'profiler.timeline.max_events', or record fewer frames with "
                              "'start_frame' and 'frames'.",
                              stats.dropped_events, m_max_events);
        }
        return {};
    }

private:
    [[nodiscard]] static core::Error io_error(std::string message) {
        return core::Error{core::ErrorCode::IoError,
                           std::format("Profile not saved. {}", std::move(message))};
    }

    timeline::Recorder m_recorder;
    std::filesystem::path m_path;
    std::string m_process_name;
    std::int64_t m_max_events;
    std::mutex m_flush_mutex;
};

// Settings, all optional:
//
//     [profiler.timeline]
//     path = "trace.json"    # Where flush writes, relative to the working directory.
//     max_events = 1000000   # About 48 bytes each. Events beyond it are dropped.
//     start_frame = 0        # Frames to skip before recording, such as while loading.
//     frames = 0             # How many frames to record. 0 means until the end.
//
// The trace is named after `title` under [app].
core::Result<std::unique_ptr<Profiler>> create(const core::Config& config) {
    if (config.contains("profiler.timeline.path") &&
        !config.get_string("profiler.timeline.path").has_value()) {
        return setting_error("path", "must be a string");
    }
    const std::string_view path =
        config.get_string("profiler.timeline.path").value_or("trace.json");
    if (path.empty()) {
        return setting_error("path", "must not be empty");
    }

    const core::Result<std::int64_t> max_events =
        read_whole_number(config, "max_events", k_default_max_events, 1);
    if (!max_events.has_value()) {
        return max_events.error();
    }
    const core::Result<std::int64_t> start_frame = read_whole_number(config, "start_frame", 0, 0);
    if (!start_frame.has_value()) {
        return start_frame.error();
    }
    const core::Result<std::int64_t> frames = read_whole_number(config, "frames", 0, 0);
    if (!frames.has_value()) {
        return frames.error();
    }
    if (static_cast<std::uint64_t>(max_events.value()) >
        std::numeric_limits<std::size_t>::max() / 2) {
        return setting_error("max_events", "is too large");
    }

    timeline::Settings settings{
        .max_events = static_cast<std::size_t>(max_events.value()),
        .start_frame = static_cast<std::uint64_t>(start_frame.value()),
        .frame_count = static_cast<std::uint64_t>(frames.value()),
    };
    return std::unique_ptr<Profiler>{std::make_unique<TimelineProfiler>(
        std::move(settings), path_from_utf8(path),
        std::string(config.get_string("app.title").value_or("Dream Engine")),
        max_events.value())};
}

} // namespace

[[nodiscard]] core::Result<void> register_profiler_timeline(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Profiler>{.name = "timeline", .create = &create});
}

} // namespace dream::modules
