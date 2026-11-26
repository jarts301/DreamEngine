// Uploads crash reports to Backtrace (ADR 0043), as its documentation describes, with nothing but
// HTTP requests: a report with a minidump goes to the minidump endpoint, with the text report
// attached; one without, as on Android, goes as a JSON crash with the report as an annotation.
//
//     [crash_upload.backtrace]
//     url = "https://submit.backtrace.io/<universe>/<token>"   # The project's submission URL.

#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/crash_upload/crash_upload.h"
#include "dream/crash_upload/request_parts.h"

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dream::modules {
namespace {

using namespace crash_upload;

class BacktraceCrashUploader final : public CrashUploader {
public:
    explicit BacktraceCrashUploader(std::string url) : m_url(std::move(url)) {}

    bool sends() const override {
        return true;
    }

    core::Result<void> upload(const Report& report, const Sender& send) override {
        const platform::HttpRequest request =
            report.minidump.empty() ? json_request(report) : minidump_request(report);
        const core::Result<platform::HttpResponse> answer = send(request);
        if (!answer.has_value()) {
            return answer.error();
        }
        if (answer.value().status < 200 || answer.value().status >= 300) {
            return core::Error{core::ErrorCode::IoError,
                               std::format("Backtrace refused '{}' with status {}: {}",
                                           report.name, answer.value().status,
                                           first_line(answer.value().body))};
        }
        return {};
    }

private:
    // The minidump, the text report as an attachment, and attributes to sort crashes by.
    [[nodiscard]] platform::HttpRequest minidump_request(const Report& report) const {
        MultipartBody body;
        body.add_file("upload_file_minidump", report.name + ".dmp", "application/octet-stream",
                      report.minidump);
        body.add_file("attachment_" + report.name + ".txt", report.name + ".txt", "text/plain",
                      bytes_of(report.text));
        body.add_field("application", report.application);
        body.add_field("platform", report.platform);
        body.add_field("engine", report.engine_version);
        if (!report.application_version.empty()) {
            body.add_field("version", report.application_version);
        }
        return platform::HttpRequest{.url = m_url + "/minidump",
                                     .headers = {{"Content-Type", body.content_type()}},
                                     .body = body.finish(),
                                     .timeout_seconds = 10};
    }

    // Backtrace's JSON crash format, with the report's first line as the error and the whole
    // report as an annotation.
    [[nodiscard]] platform::HttpRequest json_request(const Report& report) const {
        const std::string id = random_id();
        const std::string uuid = std::format("{}-{}-{}-{}-{}", id.substr(0, 8), id.substr(8, 4),
                                             id.substr(12, 4), id.substr(16, 4), id.substr(20));
        const std::string json = std::format(
            "{{\"uuid\":\"{}\",\"timestamp\":{},\"lang\":\"c++\",\"langVersion\":\"C++20\","
            "\"agent\":\"dreamengine\",\"agentVersion\":{},"
            "\"mainThread\":\"main\",\"threads\":{{\"main\":{{\"name\":\"main\",\"fault\":true,"
            "\"stack\":[]}}}},"
            "\"attributes\":{{\"application\":{},\"version\":{},\"platform\":{},"
            "\"error.message\":{}}},"
            "\"annotations\":{{\"Report\":{}}}}}",
            uuid, unix_seconds(), json_string(report.engine_version),
            json_string(report.application), json_string(report.application_version),
            json_string(report.platform), json_string(first_line(report.text)),
            json_string(report.text));
        return platform::HttpRequest{.url = m_url + "/json",
                                     .headers = {{"Content-Type", "application/json"}},
                                     .body = bytes_of(json),
                                     .timeout_seconds = 10};
    }

    std::string m_url;
};

core::Result<std::unique_ptr<CrashUploader>> create(const core::Config& config) {
    const std::optional<std::string_view> url = config.get_string("crash_upload.backtrace.url");
    if (!url.has_value() || !(url->starts_with("https://") || url->starts_with("http://"))) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "The backtrace crash upload needs [crash_upload.backtrace] url, the "
                           "project's submission URL, such as "
                           "https://submit.backtrace.io/<universe>/<token>"};
    }
    std::string trimmed(*url);
    while (trimmed.ends_with('/')) {
        trimmed.pop_back();
    }
    return std::unique_ptr<CrashUploader>{std::make_unique<BacktraceCrashUploader>(trimmed)};
}

} // namespace

[[nodiscard]] core::Result<void> register_crash_upload_backtrace(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<CrashUploader>{.name = "backtrace", .create = &create});
}

} // namespace dream::modules
