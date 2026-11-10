#include "dream/runtime/physics_2d.h"
#include "dream/runtime/physics_3d.h"

#include "dream/runtime/app.h"
#include "fake_physics.h"
#include "fake_platform.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using core::Quat;
using core::Vec2;
using core::Vec3;
using doctest::Approx;

namespace {

using Calls = std::vector<std::string>;

const core::Config k_config = core::Config::parse(R"(
[physics3d]
gravity = [0.0, -5.0, 0.0]

[physics2d]
gravity = [1.0, -2.0]
)")
                                  .value();

// An App to hold the scene loader and event bus, and a Physics3D using the fake backend.
struct Physics3DFixture {
    FakeState platform;
    FakePhysics3DState backend;
    App app{std::make_unique<FakePlatform>(platform), k_config};
    Physics3D physics;
    World world;

    Physics3DFixture() {
        REQUIRE(physics.initialize(app, std::make_unique<FakePhysics3D>(backend)).has_value());
        physics.attach(&world);
    }

    Entity add_body(Vec3 position, physics3d::MotionType motion = physics3d::MotionType::Dynamic) {
        const Entity entity = world.create_entity();
        world.registry().get<Transform>(entity).position = position;
        RigidBody3D body;
        body.desc.motion = motion;
        world.registry().emplace<RigidBody3D>(entity, body);
        return entity;
    }

    bool called(std::string_view call) const {
        return std::find(backend.calls.begin(), backend.calls.end(), call) != backend.calls.end();
    }
};

struct Physics2DFixture {
    FakeState platform;
    FakePhysics2DState backend;
    App app{std::make_unique<FakePlatform>(platform), k_config};
    Physics2D physics;
    World world;

    Physics2DFixture() {
        REQUIRE(physics.initialize(app, std::make_unique<FakePhysics2D>(backend)).has_value());
        physics.attach(&world);
    }
};

} // namespace

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D reads gravity from the configuration") {
    CHECK(backend.gravity.y == Approx(-5.0f));
    physics.set_gravity(Vec3{0.0f, -1.0f, 0.0f});
    CHECK(backend.gravity.y == Approx(-1.0f));
}

TEST_CASE("Physics3D rejects badly written gravity") {
    FakeState platform;
    FakePhysics3DState backend;
    const core::Config config = core::Config::parse("[physics3d]\ngravity = [1.0, 2.0]").value();
    App app(std::make_unique<FakePlatform>(platform), config);
    Physics3D physics;
    CHECK_FALSE(physics.initialize(app, std::make_unique<FakePhysics3D>(backend)).has_value());
    CHECK_FALSE(physics.initialize(app, nullptr).has_value());
}

