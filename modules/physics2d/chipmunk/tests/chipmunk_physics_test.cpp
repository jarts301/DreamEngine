#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/collision_conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <cmath>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_physics2d_chipmunk(core::ModuleRegistry& registry);
}

std::unique_ptr<dream::physics2d::PhysicsWorld> make_world() {
    using namespace dream;
    core::ModuleRegistry registry;
    REQUIRE(modules::register_physics2d_chipmunk(registry).has_value());

    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"chipmunk"};
    auto created = registry.create<physics2d::PhysicsWorld>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

TEST_CASE("Chipmunk fixed joints keep bodies' relative angle and distance") {
    using namespace conformance;
    const auto world = make_world();
    world->set_gravity(dream::core::Vec2{});

    BodyDesc tilted = floating_ball({2.0f, 0.0f});
    tilted.angle = 0.4f;
    const BodyHandle a = world->create_body(floating_ball()).value();
    const BodyHandle b = world->create_body(tilted).value();
    REQUIRE(
        world->create_joint(JointDesc{.body_a = a, .body_b = b, .type = FixedJoint{}}).has_value());

    world->set_angular_velocity(a, 3.0f);
    run(*world, 120);
    const BodyState state_a = world->body_state(a).value();
    const BodyState state_b = world->body_state(b).value();
    CHECK(state_a.angle - state_b.angle == doctest::Approx(-0.4f).epsilon(0.02));
    CHECK(dream::core::length(state_a.position - state_b.position) ==
          doctest::Approx(2.0f).epsilon(0.02));
}

TEST_CASE("Chipmunk worlds can coexist") {
    auto first = make_world();
    auto second = make_world();
    REQUIRE(first->create_body(conformance::floating_ball()).has_value());
    first.reset();
    second->step(conformance::k_step);
    CHECK(second->body_count() == 0);
}

// The 2D scene of samples/physics_demo (assets/scenes/flatland.toml) and its scripted check: the
// stack of boxes still stands, and a ball dropped on the seesaw's right end tips it down. The check
// steps 120 times a second, and games usually 60.
TEST_CASE("Chipmunk runs the physics demo's 2D scene as scripted") {
    using namespace conformance;
    using dream::core::Vec2;
    for (const int rate : {60, 120}) {
        CAPTURE(rate);
        const float step = 1.0f / static_cast<float>(rate);
        const auto world = make_world();

        REQUIRE(world
                    ->create_body(BodyDesc{.shape = BoxShape{Vec2{12.0f, 0.5f}},
                                           .motion = MotionType::Static,
                                           .position = {0.0f, -0.5f},
                                           .friction = 0.8f})
                    .has_value());
        BodyHandle top;
        for (const float y : {0.5f, 1.5f, 2.5f}) {
            top = world->create_body(BodyDesc{.shape = BoxShape{}, .position = {-5.0f, y}}).value();
        }
        const BodyHandle plank =
            world
                ->create_body(BodyDesc{
                    .shape = BoxShape{Vec2{3.0f, 0.15f}}, .position = {4.0f, 1.35f}, .mass = 3.0f})
                .value();
        REQUIRE(world
                    ->create_joint(JointDesc{
                        .body_a = plank,
                        .type = HingeJoint{.anchor = {4.0f, 1.35f},
                                           .min_angle_radians = -25.0f * dream::core::k_pi / 180.0f,
                                           .max_angle_radians = 25.0f * dream::core::k_pi / 180.0f},
                    })
                    .has_value());
        const BodyHandle bob = world
                                   ->create_body(BodyDesc{.shape = CircleShape{0.4f},
                                                          .position = {-1.0f, 7.5f},
                                                          .linear_velocity = {4.0f, 0.0f}})
                                   .value();
        REQUIRE(world
                    ->create_joint(JointDesc{
                        .body_a = bob,
                        .type = DistanceJoint{.anchor_a = {-1.0f, 7.5f},
                                              .anchor_b = {-1.0f, 10.5f},
                                              .max_distance = 3.0f},
                    })
                    .has_value());

        int contacts = 0;
        const int steps = static_cast<int>(4.2f * static_cast<float>(rate) + 0.5f);
        const int drop = static_cast<int>(2.6f * static_cast<float>(rate) + 0.5f);
        for (int i = 1; i <= steps; ++i) {
            if (i == drop) {
                // As the demo's script drops a ball.
                REQUIRE(world
                            ->create_body(BodyDesc{.shape = CircleShape{0.45f},
                                                   .position = {6.0f, 4.0f},
                                                   .mass = 4.0f,
                                                   .restitution = 0.2f})
                            .has_value());
            }
            world->step(step);
            contacts += static_cast<int>(world->contact_events().size());
        }

        const BodyState stacked = world->body_state(top).value();
        CHECK(std::abs(stacked.position.y - 2.5f) < 0.1f);
        CHECK(std::abs(stacked.position.x + 5.0f) < 0.1f);
        CHECK(world->body_state(plank)->angle * 180.0f / dream::core::k_pi < -15.0f);
        CHECK(dream::core::length(world->body_state(bob)->position - Vec2{-1.0f, 10.5f}) < 3.05f);
        CHECK(contacts > 0);
    }
}
