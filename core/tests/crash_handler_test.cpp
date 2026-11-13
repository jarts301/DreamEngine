#include "dream/core/crash_handler.h"

// The internals that run after a crash, tested here because a real crash can't be inside a test.
#include "crash_report.h"

#include <doctest/doctest.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <string_view>
#include <system_error>

#if defined(__linux__) && !defined(__ANDROID__)
#include <csignal>
#endif

using namespace dream::core;

namespace {

namespace fs = std::filesystem;

fs::path fresh_folder(std::string_view name) {
    const fs::path folder = fs::temp_directory_path() / "dream_crash_handler_tests" / name;
    std::error_code error;
    fs::remove_all(folder, error);
    return folder;
}

void test_terminate() {}

std::string write_to_string(void (*write)(crash::ReportWriter&)) {
    std::string text;
    {
        crash::ReportWriter writer(
            [](void* target, const char* data, std::size_t size) {
                static_cast<std::string*>(target)->append(data, size);
            },
            &text);
        write(writer);
    }
    return text;
}

} // namespace

TEST_CASE("crash kinds have names, both ways") {
    for (const CrashKind kind : {CrashKind::NullPointer, CrashKind::DivideByZero,
                                 CrashKind::StackOverflow, CrashKind::Abort, CrashKind::Assert,
                                 CrashKind::Terminate}) {
        CHECK(crash_kind_from_name(crash_kind_name(kind)) == kind);
    }
    CHECK(crash_kind_from_name("null_pointer") == CrashKind::NullPointer);
    CHECK_FALSE(crash_kind_from_name("segfault").has_value());
    CHECK_FALSE(crash_kind_from_name("").has_value());
}

#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))

TEST_CASE("a crash handler makes its folder, and puts back the handlers it replaced") {
    const fs::path folder = fresh_folder("installs") / "nested";
    const std::terminate_handler before = std::set_terminate(&test_terminate);
#if defined(__linux__)
    struct sigaction previous {};
    sigaction(SIGSEGV, nullptr, &previous);
#endif
    {
        const auto handler = CrashHandler::install(CrashReportSettings{.directory = folder});
        REQUIRE(handler.has_value());
        CHECK(fs::is_directory(folder));
        CHECK(handler.value()->directory().is_absolute());
        CHECK(std::get_terminate() != &test_terminate);
#if defined(__linux__)
        struct sigaction during {};
        sigaction(SIGSEGV, nullptr, &during);
        CHECK((during.sa_flags & SA_SIGINFO) != 0);
#endif
    }
    CHECK(std::get_terminate() == &test_terminate);
#if defined(__linux__)
    struct sigaction after {};
    sigaction(SIGSEGV, nullptr, &after);
    // The flags aren't compared whole, because the C library adds its own when an action is set.
    CHECK(after.sa_handler == previous.sa_handler);
    CHECK((after.sa_flags & SA_SIGINFO) == (previous.sa_flags & SA_SIGINFO));
#endif
    std::set_terminate(before);
}

TEST_CASE("only one crash handler is installed at a time") {
    const fs::path folder = fresh_folder("one_at_a_time");
    {
        const auto first = CrashHandler::install(CrashReportSettings{.directory = folder});
        REQUIRE(first.has_value());
        const auto second = CrashHandler::install(CrashReportSettings{.directory = folder});
        CHECK_FALSE(second.has_value());
    }
    CHECK(CrashHandler::install(CrashReportSettings{.directory = folder}).has_value());
}

TEST_CASE("a crash handler can't be installed where its folder can't be made") {
    const fs::path folder = fresh_folder("blocked");
    fs::create_directories(folder);
    std::ofstream(folder / "a_file") << "not a folder";
    const auto handler =
        CrashHandler::install(CrashReportSettings{.directory = folder / "a_file" / "reports"});
    REQUIRE_FALSE(handler.has_value());
    CHECK(handler.error().code == ErrorCode::IoError);
    // A failed install leaves room for the next.
    CHECK(CrashHandler::install(CrashReportSettings{.directory = folder / "ok"}).has_value());
}

#endif

TEST_CASE("crash report times are UTC dates, worked out without the C library") {
    const auto check = [](std::int64_t seconds, crash::DateTime expected) {
        const crash::DateTime time = crash::to_date_time(seconds);
        CHECK(time.year == expected.year);
        CHECK(time.month == expected.month);
        CHECK(time.day == expected.day);
        CHECK(time.hour == expected.hour);
        CHECK(time.minute == expected.minute);
        CHECK(time.second == expected.second);
    };
    check(0, {1970, 1, 1, 0, 0, 0});
    check(951'782'400, {2000, 2, 29, 0, 0, 0});
    check(1'700'000'000, {2023, 11, 14, 22, 13, 20});
    check(4'102'444'799, {2099, 12, 31, 23, 59, 59});
    check(-1, {1969, 12, 31, 23, 59, 59});
}

TEST_CASE("the report writer writes numbers and text without allocating") {
    CHECK(write_to_string([](crash::ReportWriter& writer) {
              writer.text("n=");
              writer.number(0);
              writer.text(" padded=");
              writer.number(7, 3);
              writer.text(" big=");
              writer.number(18'446'744'073'709'551'615ull);
              writer.text(" hex=");
              writer.hex(0xdeadbeef);
          }) == "n=0 padded=007 big=18446744073709551615 hex=0x00000000deadbeef");

    // More than its buffer holds arrives whole.
    const std::string long_text(5000, 'x');
    std::string written;
    {
        crash::ReportWriter writer(
            [](void* target, const char* data, std::size_t size) {
                static_cast<std::string*>(target)->append(data, size);
            },
            &written);
        writer.text(long_text);
        const char fixed[8] = {'a', 'b', 'c', '\0', 'z', 'z', 'z', 'z'};
        writer.text(fixed, sizeof(fixed));
    }
    CHECK(written == long_text + "abc");
}

TEST_CASE("fixed text stops quietly when full") {
    crash::FixedText text;
    text.append("Access violation, reading address ");
    text.append_hex(0x10);
    CHECK(text.view() == "Access violation, reading address 0x0000000000000010");
    for (int i = 0; i < 100; ++i) {
        text.append("0123456789");
    }
    CHECK(text.view().size() < 320);
    CHECK(text.view().starts_with("Access violation"));
}

TEST_CASE("crash report names hold the time and the process") {
    const std::string_view folder = "/tmp/reports";
    std::copy(folder.begin(), folder.end(), crash::g_state.directory);
    crash::g_state.directory[folder.size()] = '\0';

    char name[128];
    const std::size_t length = crash::report_stem(name, sizeof(name), 1'700'000'000, 4321);
    CHECK(std::string_view(name, length) == "/tmp/reports/crash-20231114-221320-4321");

    char too_small[16];
    CHECK(crash::report_stem(too_small, sizeof(too_small), 0, 1) == 0);
}
