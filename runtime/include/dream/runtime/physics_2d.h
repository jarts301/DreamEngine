#pragma once

#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/physics2d/physics2d.h"
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

// The 2D counterpart of RigidBody3D, simulated in the XY plane. The body takes its position from
// the Transform's X and Y and its angle from the Transform's rotation around Z. A dynamic body
// writes back X, Y and a rotation purely around Z, and leaves Z alone.
struct RigidBody2D {
    physics2d::BodyDesc desc;

    // Set by Physics2D.
    physics2d::BodyHandle body;
    bool failed = false;
};

// Joins this entity's body to the `connected` entity's body, or to the world without one. `anchor`
// is in this entity's own space, and `connected_anchor` in the connected entity's, or the world's.
struct Joint2D {
    enum class Type : std::uint8_t { Fixed, Hinge, Distance };

    Type type = Type::Hinge;
    Entity connected = k_no_entity;
    core::Vec2 anchor;

    // Hinge only. Angles are of this entity relative to the connected one; see physics2d.h.
    std::optional<float> min_angle_radians;
    std::optional<float> max_angle_radians;

    // Distance only.
    core::Vec2 connected_anchor;
    float min_distance = 0.0f;
    float max_distance = 1.0f;

    // Set by Physics2D.
    physics2d::JointHandle joint;
    bool failed = false;
};

// Published on the App's event bus from Physics2D::step.
struct Contact2D {
    physics2d::ContactEvent::Kind kind = physics2d::ContactEvent::Kind::Began;
    Entity a = k_no_entity;
    Entity b = k_no_entity;

    // Only set when a contact between solid bodies begins.
    core::Vec2 point;
    core::Vec2 normal;
};

struct RayHit2D {
    Entity entity = k_no_entity;
    core::Vec2 point;
    core::Vec2 normal;
    float distance = 0.0f;
};

// The interpolation state Physics2D keeps on each dynamic entity.
struct PhysicsPose2D {
    core::Vec2 previous_position;
    float previous_angle = 0.0f;
    core::Vec2 position;
    float angle = 0.0f;
};

// The angle of a rotation around Z, in radians. Rotation around other axes is ignored.
[[nodiscard]] float angle_around_z(core::Quat rotation) noexcept;

// Connects a World's entities to a physics2d backend, the same way Physics3D does. Scene files get
// the components "rigid_body_2d" and "joint_2d":
//
//     [entities.crate.rigid_body_2d]
//     motion = "dynamic"            # "static", "kinematic" or "dynamic" (the default).
//     shape = "box"                 # "box" (the default), "circle" or "capsule".
//     half_extents = [0.5, 0.5]     # Box.
//     radius = 0.5                  # Circle and capsule.
//     half_height = 0.5             # Capsule.
//     mass = 1.0                    # These are all optional. See physics2d::BodyDesc.
//     friction = 0.5
//     restitution = 0.0
//     linear_damping = 0.0
//     angular_damping = 0.05
//     gravity_scale = 1.0
//     velocity = [0.0, 0.0]
//     angular_velocity = 0.0        # Radians per second.
//     fixed_rotation = false
//     sensor = false
//     continuous = false
//
//     [entities.wheel.joint_2d]
//     type = "hinge"                # "fixed", "hinge" (the default) or "distance".
//     connected = "car"             # Optional. Without it, the joint holds onto the world.
//     anchor = [0.0, 0.0]
//     min_angle = -45.0             # Hinge, optional, in degrees.
//     max_angle = 45.0
//     connected_anchor = [0.0, 0.0] # Distance.
//     min_distance = 0.0
//     max_distance = 1.0
//
// Starting gravity comes from
//
//     [physics2d]
//     gravity = [0.0, -9.81]
class Physics2D {
public:
    Physics2D() = default;
    ~Physics2D();

    Physics2D(const Physics2D&) = delete;
    Physics2D& operator=(const Physics2D&) = delete;

    [[nodiscard]] core::Result<void> initialize(App& app);
    [[nodiscard]] core::Result<void> initialize(App& app,
                                                std::unique_ptr<physics2d::PhysicsWorld> backend);

    // Call with nullptr, or another world, before destroying the attached world.
    void attach(World* world);

    [[nodiscard]] World* world() const noexcept {
        return m_world;
    }

    void step(float step_seconds);
    void update_transforms(float alpha);
    void debug_draw(DebugLines& lines) const;

    [[nodiscard]] std::optional<physics2d::BodyState> body_state(Entity entity) const;
    void add_force(Entity entity, core::Vec2 force);
    void add_impulse(Entity entity, core::Vec2 impulse);
    void set_linear_velocity(Entity entity, core::Vec2 velocity);
    void set_angular_velocity(Entity entity, float velocity);
    void teleport(Entity entity, core::Vec2 position, float angle);

    [[nodiscard]] std::optional<RayHit2D> cast_ray(core::Vec2 origin, core::Vec2 direction,
                                                   float max_distance) const;
    void overlap(const physics2d::Shape& shape, core::Vec2 position, float angle,
                 std::vector<Entity>& entities) const;

    void set_gravity(core::Vec2 gravity);

    [[nodiscard]] physics2d::PhysicsWorld* backend() noexcept {
        return m_backend.get();
    }

private:
    struct TrackedJoint {
        physics2d::JointHandle joint;
        physics2d::BodyHandle body_a;
        physics2d::BodyHandle body_b;
    };

    void remove_stale();
    void create_bodies();
    void create_joints();
    [[nodiscard]] physics2d::BodyHandle body_of(Entity entity) const;
    [[nodiscard]] Entity entity_of(physics2d::BodyHandle body) const;
    void clear();

    std::unique_ptr<physics2d::PhysicsWorld> m_backend;
    EventBus* m_events = nullptr;
    World* m_world = nullptr;

    std::unordered_map<Entity, physics2d::BodyHandle> m_bodies;
    std::unordered_map<std::uint64_t, Entity> m_entities;
    std::unordered_map<Entity, TrackedJoint> m_joints;
    std::vector<core::Vec2> m_contact_points;
};

} // namespace dream::runtime
