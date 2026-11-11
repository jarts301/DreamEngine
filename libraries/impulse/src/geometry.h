#pragma once

// Shapes as the solver and queries see them: placed in the world, bounded, weighed and hit by rays.

#include "impulse/world.h"

#include <optional>

namespace impulse::detail {

struct Transform {
    Vec3 position;
    Quat rotation;

    // Its columns are the shape's own axes in world space.
    Mat3 basis;

    [[nodiscard]] static Transform make(Vec3 position, Quat rotation) {
        return Transform{position, rotation, rotation_matrix(rotation)};
    }
    [[nodiscard]] Vec3 to_world(Vec3 local) const {
        return position + basis * local;
    }
    [[nodiscard]] Vec3 to_local(Vec3 world) const {
        return transpose(basis) * (world - position);
    }
    [[nodiscard]] Vec3 axis(int i) const {
        return basis.column(i);
    }
};

struct Aabb {
    Vec3 min;
    Vec3 max;

    [[nodiscard]] bool overlaps(const Aabb& other) const {
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y &&
               max.y >= other.min.y && min.z <= other.max.z && max.z >= other.min.z;
    }
};

// The capsule's inner segment, or a sphere's center twice.
struct Segment {
    Vec3 start;
    Vec3 end;
};

[[nodiscard]] bool is_valid(const Shape& shape);

[[nodiscard]] Aabb bounds(const Shape& shape, const Transform& transform);

// The diagonal of the inverse inertia tensor in the shape's own axes.
[[nodiscard]] Vec3 inverse_inertia(const Shape& shape, float mass);

// For spheres and capsules: the segment their surface surrounds, and its radius.
[[nodiscard]] Segment core_segment(const Shape& shape, const Transform& transform);
[[nodiscard]] float core_radius(const Shape& shape);

struct RayResult {
    float distance = 0.0f;
    Vec3 normal;
};

// `direction` must be a unit vector. A ray starting inside a shape hits it at distance zero.
[[nodiscard]] std::optional<RayResult> cast_ray(const Shape& shape, const Transform& transform,
                                                Vec3 origin, Vec3 direction, float max_distance);

// The closest points between two segments, as parameters from 0 to 1 along each.
void closest_parameters(Segment a, Segment b, float& s, float& t);

[[nodiscard]] inline Vec3 point_at(Segment segment, float t) {
    return segment.start + (segment.end - segment.start) * t;
}

} // namespace impulse::detail
