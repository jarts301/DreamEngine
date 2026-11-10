#pragma once

#include "dream/core/handle.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dream::physics3d {

// 3D rigid-body physics. Units are meters, kilograms and seconds, and +Y is up.
//
// Like the graphics interface, it holds only what can be implemented on every backend. Positions
// are body centers, which are also the centers of their shapes.

struct BodyTag;
struct JointTag;

using BodyHandle = core::Handle<BodyTag>;
using JointHandle = core::Handle<JointTag>;

// For backends, which keep their own types in a core::HandlePool.
template <typename To, typename From>
[[nodiscard]] constexpr core::Handle<To> handle_cast(core::Handle<From> handle) noexcept {
    return core::Handle<To>{.index = handle.index, .generation = handle.generation};
}

struct BoxShape {
    core::Vec3 half_extents{0.5f, 0.5f, 0.5f};
};

struct SphereShape {
    float radius = 0.5f;
};

// Upright along Y. The total height is 2 * (half_height + radius).
struct CapsuleShape {
    float half_height = 0.5f;
    float radius = 0.5f;
};

using Shape = std::variant<BoxShape, SphereShape, CapsuleShape>;

// Static bodies never move. Kinematic bodies move only as told, and push dynamic bodies without
// being pushed back. Dynamic bodies are simulated.
enum class MotionType : std::uint8_t { Static, Kinematic, Dynamic };

struct BodyDesc {
    Shape shape;
    MotionType motion = MotionType::Dynamic;
    core::Vec3 position;
    core::Quat rotation;
    core::Vec3 linear_velocity;
    core::Vec3 angular_velocity;

    // For dynamic bodies. Rotational inertia follows from the shape.
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
    float gravity_scale = 1.0f;

    // A sensor reports contacts but doesn't collide.
    bool is_sensor = false;

    // Stops fast, small bodies from passing through thin ones, at some cost.
    bool continuous_collision = false;

    // Returned with contacts and queries. The runtime stores the entity here.
    std::uint64_t user_data = 0;
};

struct BodyState {
    core::Vec3 position;
    core::Quat rotation;
    core::Vec3 linear_velocity;
    core::Vec3 angular_velocity;

    // A body at rest is put to sleep, and wakes when something touches or moves it.
    bool is_awake = true;
};

// Keeps both bodies' relative position and rotation as they are when the joint is created.
struct FixedJoint {};

// Keeps a point of both bodies together, letting them rotate freely around it.
struct PointJoint {
    core::Vec3 anchor;
};

// Lets the bodies rotate around one axis through `anchor`, optionally between two angles. The angle
// is body_a's rotation relative to body_b (or the world) around `axis`, by the right-hand rule,
// and is zero when the joint is created.
struct HingeJoint {
    core::Vec3 anchor;
    core::Vec3 axis{0.0f, 1.0f, 0.0f};
    std::optional<float> min_angle_radians;
    std::optional<float> max_angle_radians;
};

// Keeps the distance between two points, one on each body, within a range.
struct DistanceJoint {
    core::Vec3 anchor_a;
    core::Vec3 anchor_b;
    float min_distance = 0.0f;
    float max_distance = 1.0f;
};

using JointType = std::variant<FixedJoint, PointJoint, HingeJoint, DistanceJoint>;

// Anchors and axes are in world space, at the moment the joint is created.
struct JointDesc {
    BodyHandle body_a;

    // An invalid handle attaches body_a to the world.
    BodyHandle body_b;
    JointType type;
};

struct RayHit {
    BodyHandle body;
    std::uint64_t user_data = 0;
    core::Vec3 point;
    core::Vec3 normal;
    float distance = 0.0f;
};

struct ContactEvent {
    enum class Kind : std::uint8_t { Began, Ended };

    Kind kind = Kind::Began;
    BodyHandle body_a;
    BodyHandle body_b;
    std::uint64_t user_data_a = 0;
    std::uint64_t user_data_b = 0;

    // Only set for Began: a point where the bodies touch, and the direction from a to b.
    core::Vec3 point;
    core::Vec3 normal;
};

