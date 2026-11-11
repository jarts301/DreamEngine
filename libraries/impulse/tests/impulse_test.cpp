#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "impulse/world.h"

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

using namespace impulse;

namespace {

constexpr float k_step = 1.0f / 60.0f;
constexpr float k_pi = std::numbers::pi_v<float>;

void run(World& world, int steps) {
    for (int i = 0; i < steps; ++i) {
        world.step(k_step);
    }
}

// A floor whose top is at y = 0.
BodyId ground(World& world) {
    return world
        .create_body(BodyDef{.shape = Box{Vec3{20.0f, 0.5f, 20.0f}},
                             .motion = Motion::Static,
                             .position = {0.0f, -0.5f, 0.0f}})
        .value();
}

BodyId box_at(World& world, Vec3 position, Quat rotation = {}) {
    return world.create_body(BodyDef{.shape = Box{}, .position = position, .rotation = rotation})
        .value();
}

bool finite(const BodyState& state) {
    return is_finite(state.position) && is_finite(state.linear_velocity) &&
           std::isfinite(state.rotation.w);
}

// The world direction of a body's own +Y axis.
Vec3 up_of(const BodyState& state) {
    return rotate(state.rotation, Vec3{0.0f, 1.0f, 0.0f});
}

} // namespace

TEST_CASE("A box dropped flat comes to rest on the ground and sleeps") {
    World world;
    ground(world);
    const BodyId box = box_at(world, Vec3{0.0f, 2.0f, 0.0f});
    run(world, 240);
    const BodyState state = world.state(box).value();
    CHECK(state.position.y == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(up_of(state).y == doctest::Approx(1.0f).epsilon(0.001));
    CHECK_FALSE(state.awake);
}

TEST_CASE("A tall stack of boxes stands and falls asleep") {
    World world;
    ground(world);
    std::vector<BodyId> stack;
    for (int i = 0; i < 12; ++i) {
        stack.push_back(box_at(world, Vec3{0.0f, 0.5f + static_cast<float>(i), 0.0f}));
    }
    run(world, 600);
    const BodyState top = world.state(stack.back()).value();
    CHECK(top.position.y == doctest::Approx(11.5f).epsilon(0.01));
    CHECK(std::hypot(top.position.x, top.position.z) < 0.05f);
    CHECK_FALSE(top.awake);
}

TEST_CASE("Tilted boxes land, tip over onto a face and settle") {
    World world;
    ground(world);
    // Balanced on an edge, and on a corner.
    const BodyId on_edge =
        box_at(world, Vec3{-3.0f, 1.5f, 0.0f}, from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 0.7f));
    const BodyId on_corner = box_at(world, Vec3{3.0f, 2.0f, 0.0f},
                                    normalized(from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, 0.6f) *
                                               from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 0.5f)));
    run(world, 600);
    for (const BodyId box : {on_edge, on_corner}) {
        const BodyState state = world.state(box).value();
        REQUIRE(finite(state));
        CHECK(state.position.y == doctest::Approx(0.5f).epsilon(0.03));
        CHECK(length(state.linear_velocity) < 0.05f);
        // Some face is down: one of the box's axes is vertical.
        const Mat3 axes = rotation_matrix(state.rotation);
        const float most_vertical = std::max(
            {std::abs(axes.rows[1].x), std::abs(axes.rows[1].y), std::abs(axes.rows[1].z)});
        CHECK(most_vertical > 0.99f);
    }
}

TEST_CASE("Boxes meeting edge to edge push each other apart") {
    World world;
    world.set_gravity(Vec3{});
    const Quat turned = normalized(from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, k_pi / 4.0f));
    const Quat crossed = normalized(from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, k_pi / 4.0f));
    const BodyId a = box_at(world, Vec3{0.0f, 0.0f, 0.0f}, turned);
    const BodyId b = box_at(world, Vec3{0.0f, 1.5f, 0.0f}, crossed);
    world.set_linear_velocity(b, Vec3{0.0f, -2.0f, 0.0f});
    bool touched = false;
    for (int i = 0; i < 120; ++i) {
        world.step(k_step);
        touched = touched || !world.contact_events().empty();
    }
    CHECK(touched);
    CHECK(world.state(b)->position.y - world.state(a)->position.y > 1.3f);
}

