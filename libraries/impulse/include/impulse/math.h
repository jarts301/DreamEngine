#pragma once

// The small amount of vector math the library needs. Right-handed coordinates; quaternions rotate
// vectors as q * v * conjugate(q).

#include <algorithm>
#include <cmath>

namespace impulse {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3& operator+=(Vec3 v) noexcept {
        x += v.x;
        y += v.y;
        z += v.z;
        return *this;
    }
    constexpr Vec3& operator-=(Vec3 v) noexcept {
        x -= v.x;
        y -= v.y;
        z -= v.z;
        return *this;
    }
    constexpr Vec3& operator*=(float s) noexcept {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
    [[nodiscard]] constexpr float& operator[](int i) noexcept {
        return i == 0 ? x : (i == 1 ? y : z);
    }
    [[nodiscard]] constexpr float operator[](int i) const noexcept {
        return i == 0 ? x : (i == 1 ? y : z);
    }
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] constexpr Vec3 operator-(Vec3 v) noexcept {
    return {-v.x, -v.y, -v.z};
}
[[nodiscard]] constexpr Vec3 operator*(Vec3 v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}
[[nodiscard]] constexpr Vec3 operator*(float s, Vec3 v) noexcept {
    return v * s;
}
[[nodiscard]] constexpr Vec3 operator/(Vec3 v, float s) noexcept {
    return {v.x / s, v.y / s, v.z / s};
}
[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Vec3 v) noexcept {
    return std::sqrt(dot(v, v));
}
[[nodiscard]] constexpr float length_squared(Vec3 v) noexcept {
    return dot(v, v);
}
// Zero stays zero.
[[nodiscard]] inline Vec3 normalized(Vec3 v) noexcept {
    const float size = length(v);
    return size > 1e-12f ? v / size : Vec3{};
}
[[nodiscard]] inline Vec3 abs(Vec3 v) noexcept {
    return {std::abs(v.x), std::abs(v.y), std::abs(v.z)};
}
[[nodiscard]] inline Vec3 min(Vec3 a, Vec3 b) noexcept {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
[[nodiscard]] inline Vec3 max(Vec3 a, Vec3 b) noexcept {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
[[nodiscard]] inline bool is_finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Two unit vectors at right angles to unit vector `n` and to each other.
inline void tangents(Vec3 n, Vec3& t1, Vec3& t2) noexcept {
    // Always the same pair for the same normal, so friction impulses carry over between steps.
    if (std::abs(n.x) >= 0.57735f) {
        t1 = normalized(Vec3{n.y, -n.x, 0.0f});
    } else {
        t1 = normalized(Vec3{0.0f, n.z, -n.y});
    }
    t2 = cross(n, t1);
}

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

[[nodiscard]] constexpr Quat operator*(Quat a, Quat b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
[[nodiscard]] constexpr Quat conjugate(Quat q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}
[[nodiscard]] constexpr float dot(Quat a, Quat b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
[[nodiscard]] inline Quat normalized(Quat q) noexcept {
    const float size = std::sqrt(dot(q, q));
    return size > 1e-12f ? Quat{q.x / size, q.y / size, q.z / size, q.w / size} : Quat{};
}
[[nodiscard]] inline Vec3 rotate(Quat q, Vec3 v) noexcept {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = 2.0f * cross(u, v);
    return v + q.w * t + cross(u, t);
}
// A rotation of `angle` radians around unit vector `axis`, by the right-hand rule.
[[nodiscard]] inline Quat from_axis_angle(Vec3 axis, float angle) noexcept {
    const float s = std::sin(0.5f * angle);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(0.5f * angle)};
}
// The rotation by rotation vector `r`: around r's direction, by its length in radians.
[[nodiscard]] inline Quat from_rotation_vector(Vec3 r) noexcept {
    const float angle = length(r);
    if (angle < 1e-9f) {
        return normalized(Quat{0.5f * r.x, 0.5f * r.y, 0.5f * r.z, 1.0f});
    }
    return from_axis_angle(r / angle, angle);
}
// The rotation vector of `q`, taking the shorter way round.
[[nodiscard]] inline Vec3 to_rotation_vector(Quat q) noexcept {
    if (q.w < 0.0f) {
        q = Quat{-q.x, -q.y, -q.z, -q.w};
    }
    const Vec3 v{q.x, q.y, q.z};
    const float s = length(v);
    if (s < 1e-9f) {
        return 2.0f * v;
    }
    return v * (2.0f * std::atan2(s, q.w) / s);
}

// A 3x3 matrix, stored as rows.
struct Mat3 {
    Vec3 rows[3]{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};

    [[nodiscard]] static constexpr Mat3 zero() noexcept {
        return Mat3{{Vec3{}, Vec3{}, Vec3{}}};
    }
    [[nodiscard]] static constexpr Mat3 diagonal(Vec3 d) noexcept {
        return Mat3{{Vec3{d.x, 0.0f, 0.0f}, Vec3{0.0f, d.y, 0.0f}, Vec3{0.0f, 0.0f, d.z}}};
    }
    [[nodiscard]] constexpr Vec3 column(int i) const noexcept {
        return {rows[0][i], rows[1][i], rows[2][i]};
    }
};

[[nodiscard]] constexpr Vec3 operator*(const Mat3& m, Vec3 v) noexcept {
    return {dot(m.rows[0], v), dot(m.rows[1], v), dot(m.rows[2], v)};
}
[[nodiscard]] constexpr Mat3 operator*(const Mat3& a, const Mat3& b) noexcept {
    Mat3 result;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            result.rows[r][c] = dot(a.rows[r], b.column(c));
        }
    }
    return result;
}
[[nodiscard]] constexpr Mat3 operator+(const Mat3& a, const Mat3& b) noexcept {
    Mat3 result;
    for (int r = 0; r < 3; ++r) {
        result.rows[r] = a.rows[r] + b.rows[r];
    }
    return result;
}
[[nodiscard]] constexpr Mat3 transpose(const Mat3& m) noexcept {
    return Mat3{{m.column(0), m.column(1), m.column(2)}};
}
// The matrix of v x, so that skew(v) * u == cross(v, u).
[[nodiscard]] constexpr Mat3 skew(Vec3 v) noexcept {
    return Mat3{{Vec3{0.0f, -v.z, v.y}, Vec3{v.z, 0.0f, -v.x}, Vec3{-v.y, v.x, 0.0f}}};
}
[[nodiscard]] inline Mat3 rotation_matrix(Quat q) noexcept {
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return Mat3{{Vec3{1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy)},
                 Vec3{2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx)},
                 Vec3{2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy)}}};
}
// Solves m * x = b. A matrix too close to singular gives zero.
[[nodiscard]] inline Vec3 solve(const Mat3& m, Vec3 b) noexcept {
    const Vec3 c0 = m.column(0), c1 = m.column(1), c2 = m.column(2);
    const float determinant = dot(c0, cross(c1, c2));
    if (std::abs(determinant) < 1e-12f) {
        return Vec3{};
    }
    const float inverse = 1.0f / determinant;
    return Vec3{inverse * dot(b, cross(c1, c2)), inverse * dot(c0, cross(b, c2)),
                inverse * dot(c0, cross(c1, b))};
}

} // namespace impulse