TEST_CASE("Physics3D needs a module project to make its own backend") {
    FakeState platform;
    App app(std::make_unique<FakePlatform>(platform), k_config);
    Physics3D physics;
    CHECK_FALSE(physics.initialize(app).has_value());
    CHECK(physics.backend() == nullptr);
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D creates bodies where their entities are") {
    const Entity entity = add_body(Vec3{1.0f, 2.0f, 3.0f});
    physics.step(0.1f);

    const RigidBody3D& body = world.registry().get<RigidBody3D>(entity);
    REQUIRE(body.body.is_valid());
    const auto& created = backend.bodies.at(body.body.index);
    CHECK(created.desc.position.y == Approx(2.0f));
    CHECK(created.desc.user_data == entt::to_integral(entity));

    // Each body is created once.
    physics.step(0.1f);
    CHECK(std::count(backend.calls.begin(), backend.calls.end(), "create 1") == 1);
    CHECK(backend.bodies.size() == 1);
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D refuses bodies on entities with a parent") {
    const Entity parent = world.create_entity();
    const Entity child = add_body(Vec3{});
    REQUIRE(world.set_parent(child, parent).has_value());
    physics.step(0.1f);

    CHECK(world.registry().get<RigidBody3D>(child).failed);
    CHECK(backend.bodies.empty());
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D marks bodies the backend refuses") {
    const Entity entity = add_body(Vec3{});
    world.registry().get<RigidBody3D>(entity).desc.mass = -1.0f;
    physics.step(0.1f);
    CHECK(world.registry().get<RigidBody3D>(entity).failed);
    physics.step(0.1f);
    CHECK(backend.bodies.empty());
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D moves dynamic transforms smoothly between steps") {
    const Entity entity = add_body(Vec3{});
    physics.step(0.1f);
    physics.update_transforms(1.0f);
    CHECK(world.registry().get<Transform>(entity).position.x == Approx(1.0f));

    physics.step(0.1f);
    physics.update_transforms(0.25f);
    CHECK(world.registry().get<Transform>(entity).position.x == Approx(1.25f));
    physics.update_transforms(1.0f);
    CHECK(world.registry().get<Transform>(entity).position.x == Approx(2.0f));

    // Teleporting skips the smoothing.
    physics.teleport(entity, Vec3{10.0f, 0.0f, 0.0f}, Quat{});
    physics.update_transforms(0.5f);
    CHECK(world.registry().get<Transform>(entity).position.x == Approx(10.0f));
    CHECK(physics.body_state(entity)->position.x == Approx(10.0f));
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D drives kinematic bodies from their transforms") {
    const Entity entity = add_body(Vec3{}, physics3d::MotionType::Kinematic);
    const Entity still = add_body(Vec3{5.0f, 0.0f, 0.0f}, physics3d::MotionType::Static);
    physics.step(0.1f);
    world.registry().get<Transform>(entity).position.x = 3.0f;
    physics.step(0.1f);

    const std::uint32_t index = world.registry().get<RigidBody3D>(entity).body.index;
    CHECK(called(std::format("kinematic {} 3", index)));
    physics.update_transforms(1.0f);
    CHECK(world.registry().get<Transform>(entity).position.x == Approx(3.0f));
    CHECK(world.registry().get<Transform>(still).position.x == Approx(5.0f));
    CHECK_FALSE(world.registry().all_of<PhysicsPose3D>(entity));
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D removes bodies with their entities or components") {
    const Entity removed = add_body(Vec3{});
    const Entity destroyed = add_body(Vec3{});
    const Entity rebuilt = add_body(Vec3{});
    physics.step(0.1f);
    REQUIRE(backend.bodies.size() == 3);

    world.registry().remove<RigidBody3D>(removed);
    world.destroy_entity(destroyed);
    world.registry().replace<RigidBody3D>(rebuilt, RigidBody3D{});
    physics.step(0.1f);

    CHECK(called("destroy 1"));
    CHECK(called("destroy 2"));
    CHECK(called("destroy 3"));
    CHECK(backend.bodies.size() == 1);
    CHECK(world.registry().get<RigidBody3D>(rebuilt).body.index == 4);
    CHECK_FALSE(world.registry().all_of<PhysicsPose3D>(removed));
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D attached to another world drops the old bodies") {
    add_body(Vec3{});
    physics.step(0.1f);

    World other;
    physics.attach(&other);
    CHECK(backend.bodies.empty());
    CHECK(physics.world() == &other);

    // Coming back rebuilds the bodies.
    physics.attach(&world);
    physics.step(0.1f);
    CHECK(backend.bodies.size() == 1);

    physics.attach(nullptr);
    physics.step(0.1f);
    CHECK(backend.bodies.empty());
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D passes body controls to the backend") {
    const Entity entity = add_body(Vec3{});
    const Entity bodiless = world.create_entity();
    physics.step(0.1f);

    physics.add_force(entity, Vec3{0.0f, 2.0f, 0.0f});
    physics.add_impulse(entity, Vec3{0.0f, 3.0f, 0.0f});
    physics.set_linear_velocity(entity, Vec3{4.0f, 0.0f, 0.0f});
    physics.set_angular_velocity(entity, Vec3{});
    physics.add_force(bodiless, Vec3{});
    CHECK(called("force 1 2"));
    CHECK(called("impulse 1 3"));
    CHECK(called("velocity 1 4"));
    CHECK(called("spin 1"));
    CHECK_FALSE(physics.body_state(bodiless).has_value());
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D publishes contacts between entities") {
    const Entity a = add_body(Vec3{});
    const Entity b = add_body(Vec3{});
    physics.step(0.1f);

    std::vector<Contact3D> heard;
    const Subscription subscription =
        app.events().subscribe<Contact3D>([&](const Contact3D& contact) { heard.push_back(contact); });

    backend.next_contacts.push_back(physics3d::ContactEvent{
        .kind = physics3d::ContactEvent::Kind::Began,
        .body_a = world.registry().get<RigidBody3D>(a).body,
        .body_b = world.registry().get<RigidBody3D>(b).body,
        .user_data_a = entt::to_integral(a),
        .user_data_b = entt::to_integral(b),
        .point = Vec3{0.0f, 1.0f, 0.0f},
        .normal = Vec3{0.0f, 1.0f, 0.0f},
    });
    physics.step(0.1f);

    REQUIRE(heard.size() == 1);
    CHECK(heard[0].a == a);
    CHECK(heard[0].b == b);
    CHECK(heard[0].point.y == Approx(1.0f));

    // Contact points are marked in the debug drawing.
    DebugLines lines;
    physics.debug_draw(lines);
    CHECK(lines.vertices().size() == 2 * 12 * 2 + 2 * 3);
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D finds entities with queries") {
    const Entity entity = add_body(Vec3{});
    physics.step(0.1f);
    const physics3d::BodyHandle body = world.registry().get<RigidBody3D>(entity).body;

    backend.ray_hit = physics3d::RayHit{.body = body, .distance = 2.0f};
    const std::optional<RayHit3D> hit =
        physics.cast_ray(Vec3{}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == entity);
    CHECK(hit->distance == Approx(2.0f));

    backend.overlapping = {body, physics3d::BodyHandle{.index = 99, .generation = 1}};
    std::vector<Entity> found;
    physics.overlap(physics3d::SphereShape{}, Vec3{}, Quat{}, found);
    CHECK(found == std::vector<Entity>{entity});
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D builds joints in world space") {
    const Entity post = add_body(Vec3{0.0f, 5.0f, 0.0f}, physics3d::MotionType::Static);
    const Entity door = add_body(Vec3{2.0f, 5.0f, 0.0f});
    world.registry().get<Transform>(door).rotation =
        core::quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, core::k_pi / 2.0f);
    world.registry().emplace<Joint3D>(door, Joint3D{
                                                .type = Joint3D::Type::Hinge,
                                                .connected = post,
                                                .anchor = Vec3{1.0f, 0.0f, 0.0f},
                                                .axis = Vec3{1.0f, 0.0f, 0.0f},
                                            });
    physics.step(0.1f);

    REQUIRE(backend.joints.size() == 1);
    const physics3d::JointDesc& desc = backend.joints.begin()->second;
    CHECK(desc.body_a == world.registry().get<RigidBody3D>(door).body);
    CHECK(desc.body_b == world.registry().get<RigidBody3D>(post).body);
    const auto& hinge = std::get<physics3d::HingeJoint>(desc.type);
    // The door is turned a quarter turn around Z, so its local +X is the world's +Y.
    CHECK(hinge.anchor.x == Approx(2.0f));
    CHECK(hinge.anchor.y == Approx(6.0f));
    CHECK(hinge.axis.y == Approx(1.0f));

    // Rebuilding the post's body rebuilds the joint.
    world.registry().replace<RigidBody3D>(post, RigidBody3D{.desc = {.motion = physics3d::MotionType::Static}});
    physics.step(0.1f);
    CHECK(called("unjoint 3"));
    CHECK(backend.joints.size() == 1);

    // Removing it removes the joint.
    world.registry().remove<Joint3D>(door);
    physics.step(0.1f);
    CHECK(backend.joints.empty());
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D refuses joints to entities without bodies") {
    const Entity lonely = world.create_entity();
    const Entity body = add_body(Vec3{});
    world.registry().emplace<Joint3D>(body, Joint3D{.connected = lonely});
    physics.step(0.1f);
    CHECK(world.registry().get<Joint3D>(body).failed);
    CHECK(backend.joints.empty());
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D adds rigid bodies and joints to scene files") {
    const std::string_view scene = R"(
