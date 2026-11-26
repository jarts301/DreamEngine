// Uploads crash reports to Sentry (ADR 0043), as its documentation describes, with nothing but
// HTTP requests: a report with a minidump goes to the project's minidump endpoint, with the text
// report attached; one without, as on Android, goes as a fatal event in an envelope, with the text
// report attached too.
//
//     [crash_upload.sentry]
//     dsn = "https://<public key>@<host>/<project id>"   # From the project's Client Keys page.
//     environment = "production"                          # Optional.

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

struct Dsn {
    std::string scheme;
    std::string key;
    std::string host;
    std::string project;
};

core::Result<Dsn> parse_dsn(std::string_view text) {
    const auto invalid = [&] {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("Setting 'crash_upload.sentry.dsn' must look like "
                                       "https://<key>@<host>/<project>, not '{}'",
                                       text)};
    };
    const std::size_t scheme_end = text.find("://");
    const std::size_t at = text.find('@');
    const std::size_t slash = text.rfind('/');
    if (scheme_end == std::string_view::npos || at == std::string_view::npos ||
        slash == std::string_view::npos || at < scheme_end || slash < at) {
        return invalid();
    }
    Dsn dsn{.scheme = std::string(text.substr(0, scheme_end)),
            .key = std::string(text.substr(scheme_end + 3, at - scheme_end - 3)),
            .host = std::string(text.substr(at + 1, slash - at - 1)),
            .project = std::string(text.substr(slash + 1))};
    // A key may carry a secret after a colon, which newer projects don't use.
    dsn.key = dsn.key.substr(0, dsn.key.find(':'));
    if (dsn.key.empty() || dsn.host.empty() || dsn.project.empty()) {
        return invalid();
    }
    return dsn;
}

class SentryCrashUploader final : public CrashUploader {
public:
    SentryCrashUploader(Dsn dsn, std::string text, std::string environment)
        : m_dsn(std::move(dsn)), m_text(std::move(text)), m_environment(std::move(environment)) {}

    bool sends() const override {
        return true;
    }

    core::Result<void> upload(const Report& report, const Sender& send) override {
        const platform::HttpRequest request =
            report.minidump.empty() ? event_request(report) : minidump_request(report);
        const core::Result<platform::HttpResponse> answer = send(request);
        if (!answer.has_value()) {
            return answer.error();
        }
        if (answer.value().status < 200 || answer.value().status >= 300) {
            return core::Error{core::ErrorCode::IoError,
                               std::format("Sentry refused '{}' with status {}: {}", report.name,
                                           answer.value().status,
                                           first_line(answer.value().body))};
        }
        return {};
    }

private:
    [[nodiscard]] std::string base() const {
        return std::format("{}://{}/api/{}", m_dsn.scheme, m_dsn.host, m_dsn.project);
    }

    // Sentry's release, "<application>@<version>", which its symbols are uploaded for; nothing
    // for a game without a version.
    [[nodiscard]] static std::string release(const Report& report) {
        if (report.application_version.empty()) {
            return {};
        }
        return std::format(",\"release\":{}",
                           json_string(report.application + "@" + report.application_version));
    }

    [[nodiscard]] std::string tags(const Report& report) const {
        return std::format("{{\"application\":{},\"platform\":{},\"engine\":{}}}",
                           json_string(report.application), json_string(report.platform),
                           json_string(report.engine_version));
    }

    // The minidump, which Sentry symbolicates, and the text report as an attachment.
    [[nodiscard]] platform::HttpRequest minidump_request(const Report& report) const {
        MultipartBody body;
        body.add_file("upload_file_minidump", report.name + ".dmp", "application/octet-stream",
                      report.minidump);
        body.add_file("report", report.name + ".txt", "text/plain", bytes_of(report.text));
        std::string sentry = std::format("{{\"level\":\"fatal\",\"tags\":{}", tags(report));
        if (!m_environment.empty()) {
            sentry += std::format(",\"environment\":{}", json_string(m_environment));
        }
        sentry += release(report);
        body.add_field("sentry", sentry + "}");
        return platform::HttpRequest{
            .url = std::format("{}/minidump/?sentry_key={}", base(), m_dsn.key),
            .headers = {{"Content-Type", body.content_type()}},
            .body = body.finish(),
            .timeout_seconds = 10};
    }

    // A fatal event named after the report's first line, with the report attached.
    [[nodiscard]] platform::HttpRequest event_request(const Report& report) const {
        const std::string id = random_id();
        std::string event = std::format(
            "{{\"event_id\":\"{}\",\"timestamp\":{},\"platform\":\"native\",\"level\":\"fatal\","
            "\"logger\":\"dreamengine\",\"message\":{{\"formatted\":{}}},\"tags\":{}",
            id, unix_seconds(), json_string(first_line(report.text)), tags(report));
        if (!m_environment.empty()) {
            event += std::format(",\"environment\":{}", json_string(m_environment));
        }
        event += release(report);
        event += "}";
        std::string envelope = std::format("{{\"event_id\":\"{}\",\"dsn\":{}}}\n", id,
                                           json_string(m_text));
        envelope += std::format("{{\"type\":\"event\",\"length\":{}}}\n{}\n", event.size(), event);
        envelope += std::format("{{\"type\":\"attachment\",\"length\":{},\"filename\":\"{}.txt\","
                                "\"content_type\":\"text/plain\"}}\n{}\n",
                                report.text.size(), report.name, report.text);
        return platform::HttpRequest{
            .url = base() + "/envelope/",
            .headers = {{"Content-Type", "application/x-sentry-envelope"},
                        {"X-Sentry-Auth",
                         std::format("Sentry sentry_version=7, sentry_client=dreamengine/{}, "
                                     "sentry_key={}",
                                     report.engine_version, m_dsn.key)}},
            .body = bytes_of(envelope),
            .timeout_seconds = 10};
    }

    Dsn m_dsn;
    std::string m_text;
    std::string m_environment;
};

core::Result<std::unique_ptr<CrashUploader>> create(const core::Config& config) {
    const std::optional<std::string_view> dsn = config.get_string("crash_upload.sentry.dsn");
    if (!dsn.has_value()) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "The sentry crash upload needs [crash_upload.sentry] dsn, from the "
                           "Sentry project's Client Keys page"};
    }
    core::Result<Dsn> parsed = parse_dsn(*dsn);
    if (!parsed.has_value()) {
        return parsed.error();
    }
    const std::string environment(
        config.get_string("crash_upload.sentry.environment").value_or(""));
    return std::unique_ptr<CrashUploader>{std::make_unique<SentryCrashUploader>(
        std::move(parsed).value(), std::string(*dsn), environment)};
}

} // namespace

[[nodiscard]] core::Result<void> register_crash_upload_sentry(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<CrashUploader>{.name = "sentry", .create = &create});
}

} // namespace dream::modules
