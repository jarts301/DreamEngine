#pragma once

#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"

#include <array>
#include <cmath>
#include <optional>

namespace dream::core {

// Column-major, for column vectors: `a * b` applies b first, then a. Default-constructed to the
// identity. Conventions are recorded in docs/adr/0005-core-design-choices.md.
struct Mat4 {
    std::array<Vec4, 4> columns{
        Vec4{1.0f, 0.0f, 0.0f, 0.0f},
        Vec4{0.0f, 1.0f, 0.0f, 0.0f},
        Vec4{0.0f, 0.0f, 1.0f, 0.0f},
        Vec4{0.0f, 0.0f, 0.0f, 1.0f},
    };

    constexpr bool operator==(const Mat4&) const = default;
};

[[nodiscard]] constexpr Vec4 operator*(const Mat4& m, Vec4 v) noexcept {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z + m.columns[3] * v.w;
}

[[nodiscard]] constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    return Mat4{{a * b.columns[0], a * b.columns[1], a * b.columns[2], a * b.columns[3]}};
}

// Transforms a position: translation applies.
[[nodiscard]] constexpr Vec3 transform_point(const Mat4& m, Vec3 p) noexcept {
    const Vec4 result = m * Vec4{p.x, p.y, p.z, 1.0f};
    return {result.x, result.y, result.z};
}

// Transforms a direction: translation does not apply.
[[nodiscard]] constexpr Vec3 transform_direction(const Mat4& m, Vec3 d) noexcept {
    const Vec4 result = m * Vec4{d.x, d.y, d.z, 0.0f};
    return {result.x, result.y, result.z};
}

[[nodiscard]] constexpr Mat4 transpose(const Mat4& m) noexcept {
    const auto& c = m.columns;
    return Mat4{{
        Vec4{c[0].x, c[1].x, c[2].x, c[3].x},
        Vec4{c[0].y, c[1].y, c[2].y, c[3].y},
        Vec4{c[0].z, c[1].z, c[2].z, c[3].z},
        Vec4{c[0].w, c[1].w, c[2].w, c[3].w},
    }};
}

[[nodiscard]] constexpr Mat4 translation(Vec3 offset) noexcept {
    Mat4 result;
    result.columns[3] = Vec4{offset.x, offset.y, offset.z, 1.0f};
    return result;
}

[[nodiscard]] constexpr Mat4 scaling(Vec3 factors) noexcept {
    Mat4 result;
    result.columns[0].x = factors.x;
    result.columns[1].y = factors.y;
    result.columns[2].z = factors.z;
    return result;
}

// Expects a normalized quaternion.
[[nodiscard]] constexpr Mat4 rotation(Quat q) noexcept {
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;

    return Mat4{{
        Vec4{1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy), 0.0f},
        Vec4{2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx), 0.0f},
        Vec4{2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy), 0.0f},
        Vec4{0.0f, 0.0f, 0.0f, 1.0f},
    }};
}

// Applies scale first, then rotation, then translation.
[[nodiscard]] constexpr Mat4 transform(Vec3 offset, Quat orientation, Vec3 factors) noexcept {
    return translation(offset) * rotation(orientation) * scaling(factors);
}

[[nodiscard]] inline Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const Vec3 forward = normalized(target - eye);
    const Vec3 right = normalized(cross(forward, up));
    const Vec3 camera_up = cross(right, forward);

    return Mat4{{
        Vec4{right.x, camera_up.x, -forward.x, 0.0f},
        Vec4{right.y, camera_up.y, -forward.y, 0.0f},
        Vec4{right.z, camera_up.z, -forward.z, 0.0f},
        Vec4{-dot(right, eye), -dot(camera_up, eye), dot(forward, eye), 1.0f},
    }};
}

