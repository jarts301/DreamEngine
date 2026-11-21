#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/crash_upload/crash_upload.h"

#include <doctest/doctest.h>

#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <utility>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_crash_upload_sentry(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::crash_upload;

namespace {

core::Result<std::unique_ptr<CrashUploader>> make(std::string_view config_text) {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_crash_upload_sentry(registry).has_value());
    const core::Config config = core::Config::parse(config_text).value();
    auto created = registry.create<CrashUploader>(std::vector<std::string>{"sentry"}, config);
    if (!created.has_value()) {
        return created.error();
    }
    return std::move(created.value().instance);
}

constexpr std::string_view k_config =
    "[crash_upload.sentry]\ndsn = \"https://abc123@o42.ingest.sentry.io/7\"\n"
    "environment = \"test\"";

Report report(bool minidump) {
    Report made{.name = "crash-1",
                .text = "Crash: access violation\nstack...",
                .minidump = {},
                .application = "Toppler",
                .platform = minidump ? "windows" : "android",
                .engine_version = "0.1.0"};
    if (minidump) {
        made.minidump = {std::byte{'M'}, std::byte{'D'}, std::byte{'M'}, std::byte{'P'}};
    }
    return made;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string header(const platform::HttpRequest& request, std::string_view name) {
    for (const auto& [key, value] : request.headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

} // namespace

TEST_CASE("Sentry gets a minidump with the report attached") {
    auto uploader = make(k_config);
    REQUIRE(uploader.has_value());
    CHECK(uploader.value()->sends());
    platform::HttpRequest sent;
    const auto result = uploader.value()->upload(report(true), [&](const platform::HttpRequest& r) {
        sent = r;
        return core::Result<platform::HttpResponse>(platform::HttpResponse{.status = 200});
    });
    REQUIRE(result.has_value());
    CHECK(sent.url == "https://o42.ingest.sentry.io/api/7/minidump/?sentry_key=abc123");
    CHECK(header(sent, "Content-Type").starts_with("multipart/form-data; boundary="));
    const std::string body = text_of(sent.body);
    CHECK(body.find("name=\"upload_file_minidump\"; filename=\"crash-1.dmp\"") != std::string::npos);
    CHECK(body.find("MDMP") != std::string::npos);
    CHECK(body.find("Crash: access violation") != std::string::npos);
    CHECK(body.find("\"environment\":\"test\"") != std::string::npos);
    CHECK(body.find("\"application\":\"Toppler\"") != std::string::npos);
}

TEST_CASE("Sentry gets an envelope with a fatal event where there is no minidump") {
    auto uploader = make(k_config);
    REQUIRE(uploader.has_value());
    platform::HttpRequest sent;
    REQUIRE(uploader.value()
                ->upload(report(false),
                         [&](const platform::HttpRequest& r) {
                             sent = r;
                             return core::Result<platform::HttpResponse>(
                                 platform::HttpResponse{.status = 200});
                         })
                .has_value());
    CHECK(sent.url == "https://o42.ingest.sentry.io/api/7/envelope/");
    CHECK(header(sent, "Content-Type") == "application/x-sentry-envelope");
    CHECK(header(sent, "X-Sentry-Auth").find("sentry_key=abc123") != std::string::npos);
    const std::string body = text_of(sent.body);
    // Three lines of headers and items: the envelope, the event, and the attachment.
    CHECK(body.find("{\"type\":\"event\"") != std::string::npos);
    CHECK(body.find("\"level\":\"fatal\"") != std::string::npos);
    CHECK(body.find("\"formatted\":\"Crash: access violation\"") != std::string::npos);
    CHECK(body.find("{\"type\":\"attachment\"") != std::string::npos);
    CHECK(body.find("stack...") != std::string::npos);
}

TEST_CASE("Sentry uploads fail on a refusal or a network error, keeping the report") {
    auto uploader = make(k_config);
    REQUIRE(uploader.has_value());
    CHECK_FALSE(uploader.value()
                    ->upload(report(false),
                             [](const platform::HttpRequest&) {
                                 return core::Result<platform::HttpResponse>(
                                     platform::HttpResponse{.status = 403, .body = "bad key"});
                             })
                    .has_value());
    CHECK_FALSE(uploader.value()
                    ->upload(report(false),
                             [](const platform::HttpRequest&) {
                                 return core::Result<platform::HttpResponse>(core::Error{
                                     core::ErrorCode::IoError, "offline"});
                             })
                    .has_value());
}

TEST_CASE("Sentry needs a DSN of the right form") {
    CHECK_FALSE(make("").has_value());
    CHECK_FALSE(make("[crash_upload.sentry]\ndsn = \"not a dsn\"").has_value());
    CHECK_FALSE(make("[crash_upload.sentry]\ndsn = \"https://host/7\"").has_value());
}
