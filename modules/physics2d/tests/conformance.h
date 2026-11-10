#pragma once

// Tests that every physics2d backend must pass. A backend's test program defines make_world() and
// includes this file once. They avoid collisions, so a backend without them passes too.

#include "dream/physics2d/physics2d.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <memory>

[[nodiscard]] std::unique_ptr<dream::physics2d::PhysicsWorld> make_world();

namespace conformance {

using namespace dream;
using namespace dream::physics2d;

constexpr float k_step = 1.0f / 60.0f;

inline void run(PhysicsWorld& world, int steps) {
    for (int i = 0; i < steps; ++i) {
        world.step(k_step);
    }
}

inline BodyDesc floating_ball(core::Vec2 position = {}) {
    return BodyDesc{
        .shape = CircleShape{0.5f},
        .position = position,
        .linear_damping = 0.0f,
        .angular_damping = 0.0f,
    };
}

} // namespace conformance

TEST_CASE("Physics2D rejects invalid bodies") {
    using namespace conformance;
    const auto world = make_world();

    BodyDesc desc = floating_ball();
    desc.shape = BoxShape{core::Vec2{1.0f, 0.0f}};
    CHECK_FALSE(world->create_body(desc).has_value());

    desc = floating_ball();
    desc.mass = 0.0f;
    CHECK_FALSE(world->create_body(desc).has_value());

    desc = floating_ball();
    desc.position.y = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(world->create_body(desc).has_value());

    desc = floating_ball();
    desc.restitution = 2.0f;
    CHECK_FALSE(world->create_body(desc).has_value());

    // Mass doesn't matter for a body that never moves.
    desc = floating_ball();
    desc.motion = MotionType::Static;
    desc.mass = 0.0f;
    CHECK(world->create_body(desc).has_value());
    CHECK(world->body_count() == 1);
}

TEST_CASE("Physics2D bodies start where they are described and can be destroyed") {
    using namespace conformance;
    const auto world = make_world();

    const BodyHandle body = world->create_body(floating_ball({1.0f, 2.0f})).value();
    const std::optional<BodyState> state = world->body_state(body);
    REQUIRE(state.has_value());
    CHECK(state->position.x == doctest::Approx(1.0f));
    CHECK(state->position.y == doctest::Approx(2.0f));

    world->destroy_body(body);
    world->destroy_body(body);
    CHECK_FALSE(world->body_state(body).has_value());
    CHECK(world->body_count() == 0);

    // A new body doesn't bring the old handle back.
    const BodyHandle next = world->create_body(floating_ball()).value();
    CHECK(next != body);
    CHECK_FALSE(world->body_state(body).has_value());

    // Calls with a stale handle are ignored.
    world->add_impulse(body, core::Vec2{1.0f, 0.0f});
    world->set_transform(body, core::Vec2{}, 0.0f);
}

TEST_CASE("Physics2D dynamic bodies fall under gravity and static ones don't move") {
    using namespace conformance;
    const auto world = make_world();
    CHECK(world->gravity().y == doctest::Approx(-9.81f));

    const BodyHandle falling = world->create_body(floating_ball({0.0f, 100.0f})).value();

    BodyDesc floating = floating_ball({10.0f, 100.0f});
    floating.gravity_scale = 0.0f;
    const BodyHandle weightless = world->create_body(floating).value();

    BodyDesc fixed = floating_ball({20.0f, 100.0f});
    fixed.motion = MotionType::Static;
    const BodyHandle wall = world->create_body(fixed).value();

    run(*world, 60);

    // Free fall for one second: about 4.9 m, at about 9.8 m/s.
    const BodyState fallen = world->body_state(falling).value();
    CHECK(fallen.position.y == doctest::Approx(100.0f - 4.9f).epsilon(0.002));
    CHECK(fallen.linear_velocity.y == doctest::Approx(-9.81f).epsilon(0.02));
    CHECK(world->body_state(weightless)->position.y == doctest::Approx(100.0f));
    CHECK(world->body_state(wall)->position.y == doctest::Approx(100.0f));
}

