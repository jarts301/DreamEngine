#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"
#include "dream/crash_upload/crash_upload.h"
#include "dream/platform/platform.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace dream::runtime {

// Uploads the crash reports that earlier runs saved, in the background, with the project's
// crash_upload module (ADR 0043). run_program starts it once the platform is up, when the project
// lists a module that sends:
//
//     [modules.windows]
//     crash_upload = ["sentry"]          # Or "backtrace", or "null" for none.
//
//     [crash_upload]
//     enabled = true                     # All optional.
//     max_reports = 5                    # Uploaded per start, oldest first.
//
// Each report, with its minidump where there is one, moves into a "sent" folder beside it once its
// service has it. One that fails stays, and is tried the next time; a network error stops the rest
// until then. Reports describe the player's device, so a game should ask its players before
// listing a module that sends, where the law asks for that. docs/reports/crash-reports.md explains
// setting up each service.
class CrashUploads {
public:
    struct Settings {
        bool enabled = true;
        std::uint32_t max_reports = 5;
        // Where Core writes reports, [crash_report] directory.
        std::filesystem::path directory;
        std::string application;
        std::string platform;
    };

    [[nodiscard]] static core::Result<Settings> read_settings(const core::Config& config);

    // Starts uploading on a thread of its own. `platform` must outlive this.
    CrashUploads(std::unique_ptr<crash_upload::CrashUploader> uploader, platform::Platform& platform,
                 Settings settings);

    // Waits for an upload under way to end; the rest wait for the next start.
    ~CrashUploads();

    CrashUploads(const CrashUploads&) = delete;
    CrashUploads& operator=(const CrashUploads&) = delete;

    // Waits for every upload to end, for tests.
    void wait();

    // How many reports were sent, and how many failed, so far.
    [[nodiscard]] std::uint32_t sent() const noexcept {
        return m_sent.load();
    }
    [[nodiscard]] std::uint32_t failed() const noexcept {
        return m_failed.load();
    }

private:
    void run();

    std::unique_ptr<crash_upload::CrashUploader> m_uploader;
    platform::Platform& m_platform;
    Settings m_settings;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint32_t> m_sent{0};
    std::atomic<std::uint32_t> m_failed{0};
    std::thread m_thread;
};

} // namespace dream::runtime
