#pragma once

#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/physics3d/physics3d.h"
#include "dream/runtime/debug_lines.h"
#include "dream/runtime/ecs.h"
#include "dream/runtime/world.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dream::runtime {

class App;
class EventBus;

// Gives an entity a body in the Physics3D attached to its world. The body starts at the entity's
// Transform, which must be relative to the world: entities with a Parent are refused. Then:
//
// - Dynamic bodies move the Transform.
// - Kinematic bodies follow the Transform, which the game moves.
// - Static bodies stay where they started. Physics3D::teleport moves one.
//
// The Transform's scale doesn't affect the shape. Changing `desc` once the body exists has no
// effect; replace the component to rebuild the body. desc.position, rotation and user_data are
// filled in by Physics3D.
struct RigidBody3D {
    physics3d::BodyDesc desc;

    // Set by Physics3D once the body exists.
    physics3d::BodyHandle body;

    // Set by Physics3D if the body couldn't be created, after logging why.
    bool failed = false;
};

// Joins this entity's body to the `connected` entity's body, or to the world without one. Both
// entities need a RigidBody3D. Anchors and the axis are in this entity's own space, except
// `connected_anchor`, which is in the connected entity's space, or the world's without one.
struct Joint3D {
    enum class Type : std::uint8_t { Fixed, Point, Hinge, Distance };

    Type type = Type::Point;
    Entity connected = k_no_entity;
    core::Vec3 anchor;

    // Hinge only. Angles are of this entity relative to the connected one; see physics3d.h.
    core::Vec3 axis{0.0f, 1.0f, 0.0f};
    std::optional<float> min_angle_radians;
    std::optional<float> max_angle_radians;

    // Distance only.
    core::Vec3 connected_anchor;
    float min_distance = 0.0f;
    float max_distance = 1.0f;

    // Set by Physics3D.
    physics3d::JointHandle joint;
    bool failed = false;
};

// Published on the App's event bus from Physics3D::step. For a contact that ended because an
// entity was destroyed, that entity no longer exists.
struct Contact3D {
    physics3d::ContactEvent::Kind kind = physics3d::ContactEvent::Kind::Began;
    Entity a = k_no_entity;
    Entity b = k_no_entity;

    // Only set when a contact begins: a point where they touch, and the direction from a to b.
    core::Vec3 point;
    core::Vec3 normal;
};

struct RayHit3D {
    Entity entity = k_no_entity;
    core::Vec3 point;
    core::Vec3 normal;
    float distance = 0.0f;
};

// The interpolation state Physics3D keeps on each moving entity.
struct PhysicsPose3D {
    core::Vec3 previous_position;
    core::Quat previous_rotation;
    core::Vec3 position;
    core::Quat rotation;
};

// Connects a World's entities to a physics3d backend. Typical use:
//
//     on_start:          physics.initialize(app); world = load scene; physics.attach(world.get());
//     on_fixed_update:   physics.step(step_seconds);
//     on_update:         physics.update_transforms(app.fixed_step_alpha()); world->update_transforms();
//     on_render:         physics.debug_draw(renderer.debug_lines());   // Optional.
//
// initialize also lets scene files use the components "rigid_body" and "joint":
//
//     [entities.crate.rigid_body]
//     motion = "dynamic"            # "static", "kinematic" or "dynamic" (the default).
//     shape = "box"                 # "box" (the default), "sphere" or "capsule".
//     half_extents = [0.5, 0.5, 0.5]  # Box.
//     radius = 0.5                  # Sphere and capsule.
//     half_height = 0.5             # Capsule.
//     mass = 1.0                    # These are all optional. See physics3d::BodyDesc.
//     friction = 0.5
//     restitution = 0.0
//     linear_damping = 0.05
//     angular_damping = 0.05
//     gravity_scale = 1.0
//     velocity = [0.0, 0.0, 0.0]
//     angular_velocity = [0.0, 0.0, 0.0]
//     sensor = false
//     continuous = false
//
//     [entities.door.joint]
//     type = "hinge"                # "fixed", "point" (the default), "hinge" or "distance".
//     connected = "frame"           # Optional. Without it, the joint holds onto the world.
//     anchor = [0.0, 0.0, 0.0]
//     axis = [0.0, 1.0, 0.0]        # Hinge.
//     min_angle = -90.0             # Hinge, optional, in degrees.
//     max_angle = 90.0
//     connected_anchor = [0.0, 0.0, 0.0]  # Distance.
//     min_distance = 0.0            # Distance.
//     max_distance = 1.0
//
// The backend comes from the project configuration's physics3d list, and the starting gravity from
//
//     [physics3d]
//     gravity = [0.0, -9.81, 0.0]
class Physics3D {
public:
    Physics3D() = default;
    ~Physics3D();