[entities.ground.rigid_body]
motion = "static"
half_extents = [10.0, 0.5, 10.0]

[entities.ball.transform]
position = [0.0, 4.0, 0.0]

[entities.ball.rigid_body]
shape = "sphere"
radius = 0.25
mass = 2.0
restitution = 0.5
velocity = [1.0, 0.0, 0.0]
sensor = true

[entities.ball.joint]
type = "distance"
connected = "ground"
anchor = [0.0, 0.25, 0.0]
connected_anchor = [0.0, 0.5, 0.0]
max_distance = 6.0

[entities.pill.rigid_body]
motion = "kinematic"
shape = "capsule"
radius = 0.3
half_height = 0.7

[entities.flap.rigid_body]
[entities.flap.joint]
type = "hinge"
min_angle = -90.0
max_angle = 45.0
)";
    core::Result<std::unique_ptr<World>> loaded = app.scenes().parse(scene);
    REQUIRE(loaded.has_value());
    World& scene_world = *loaded.value();
    const Registry& registry = scene_world.registry();

    const RigidBody3D& ball = registry.get<RigidBody3D>(scene_world.find("ball"));
    CHECK(std::get<physics3d::SphereShape>(ball.desc.shape).radius == Approx(0.25f));
    CHECK(ball.desc.mass == Approx(2.0f));
    CHECK(ball.desc.restitution == Approx(0.5f));
    CHECK(ball.desc.linear_velocity.x == Approx(1.0f));
    CHECK(ball.desc.is_sensor);

    const RigidBody3D& ground = registry.get<RigidBody3D>(scene_world.find("ground"));
    CHECK(ground.desc.motion == physics3d::MotionType::Static);
    CHECK(std::get<physics3d::BoxShape>(ground.desc.shape).half_extents.x == Approx(10.0f));

    const RigidBody3D& pill = registry.get<RigidBody3D>(scene_world.find("pill"));
    CHECK(pill.desc.motion == physics3d::MotionType::Kinematic);
    CHECK(std::get<physics3d::CapsuleShape>(pill.desc.shape).half_height == Approx(0.7f));

    const Joint3D& rope = registry.get<Joint3D>(scene_world.find("ball"));
    CHECK(rope.type == Joint3D::Type::Distance);
    CHECK(rope.connected == scene_world.find("ground"));
    CHECK(rope.max_distance == Approx(6.0f));

    const Joint3D& flap = registry.get<Joint3D>(scene_world.find("flap"));
    CHECK(flap.connected == k_no_entity);
    CHECK(*flap.min_angle_radians == Approx(-core::k_pi / 2.0f));

    physics.attach(&scene_world);
    physics.step(0.1f);
    CHECK(backend.bodies.size() == 4);
    CHECK(backend.joints.size() == 2);
    physics.attach(nullptr);
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D reports mistakes in scene files") {
    for (const char* scene : {
             "[entities.a.rigid_body]\nshape = \"cube\"",
             "[entities.a.rigid_body]\nmotion = \"floating\"",
             "[entities.a.rigid_body]\nradius = 1.0\nradios = 2.0",
             "[entities.a.rigid_body]\nshape = \"sphere\"\nradius = -1.0",
             "[entities.a.rigid_body]\nsensor = 1",
             "[entities.a.joint]\nconnected = \"nobody\"",
             "[entities.a.joint]\ntype = \"spring\"",
         }) {
        CAPTURE(scene);
        CHECK_FALSE(app.scenes().parse(scene).has_value());
    }
}

