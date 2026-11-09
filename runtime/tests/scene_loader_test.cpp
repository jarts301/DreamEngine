#include "dream/runtime/scene_loader.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <ostream> // Needed for doctest to print std::string.
#include <string>

using namespace dream;
using namespace dream::runtime;
using core::Vec3;
using doctest::Approx;
using dream::runtime::tests::FakePlatform;
using dream::runtime::tests::FakeState;

namespace {

struct Spin {
    float speed = 0.0f;
};

struct Label {
    std::string text;
};

struct Fixture {
    FakeState state;
    FakePlatform platform{state};
    VirtualFileSystem files{platform};
    AssetManager assets{files};
    SceneLoader scenes{files, assets};

    Fixture() {
        scenes.register_component("spin", [](ComponentContext& context) -> core::Result<void> {
            const core::Result<float> speed = context.data.number("speed", 1.0f);
            if (!speed.has_value()) {
                return speed.error();
            }
            context.world.registry().emplace<Spin>(context.entity, speed.value());
            return {};
        });
        scenes.register_component("label", [](ComponentContext& context) -> core::Result<void> {
            core::Result<std::string> text = context.data.text("text");
            if (!text.has_value()) {
                return text.error();
            }
            context.world.registry().emplace<Label>(context.entity, std::move(text).value());
            return {};
        });
    }

    void expect_error(const char* scene, const char* expected) {
        CAPTURE(scene);
        const auto world = scenes.parse(scene);
        REQUIRE_FALSE(world.has_value());
        CAPTURE(world.error().message);
        CHECK(world.error().message.find(expected) != std::string::npos);
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "SceneLoader builds entities with their components") {
    const auto loaded = scenes.parse(R"(
[entities.crate.transform]
position = [1, 2, 3]
scale = [2, 2, 2]

[entities.crate.spin]
speed = 4.5

[entities.lamp.label]
text = "hello"
)");
    REQUIRE(loaded.has_value());
    World& world = *loaded.value();

    const Entity crate = world.find("crate");
    REQUIRE(crate != k_no_entity);
    const Transform& transform = world.registry().get<Transform>(crate);
    CHECK(transform.position.y == Approx(2.0f));
    CHECK(transform.scale.x == Approx(2.0f));
    CHECK(world.registry().get<Spin>(crate).speed == Approx(4.5f));

    const Entity lamp = world.find("lamp");
    REQUIRE(lamp != k_no_entity);
    CHECK(world.registry().get<Label>(lamp).text == "hello");
    CHECK_FALSE(world.registry().all_of<Spin>(lamp));
}

TEST_CASE_FIXTURE(Fixture, "SceneLoader uses defaults for anything left out") {
    const auto loaded = scenes.parse("[entities.a.spin]\n\n[entities.b]\n");
    REQUIRE(loaded.has_value());
    World& world = *loaded.value();
    REQUIRE(world.find("a") != k_no_entity);
    CHECK(world.registry().get<Spin>(world.find("a")).speed == Approx(1.0f));
    CHECK(world.find("b") != k_no_entity);

    const auto with_value = scenes.parse("[entities.a.transform]\nposition = [1, 0, 0]\n");
    REQUIRE(with_value.has_value());
    const Transform& transform =
        with_value.value()->registry().get<Transform>(with_value.value()->find("a"));
    CHECK(transform.scale.x == Approx(1.0f));
    CHECK(transform.rotation.w == Approx(1.0f));
}

TEST_CASE_FIXTURE(Fixture, "SceneLoader reads rotation in degrees") {
    const auto loaded = scenes.parse("[entities.a.transform]\nrotation = [0, 90, 0]\n");
    REQUIRE(loaded.has_value());

    const Transform& transform =
        loaded.value()->registry().get<Transform>(loaded.value()->find("a"));
    const Vec3 turned = core::rotate(transform.rotation, Vec3{1.0f, 0.0f, 0.0f});
    CHECK(turned.z == Approx(-1.0f));
}

TEST_CASE_FIXTURE(Fixture, "SceneLoader links parents, whichever comes first in the file") {
    const auto loaded = scenes.parse(R"(
[entities.a_child]
parent = "z_parent"

[entities.a_child.transform]
position = [1, 0, 0]

[entities.z_parent.transform]
position = [10, 0, 0]
)");
    REQUIRE(loaded.has_value());
    World& world = *loaded.value();

    const Vec3 position = core::transform_point(
        world.registry().get<WorldTransform>(world.find("a_child")).matrix, Vec3{});
    CHECK(position.x == Approx(11.0f));
}

TEST_CASE_FIXTURE(Fixture, "SceneLoader loads a scene through the file system") {
    const std::string_view text = "[entities.a.transform]\nposition = [1, 0, 0]\n";
    const auto view = std::as_bytes(std::span(text));
    state.assets["scenes/one.toml"] = std::vector<std::byte>(view.begin(), view.end());

    const auto loaded = scenes.load("assets:/scenes/one.toml");
    REQUIRE(loaded.has_value());
    CHECK(loaded.value()->find("a") != k_no_entity);
}

TEST_CASE_FIXTURE(Fixture, "SceneLoader names the scene in its errors") {
    const std::string_view text = "[entities.a.nope]\nvalue = 1\n";
    const auto view = std::as_bytes(std::span(text));
    state.assets["bad.toml"] = std::vector<std::byte>(view.begin(), view.end());

    const auto loaded = scenes.load("assets:/bad.toml");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().message.find("bad.toml") != std::string::npos);

    CHECK_FALSE(scenes.load("assets:/missing.toml").has_value());
}

TEST_CASE_FIXTURE(Fixture, "SceneLoader accepts a scene with no entities") {
    const auto loaded = scenes.parse("title = \"empty\"\n");
    REQUIRE(loaded.has_value());
    CHECK(loaded.value()->registry().view<Transform>().empty());
}

TEST_CASE_FIXTURE(Fixture, "SceneLoader reports every kind of mistake") {
    expect_error("broken =", "Line 1");
    expect_error("[entities.a.typo]\nvalue = 1\n", "unknown component 'typo'");
    expect_error("[entities.a]\ncolour = 1\n", "unknown setting 'colour'");
    expect_error("[entities.a]\nparent = \"nobody\"\n", "no entity named 'nobody'");
    expect_error("[entities.a]\nparent = 3\n", "'parent' must be a string");
    expect_error("[entities.a]\nparent = \"a\"\n", "its own ancestor");
    expect_error("[entities.a]\nparent = \"b\"\n[entities.b]\nparent = \"a\"\n", "its own ancestor");
    expect_error("[entities.a.transform]\nposition = [1, 2]\n", "three numbers");
    expect_error("[entities.a.transform]\nposition = \"here\"\n", "three numbers");
    expect_error("[entities.a.spin]\nspeed = \"fast\"\n", "'speed' must be a number");
    expect_error("[entities.a.label]\nother = 1\n", "'text' must be a string");
}
