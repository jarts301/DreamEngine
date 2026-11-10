#pragma once

// Tests for backends that simulate collisions. Include after conformance.h.

#include "tests/conformance.h"

#include <algorithm>
#include <cmath>

namespace conformance {

// A floor whose top is at y = 0.5.
inline BodyHandle add_ground(PhysicsWorld& world, std::uint64_t user_data = 1) {
    return world
        .create_body(BodyDesc{
            .shape = BoxShape{core::Vec2{20.0f, 0.5f}},
            .motion = MotionType::Static,
            .user_data = user_data,
        })
        .value();
}

inline const ContactEvent* find_contact(const PhysicsWorld& world, ContactEvent::Kind kind,
                                        std::uint64_t a, std::uint64_t b) {
    for (const ContactEvent& event : world.contact_events()) {
        if (event.kind == kind && ((event.user_data_a == a && event.user_data_b == b) ||
                                   (event.user_data_a == b && event.user_data_b == a))) {
            return &event;
        }
    }
    return nullptr;
}

} // namespace conformance

TEST_CASE("Physics2D a dropped ball comes to rest on the ground, then sleeps") {
    using namespace conformance;
    const auto world = make_world();
    add_ground(*world);

    BodyDesc desc = floating_ball({0.0f, 3.0f});
    desc.angular_damping = 0.05f;
    const BodyHandle ball = world->create_body(desc).value();

    run(*world, 180);
    const BodyState rested = world->body_state(ball).value();
    CHECK(rested.position.y == doctest::Approx(1.0f).epsilon(0.03));
    CHECK(core::length(rested.linear_velocity) < 0.05f);

    run(*world, 180);
    CHECK_FALSE(world->body_state(ball)->is_awake);

    world->add_impulse(ball, core::Vec2{1.0f, 0.0f});
    CHECK(world->body_state(ball)->is_awake);
}

TEST_CASE("Physics2D reports when contacts begin and end") {
    using namespace conformance;
    const auto world = make_world();
    add_ground(*world, 1);

    BodyDesc desc = floating_ball({0.0f, 1.2f});
    desc.user_data = 2;
    const BodyHandle ball = world->create_body(desc).value();

    const ContactEvent* began = nullptr;
    for (int i = 0; i < 60 && began == nullptr; ++i) {
        world->step(k_step);
        began = find_contact(*world, ContactEvent::Kind::Began, 1, 2);
    }
    REQUIRE(began != nullptr);
    CHECK((began->body_a == ball || began->body_b == ball));
    CHECK(began->point.y == doctest::Approx(0.5f).epsilon(0.1));

    // The normal points from body a to body b.
    const float expected = began->user_data_a == 1 ? 1.0f : -1.0f;
    CHECK(began->normal.y == doctest::Approx(expected).epsilon(0.05));

    world->set_transform(ball, core::Vec2{0.0f, 10.0f}, 0.0f);
    bool ended = false;
    for (int i = 0; i < 5 && !ended; ++i) {
        world->step(k_step);
        ended = find_contact(*world, ContactEvent::Kind::Ended, 1, 2) != nullptr;
    }
    CHECK(ended);
}

TEST_CASE("Physics2D reports a contact ending when a body is destroyed") {
    using namespace conformance;
    const auto world = make_world();
    add_ground(*world, 1);
    BodyDesc desc = floating_ball({0.0f, 1.0f});
    desc.user_data = 7;
    const BodyHandle ball = world->create_body(desc).value();
    run(*world, 10);

    world->destroy_body(ball);
    bool ended = false;
    for (int i = 0; i < 3 && !ended; ++i) {
        world->step(k_step);
        ended = find_contact(*world, ContactEvent::Kind::Ended, 1, 7) != nullptr;
    }
    CHECK(ended);
}

