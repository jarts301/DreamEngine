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
[[nodiscard]] core::Result<void> register_crash_upload_backtrace(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::crash_upload;

namespace {

core::Result<std::unique_ptr<CrashUploader>> make(std::string_view config_text) {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_crash_upload_backtrace(registry).has_value());
    const core::Config config = core::Config::parse(config_text).value();
    auto created = registry.create<CrashUploader>(std::vector<std::string>{"backtrace"}, config);
    if (!created.has_value()) {
        return created.error();
    }
    return std::move(created.value().instance);
}

constexpr std::string_view k_config =
    "[crash_upload.backtrace]\nurl = \"https://submit.backtrace.io/studio/0123abcd/\"";

Report report(bool minidump) {
    Report made{.name = "crash-2",
                .text = "Crash: abort\n\"quoted\" stack",
                .minidump = {},
                .application = "Arcade",
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

platform::HttpRequest upload(CrashUploader& uploader, const Report& crash) {
    platform::HttpRequest sent;
    REQUIRE(uploader
                .upload(crash,
                        [&](const platform::HttpRequest& r) {
                            sent = r;
                            return core::Result<platform::HttpResponse>(
                                platform::HttpResponse{.status = 200});
                        })
                .has_value());
    return sent;
}

} // namespace

TEST_CASE("Backtrace gets a minidump with the report attached and attributes") {
    auto uploader = make(k_config);
    REQUIRE(uploader.has_value());
    const platform::HttpRequest sent = upload(*uploader.value(), report(true));
    // The trailing slash of the configured URL is dropped.
    CHECK(sent.url == "https://submit.backtrace.io/studio/0123abcd/minidump");
    const std::string body = text_of(sent.body);
    CHECK(body.find("name=\"upload_file_minidump\"") != std::string::npos);
    CHECK(body.find("name=\"attachment_crash-2.txt\"") != std::string::npos);
    CHECK(body.find("name=\"application\"\r\n\r\nArcade") != std::string::npos);
}

TEST_CASE("Backtrace gets a JSON crash where there is no minidump") {
    auto uploader = make(k_config);
    REQUIRE(uploader.has_value());
    const platform::HttpRequest sent = upload(*uploader.value(), report(false));
    CHECK(sent.url == "https://submit.backtrace.io/studio/0123abcd/json");
    const std::string body = text_of(sent.body);
    CHECK(body.find("\"error.message\":\"Crash: abort\"") != std::string::npos);
    // Quotes and new lines escaped.
    CHECK(body.find("\"Report\":\"Crash: abort\\n\\\"quoted\\\" stack\"") != std::string::npos);
    CHECK(body.find("\"mainThread\":\"main\"") != std::string::npos);
}

TEST_CASE("Backtrace needs a submission URL, and fails on a refusal") {
    CHECK_FALSE(make("").has_value());
    CHECK_FALSE(make("[crash_upload.backtrace]\nurl = \"submit.backtrace.io\"").has_value());
    auto uploader = make(k_config);
    REQUIRE(uploader.has_value());
    CHECK_FALSE(uploader.value()
                    ->upload(report(false),
                             [](const platform::HttpRequest&) {
                                 return core::Result<platform::HttpResponse>(
                                     platform::HttpResponse{.status = 500});
                             })
                    .has_value());
}
