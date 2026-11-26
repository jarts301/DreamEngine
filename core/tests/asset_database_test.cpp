#include "dream/core/asset_database.h"

#include <doctest/doctest.h>

#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using core::AssetDatabase;

namespace {

// As the build writes it.
constexpr std::string_view k_database = R"(# Made by the build.
platform = "windows"
texture_cooker = "ktx2_uastc"
bundles = [
    "forest",
]

[assets]
paths = [
    "models/crate.glb",
    "models/textures/crate.ktx2",
    "scenes/forest.toml",
    "sounds/wind.wav",
    "ui/menu.rml",
]
kinds = [
    "model",
    "texture",
    "scene",
    "sound",
    "ui",
]
bytes = [2028, 512, 300, 44100, 90]
needs = [
    "models/textures/crate.ktx2",
    "",
    "models/crate.glb|sounds/wind.wav",
    "",
    "models/textures/crate.ktx2",
]
bundles = [
    "base",
    "base",
    "forest",
    "forest",
    "base",
]
)";

AssetDatabase parsed(std::string_view text) {
    const core::Result<AssetDatabase> database = AssetDatabase::parse(text);
    REQUIRE_MESSAGE(database.has_value(), database.error().message);
    return database.value();
}

} // namespace

TEST_CASE("The asset database lists each file with its kind, size, needs and bundle") {
    const AssetDatabase database = parsed(k_database);
    CHECK(database.platform() == "windows");
    CHECK(database.texture_cooker() == "ktx2_uastc");
    CHECK(database.bundles() == std::vector<std::string>{"forest"});
    REQUIRE(database.assets().size() == 5);

    const AssetDatabase::Asset* crate = database.find("assets:/models/crate.glb");
    REQUIRE(crate != nullptr);
    CHECK(crate == database.find("models/crate.glb"));
    CHECK(crate->kind == "model");
    CHECK(crate->bytes == 2028);
    CHECK(crate->needs == std::vector<std::string>{"models/textures/crate.ktx2"});
    CHECK(database.find("models/missing.glb") == nullptr);
    CHECK(database.find("sounds/wind.wav")->needs.empty());
}

TEST_CASE("The asset database follows what files need, both ways") {
    const AssetDatabase database = parsed(k_database);
    CHECK(database.needs_all("assets:/scenes/forest.toml") ==
          std::vector<std::string>{"models/crate.glb", "sounds/wind.wav",
                                   "models/textures/crate.ktx2"});
    CHECK(database.needs_all("sounds/wind.wav").empty());
    CHECK(database.needed_by("models/textures/crate.ktx2") ==
          std::vector<std::string>{"models/crate.glb", "ui/menu.rml"});

    CHECK(database.bundle_of("scenes/forest.toml") == "forest");
    CHECK(database.bundle_of("models/crate.glb") == "base");
    CHECK(database.bundle_of("not/listed.png") == "base");
    CHECK(database.files_in("forest") ==
          std::vector<std::string>{"scenes/forest.toml", "sounds/wind.wav"});
}

TEST_CASE("An asset database whose lists don't match, or that isn't TOML, is refused") {
    CHECK_FALSE(AssetDatabase::parse("[assets]\npaths = [\"a\"]\nkinds = []\n").has_value());
    CHECK_FALSE(AssetDatabase::parse("[assets\n").has_value());
    const core::Result<AssetDatabase> twice = AssetDatabase::parse(
        "[assets]\npaths = [\"a\", \"a\"]\nkinds = [\"file\", \"file\"]\nbytes = [1, 1]\n"
        "needs = [\"\", \"\"]\nbundles = [\"base\", \"base\"]\n");
    REQUIRE_FALSE(twice.has_value());
    CHECK(twice.error().message.find("'a' is listed twice") != std::string::npos);
    // An empty game has an empty database.
    const core::Result<AssetDatabase> empty = AssetDatabase::parse(
        "bundles = []\n[assets]\npaths = []\nkinds = []\nbytes = []\nneeds = []\nbundles = []\n");
    REQUIRE(empty.has_value());
    CHECK(empty.value().assets().empty());
}