TEST_CASE("Capsules lie down or stand, and touch each other and spheres") {
    World world;
    ground(world);
    const BodyId lying =
        world
            .create_body(BodyDef{.shape = Capsule{0.5f, 0.25f},
                                 .position = {0.0f, 1.0f, 0.0f},
                                 .rotation = from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, k_pi / 2.0f)})
            .value();
    const BodyId standing = world
                                .create_body(BodyDef{.shape = Capsule{0.5f, 0.25f},
                                                     .position = {3.0f, 1.0f, 0.0f},
                                                     .angular_damping = 2.0f})
                                .value();
    run(world, 300);
    const BodyState flat = world.state(lying).value();
    CHECK(flat.position.y == doctest::Approx(0.25f).epsilon(0.03));
    CHECK(std::abs(up_of(flat).y) < 0.05f);
    // An upright capsule is unstable, but it stays on the ground either way.
    const BodyState upright = world.state(standing).value();
    CHECK(upright.position.y > 0.2f);
    CHECK(upright.position.y < 0.8f);

    // A ball dropped onto the lying capsule rolls off it rather than passing through.
    const BodyId ball =
        world.create_body(BodyDef{.shape = Sphere{0.2f}, .position = {0.1f, 2.0f, 0.0f}}).value();
    float lowest = 10.0f;
    for (int i = 0; i < 60; ++i) {
        world.step(k_step);
        const BodyState state = world.state(ball).value();
        if (std::abs(state.position.x) < 0.3f) {
            lowest = std::min(lowest, state.position.y);
        }
    }
    CHECK(lowest > 0.6f);
}

TEST_CASE("Continuous bodies don't pass through thin walls") {
    const auto shoot = [](bool continuous) {
        World world;
        world.set_gravity(Vec3{});
        REQUIRE(world
                    .create_body(BodyDef{.shape = Box{Vec3{0.025f, 2.0f, 2.0f}},
                                         .motion = Motion::Static,
                                         .position = {5.0f, 0.0f, 0.0f}})
                    .has_value());
        const BodyId bullet = world
                                  .create_body(BodyDef{.shape = Sphere{0.05f},
                                                       .linear_velocity = {90.0f, 0.0f, 0.0f},
                                                       .continuous = continuous})
                                  .value();
        run(world, 30);
        return world.state(bullet)->position.x;
    };
    CHECK(shoot(true) < 5.0f);
    CHECK(shoot(false) > 5.0f);
}

