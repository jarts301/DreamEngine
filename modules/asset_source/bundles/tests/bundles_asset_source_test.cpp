#include "dream/asset_source/asset_source.h"
#include "dream/core/archive.h"
#include "dream/core/module_registry.h"
#include "fake_platform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_asset_source_bundles(core::ModuleRegistry& registry);
}

using namespace dream;
using dream::runtime::tests::FakePlatform;
using dream::runtime::tests::FakeState;

namespace {

std::vector<std::byte> bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span(text));
    return {view.begin(), view.end()};
}

// A crate in the base, and a forest scene with its wind in the "forest" bundle.
core::AssetDatabase database() {
    return core::AssetDatabase::parse(R"(bundles = ["forest", "beach"]
[assets]
paths = ["models/crate.glb", "scenes/forest.toml", "sounds/wind.wav"]
kinds = ["model", "scene", "sound"]
bytes = [5, 6, 4]
needs = ["", "models/crate.glb|sounds/wind.wav", ""]
bundles = ["base", "forest", "forest"]
)")
        .value();
}

struct Fixture {
    FakeState state;
    FakePlatform platform{state};
    core::AssetDatabase assets = database();
    std::unique_ptr<asset_source::AssetSource> source;

    Fixture() {
        state.assets["models/crate.glb"] = bytes("crate");
        state.assets["scenes/forest.toml"] = bytes("forest");
        state.assets["sounds/wind.wav"] = bytes("wind");
        core::ModuleRegistry registry;
        REQUIRE(modules::register_asset_source_bundles(registry).has_value());
        const std::vector<std::string> names{"bundles"};
        auto created =
            registry.create<asset_source::AssetSource>(names, core::Config::parse("").value());
        REQUIRE(created.has_value());
        source = std::move(created.value().instance);
        REQUIRE(source->initialize(platform, assets).has_value());
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "A bundle's files can be read only while it is mounted") {
    CHECK(source->read("models/crate.glb").value() == bytes("crate"));
    CHECK(source->mounted("base"));
    CHECK_FALSE(source->mounted("forest"));

    const auto refused = source->read("scenes/forest.toml");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message ==
          "assets:/scenes/forest.toml is in the bundle 'forest', which isn't mounted");

    REQUIRE(source->mount("forest").has_value());
    CHECK(source->mounted("forest"));
    CHECK(source->read("scenes/forest.toml").value() == bytes("forest"));
    CHECK(source->read("sounds/wind.wav").value() == bytes("wind"));

    source->unmount("forest");
    CHECK_FALSE(source->read("sounds/wind.wav").has_value());
    // Mounting twice, or the base, changes nothing.
    REQUIRE(source->mount("base").has_value());
    REQUIRE(source->mount("beach").has_value());
    REQUIRE(source->mount("beach").has_value());
    CHECK(source->mounted("beach"));
}

TEST_CASE_FIXTURE(Fixture, "Mounting a bundle the project doesn't name fails, naming those it does") {
    const auto mounted = source->mount("cave");
    REQUIRE_FALSE(mounted.has_value());
    CHECK(mounted.error().message == "There is no bundle 'cave'; the project's are: forest, beach");
}

TEST_CASE_FIXTURE(Fixture, "A bundle packed into its own archive is read from it while mounted") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "dream_bundles_test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    {
        core::Result<core::ArchiveWriter> writer =
            core::ArchiveWriter::create((folder / "forest.pak").string());
        REQUIRE(writer.has_value());
        REQUIRE(writer.value().add("scenes/forest.toml", bytes("packed forest")).has_value());
        REQUIRE(writer.value().add("sounds/wind.wav", bytes("packed wind")).has_value());
        REQUIRE(writer.value().finish().has_value());
    }
    state.program_folder = folder.string() + "/";
    state.reports_versions = true;

    REQUIRE(source->mount("forest").has_value());
    CHECK(source->read("scenes/forest.toml").value() == bytes("packed forest"));
    // Packed files don't change, so hot reload doesn't look at them; the base's still can.
    CHECK_FALSE(source->version("sounds/wind.wav").has_value());
    CHECK(source->version("models/crate.glb") == 1);
    source->unmount("forest");
    // Closed, so the archive can be replaced, as by a patch.
    std::filesystem::remove_all(folder);
    CHECK_FALSE(std::filesystem::exists(folder));
}
