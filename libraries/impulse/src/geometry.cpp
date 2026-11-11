#include "geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace impulse::detail {
namespace {

bool positive(float value) {
    return value > 0.0f && std::isfinite(value);
}

// The nearer hit of a ray with a sphere, if any, for unit `direction`.
std::optional<float> ray_sphere(Vec3 origin, Vec3 direction, Vec3 center, float radius) {
    const Vec3 offset = origin - center;
    const float b = dot(offset, direction);
    const float c = dot(offset, offset) - radius * radius;
    if (c <= 0.0f) {
        return 0.0f;
    }
    if (b > 0.0f) {
        return std::nullopt;
    }
    const float discriminant = b * b - c;
    if (discriminant < 0.0f) {
        return std::nullopt;
    }
    return -b - std::sqrt(discriminant);
}

} // namespace

bool is_valid(const Shape& shape) {
    if (const auto* box = std::get_if<Box>(&shape)) {
        return positive(box->half_extents.x) && positive(box->half_extents.y) &&
               positive(box->half_extents.z);
    }
    if (const auto* sphere = std::get_if<Sphere>(&shape)) {
        return positive(sphere->radius);
    }
    const auto& capsule = std::get<Capsule>(shape);
    return positive(capsule.radius) && capsule.half_height >= 0.0f &&
           std::isfinite(capsule.half_height);
}

Aabb bounds(const Shape& shape, const Transform& transform) {
    if (const auto* box = std::get_if<Box>(&shape)) {
        const Mat3& m = transform.basis;
        const Vec3 extent{dot(abs(m.rows[0]), box->half_extents),
                          dot(abs(m.rows[1]), box->half_extents),
                          dot(abs(m.rows[2]), box->half_extents)};
        return Aabb{transform.position - extent, transform.position + extent};
    }
    const Segment core = core_segment(shape, transform);
    const float radius = core_radius(shape);
    const Vec3 round{radius, radius, radius};
    return Aabb{min(core.start, core.end) - round, max(core.start, core.end) + round};
}

Vec3 inverse_inertia(const Shape& shape, float mass) {
    Vec3 inertia;
    if (const auto* box = std::get_if<Box>(&shape)) {
        const Vec3 h = box->half_extents;
        inertia = Vec3{h.y * h.y + h.z * h.z, h.x * h.x + h.z * h.z, h.x * h.x + h.y * h.y} *
                  (mass / 3.0f);
    } else if (const auto* sphere = std::get_if<Sphere>(&shape)) {
        const float i = 0.4f * mass * sphere->radius * sphere->radius;
        inertia = Vec3{i, i, i};
    } else {
        // A cylinder and two half spheres, sharing the mass by volume.
        const auto& capsule = std::get<Capsule>(shape);
        const float r = capsule.radius;
        const float h = 2.0f * capsule.half_height;
        const float cylinder_volume = std::numbers::pi_v<float> * r * r * h;
        const float sphere_volume = 4.0f / 3.0f * std::numbers::pi_v<float> * r * r * r;
        const float cylinder = mass * cylinder_volume / (cylinder_volume + sphere_volume);
        const float caps = mass - cylinder;
        const float around = 0.5f * cylinder * r * r + 0.4f * caps * r * r;
        const float across = cylinder * (r * r / 4.0f + h * h / 12.0f) +
                             caps * (0.4f * r * r + h * h / 4.0f + 0.375f * h * r);
        inertia = Vec3{across, around, across};
    }
    return Vec3{1.0f / inertia.x, 1.0f / inertia.y, 1.0f / inertia.z};
}

Segment core_segment(const Shape& shape, const Transform& transform) {
    if (const auto* capsule = std::get_if<Capsule>(&shape)) {
        const Vec3 half = transform.axis(1) * capsule->half_height;
        return Segment{transform.position - half, transform.position + half};
    }
    return Segment{transform.position, transform.position};
}

float core_radius(const Shape& shape) {
    if (const auto* sphere = std::get_if<Sphere>(&shape)) {
        return sphere->radius;
    }
    if (const auto* capsule = std::get_if<Capsule>(&shape)) {
        return capsule->radius;
    }
    return 0.0f;
}

