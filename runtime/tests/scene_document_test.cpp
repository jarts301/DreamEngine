#include "dream/runtime/scene_document.h"

#include <doctest/doctest.h>

#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;

namespace {

constexpr std::string_view k_scene = R"(# A test yard.
# Two lines of description.

[entities.camera.transform]
position = [0.0, 5.0, 11.0]

[entities.camera.camera]
vertical_fov = 45.0
target = [0.0, 0.8, 0.0]

[entities.crate]
parent = "camera"

[entities.crate.mesh]
model = "assets:/models/crate.glb"
tint = [0.75, 0.9, 1.0]
casts_shadows = false

[entities.crate.transform]
scale = [2, 2, 2]

[entities.hero.sprite_animator]
clip = "walk"
speed = 2

[entities.hero.sprite_animator.clips.walk]
frames = [0, 1, 2]
fps = 8.5
)";

SceneDocument parsed(std::string_view text) {
    const core::Result<SceneDocument> document = SceneDocument::parse(text);
    REQUIRE_MESSAGE(document.has_value(), document.error().message);
    return document.value();
}

} // namespace

TEST_CASE("A scene document keeps the file's order of entities, components and settings") {
    const SceneDocument document = parsed(k_scene);
    CHECK(document.entity_names() == std::vector<std::string>{"camera", "crate", "hero"});
    const SceneTable* crate = document.entity("crate");
    REQUIRE(crate != nullptr);
    REQUIRE(crate->tables().size() == 2);
    CHECK(crate->tables()[0].first == "mesh");
    CHECK(crate->tables()[1].first == "transform");
    const SceneTable* mesh = crate->table("mesh");
    REQUIRE(mesh->values().size() == 3);
    CHECK(mesh->values()[0].first == "model");
    CHECK(mesh->values()[2].first == "casts_shadows");
    CHECK(document.parent("crate") == "camera");
    // Integers stay integers.
    CHECK(std::holds_alternative<std::int64_t>(
        *document.entity("hero")->table("sprite_animator")->value("speed")));
}

TEST_CASE("A scene document writes TOML that reads back to the same document and scene") {
    const SceneDocument document = parsed(k_scene);
    const std::string written = document.to_toml();
    CHECK(written.starts_with("# A test yard.\n# Two lines of description.\n\n[entities.camera"));
    CHECK(written.find("[entities.hero.sprite_animator.clips.walk]\nframes = [0.0, 1.0, 2.0]\n"
                       "fps = 8.5\n") != std::string::npos);
    CHECK(parsed(written) == document);

    // The same values as the original, for every key.
    const core::Config before = core::Config::parse(k_scene).value();
    const core::Config after = core::Config::parse(written).value();
    for (const std::string_view key : {"entities.camera.camera.vertical_fov",
                                       "entities.crate.mesh.model", "entities.crate.parent",
                                       "entities.hero.sprite_animator.speed"}) {
        REQUIRE(after.value(key) != nullptr);
        CHECK(*after.value(key) == *before.value(key));
    }
}

TEST_CASE("Scene documents edit settings, components and entities") {
    SceneDocument document = parsed(k_scene);
    SceneTable& mesh = *document.entity("crate")->table("mesh");
    mesh.set("tint", std::vector<double>{1.0, 0.5, 0.25});
    mesh.set("cast", true);
    CHECK(mesh.values().size() == 4);
    CHECK(mesh.values()[1].first == "tint");
    CHECK(mesh.remove("cast"));
    CHECK_FALSE(mesh.remove("cast"));

    CHECK(document.add_entity("crate") == "crate_2");
    CHECK(document.add_entity("bad name!") == "entity");
    CHECK(document.entity_names().back() == "entity");
    document.entity("crate_2")->add_table("transform").set("position",
                                                           std::vector<double>{1.0, 2.0, 3.0});

    REQUIRE(document.set_parent("hero", "crate").has_value());
    CHECK_FALSE(document.set_parent("camera", "crate").has_value()); // Its own ancestor.
    CHECK_FALSE(document.set_parent("hero", "nobody").has_value());

    REQUIRE(document.rename_entity("crate", "box").has_value());
    CHECK(document.parent("hero") == "box");
    CHECK_FALSE(document.rename_entity("box", "camera").has_value());
    CHECK_FALSE(document.rename_entity("box", "a.b").has_value());

    // Children of a removed entity move up to its parent.
    CHECK(document.remove_entity("box"));
    CHECK(document.parent("hero") == "camera");
    CHECK(document.entity("box") == nullptr);

    const SceneDocument again = parsed(document.to_toml());
    CHECK(again == document);
    CHECK(again.entity("crate_2")->table("transform")->value("position") != nullptr);
}

TEST_CASE("Scene documents keep empty entities, quote odd keys, and write floats that stay floats") {
    SceneDocument document = parsed("");
    document.add_entity("empty");
    document.entity("empty")->set("odd key", std::string("say \"hi\"\n"));
    document.add_entity("marker");
    const std::string written = document.to_toml();
    CHECK(written.find("[entities.marker]\n") != std::string::npos);
    CHECK(written.find("\"odd key\" = \"say \\\"hi\\\"\\n\"") != std::string::npos);
    CHECK(parsed(written) == document);

    CHECK(toml_value(core::ConfigValue{2.0}) == "2.0");
    CHECK(toml_value(core::ConfigValue{0.1}) == "0.1");
    CHECK(toml_value(core::ConfigValue{std::int64_t{3}}) == "3");
    CHECK(toml_value(core::ConfigValue{std::vector<std::string>{"a", "b"}}) == "[\"a\", \"b\"]");
    CHECK_FALSE(SceneDocument::parse("[broken").has_value());
}
