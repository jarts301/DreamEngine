#include "dream/runtime/world.h"

#include <doctest/doctest.h>

#include <ostream> // Needed for doctest to print std::string.
#include <vector>

using namespace dream;
using namespace dream::runtime;
using core::Vec3;
using doctest::Approx;

namespace {

Vec3 world_position(World& world, Entity entity) {
    return core::transform_point(world.registry().get<WorldTransform>(entity).matrix, Vec3{});
}

void set_position(World& world, Entity entity, Vec3 position) {
    world.registry().get<Transform>(entity).position = position;
}

} // namespace

TEST_CASE("World creates entities with a transform and an optional name") {
    World world;

    const Entity named = world.create_entity("crate");
    const Entity unnamed = world.create_entity();

    CHECK(world.registry().all_of<Transform, WorldTransform>(named));
    CHECK(world.registry().get<Name>(named).value == "crate");
    CHECK_FALSE(world.registry().all_of<Name>(unnamed));
}

TEST_CASE("World finds entities by name") {
    World world;
    const Entity crate = world.create_entity("crate");
    world.create_entity("barrel");

    CHECK(world.find("crate") == crate);
    CHECK(world.find("missing") == k_no_entity);
}

TEST_CASE("World updates a root's matrix from its transform") {
    World world;
    const Entity entity = world.create_entity();
    set_position(world, entity, Vec3{1.0f, 2.0f, 3.0f});

    world.update_transforms();

    const Vec3 position = world_position(world, entity);
    CHECK(position.x == Approx(1.0f));
    CHECK(position.y == Approx(2.0f));
    CHECK(position.z == Approx(3.0f));
}

TEST_CASE("World applies parents down a chain, however the entities were created") {
    World world;

    // The child exists before its parent, so storage order is the opposite of hierarchy order.
    const Entity grandchild = world.create_entity();
    const Entity child = world.create_entity();
    const Entity root = world.create_entity();
    REQUIRE(world.set_parent(child, root).has_value());
    REQUIRE(world.set_parent(grandchild, child).has_value());

    set_position(world, root, Vec3{10.0f, 0.0f, 0.0f});
    set_position(world, child, Vec3{0.0f, 5.0f, 0.0f});
    set_position(world, grandchild, Vec3{0.0f, 0.0f, 1.0f});

    world.update_transforms();

    const Vec3 position = world_position(world, grandchild);
    CHECK(position.x == Approx(10.0f));
    CHECK(position.y == Approx(5.0f));
    CHECK(position.z == Approx(1.0f));
}

TEST_CASE("World applies a parent's rotation and scale to its children") {
    World world;
    const Entity parent = world.create_entity();
    const Entity child = world.create_entity();
    REQUIRE(world.set_parent(child, parent).has_value());

    auto& transform = world.registry().get<Transform>(parent);
    transform.rotation = core::quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, core::k_pi * 0.5f);
    transform.scale = Vec3{2.0f, 2.0f, 2.0f};
    set_position(world, child, Vec3{1.0f, 0.0f, 0.0f});

    world.update_transforms();

    // A quarter turn around +Y sends +X to -Z.
    const Vec3 position = world_position(world, child);
    CHECK(position.x == Approx(0.0f).epsilon(0.001));
    CHECK(position.z == Approx(-2.0f));
}

TEST_CASE("World refuses a parent link that would make a loop") {
    World world;
    const Entity a = world.create_entity();
    const Entity b = world.create_entity();
    const Entity c = world.create_entity();
    REQUIRE(world.set_parent(b, a).has_value());
    REQUIRE(world.set_parent(c, b).has_value());

    CHECK_FALSE(world.set_parent(a, c).has_value());
    CHECK_FALSE(world.set_parent(a, a).has_value());
    CHECK_FALSE(world.registry().all_of<Parent>(a));
}

TEST_CASE("World refuses links to entities that don't exist") {
    World world;
    const Entity entity = world.create_entity();
    const Entity gone = world.create_entity();
    world.destroy_entity(gone);

    CHECK_FALSE(world.set_parent(entity, gone).has_value());
    CHECK_FALSE(world.set_parent(gone, entity).has_value());
}

TEST_CASE("World can remove a parent") {
    World world;
    const Entity parent = world.create_entity();
    const Entity child = world.create_entity();
    REQUIRE(world.set_parent(child, parent).has_value());
    set_position(world, parent, Vec3{5.0f, 0.0f, 0.0f});

    REQUIRE(world.set_parent(child, k_no_entity).has_value());
    world.update_transforms();

    CHECK(world_position(world, child).x == Approx(0.0f));
}

TEST_CASE("World destroys an entity's whole subtree") {
    World world;
    const Entity root = world.create_entity();
    const Entity child = world.create_entity();
    const Entity grandchild = world.create_entity();
    const Entity other = world.create_entity();
    REQUIRE(world.set_parent(child, root).has_value());
    REQUIRE(world.set_parent(grandchild, child).has_value());

    world.destroy_entity(root);

    CHECK_FALSE(world.registry().valid(root));
    CHECK_FALSE(world.registry().valid(child));
    CHECK_FALSE(world.registry().valid(grandchild));
    CHECK(world.registry().valid(other));

    world.destroy_entity(root);
}

TEST_CASE("World treats many children of one parent correctly") {
    World world;
    const Entity parent = world.create_entity();
    set_position(world, parent, Vec3{1.0f, 0.0f, 0.0f});

    std::vector<Entity> children;
    for (int index = 0; index < 100; ++index) {
        children.push_back(world.create_entity());
        REQUIRE(world.set_parent(children.back(), parent).has_value());
    }

    world.update_transforms();

    for (const Entity child : children) {
        CHECK(world_position(world, child).x == Approx(1.0f));
    }
}
