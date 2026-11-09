#pragma once

#include "dream/core/math/vector.h"

#include <cmath>

namespace dream::core {

// A rotation. Default-constructed to the identity.
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr bool operator==(const Quat&) const = default;
};

// Combines rotations: `a * b` applies b first, then a.
[[nodiscard]] constexpr Quat operator*(Quat a, Quat b) noexcept {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

[[nodiscard]] constexpr float dot(Quat a, Quat b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

[[nodiscard]] constexpr Quat conjugate(Quat q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}

// Returns the identity for a zero-length input.
[[nodiscard]] inline Quat normalized(Quat q) noexcept {
    const float size = std::sqrt(dot(q, q));
    if (size <= 0.0f) {
        return Quat{};
    }
    return {q.x / size, q.y / size, q.z / size, q.w / size};
}

// The axis does not need to be normalized. Positive angles follow the right-hand rule.
[[nodiscard]] inline Quat quat_from_axis_angle(Vec3 axis, float angle_radians) noexcept {
    const Vec3 unit = normalized(axis);
    const float half = angle_radians * 0.5f;
    const float sine = std::sin(half);
    return {unit.x * sine, unit.y * sine, unit.z * sine, std::cos(half)};
}

// Expects a normalized quaternion.
[[nodiscard]] constexpr Vec3 rotate(Quat q, Vec3 v) noexcept {
    const Vec3 axis{q.x, q.y, q.z};
    const Vec3 t = cross(axis, v) * 2.0f;
    return v + t * q.w + cross(axis, t);
}

// Interpolates along the shortest path. Expects normalized quaternions.
[[nodiscard]] inline Quat slerp(Quat a, Quat b, float t) noexcept {
    float cos_angle = dot(a, b);
    if (cos_angle < 0.0f) {
        b = Quat{-b.x, -b.y, -b.z, -b.w};
        cos_angle = -cos_angle;
    }

    float weight_a = 1.0f - t;
    float weight_b = t;
    // Nearly identical rotations would divide by almost zero, so they use a linear blend instead.
    if (cos_angle < 0.9995f) {
        const float angle = std::acos(cos_angle);
        const float sine = std::sin(angle);
        weight_a = std::sin((1.0f - t) * angle) / sine;
        weight_b = std::sin(t * angle) / sine;
    }

    return normalized(Quat{
        a.x * weight_a + b.x * weight_b,
        a.y * weight_a + b.y * weight_b,
        a.z * weight_a + b.z * weight_b,
        a.w * weight_a + b.w * weight_b,
    });
}

} // namespace dream::core