TEST_CASE("Physics2D rays hit the closest solid body") {
    using namespace conformance;
    const auto world = make_world();
    const BodyHandle ground = add_ground(*world, 42);

    REQUIRE(world
                ->create_body(BodyDesc{
                    .shape = BoxShape{core::Vec2{1.0f, 1.0f}},
                    .motion = MotionType::Static,
                    .position = {0.0f, 4.0f},
                    .is_sensor = true,
                })
                .has_value());

    const std::optional<RayHit> hit =
        world->cast_ray(core::Vec2{0.0f, 10.0f}, core::Vec2{0.0f, -2.0f}, 50.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->body == ground);
    CHECK(hit->user_data == 42);
    CHECK(hit->distance == doctest::Approx(9.5f).epsilon(0.01));
    CHECK(hit->point.y == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(0.01));

    CHECK_FALSE(world->cast_ray(core::Vec2{0.0f, 10.0f}, core::Vec2{0.0f, -1.0f}, 5.0f).has_value());
    CHECK_FALSE(world->cast_ray(core::Vec2{0.0f, 10.0f}, core::Vec2{0.0f, 1.0f}, 50.0f).has_value());
}

TEST_CASE("Physics2D overlap finds the bodies a shape touches") {
    using namespace conformance;
    const auto world = make_world();
    const BodyHandle ground = add_ground(*world);
    const BodyHandle ball = world->create_body(floating_ball({5.0f, 3.0f})).value();

    std::vector<BodyHandle> found;
    // Reaches from y = 0 to 3: into the ground, whose top is at 0.5, and the ball, from 2.5.
    world->overlap(CircleShape{1.5f}, core::Vec2{5.0f, 1.5f}, 0.0f, found);
    CHECK(found.size() == 2);
    CHECK(std::find(found.begin(), found.end(), ground) != found.end());
    CHECK(std::find(found.begin(), found.end(), ball) != found.end());

    world->overlap(BoxShape{}, core::Vec2{-5.0f, 3.0f}, 0.0f, found);
    CHECK(found.empty());
}

TEST_CASE("Physics2D sensors report contacts without stopping anything") {
    using namespace conformance;
    const auto world = make_world();
    REQUIRE(world
                ->create_body(BodyDesc{
                    .shape = BoxShape{core::Vec2{2.0f, 0.5f}},
                    .motion = MotionType::Static,
                    .is_sensor = true,
                    .user_data = 1,
                })
                .has_value());
    BodyDesc desc = floating_ball({0.0f, 2.0f});
    desc.user_data = 2;
    const BodyHandle ball = world->create_body(desc).value();

    bool began = false;
    for (int i = 0; i < 90; ++i) {
        world->step(k_step);
        began = began || find_contact(*world, ContactEvent::Kind::Began, 1, 2) != nullptr;
    }
    CHECK(began);
    CHECK(world->body_state(ball)->position.y < -3.0f);
}

TEST_CASE("Physics2D bouncy bodies bounce higher") {
    using namespace conformance;
    const auto world = make_world();
    add_ground(*world);

    BodyDesc dull = floating_ball({-3.0f, 3.0f});
    BodyDesc bouncy = floating_ball({3.0f, 3.0f});
    bouncy.restitution = 0.9f;
    const BodyHandle dull_ball = world->create_body(dull).value();
    const BodyHandle bouncy_ball = world->create_body(bouncy).value();

    float dull_peak = 0.0f;
    float bouncy_peak = 0.0f;
    bool bounced = false;
    for (int i = 0; i < 120; ++i) {
        world->step(k_step);
        const BodyState state = world->body_state(bouncy_ball).value();
        bounced = bounced || state.linear_velocity.y > 0.5f;
        if (bounced) {
            bouncy_peak = std::max(bouncy_peak, state.position.y);
            dull_peak = std::max(dull_peak, world->body_state(dull_ball)->position.y);
        }
    }
    CHECK(bounced);
    CHECK(bouncy_peak > 2.0f);
    CHECK(dull_peak < 1.2f);
}

