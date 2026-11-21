#include "dream/runtime/crash_uploads.h"

#include "dream/core/log.h"
#include "dream/core/version.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

namespace dream::runtime {
namespace {

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::transform(chars.begin(), chars.end(), bytes.begin(),
                   [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

// The reports Core saved: each crash-*.txt in the folder, oldest first.
std::vector<std::filesystem::path> saved_reports(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> reports;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::filesystem::path& path = entry.path();
        if (entry.is_regular_file(error) && path.extension() == ".txt" &&
            path.filename().string().starts_with("crash-")) {
            reports.push_back(path);
        }
    }
    // Their names start with the date and time, so this is oldest first.
    std::ranges::sort(reports);
    return reports;
}

void move_to_sent(const std::filesystem::path& file) {
    if (!std::filesystem::exists(file)) {
        return;
    }
    const std::filesystem::path sent = file.parent_path() / "sent";
    std::error_code error;
    std::filesystem::create_directories(sent, error);
    std::filesystem::rename(file, sent / file.filename(), error);
    if (error) {
        DREAM_LOG_WARNING("Could not move '{}' into the sent folder: {}", file.string(),
                          error.message());
    }
}

} // namespace

core::Result<CrashUploads::Settings> CrashUploads::read_settings(const core::Config& config) {
    Settings settings;
    if (config.contains("crash_upload.enabled")) {
        const std::optional<bool> enabled = config.get_bool("crash_upload.enabled");
        if (!enabled.has_value()) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Setting 'crash_upload.enabled' must be true or false"};
        }
        settings.enabled = *enabled;
    }
    if (config.contains("crash_upload.max_reports")) {
        const std::optional<std::int64_t> count = config.get_int("crash_upload.max_reports");
        if (!count.has_value() || *count < 1 || *count > 100) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Setting 'crash_upload.max_reports' must be from 1 to 100"};
        }
        settings.max_reports = static_cast<std::uint32_t>(*count);
    }
    return settings;
}

CrashUploads::CrashUploads(std::unique_ptr<crash_upload::CrashUploader> uploader,
                           platform::Platform& platform, Settings settings)
    : m_uploader(std::move(uploader)), m_platform(platform), m_settings(std::move(settings)) {
    if (m_uploader != nullptr && m_uploader->sends() && m_settings.enabled) {
        m_thread = std::thread([this] { run(); });
    }
}

CrashUploads::~CrashUploads() {
    m_stop = true;
    wait();
}

void CrashUploads::wait() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void CrashUploads::run() {
    const std::vector<std::filesystem::path> reports = saved_reports(m_settings.directory);
    if (reports.empty()) {
        return;
    }
    const core::Version version = core::engine_version();
    const crash_upload::Sender send = [this](const platform::HttpRequest& request) {
        return m_platform.post_http(request);
    };
    std::uint32_t tried = 0;
    for (const std::filesystem::path& text_path : reports) {
        if (m_stop || tried >= m_settings.max_reports) {
            break;
        }
        ++tried;
        const std::optional<std::vector<std::byte>> text = read_file(text_path);
        if (!text.has_value()) {
            continue;
        }
        std::filesystem::path minidump_path = text_path;
        minidump_path.replace_extension(".dmp");
        crash_upload::Report report{
            .name = text_path.stem().string(),
            .text = std::string(reinterpret_cast<const char*>(text->data()), text->size()),
            .minidump = read_file(minidump_path).value_or(std::vector<std::byte>{}),
            .application = m_settings.application,
            .platform = m_settings.platform,
            .engine_version = std::format("{}.{}.{}", version.major_number,
                                          version.minor_number, version.patch_number)};
        const core::Result<void> uploaded = m_uploader->upload(report, send);
        if (!uploaded.has_value()) {
            ++m_failed;
            DREAM_LOG_WARNING("Crash report '{}' wasn't uploaded, and will be tried again next "
                              "time: {}",
                              report.name, uploaded.error().message);
            // Most failures are the network's, which the next report would meet too.
            break;
        }
        ++m_sent;
        DREAM_LOG_INFO("Crash report '{}' uploaded", report.name);
        move_to_sent(text_path);
        move_to_sent(minidump_path);
    }
}

} // namespace dream::runtime