std::optional<RayResult> cast_ray(const Shape& shape, const Transform& transform, Vec3 origin,
                                  Vec3 direction, float max_distance) {
    if (const auto* box = std::get_if<Box>(&shape)) {
        // The slab method, in the box's own axes.
        const Vec3 o = transform.to_local(origin);
        const Vec3 d = transpose(transform.basis) * direction;
        const Vec3 h = box->half_extents;
        float enter = 0.0f;
        float leave = max_distance;
        int enter_axis = -1;
        float enter_sign = 0.0f;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(d[axis]) < 1e-9f) {
                if (std::abs(o[axis]) > h[axis]) {
                    return std::nullopt;
                }
                continue;
            }
            const float inverse = 1.0f / d[axis];
            float near = (-h[axis] - o[axis]) * inverse;
            float far = (h[axis] - o[axis]) * inverse;
            float sign = -1.0f;
            if (near > far) {
                std::swap(near, far);
                sign = 1.0f;
            }
            if (near > enter) {
                enter = near;
                enter_axis = axis;
                enter_sign = sign;
            }
            leave = std::min(leave, far);
            if (enter > leave) {
                return std::nullopt;
            }
        }
        if (enter_axis < 0) {
            // Starts inside the box.
            return RayResult{0.0f, -direction};
        }
        Vec3 normal;
        normal[enter_axis] = enter_sign;
        return RayResult{enter, transform.basis * normal};
    }

    const Segment core = core_segment(shape, transform);
    const float radius = core_radius(shape);
    std::optional<RayResult> best;
    const auto consider = [&](float distance, Vec3 center) {
        if (distance <= max_distance && (!best.has_value() || distance < best->distance)) {
            const Vec3 point = origin + direction * distance;
            const Vec3 normal = normalized(point - center);
            best = RayResult{distance, length_squared(normal) > 0.0f ? normal : -direction};
        }
    };

    // The end spheres.
    for (const Vec3 end : {core.start, core.end}) {
        if (const std::optional<float> hit = ray_sphere(origin, direction, end, radius)) {
            consider(*hit, end);
        }
    }
    // The side of the cylinder between them.
    const Vec3 axis_vector = core.end - core.start;
    const float axis_length = length(axis_vector);
    if (axis_length > 1e-6f) {
        const Vec3 axis = axis_vector / axis_length;
        const Vec3 offset = origin - core.start;
        const Vec3 d = direction - axis * dot(direction, axis);
        const Vec3 o = offset - axis * dot(offset, axis);
        const float a = dot(d, d);
        const float b = dot(o, d);
        const float c = dot(o, o) - radius * radius;
        if (a > 1e-12f) {
            const float discriminant = b * b - a * c;
            // Inside the infinite cylinder the ray starts in contact; outside, a hit behind the
            // origin doesn't count.
            const float distance =
                c <= 0.0f ? 0.0f
                          : (discriminant >= 0.0f ? (-b - std::sqrt(discriminant)) / a : -1.0f);
            if (distance >= 0.0f) {
                const float along = dot(offset + direction * distance, axis);
                if (along >= 0.0f && along <= axis_length) {
                    consider(distance, core.start + axis * along);
                }
            }
        }
    }
    return best;
}

void closest_parameters(Segment a, Segment b, float& s, float& t) {
    // Ericson, Real-Time Collision Detection, 5.1.9.
    constexpr float epsilon = 1e-12f;
    const Vec3 d1 = a.end - a.start;
    const Vec3 d2 = b.end - b.start;
    const Vec3 r = a.start - b.start;
    const float aa = dot(d1, d1);
    const float ee = dot(d2, d2);
    const float f = dot(d2, r);
    if (aa <= epsilon && ee <= epsilon) {
        s = t = 0.0f;
        return;
    }
    if (aa <= epsilon) {
        s = 0.0f;
        t = std::clamp(f / ee, 0.0f, 1.0f);
        return;
    }
    const float c = dot(d1, r);
    if (ee <= epsilon) {
        t = 0.0f;
        s = std::clamp(-c / aa, 0.0f, 1.0f);
        return;
    }
    const float bb = dot(d1, d2);
    const float denominator = aa * ee - bb * bb;
    s = denominator > epsilon ? std::clamp((bb * f - c * ee) / denominator, 0.0f, 1.0f) : 0.0f;
    t = (bb * s + f) / ee;
    if (t < 0.0f) {
        t = 0.0f;
        s = std::clamp(-c / aa, 0.0f, 1.0f);
    } else if (t > 1.0f) {
        t = 1.0f;
        s = std::clamp((bb - c) / aa, 0.0f, 1.0f);
    }
}

} // namespace impulse::detail