TEST_CASE_FIXTURE(Physics3DFixture, "Physics3D outlines bodies for debugging") {
    add_body(Vec3{});
    const Entity ball = add_body(Vec3{});
    world.registry().get<RigidBody3D>(ball).desc.shape = physics3d::SphereShape{};
    physics.step(0.1f);

    DebugLines lines;
    physics.debug_draw(lines);
    CHECK(lines.vertices().size() == 2 * 12 + 2 * 3 * 24);
}

TEST_CASE("angle_around_z reads rotations around Z") {
    for (const float angle : {0.0f, 0.5f, -2.0f, 3.0f}) {
        CHECK(angle_around_z(core::quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, angle)) ==
              Approx(angle).epsilon(1e-4));
    }
}

TEST_CASE_FIXTURE(Physics2DFixture, "Physics2D works in the XY plane of each transform") {
    CHECK(backend.gravity.x == Approx(1.0f));

    const Entity entity = world.create_entity();
    Transform& transform = world.registry().get<Transform>(entity);
    transform.position = Vec3{1.0f, 2.0f, -7.0f};
    transform.rotation = core::quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, 0.5f);
    world.registry().emplace<RigidBody2D>(entity);
    physics.step(0.1f);

    const RigidBody2D& body = world.registry().get<RigidBody2D>(entity);
    REQUIRE(body.body.is_valid());
    const auto& created = backend.bodies.at(body.body.index);
    CHECK(created.desc.position.x == Approx(1.0f));
    CHECK(created.desc.position.y == Approx(2.0f));
    CHECK(created.desc.angle == Approx(0.5f));

    // Halfway through a step that moved it 1 m and turned it a quarter turn. Z is left alone.
    physics.step(0.1f);
    physics.update_transforms(0.5f);
    const Transform& moved = world.registry().get<Transform>(entity);
    CHECK(moved.position.x == Approx(2.5f));
    CHECK(moved.position.z == Approx(-7.0f));
    CHECK(angle_around_z(moved.rotation) ==
          Approx(0.5f + core::k_pi / 2.0f + core::k_pi / 4.0f).epsilon(1e-4));

    DebugLines lines;
    physics.debug_draw(lines);
    CHECK(lines.vertices().size() == 2 * 4);
    for (const DebugVertex& vertex : lines.vertices()) {
        CHECK(vertex.position.z == Approx(-7.0f));
    }

    world.destroy_entity(entity);
    physics.step(0.1f);
    CHECK(backend.bodies.empty());
}