TEST_CASE("Physics2D gravity can be changed") {
    using namespace conformance;
    const auto world = make_world();
    world->set_gravity(core::Vec2{2.0f, 0.0f});
    CHECK(world->gravity().x == doctest::Approx(2.0f));

    const BodyHandle body = world->create_body(floating_ball()).value();
    run(*world, 60);
    CHECK(world->body_state(body)->linear_velocity.x == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(world->body_state(body)->position.y == doctest::Approx(0.0f));
}

TEST_CASE("Physics2D impulses, forces and velocities move dynamic bodies") {
    using namespace conformance;
    const auto world = make_world();
    world->set_gravity(core::Vec2{});

    BodyDesc desc = floating_ball();
    desc.mass = 2.0f;
    const BodyHandle body = world->create_body(desc).value();

    world->add_impulse(body, core::Vec2{4.0f, 0.0f});
    world->step(k_step);
    CHECK(world->body_state(body)->linear_velocity.x == doctest::Approx(2.0f).epsilon(0.01));

    world->set_linear_velocity(body, core::Vec2{-3.0f, 0.0f});
    world->step(k_step);
    CHECK(world->body_state(body)->linear_velocity.x == doctest::Approx(-3.0f).epsilon(0.01));
    world->set_linear_velocity(body, core::Vec2{});
    world->step(k_step);
    CHECK(world->body_state(body)->linear_velocity.x == doctest::Approx(0.0f));

    // 6 N on 2 kg for one second adds 3 m/s.
    world->set_linear_velocity(body, core::Vec2{});
    for (int i = 0; i < 60; ++i) {
        world->add_force(body, core::Vec2{0.0f, 6.0f});
        world->step(k_step);
    }
    CHECK(world->body_state(body)->linear_velocity.y == doctest::Approx(3.0f).epsilon(0.02));

    world->set_angular_velocity(body, 1.0f);
    world->step(k_step);
    CHECK(world->body_state(body)->angular_velocity == doctest::Approx(1.0f).epsilon(0.02));
}

TEST_CASE("Physics2D kinematic bodies reach their target in one step") {
    using namespace conformance;
    const auto world = make_world();

    BodyDesc desc = floating_ball();
    desc.motion = MotionType::Kinematic;
    const BodyHandle body = world->create_body(desc).value();

    // A modest turn: engines such as Box2D approximate rotation within a step, which only shows at
    // tens of radians per second.
    world->move_kinematic(body, core::Vec2{1.0f, 0.0f}, 0.05f, k_step);
    world->step(k_step);

    const BodyState state = world->body_state(body).value();
    CHECK(state.position.x == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(state.position.y == doctest::Approx(0.0f));
    CHECK(state.angle == doctest::Approx(0.05f).epsilon(0.001));
}

TEST_CASE("Physics2D set_transform moves a body at once") {
    using namespace conformance;
    const auto world = make_world();
    const BodyHandle body = world->create_body(floating_ball()).value();

    world->set_transform(body, core::Vec2{5.0f, 6.0f}, 1.0f);
    CHECK(world->body_state(body)->position.y == doctest::Approx(6.0f));
    CHECK(world->body_state(body)->angle == doctest::Approx(1.0f));
}

TEST_CASE("Physics2D joints need existing bodies and go away with them") {
    using namespace conformance;
    const auto world = make_world();
    const BodyHandle a = world->create_body(floating_ball()).value();
    const BodyHandle b = world->create_body(floating_ball({2.0f, 0.0f})).value();

    CHECK_FALSE(world->create_joint(JointDesc{.body_a = a, .body_b = a}).has_value());
    CHECK_FALSE(world->create_joint(JointDesc{.body_a = BodyHandle{}}).has_value());

    const JointHandle hinge =
        world
            ->create_joint(JointDesc{
                .body_a = a,
                .body_b = b,
                .type = HingeJoint{.anchor = {1.0f, 0.0f}},
            })
            .value();
    const JointHandle to_world =
        world->create_joint(JointDesc{.body_a = b, .type = HingeJoint{.anchor = {2.0f, 1.0f}}})
            .value();
    CHECK(world
              ->create_joint(JointDesc{
                  .body_a = a,
                  .body_b = b,
                  .type = DistanceJoint{.anchor_a = {}, .anchor_b = {2.0f, 0.0f},
                                        .min_distance = 1.0f, .max_distance = 3.0f},
              })
              .has_value());
    CHECK(world->create_joint(JointDesc{.body_a = a, .body_b = b, .type = FixedJoint{}})
              .has_value());

    world->destroy_body(b);
    world->destroy_joint(hinge);
    world->destroy_joint(to_world);
    run(*world, 2);
    CHECK(world->body_count() == 1);
}

TEST_CASE("Physics2D queries find nothing in an empty world") {
    using namespace conformance;
    const auto world = make_world();
    CHECK_FALSE(world->cast_ray(core::Vec2{}, core::Vec2{0.0f, -1.0f}, 100.0f).has_value());

    std::vector<BodyHandle> found{BodyHandle{}};
    world->overlap(CircleShape{1.0f}, core::Vec2{}, 0.0f, found);
    CHECK(found.empty());
    CHECK(world->contact_events().empty());
}
