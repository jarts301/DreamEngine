#include "editor_scene.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <thread>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using dream::editor::EditorScene;
using dream::runtime::tests::FakePlatform;
using dream::runtime::tests::FakeState;
using doctest::Approx;

namespace {

struct Spin {
    float speed = 0.0f;
};

constexpr std::string_view k_scene = R"(# The test scene.

[entities.crate.transform]
position = [1.0, 2.0, 3.0]

[entities.crate.spin]
speed = 2.0

[entities.lid]
parent = "crate"

[entities.lid.wobble]
amount = 1
)";

struct Fixture {
    FakeState state;
    FakePlatform platform{state};
    VirtualFileSystem files{platform};
    AssetManager assets{files};
    SceneLoader loader{files, assets};
    EditorScene scene{loader};
    std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "dream_editor_scene_test";
    std::filesystem::path file = folder / "main.toml";

    Fixture() {
        loader.register_component(
            "spin",
            [](ComponentContext& context) -> core::Result<void> {
                const core::Result<float> speed = context.data.number("speed", 1.0f);
                if (!speed.has_value()) {
                    return speed.error();
                }
                if (speed.value() < 0.0f) {
                    return core::Error{core::ErrorCode::InvalidArgument, "speed below 0"};
                }
                context.world.registry().emplace<Spin>(context.entity, speed.value());
                return {};
            },
            {core::FieldInfo{.name = "speed", .kind = core::FieldKind::Number, .fallback = 1.0}});
        // As the editor does: "wobble" is a game's own, which it can't build.
        loader.set_skip_unknown_components(true);
        std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder);
        write(k_scene);
        REQUIRE(scene.open(file).has_value());
    }
    ~Fixture() {
        std::filesystem::remove_all(folder);
    }

    void write(std::string_view text) const {
        std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
    }
    [[nodiscard]] std::string on_disk() const {
        std::ifstream in(file, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] float speed() {
        return scene.world()->registry().get<Spin>(scene.entity_in_world("crate")).speed;
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "The editor's scene builds a world, keeping a game's own components") {
    CHECK(scene.entities() == std::vector<std::string>{"crate", "lid"});
    CHECK(scene.parent("lid") == "crate");
    CHECK(scene.components("lid") == std::vector<std::string>{"wobble"});
    CHECK(scene.setting("lid", "wobble", "amount") == core::ConfigValue{std::int64_t{1}});
    REQUIRE(scene.world() != nullptr);
    CHECK(speed() == Approx(2.0f));
    CHECK(scene.problem().empty());
    CHECK_FALSE(scene.unsaved());
    CHECK(scene.component_type("spin") != nullptr);
    CHECK(scene.component_type("wobble") == nullptr);
}

TEST_CASE_FIXTURE(Fixture, "Changes build the world again, and a refused one keeps the last world") {
    const std::uint64_t version = scene.world_version();
    scene.set_setting("crate", "spin", "speed", 5.0);
    CHECK(scene.world_version() > version);
    CHECK(speed() == Approx(5.0f));
    CHECK(scene.unsaved());

    const std::uint64_t built = scene.world_version();
    scene.set_setting("crate", "spin", "speed", -1.0);
    CHECK(scene.problem().find("speed below 0") != std::string::npos);
    CHECK(scene.world_version() == built);
    CHECK(speed() == Approx(5.0f));
    // The change is kept, so the next one can fix it.
    CHECK(scene.setting("crate", "spin", "speed") == core::ConfigValue{-1.0});
    scene.set_setting("crate", "spin", "speed", 3.0);
    CHECK(scene.problem().empty());
    CHECK(speed() == Approx(3.0f));
}

TEST_CASE_FIXTURE(Fixture, "Changes in a moment undo as one, and redo") {
    scene.set_setting("crate", "spin", "speed", 3.0);
    scene.set_setting("crate", "spin", "speed", 4.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    scene.add_component("lid", "transform");
    CHECK(scene.components("lid").size() == 2);

    scene.undo();
    CHECK(scene.components("lid").size() == 1);
    CHECK(speed() == Approx(4.0f));
    scene.undo();
    CHECK(speed() == Approx(2.0f));
    CHECK_FALSE(scene.can_undo());
    scene.redo();
    CHECK(speed() == Approx(4.0f));
    CHECK(scene.can_redo());
    // A new change ends the redos.
    scene.set_setting("crate", "spin", "speed", 9.0);
    CHECK_FALSE(scene.can_redo());
}

TEST_CASE_FIXTURE(Fixture, "Entities are added, renamed, reparented and removed in the editor's scene") {
    const std::string added = scene.add_entity("crate");
    CHECK(added == "crate_2");
    CHECK(scene.selected() == "crate_2");
    REQUIRE(scene.rename_entity("crate", "box").has_value());
    CHECK(scene.parent("lid") == "box");
    CHECK(scene.entity_in_world("box") != k_no_entity);
    CHECK_FALSE(scene.rename_entity("box", "crate_2").has_value());
    REQUIRE(scene.set_parent("crate_2", "lid").has_value());
    CHECK_FALSE(scene.set_parent("box", "crate_2").has_value());
    scene.select("lid");
    scene.remove_entity("lid");
    CHECK(scene.selected().empty());
    CHECK(scene.parent("crate_2") == "box");
    scene.select("nobody");
    CHECK(scene.selected().empty());

    // Settings in tables inside a component, by dotted names.
    scene.add_component("box", "anim");
    scene.set_setting("box", "anim", "clips.walk.fps", 8.0);
    CHECK(scene.settings("box", "anim") == std::vector<std::string>{"clips.walk.fps"});
    CHECK(scene.setting("box", "anim", "clips.walk.fps") == core::ConfigValue{8.0});
    scene.remove_setting("box", "anim", "clips.walk.fps");
    CHECK(scene.settings("box", "anim").empty());
}

TEST_CASE_FIXTURE(Fixture, "The editor's scene saves, and notices changes on disk") {
    scene.set_setting("crate", "spin", "speed", 7.0);
    REQUIRE(scene.save().has_value());
    CHECK_FALSE(scene.unsaved());
    const std::string saved = on_disk();
    CHECK(saved.starts_with("# The test scene.\n"));
    CHECK(saved.find("speed = 7.0") != std::string::npos);
    CHECK(saved.find("[entities.lid.wobble]\namount = 1") != std::string::npos);
    CHECK_FALSE(scene.changed_on_disk());

    // File times can be coarse, so the change waits a moment.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write("[entities.only.transform]\n");
    CHECK(scene.changed_on_disk());
    REQUIRE(scene.reload().has_value());
    CHECK(scene.entities() == std::vector<std::string>{"only"});
    CHECK_FALSE(scene.changed_on_disk());

    EditorScene missing(loader);
    CHECK_FALSE(missing.open(folder / "missing.toml").has_value());
    write("[broken");
    CHECK_FALSE(missing.open(file).has_value());
}