TEST_CASE_FIXTURE(Physics2DFixture, "Physics2D adds its components to scene files") {
    const std::string_view scene = R"(
[entities.floor.rigid_body_2d]
motion = "static"
half_extents = [8.0, 0.5]

[entities.wheel.transform]
position = [2.0, 3.0, 0.0]

[entities.wheel.rigid_body_2d]
shape = "circle"
radius = 0.4
angular_velocity = 2.0
fixed_rotation = false

[entities.wheel.joint_2d]
connected = "floor"
anchor = [0.0, 0.0]

[entities.player.rigid_body_2d]
shape = "capsule"
fixed_rotation = true
)";
    core::Result<std::unique_ptr<World>> loaded = app.scenes().parse(scene);
    REQUIRE(loaded.has_value());
    World& scene_world = *loaded.value();
    const Registry& registry = scene_world.registry();

    const RigidBody2D& wheel = registry.get<RigidBody2D>(scene_world.find("wheel"));
    CHECK(std::get<physics2d::CircleShape>(wheel.desc.shape).radius == Approx(0.4f));
    CHECK(wheel.desc.angular_velocity == Approx(2.0f));
    CHECK(registry.get<RigidBody2D>(scene_world.find("player")).desc.fixed_rotation);

    physics.attach(&scene_world);
    physics.step(0.1f);
    REQUIRE(backend.joints.size() == 1);
    const auto& hinge = std::get<physics2d::HingeJoint>(backend.joints.begin()->second.type);
    CHECK(hinge.anchor.x == Approx(2.0f));
    CHECK(hinge.anchor.y == Approx(3.0f));
    physics.attach(nullptr);

    CHECK_FALSE(app.scenes().parse("[entities.a.rigid_body_2d]\nhalf_extents = [1.0, 1.0, 1.0]")
                    .has_value());
    CHECK_FALSE(app.scenes().parse("[entities.a.joint_2d]\ntype = \"point\"").has_value());
}