TEST_CASE("Physics2D kinematic bodies push dynamic ones") {
    using namespace conformance;
    const auto world = make_world();
    add_ground(*world);

    const BodyHandle pusher = world
                                  ->create_body(BodyDesc{
                                      .shape = BoxShape{core::Vec2{0.5f, 0.5f}},
                                      .motion = MotionType::Kinematic,
                                      .position = {-3.0f, 1.0f},
                                  })
                                  .value();
    const BodyHandle crate =
        world->create_body(BodyDesc{.shape = BoxShape{}, .position = {0.0f, 1.0f}}).value();

    for (int i = 1; i <= 120; ++i) {
        const float x = -3.0f + 6.0f * static_cast<float>(i) / 120.0f;
        world->move_kinematic(pusher, core::Vec2{x, 1.0f}, 0.0f, k_step);
        world->step(k_step);
    }
    CHECK(world->body_state(pusher)->position.x == doctest::Approx(3.0f).epsilon(0.01));
    CHECK(world->body_state(crate)->position.x > 3.5f);
}

TEST_CASE("Physics2D fixed rotation keeps a body upright") {
    using namespace conformance;
    const auto world = make_world();
    add_ground(*world);

    BodyDesc desc{.shape = BoxShape{}, .position = {0.0f, 1.0f}, .fixed_rotation = true};
    const BodyHandle box = world->create_body(desc).value();
    world->add_impulse(box, core::Vec2{0.0f, 5.0f});
    world->set_angular_velocity(box, 4.0f);
    run(*world, 60);
    CHECK(world->body_state(box)->angle == doctest::Approx(0.0f));
}

TEST_CASE("Physics2D joints hold bodies together") {
    using namespace conformance;
    const auto world = make_world();

    const core::Vec2 pivot{0.0f, 10.0f};
    const BodyHandle bob = world->create_body(floating_ball({2.0f, 10.0f})).value();
    REQUIRE(world->create_joint(JointDesc{.body_a = bob, .type = HingeJoint{.anchor = pivot}})
                .has_value());

    const BodyHandle tail = world->create_body(floating_ball({4.0f, 10.0f})).value();
    REQUIRE(world
                ->create_joint(JointDesc{
                    .body_a = bob,
                    .body_b = tail,
                    .type = DistanceJoint{.anchor_a = {2.0f, 10.0f},
                                          .anchor_b = {4.0f, 10.0f},
                                          .min_distance = 1.0f,
                                          .max_distance = 3.0f},
                })
                .has_value());

    float lowest = 10.0f;
    for (int i = 0; i < 180; ++i) {
        world->step(k_step);
        const BodyState bob_state = world->body_state(bob).value();
        const BodyState tail_state = world->body_state(tail).value();
        lowest = std::min(lowest, bob_state.position.y);
        CHECK(core::length(bob_state.position - pivot) == doctest::Approx(2.0f).epsilon(0.03));
        CHECK(core::length(tail_state.position - bob_state.position) < 3.1f);
    }
    CHECK(lowest < 8.5f);

    // A lever pinned at its left end that may only turn a quarter turn counter-clockwise.
    const BodyHandle lever = world
                                 ->create_body(BodyDesc{
                                     .shape = BoxShape{core::Vec2{0.5f, 0.05f}},
                                     .position = {20.5f, 1.0f},
                                     .gravity_scale = 0.0f,
                                 })
                                 .value();
    REQUIRE(world
                ->create_joint(JointDesc{
                    .body_a = lever,
                    .type = HingeJoint{.anchor = {20.0f, 1.0f},
                                       .min_angle_radians = 0.0f,
                                       .max_angle_radians = core::k_pi / 2.0f},
                })
                .has_value());
    world->set_angular_velocity(lever, 5.0f);
    run(*world, 120);
    const BodyState lever_state = world->body_state(lever).value();
    const core::Vec2 facing{std::cos(lever_state.angle), std::sin(lever_state.angle)};
    CHECK(core::length(lever_state.position - facing * 0.5f - core::Vec2{20.0f, 1.0f}) < 0.05f);
    CHECK(facing.x > -0.05f);
    CHECK(facing.y > 0.95f);
}