// Maps view depth z_near to 0 and z_far to 1, with +Y up in clip space.
[[nodiscard]] inline Mat4 perspective(float fov_y_radians, float aspect, float z_near,
                                      float z_far) noexcept {
    const float focal = 1.0f / std::tan(fov_y_radians * 0.5f);
    const float range = z_near - z_far;

    return Mat4{{
        Vec4{focal / aspect, 0.0f, 0.0f, 0.0f},
        Vec4{0.0f, focal, 0.0f, 0.0f},
        Vec4{0.0f, 0.0f, z_far / range, -1.0f},
        Vec4{0.0f, 0.0f, z_near * z_far / range, 0.0f},
    }};
}

// Maps view depth z_near to 0 and z_far to 1, with +Y up in clip space.
[[nodiscard]] constexpr Mat4 orthographic(float left, float right, float bottom, float top,
                                          float z_near, float z_far) noexcept {
    const float width = right - left;
    const float height = top - bottom;
    const float range = z_near - z_far;

    return Mat4{{
        Vec4{2.0f / width, 0.0f, 0.0f, 0.0f},
        Vec4{0.0f, 2.0f / height, 0.0f, 0.0f},
        Vec4{0.0f, 0.0f, 1.0f / range, 0.0f},
        Vec4{-(right + left) / width, -(top + bottom) / height, z_near / range, 1.0f},
    }};
}

// Returns nothing if the matrix cannot be inverted.
[[nodiscard]] inline std::optional<Mat4> inverse(const Mat4& m) noexcept {
    const auto& c = m.columns;

    const float b00 = c[0].x * c[1].y - c[0].y * c[1].x;
    const float b01 = c[0].x * c[1].z - c[0].z * c[1].x;
    const float b02 = c[0].x * c[1].w - c[0].w * c[1].x;
    const float b03 = c[0].y * c[1].z - c[0].z * c[1].y;
    const float b04 = c[0].y * c[1].w - c[0].w * c[1].y;
    const float b05 = c[0].z * c[1].w - c[0].w * c[1].z;
    const float b06 = c[2].x * c[3].y - c[2].y * c[3].x;
    const float b07 = c[2].x * c[3].z - c[2].z * c[3].x;
    const float b08 = c[2].x * c[3].w - c[2].w * c[3].x;
    const float b09 = c[2].y * c[3].z - c[2].z * c[3].y;
    const float b10 = c[2].y * c[3].w - c[2].w * c[3].y;
    const float b11 = c[2].z * c[3].w - c[2].w * c[3].z;

    const float determinant =
        b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (determinant == 0.0f || !std::isfinite(determinant)) {
        return std::nullopt;
    }

    const float s = 1.0f / determinant;
    return Mat4{{
        Vec4{(c[1].y * b11 - c[1].z * b10 + c[1].w * b09) * s,
             (c[0].z * b10 - c[0].y * b11 - c[0].w * b09) * s,
             (c[3].y * b05 - c[3].z * b04 + c[3].w * b03) * s,
             (c[2].z * b04 - c[2].y * b05 - c[2].w * b03) * s},
        Vec4{(c[1].z * b08 - c[1].x * b11 - c[1].w * b07) * s,
             (c[0].x * b11 - c[0].z * b08 + c[0].w * b07) * s,
             (c[3].z * b02 - c[3].x * b05 - c[3].w * b01) * s,
             (c[2].x * b05 - c[2].z * b02 + c[2].w * b01) * s},
        Vec4{(c[1].x * b10 - c[1].y * b08 + c[1].w * b06) * s,
             (c[0].y * b08 - c[0].x * b10 - c[0].w * b06) * s,
             (c[3].x * b04 - c[3].y * b02 + c[3].w * b00) * s,
             (c[2].y * b02 - c[2].x * b04 - c[2].w * b00) * s},
        Vec4{(c[1].y * b07 - c[1].x * b09 - c[1].z * b06) * s,
             (c[0].x * b09 - c[0].y * b07 + c[0].z * b06) * s,
             (c[3].y * b01 - c[3].x * b03 - c[3].z * b00) * s,
             (c[2].x * b03 - c[2].y * b01 + c[2].z * b00) * s},
    }};
}

} // namespace dream::core
