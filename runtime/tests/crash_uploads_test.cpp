#include "dream/runtime/crash_uploads.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;

namespace {

// Sends one request per report, to "https://reports/<name>", with the minidump's size in the path
// when there is one, and fails when the service refuses.
class FakeUploader final : public crash_upload::CrashUploader {
public:
    explicit FakeUploader(bool sends, std::vector<std::string>* versions = nullptr)
        : m_sends(sends), m_versions(versions) {}
    bool sends() const override {
        return m_sends;
    }
    core::Result<void> upload(const crash_upload::Report& report,
                              const crash_upload::Sender& send) override {
        if (m_versions != nullptr) {
            m_versions->push_back(report.application_version);
        }
        const auto answer = send(platform::HttpRequest{
            .url = "https://reports/" + report.name + "/" + std::to_string(report.minidump.size()),
            .headers = {},
            .body = {},
            .timeout_seconds = 1});
        if (!answer.has_value()) {
            return answer.error();
        }
        if (answer.value().status != 200) {
            return core::Error{core::ErrorCode::IoError, "refused"};
        }
        return {};
    }

private:
    bool m_sends;
    std::vector<std::string>* m_versions;
};

// A folder of saved reports, removed afterwards.
struct Reports {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "dream_crash_uploads_test";

    Reports() {
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        write("crash-20261120-100000-1.txt", "Crash one");
        write("crash-20261120-100000-1.dmp", "MDMP");
        write("crash-20261120-110000-2.txt", "Crash two");
        write("crash-20261120-120000-3.txt", "Crash three");
        write("notes.txt", "not a report");
    }
    ~Reports() {
        std::filesystem::remove_all(directory);
    }
    void write(const std::string& name, const std::string& text) const {
        std::ofstream(directory / name, std::ios::binary) << text;
    }
    [[nodiscard]] bool sent(const std::string& name) const {
        return std::filesystem::exists(directory / "sent" / name) &&
               !std::filesystem::exists(directory / name);
    }
    [[nodiscard]] bool kept(const std::string& name) const {
        return std::filesystem::exists(directory / name);
    }

    CrashUploads::Settings settings(std::uint32_t max_reports = 5) const {
        return CrashUploads::Settings{.enabled = true,
                                      .max_reports = max_reports,
                                      .directory = directory,
                                      .application = "Test",
                                      .platform = "windows"};
    }
};

} // namespace

TEST_CASE("Saved crash reports are uploaded oldest first, and moved into the sent folder") {
    const Reports reports;
    FakeState state;
    FakePlatform platform(state);
    CrashUploads uploads(std::make_unique<FakeUploader>(true), platform, reports.settings());
    uploads.wait();
    CHECK(uploads.sent() == 3);
    CHECK(state.http_urls == std::vector<std::string>{"https://reports/crash-20261120-100000-1/4",
                                                      "https://reports/crash-20261120-110000-2/0",
                                                      "https://reports/crash-20261120-120000-3/0"});
    CHECK(reports.sent("crash-20261120-100000-1.txt"));
    CHECK(reports.sent("crash-20261120-100000-1.dmp"));
    CHECK(reports.sent("crash-20261120-120000-3.txt"));
    CHECK(reports.kept("notes.txt"));
}

TEST_CASE("Crash uploads stop at the first failure, keeping the rest for next time") {
    const Reports reports;
    FakeState state;
    state.http_offline = true;
    FakePlatform platform(state);
    CrashUploads uploads(std::make_unique<FakeUploader>(true), platform, reports.settings());
    uploads.wait();
    CHECK(uploads.sent() == 0);
    CHECK(uploads.failed() == 1);
    CHECK(state.http_urls.size() == 1);
    CHECK(reports.kept("crash-20261120-100000-1.txt"));
    CHECK(reports.kept("crash-20261120-100000-1.dmp"));
}

TEST_CASE("Crash uploads send at most max_reports, and nothing with a module that doesn't send") {
    const Reports reports;
    FakeState state;
    FakePlatform platform(state);
    {
        CrashUploads uploads(std::make_unique<FakeUploader>(true), platform, reports.settings(2));
        uploads.wait();
        CHECK(uploads.sent() == 2);
        CHECK(reports.kept("crash-20261120-120000-3.txt"));
    }
    state.http_urls.clear();
    CrashUploads none(std::make_unique<FakeUploader>(false), platform, reports.settings());
    none.wait();
    CHECK(state.http_urls.empty());
    CHECK(reports.kept("crash-20261120-120000-3.txt"));
}

TEST_CASE("Crash uploads send the version that crashed, as its report says") {
    const Reports reports;
    reports.write("crash-20261120-100000-1.txt",
                  "=== Dream Engine crash report ===\nApplication: Test\nVersion: 1.4.2\n"
                  "Engine: 0.1.0\n");
    FakeState state;
    FakePlatform platform(state);
    std::vector<std::string> versions;
    CrashUploads uploads(std::make_unique<FakeUploader>(true, &versions), platform,
                         reports.settings());
    uploads.wait();
    // The first has its version; the others, written before games had one, have none.
    CHECK(versions == std::vector<std::string>{"1.4.2", "", ""});
}

TEST_CASE("Crash upload settings are read and checked") {
    const auto read = [](std::string_view text) {
        return CrashUploads::read_settings(core::Config::parse(text).value());
    };
    REQUIRE(read("").has_value());
    CHECK(read("").value().enabled);
    CHECK(read("").value().max_reports == 5);
    const auto set = read("[crash_upload]\nenabled = false\nmax_reports = 2");
    REQUIRE(set.has_value());
    CHECK_FALSE(set.value().enabled);
    CHECK(set.value().max_reports == 2);
    CHECK_FALSE(read("[crash_upload]\nmax_reports = 0").has_value());
    CHECK_FALSE(read("[crash_upload]\nenabled = 1").has_value());
}