    Physics3D(const Physics3D&) = delete;
    Physics3D& operator=(const Physics3D&) = delete;

    // Creates the configured backend. Call once, before loading scenes that use these components.
    [[nodiscard]] core::Result<void> initialize(App& app);

    // The same with a given backend, for tests or a game that makes its own.
    [[nodiscard]] core::Result<void> initialize(App& app,
                                                std::unique_ptr<physics3d::PhysicsWorld> backend);

    // Simulates `world` from now on, and removes the bodies of the world simulated before. Call
    // with nullptr, or another world, before destroying the attached world.
    void attach(World* world);

    [[nodiscard]] World* world() const noexcept {
        return m_world;
    }

    // Creates and removes bodies and joints to match the components, then advances the
    // simulation and publishes contacts. Call from Game::on_fixed_update.
    void step(float step_seconds);

    // Moves the Transform of every dynamic body to its pose `alpha` of the way from the previous
    // step to the latest, so motion looks smooth at any frame rate. Call before drawing, with
    // App::fixed_step_alpha, and then World::update_transforms.
    void update_transforms(float alpha);

    // Outlines every body, colored by kind, and marks where contacts began in the last step.
    void debug_draw(DebugLines& lines) const;

    [[nodiscard]] std::optional<physics3d::BodyState> body_state(Entity entity) const;
    void add_force(Entity entity, core::Vec3 force);
    void add_impulse(Entity entity, core::Vec3 impulse);
    void set_linear_velocity(Entity entity, core::Vec3 velocity);
    void set_angular_velocity(Entity entity, core::Vec3 velocity);

    // Moves a body and its Transform at once, without interpolating from where it was.
    void teleport(Entity entity, core::Vec3 position, core::Quat rotation);

    [[nodiscard]] std::optional<RayHit3D> cast_ray(core::Vec3 origin, core::Vec3 direction,
                                                   float max_distance) const;

    // Replaces `entities` with those whose bodies overlap `shape` at the given place.
    void overlap(const physics3d::Shape& shape, core::Vec3 position, core::Quat rotation,
                 std::vector<Entity>& entities) const;

    void set_gravity(core::Vec3 gravity);

    // Null until initialized.
    [[nodiscard]] physics3d::PhysicsWorld* backend() noexcept {
        return m_backend.get();
    }

private:
    void remove_stale();
    void create_bodies();
    void create_joints();
    [[nodiscard]] physics3d::BodyHandle body_of(Entity entity) const;
    [[nodiscard]] Entity entity_of(physics3d::BodyHandle body) const;
    void clear();

    std::unique_ptr<physics3d::PhysicsWorld> m_backend;
    EventBus* m_events = nullptr;
    World* m_world = nullptr;

    std::unordered_map<Entity, physics3d::BodyHandle> m_bodies;
    std::unordered_map<std::uint64_t, Entity> m_entities;
    struct TrackedJoint {
        physics3d::JointHandle joint;
        physics3d::BodyHandle body_a;
        physics3d::BodyHandle body_b;
    };

    std::unordered_map<Entity, TrackedJoint> m_joints;
    std::vector<core::Vec3> m_contact_points;
};

} // namespace dream::runtime