TEST_CASE("Rays hit rotated boxes, capsules and spheres from outside and inside") {
    World world;
    const BodyId box =
        world
            .create_body(BodyDef{.shape = Box{Vec3{1.0f, 1.0f, 1.0f}},
                                 .motion = Motion::Static,
                                 .rotation = from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, k_pi / 4.0f)})
            .value();
    const std::optional<RayHit> corner =
        world.cast_ray(Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 10.0f);
    REQUIRE(corner.has_value());
    CHECK(corner->body == box);
    CHECK(corner->distance == doctest::Approx(5.0f - std::sqrt(2.0f)).epsilon(0.001));

    const std::optional<RayHit> inside =
        world.cast_ray(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 10.0f);
    REQUIRE(inside.has_value());
    CHECK(inside->distance == doctest::Approx(0.0f));

    World round;
    const BodyId capsule =
        round
            .create_body(BodyDef{.shape = Capsule{1.0f, 0.5f},
                                 .motion = Motion::Static,
                                 .rotation = from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, k_pi / 2.0f)})
            .value();
    // Lying along x: the side is hit from above, and an end from the side.
    const std::optional<RayHit> side =
        round.cast_ray(Vec3{0.5f, 5.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f);
    REQUIRE(side.has_value());
    CHECK(side->body == capsule);
    CHECK(side->distance == doctest::Approx(4.5f).epsilon(0.001));
    CHECK(side->normal.y == doctest::Approx(1.0f).epsilon(0.001));
    const std::optional<RayHit> end =
        round.cast_ray(Vec3{-5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}, 10.0f);
    REQUIRE(end.has_value());
    CHECK(end->distance == doctest::Approx(3.5f).epsilon(0.001));
    CHECK(end->normal.x == doctest::Approx(-1.0f).epsilon(0.001));
    // Behind the ray, nothing.
    CHECK_FALSE(round.cast_ray(Vec3{0.0f, 5.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 10.0f).has_value());
}

TEST_CASE("Fixed joints hold two bodies as one") {
    World world;
    const BodyId a = box_at(world, Vec3{0.0f, 5.0f, 0.0f});
    const BodyId b =
        box_at(world, Vec3{1.5f, 5.0f, 0.0f}, from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 0.3f));
    REQUIRE(world.create_joint(JointDef{.a = a, .b = b, .type = FixedJoint{}}).has_value());
    world.set_angular_velocity(a, Vec3{0.0f, 2.0f, 1.0f});
    run(world, 120);
    const BodyState sa = world.state(a).value();
    const BodyState sb = world.state(b).value();
    const Quat relative = conjugate(sa.rotation) * sb.rotation;
    const Quat expected = from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 0.3f);
    CHECK(std::abs(dot(relative, expected)) == doctest::Approx(1.0f).epsilon(0.001));
    const Vec3 offset = rotate(conjugate(sa.rotation), sb.position - sa.position);
    CHECK(offset.x == doctest::Approx(1.5f).epsilon(0.01));
    CHECK(std::abs(offset.y) < 0.015f);
}

TEST_CASE("Removing a support wakes what rests on it") {
    World world;
    ground(world);
    const BodyId bottom = box_at(world, Vec3{0.0f, 0.5f, 0.0f});
    const BodyId upper = box_at(world, Vec3{0.0f, 1.5f, 0.0f});
    run(world, 240);
    REQUIRE_FALSE(world.state(upper)->awake);
    world.destroy_body(bottom);
    run(world, 60);
    CHECK(world.state(upper)->position.y == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("Worlds given the same input give the same output") {
    const auto simulate = [] {
        World world;
        ground(world);
        for (int i = 0; i < 6; ++i) {
            const auto f = static_cast<float>(i);
            box_at(world, Vec3{0.3f * f, 0.6f + 1.1f * f, 0.1f * f},
                   from_axis_angle(normalized(Vec3{1.0f, f, 0.5f}), 0.4f * f));
        }
        run(world, 200);
        std::vector<float> result;
        for (std::uint32_t i = 1; i <= 6; ++i) {
            const BodyState state = world.state(BodyId{i, 1}).value();
            result.insert(result.end(), {state.position.x, state.position.y, state.position.z});
        }
        return result;
    };
    CHECK(simulate() == simulate());
}

TEST_CASE("Invalid definitions are refused") {
    World world;
    CHECK_FALSE(world.create_body(BodyDef{.shape = Sphere{0.0f}}).has_value());
    CHECK_FALSE(world.create_body(BodyDef{.shape = Box{}, .mass = -1.0f}).has_value());
    CHECK_FALSE(world.create_body(BodyDef{.shape = Box{}, .restitution = 1.5f}).has_value());
    const BodyId a = box_at(world, Vec3{});
    CHECK_FALSE(world.create_joint(JointDef{.a = a, .b = a}).has_value());
    CHECK_FALSE(world.create_joint(JointDef{.a = a, .type = HingeJoint{.axis = {}}}).has_value());
    CHECK_FALSE(
        world.create_joint(JointDef{.a = a, .type = HingeJoint{.lower = 0.5f}}).has_value());
    CHECK_FALSE(world
                    .create_joint(JointDef{
                        .a = a, .type = DistanceJoint{.min_distance = 2.0f, .max_distance = 1.0f}})
                    .has_value());
    world.step(0.0f);
    world.step(-1.0f);
    CHECK(world.state(a)->position.y == doctest::Approx(0.0f));
}
