#include "dream/runtime/virtual_file_system.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ostream> // Needed for doctest to print std::string_view.
#include <string>
#include <vector>

using namespace dream;
using dream::runtime::VirtualFileSystem;
using dream::runtime::tests::FakePlatform;
using dream::runtime::tests::FakeState;

namespace {

std::vector<std::byte> bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span(text));
    return {view.begin(), view.end()};
}

struct Fixture {
    FakeState state;
    FakePlatform platform{state};
    VirtualFileSystem files{platform};
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "VirtualFileSystem reads each root separately") {
    state.assets["a/b.txt"] = bytes("asset");
    state.user_files["a/b.txt"] = bytes("user");

    const auto asset = files.read("assets:/a/b.txt");
    REQUIRE(asset.has_value());
    CHECK(asset.value() == bytes("asset"));

    const auto user = files.read("user:/a/b.txt");
    REQUIRE(user.has_value());
    CHECK(user.value() == bytes("user"));
}

TEST_CASE_FIXTURE(Fixture, "VirtualFileSystem reports a missing file") {
    const auto file = files.read("assets:/missing.txt");
    REQUIRE_FALSE(file.has_value());
    CHECK(file.error().code == core::ErrorCode::NotFound);
}

TEST_CASE_FIXTURE(Fixture, "VirtualFileSystem writes user files") {
    const std::vector<std::byte> data = bytes("saved");
    REQUIRE(files.write("user:/saves/one.dat", data).has_value());
    CHECK(state.user_files.at("saves/one.dat") == data);
}

TEST_CASE_FIXTURE(Fixture, "VirtualFileSystem refuses to write assets") {
    const auto written = files.write("assets:/a.txt", bytes("x"));
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().code == core::ErrorCode::InvalidArgument);
    CHECK(state.assets.empty());
}

TEST_CASE_FIXTURE(Fixture, "VirtualFileSystem rejects unsafe paths") {
    for (const char* path :
         {"", "a.txt", "/a.txt", "C:/a.txt", "other:/a.txt", "assets:/", "assets:a.txt",
          "assets://a.txt", "assets:/a//b.txt", "assets:/a/", "assets:/./a.txt",
          "assets:/../secret.txt", "assets:/a/../../secret.txt", "assets:/a/..",
          "assets:/a\\b.txt", "assets:/a:b.txt", "user:/../../outside.txt"}) {
        CAPTURE(path);
        const auto file = files.read(path);
        REQUIRE_FALSE(file.has_value());
        CHECK(file.error().code == core::ErrorCode::InvalidArgument);
    }

    CHECK_FALSE(files.write("user:/../outside.txt", bytes("x")).has_value());
    CHECK(state.user_files.empty());
}

TEST_CASE_FIXTURE(Fixture, "VirtualFileSystem allows dots inside names") {
    state.assets["a..b/c.d.txt"] = bytes("x");
    CHECK(files.read("assets:/a..b/c.d.txt").has_value());
}

TEST_CASE_FIXTURE(Fixture, "Asset folders on disk are read before the platform's assets") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "dream_vfs_asset_folder_test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "scenes");
    std::ofstream(folder / "scenes" / "main.toml", std::ios::binary) << "from disk";
    state.assets["scenes/main.toml"] = bytes("packaged");
    state.assets["shaders/lit.spv"] = bytes("shader");
    state.reports_versions = true;
    state.asset_versions["shaders/lit.spv"] = 7;

    files.add_asset_folder(folder);
    CHECK(files.read("assets:/scenes/main.toml").value() == bytes("from disk"));
    // Others still come from the platform.
    CHECK(files.read("assets:/shaders/lit.spv").value() == bytes("shader"));
    CHECK(files.version("assets:/shaders/lit.spv") == 7);
    const std::optional<std::int64_t> before = files.version("assets:/scenes/main.toml");
    REQUIRE(before.has_value());
    CHECK(*before != 0);
    // Unsafe paths are refused before any folder is looked at.
    CHECK_FALSE(files.read("assets:/../secret.txt").has_value());
    // User files never come from asset folders.
    CHECK_FALSE(files.read("user:/scenes/main.toml").has_value());
    std::filesystem::remove_all(folder);
}
