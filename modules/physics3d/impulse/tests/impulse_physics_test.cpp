#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/collision_conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_physics3d_impulse(core::ModuleRegistry& registry);
}

std::unique_ptr<dream::physics3d::PhysicsWorld> make_world() {
    using namespace dream;
    core::ModuleRegistry registry;
    REQUIRE(modules::register_physics3d_impulse(registry).has_value());

    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"impulse"};
    auto created = registry.create<physics3d::PhysicsWorld>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

namespace {

using namespace dream;
using namespace dream::physics3d;
using core::Vec3;

// The direction from a scene camera through the middle of a 1280 by 720 view, `down` of the way
// from the middle to the bottom edge, as the samples aim their shots.
Vec3 aim(Vec3 eye, Vec3 target, float vertical_fov_degrees, float down) {
    const Vec3 forward = core::normalized(target - eye);
    const Vec3 right = core::normalized(core::cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
    const Vec3 up = core::cross(right, forward);
    const float half_height = std::tan(core::radians(vertical_fov_degrees) * 0.5f);
    return core::normalized(forward - up * (down * half_height));
}

BodyHandle crate(PhysicsWorld& world, Vec3 position, float mass, std::uint64_t user_data = 0) {
    return world
        .create_body(BodyDesc{
            .shape = BoxShape{}, .position = position, .mass = mass, .user_data = user_data})
        .value();
}

} // namespace

// The 3D scene of samples/physics_demo (assets/scenes/playground.toml) and its scripted check,
// stepped 120 times a second as the check runs.
TEST_CASE("impulse runs the physics demo's 3D scene as scripted") {
    const auto world = make_world();
    constexpr float step = 1.0f / 120.0f;

    REQUIRE(world
                ->create_body(BodyDesc{.shape = BoxShape{Vec3{12.0f, 0.5f, 12.0f}},
                                       .motion = MotionType::Static,
                                       .position = {0.0f, -0.5f, 0.0f},
                                       .friction = 0.8f})
                .has_value());
    const Vec3 crates[] = {{-1.5f, 0.5f, 0.0f}, {-0.5f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.0f},
                           {1.5f, 0.5f, 0.0f},  {-1.0f, 1.5f, 0.0f}, {0.0f, 1.5f, 0.0f},
                           {1.0f, 1.5f, 0.0f},  {-0.5f, 2.5f, 0.0f}, {0.5f, 2.5f, 0.0f},
                           {0.0f, 3.5f, 0.0f}};
    BodyHandle top;
    for (const Vec3 position : crates) {
        top = crate(*world, position, 4.0f);
    }
    REQUIRE(world
                ->create_body(BodyDesc{.shape = SphereShape{0.6f},
                                       .position = {-5.0f, 4.0f, 2.0f},
                                       .restitution = 0.8f})
                .has_value());
    REQUIRE(world
                ->create_body(BodyDesc{.shape = SphereShape{0.45f},
                                       .position = {-3.5f, 6.0f, 3.0f},
                                       .restitution = 0.3f})
                .has_value());

    const Vec3 pivot{-7.0f, 5.5f, -2.0f};
    const BodyHandle bob = world
                               ->create_body(BodyDesc{.shape = SphereShape{0.5f},
                                                      .position = {-7.0f, 2.5f, -2.0f},
                                                      .linear_velocity = {0.0f, 0.0f, 5.0f},
                                                      .mass = 3.0f,
                                                      .linear_damping = 0.0f,
                                                      .angular_damping = 0.0f})
                               .value();
    REQUIRE(world->create_joint(JointDesc{.body_a = bob, .type = PointJoint{pivot}}).has_value());

    const BodyHandle gate = world
                                ->create_body(BodyDesc{.shape = BoxShape{Vec3{1.0f, 1.0f, 0.1f}},
                                                       .position = {5.5f, 1.05f, 3.0f},
                                                       .mass = 6.0f})
                                .value();
    REQUIRE(world
                ->create_joint(JointDesc{
                    .body_a = gate,
                    .type = HingeJoint{.anchor = {4.5f, 1.05f, 3.0f},
                                       .axis = {0.0f, 1.0f, 0.0f},
                                       .min_angle_radians = -core::k_pi / 2.0f,
                                       .max_angle_radians = core::k_pi / 2.0f},
                })
                .has_value());

    const Vec3 paddle_position{5.0f, 0.35f, -3.5f};
    const BodyHandle paddle = world
                                  ->create_body(BodyDesc{.shape = BoxShape{Vec3{2.5f, 0.3f, 0.3f}},
                                                         .motion = MotionType::Kinematic,
                                                         .position = paddle_position})
                                  .value();
    const BodyHandle paddle_ball =
        world->create_body(BodyDesc{.shape = SphereShape{0.5f}, .position = {6.5f, 0.5f, -3.5f}})
            .value();
    REQUIRE(world
                ->create_body(BodyDesc{.shape = BoxShape{Vec3{2.0f, 1.5f, 1.5f}},
                                       .motion = MotionType::Static,
                                       .position = {0.0f, 1.5f, 5.0f},
                                       .is_sensor = true,
                                       .user_data = 99})
                .has_value());

    const Vec3 eye{0.0f, 7.0f, 17.0f};
    const Vec3 shot = aim(eye, Vec3{0.0f, 2.0f, 0.0f}, 50.0f, 0.0f);
    int zone_entries = 0;
    float paddle_angle = 0.0f;
    for (int i = 1; i <= 504; ++i) {
        // The paddle turns as the demo's Spin component turns it.
        paddle_angle += 1.5f * step;
        world->move_kinematic(paddle, paddle_position,
                              core::quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, paddle_angle),
                              step);
        if (i == 132 || i == 168) {
            // At 1.1 and 1.4 seconds, shots through the middle of the view.
            REQUIRE(world
                        ->create_body(BodyDesc{.shape = SphereShape{0.35f},
                                               .position = eye + shot,
                                               .linear_velocity = shot * 22.0f,
                                               .mass = 2.0f,
                                               .restitution = 0.3f,
                                               .continuous_collision = true})
                        .has_value());
        }
        world->step(step);
        for (const ContactEvent& event : world->contact_events()) {
            if (event.kind == ContactEvent::Kind::Began &&
                (event.user_data_a == 99 || event.user_data_b == 99)) {
                ++zone_entries;
            }
        }

        if (i == 120) {
            // The demo measures at one second.
            const BodyState top_state = world->body_state(top).value();
            CHECK(std::abs(top_state.position.y - 3.5f) < 0.1f);
            CHECK(std::abs(top_state.position.x) < 0.1f);
            const float rope = core::length(world->body_state(bob)->position - pivot);
            CHECK(std::abs(rope - 3.0f) < 0.05f);
            const std::optional<RayHit> down =
                world->cast_ray(Vec3{0.0f, 10.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
            REQUIRE(down.has_value());
            CHECK(down->body == top);
        }
    }
    CHECK(zone_entries >= 1);
    // The paddle swept its ball away, and the gate is still on its hinge.
    CHECK(core::length(world->body_state(paddle_ball)->position - Vec3{6.5f, 0.5f, -3.5f}) > 0.5f);
    const BodyState gate_state = world->body_state(gate).value();
    const Vec3 edge = core::rotate(gate_state.rotation, Vec3{-1.0f, 0.0f, 0.0f});
    CHECK(core::length(gate_state.position + edge - Vec3{4.5f, 1.05f, 3.0f}) < 0.05f);
}

// The scene of samples/arcade_demo (assets/scenes/range.toml) and its scripted throws: balls must
// hit target crates.
TEST_CASE("impulse runs the arcade demo's throws") {
    const auto world = make_world();
    constexpr float step = 1.0f / 120.0f;

    REQUIRE(world
                ->create_body(BodyDesc{.shape = BoxShape{Vec3{10.0f, 0.5f, 10.0f}},
                                       .motion = MotionType::Static,
                                       .position = {0.0f, -0.5f, 0.0f},
                                       .friction = 0.8f})
                .has_value());
    REQUIRE(world
                ->create_body(BodyDesc{.shape = BoxShape{Vec3{10.0f, 2.0f, 0.5f}},
                                       .motion = MotionType::Static,
                                       .position = {0.0f, 2.0f, -6.0f}})
                .has_value());
    for (const float x : {-3.0f, 0.0f, 3.0f}) {
        for (const float y : {0.5f, 1.5f, 2.5f}) {
            crate(*world, Vec3{x, y, -1.0f}, 2.0f, 7);
        }
    }

    const Vec3 eye{0.0f, 3.5f, 12.0f};
    const Vec3 throw_direction = aim(eye, Vec3{0.0f, 1.5f, 0.0f}, 50.0f, 0.2f);
    int hits = 0;
    for (int i = 1; i <= 420; ++i) {
        if (i == 60 || i == 108 || i == 156) {
            // At 0.5, 0.9 and 1.3 seconds.
            REQUIRE(world
                        ->create_body(BodyDesc{.shape = SphereShape{0.3f},
                                               .position = eye + throw_direction * 1.5f,
                                               .linear_velocity =
                                                   throw_direction * 20.0f + Vec3{0.0f, 1.5f, 0.0f},
                                               .mass = 3.0f,
                                               .restitution = 0.3f,
                                               .continuous_collision = true,
                                               .user_data = 5})
                        .has_value());
        }
        world->step(step);
        for (const ContactEvent& event : world->contact_events()) {
            if (event.kind == ContactEvent::Kind::Began &&
                ((event.user_data_a == 5 && event.user_data_b == 7) ||
                 (event.user_data_a == 7 && event.user_data_b == 5))) {
                ++hits;
            }
        }
    }
    CHECK(hits >= 1);
}
