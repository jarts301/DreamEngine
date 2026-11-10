#pragma once

#include "dream/core/handle.h"
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

namespace dream::physics2d {

// 2D rigid-body physics in the XY plane. Units are meters, kilograms and seconds, +Y is up, and
// angles are in radians, counter-clockwise.
//
// It mirrors the 3D interface where it can, but is separate because 2D engines work differently
// and 2D games need different shapes. Positions are body centers, which are also shape centers.

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
    core::Vec2 half_extents{0.5f, 0.5f};
};

struct CircleShape {
    float radius = 0.5f;
};

// Upright along Y. The total height is 2 * (half_height + radius).
struct CapsuleShape {
    float half_height = 0.5f;
    float radius = 0.5f;
};

using Shape = std::variant<BoxShape, CircleShape, CapsuleShape>;

enum class MotionType : std::uint8_t { Static, Kinematic, Dynamic };

struct BodyDesc {
    Shape shape;
    MotionType motion = MotionType::Dynamic;
    core::Vec2 position;
    float angle = 0.0f;
    core::Vec2 linear_velocity;
    float angular_velocity = 0.0f;

    // For dynamic bodies. Rotational inertia follows from the shape.
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.05f;
    float gravity_scale = 1.0f;

    // Keeps the body upright, as for a platformer character.
    bool fixed_rotation = false;
    bool is_sensor = false;
    bool continuous_collision = false;
    std::uint64_t user_data = 0;
};

struct BodyState {
    core::Vec2 position;
    float angle = 0.0f;
    core::Vec2 linear_velocity;
    float angular_velocity = 0.0f;
    bool is_awake = true;
};

// Keeps both bodies' relative position and angle as they are when the joint is created.
struct FixedJoint {};

// Lets the bodies turn around `anchor`, optionally between two angles. The angle is body_a's angle
// relative to body_b (or the world), counter-clockwise, and is zero when the joint is created.
struct HingeJoint {
    core::Vec2 anchor;
    std::optional<float> min_angle_radians;
    std::optional<float> max_angle_radians;
};

// Keeps the distance between two points, one on each body, within a range.
struct DistanceJoint {
    core::Vec2 anchor_a;
    core::Vec2 anchor_b;
    float min_distance = 0.0f;
    float max_distance = 1.0f;
};

using JointType = std::variant<FixedJoint, HingeJoint, DistanceJoint>;

// Anchors are in world space, at the moment the joint is created.
struct JointDesc {
    BodyHandle body_a;

    // An invalid handle attaches body_a to the world.
    BodyHandle body_b;
    JointType type;
};

struct RayHit {
    BodyHandle body;
    std::uint64_t user_data = 0;
    core::Vec2 point;
    core::Vec2 normal;
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
    core::Vec2 point;
    core::Vec2 normal;
};

// The rules every backend applies in create_body, kept here so that backends agree.
namespace detail {

[[nodiscard]] inline bool is_positive(float value) noexcept {
    return value > 0.0f && std::isfinite(value);
}

[[nodiscard]] inline bool is_valid_shape(const Shape& shape) noexcept {
    if (const auto* box = std::get_if<BoxShape>(&shape)) {
        return is_positive(box->half_extents.x) && is_positive(box->half_extents.y);
    }
    if (const auto* circle = std::get_if<CircleShape>(&shape)) {
        return is_positive(circle->radius);
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
                            std::isfinite(desc.angle))) {
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

// One simulated plane. Not thread-safe: call everything from one thread. Handles that no longer
// exist are ignored by every function that takes one.
class PhysicsWorld {
public:
    static constexpr std::string_view k_module_interface = "physics2d";

    virtual ~PhysicsWorld() = default;

    virtual void set_gravity(core::Vec2 gravity) = 0;
    [[nodiscard]] virtual core::Vec2 gravity() const = 0;

    [[nodiscard]] virtual core::Result<BodyHandle> create_body(const BodyDesc& desc) = 0;

    // Also destroys the joints attached to the body.
    virtual void destroy_body(BodyHandle body) = 0;

    [[nodiscard]] virtual std::optional<BodyState> body_state(BodyHandle body) const = 0;

    virtual void set_transform(BodyHandle body, core::Vec2 position, float angle) = 0;
    virtual void move_kinematic(BodyHandle body, core::Vec2 position, float angle,
                                float step_seconds) = 0;
    virtual void set_linear_velocity(BodyHandle body, core::Vec2 velocity) = 0;
    virtual void set_angular_velocity(BodyHandle body, float velocity) = 0;
    virtual void add_force(BodyHandle body, core::Vec2 force) = 0;
    virtual void add_impulse(BodyHandle body, core::Vec2 impulse) = 0;

    [[nodiscard]] virtual core::Result<JointHandle> create_joint(const JointDesc& desc) = 0;
    virtual void destroy_joint(JointHandle joint) = 0;

    virtual void step(float step_seconds) = 0;

    [[nodiscard]] virtual std::span<const ContactEvent> contact_events() const = 0;

    [[nodiscard]] virtual std::optional<RayHit> cast_ray(core::Vec2 origin, core::Vec2 direction,
                                                         float max_distance) const = 0;

    virtual void overlap(const Shape& shape, core::Vec2 position, float angle,
                         std::vector<BodyHandle>& bodies) const = 0;

    [[nodiscard]] virtual std::size_t body_count() const = 0;
};

} // namespace dream::physics2d