// The rules every backend applies in create_body, kept here so that backends agree.
namespace detail {

[[nodiscard]] inline bool is_positive(float value) noexcept {
    return value > 0.0f && std::isfinite(value);
}

[[nodiscard]] inline bool is_valid_shape(const Shape& shape) noexcept {
    if (const auto* box = std::get_if<BoxShape>(&shape)) {
        const core::Vec3 size = box->half_extents;
        return is_positive(size.x) && is_positive(size.y) && is_positive(size.z);
    }
    if (const auto* sphere = std::get_if<SphereShape>(&shape)) {
        return is_positive(sphere->radius);
    }
    const auto& capsule = std::get<CapsuleShape>(shape);
    return is_positive(capsule.radius) && capsule.half_height >= 0.0f &&
           std::isfinite(capsule.half_height);
}

} // namespace detail

[[nodiscard]] inline core::Result<void> validate(const BodyDesc& desc) {
    const auto invalid = [](const char* message) {
        return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
    };
    if (!detail::is_valid_shape(desc.shape)) {
        return invalid("A body's shape needs positive, finite sizes");
    }
    if (!(std::isfinite(desc.position.x) && std::isfinite(desc.position.y) &&
                            std::isfinite(desc.position.z))) {
        return invalid("A body's position must be finite");
    }
    if (desc.motion == MotionType::Dynamic && !detail::is_positive(desc.mass)) {
        return invalid("A dynamic body needs a positive mass");
    }
    if (!(desc.friction >= 0.0f) || !(desc.restitution >= 0.0f && desc.restitution <= 1.0f) ||
        !(desc.linear_damping >= 0.0f) || !(desc.angular_damping >= 0.0f)) {
        return invalid("Friction and damping can't be negative, and restitution must be 0 to 1");
    }
    return {};
}

// One simulated space. Not thread-safe: call everything from one thread. Handles that no longer
// exist are ignored by every function that takes one.
class PhysicsWorld {
public:
    static constexpr std::string_view k_module_interface = "physics3d";

    virtual ~PhysicsWorld() = default;

    virtual void set_gravity(core::Vec3 gravity) = 0;
    [[nodiscard]] virtual core::Vec3 gravity() const = 0;

    // Fails if the description is invalid, such as a shape with a zero size.
    [[nodiscard]] virtual core::Result<BodyHandle> create_body(const BodyDesc& desc) = 0;

    // Also destroys the joints attached to the body.
    virtual void destroy_body(BodyHandle body) = 0;

    [[nodiscard]] virtual std::optional<BodyState> body_state(BodyHandle body) const = 0;

    // Moves a body instantly, without affecting what it passes through. Mainly for spawning.
    virtual void set_transform(BodyHandle body, core::Vec3 position, core::Quat rotation) = 0;

    // Moves a kinematic body so it arrives at the target by the end of the next step, pushing dynamic
    // bodies out of its way.
    virtual void move_kinematic(BodyHandle body, core::Vec3 position, core::Quat rotation,
                                float step_seconds) = 0;

    virtual void set_linear_velocity(BodyHandle body, core::Vec3 velocity) = 0;
    virtual void set_angular_velocity(BodyHandle body, core::Vec3 velocity) = 0;

    // A force acts over the next step; an impulse changes velocity at once. Both wake the body.
    virtual void add_force(BodyHandle body, core::Vec3 force) = 0;
    virtual void add_impulse(BodyHandle body, core::Vec3 impulse) = 0;

    // Fails if a body doesn't exist, or the joint is invalid.
    [[nodiscard]] virtual core::Result<JointHandle> create_joint(const JointDesc& desc) = 0;
    virtual void destroy_joint(JointHandle joint) = 0;

    // Advances the simulation. Call with a fixed step for results that don't depend on frame rate.
    virtual void step(float step_seconds) = 0;

    // The contacts that began or ended during the last step, in no particular order.
    [[nodiscard]] virtual std::span<const ContactEvent> contact_events() const = 0;

    // The closest hit along `direction` (which needn't be normalized) within `max_distance`.
    // Sensors are not hit.
    [[nodiscard]] virtual std::optional<RayHit> cast_ray(core::Vec3 origin, core::Vec3 direction,
                                                         float max_distance) const = 0;

    // Replaces `bodies` with every body whose shape overlaps `shape` placed at the given position.
    virtual void overlap(const Shape& shape, core::Vec3 position, core::Quat rotation,
                         std::vector<BodyHandle>& bodies) const = 0;

    [[nodiscard]] virtual std::size_t body_count() const = 0;
};

} // namespace dream::physics3d
