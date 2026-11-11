#pragma once

// impulse: a small 3D rigid-body physics library. Boxes, spheres and capsules; static, kinematic
// and dynamic bodies; sensors; fixed, point, hinge and distance joints; ray casts and overlap
// queries; contact events; and sleeping.
//
// It is a sequential-impulse solver with warm starting and a separate position-correction pass,
// the approach of Box2D 2.4, carried over to 3D. Boxes collide by the separating axis test with
// clipped contact faces, and contacts are speculative, so fast bodies stop at what they would pass
// through within a step.
//
// Units are meters, kilograms and seconds. Body positions are the centers of their shapes. Not
// thread-safe: use a world from one thread at a time.

#include "impulse/math.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace impulse {

struct Box {
    Vec3 half_extents{0.5f, 0.5f, 0.5f};
};

struct Sphere {
    float radius = 0.5f;
};

// Upright along Y. The total height is 2 * (half_height + radius).
struct Capsule {
    float half_height = 0.5f;
    float radius = 0.5f;
};

using Shape = std::variant<Box, Sphere, Capsule>;

enum class Motion : std::uint8_t { Static, Kinematic, Dynamic };

struct BodyDef {
    Shape shape;
    Motion motion = Motion::Dynamic;
    Vec3 position;
    Quat rotation;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    float gravity_scale = 1.0f;
    bool sensor = false;

    // Looks ahead along the body's velocity for contacts, so it can't pass through thin bodies.
    bool continuous = false;

    std::uint64_t user_data = 0;
};

struct BodyState {
    Vec3 position;
    Quat rotation;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    bool awake = false;
};

struct BodyId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const BodyId&, const BodyId&) = default;
};

struct JointId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const JointId&, const JointId&) = default;
};

// Joint anchors and axes are in world space, as they are when the joint is created.
struct FixedJoint {};

struct PointJoint {
    Vec3 anchor;
};

// The angle is body a's rotation relative to body b around `axis`, by the right-hand rule, and
// zero when the joint is created. Limits, if any, must include zero and lie within -pi to pi.
struct HingeJoint {
    Vec3 anchor;
    Vec3 axis{0.0f, 1.0f, 0.0f};
    std::optional<float> lower;
    std::optional<float> upper;
};

struct DistanceJoint {
    Vec3 anchor_a;
    Vec3 anchor_b;
    float min_distance = 0.0f;
    float max_distance = 1.0f;
};

struct JointDef {
    BodyId a;

    // Empty to attach body a to the world.
    std::optional<BodyId> b;
    std::variant<FixedJoint, PointJoint, HingeJoint, DistanceJoint> type;
};

struct ContactEvent {
    enum class Kind : std::uint8_t { Began, Ended };

    Kind kind = Kind::Began;
    BodyId a;
    BodyId b;
    std::uint64_t user_data_a = 0;
    std::uint64_t user_data_b = 0;

    // For Began: a point where the bodies touch, and the direction from a to b.
    Vec3 point;
    Vec3 normal;
};

struct RayHit {
    BodyId body;
    std::uint64_t user_data = 0;
    Vec3 point;
    Vec3 normal;
    float distance = 0.0f;
};

// Tuning, with defaults that suit human-sized things at 60 steps per second.
struct Settings {
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    int velocity_iterations = 10;
    int position_iterations = 4;

    // A body this still for this long falls asleep, with everything it touches.
    float sleep_seconds = 0.5f;
    float sleep_linear_speed = 0.05f;
    float sleep_angular_speed = 0.05f;
};

class World {
public:
    explicit World(const Settings& settings = {});
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void set_gravity(Vec3 gravity);
    [[nodiscard]] Vec3 gravity() const;

    // Empty if the definition is invalid: a shape without positive, finite sizes, a position that
    // isn't finite, a dynamic body without positive mass, negative friction or damping, or
    // restitution outside 0 to 1.
    [[nodiscard]] std::optional<BodyId> create_body(const BodyDef& def);

    // Also destroys the body's joints. Contacts it had end, reported with the next step's events.
    void destroy_body(BodyId body);

    [[nodiscard]] std::optional<BodyState> state(BodyId body) const;
    [[nodiscard]] std::size_t body_count() const;

    // Moves a body at once, waking it.
    void set_transform(BodyId body, Vec3 position, Quat rotation);

    // Moves a kinematic body to arrive at the target at the end of the next step, pushing dynamic
    // bodies aside. It stops there.
    void move_kinematic(BodyId body, Vec3 position, Quat rotation, float step_seconds);

    // For moving bodies; these wake the body.
    void set_linear_velocity(BodyId body, Vec3 velocity);
    void set_angular_velocity(BodyId body, Vec3 velocity);

    // For dynamic bodies: a force acts over the next step, an impulse changes velocity at once.
    void add_force(BodyId body, Vec3 force);
    void add_impulse(BodyId body, Vec3 impulse);

    // Empty if a body doesn't exist, both are the same body, or the joint's settings are invalid.
    // Bodies joined together don't collide with each other.
    [[nodiscard]] std::optional<JointId> create_joint(const JointDef& def);
    void destroy_joint(JointId joint);

    void step(float seconds);

    // Contacts that began or ended in the last step, and those ended by destroying bodies since.
    [[nodiscard]] std::span<const ContactEvent> contact_events() const;

    // The closest body, other than sensors, hit along `direction` within `max_distance`.
    [[nodiscard]] std::optional<RayHit> cast_ray(Vec3 origin, Vec3 direction,
                                                 float max_distance) const;

    // Replaces `bodies` with every body the shape overlaps when placed at the given position.
    void overlap(const Shape& shape, Vec3 position, Quat rotation,
                 std::vector<BodyId>& bodies) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace impulse
