#pragma once

#include <cmath>
#include <concepts>

namespace dream::core {

inline constexpr float k_pi = 3.14159265358979323846f;
inline constexpr float k_two_pi = 2.0f * k_pi;

[[nodiscard]] constexpr float radians(float degrees) noexcept {
    return degrees * (k_pi / 180.0f);
}

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr bool operator==(const Vec2&) const = default;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr bool operator==(const Vec3&) const = default;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr bool operator==(const Vec4&) const = default;
};

// Vec2

[[nodiscard]] constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept {
    return {a.x + b.x, a.y + b.y};
}

[[nodiscard]] constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept {
    return {a.x - b.x, a.y - b.y};
}

[[nodiscard]] constexpr Vec2 operator-(Vec2 v) noexcept {
    return {-v.x, -v.y};
}

[[nodiscard]] constexpr Vec2 operator*(Vec2 v, float s) noexcept {
    return {v.x * s, v.y * s};
}

[[nodiscard]] constexpr Vec2 operator/(Vec2 v, float s) noexcept {
    return {v.x / s, v.y / s};
}

[[nodiscard]] constexpr float dot(Vec2 a, Vec2 b) noexcept {
    return a.x * b.x + a.y * b.y;
}

// Vec3

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

[[nodiscard]] constexpr Vec3 operator/(Vec3 v, float s) noexcept {
    return {v.x / s, v.y / s, v.z / s};
}

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Vec4

[[nodiscard]] constexpr Vec4 operator+(Vec4 a, Vec4 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

[[nodiscard]] constexpr Vec4 operator-(Vec4 a, Vec4 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

[[nodiscard]] constexpr Vec4 operator-(Vec4 v) noexcept {
    return {-v.x, -v.y, -v.z, -v.w};
}

[[nodiscard]] constexpr Vec4 operator*(Vec4 v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s, v.w * s};
}

[[nodiscard]] constexpr Vec4 operator/(Vec4 v, float s) noexcept {
    return {v.x / s, v.y / s, v.z / s, v.w / s};
}

[[nodiscard]] constexpr float dot(Vec4 a, Vec4 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// Shared by all vector types

template <typename V>
concept Vector = std::same_as<V, Vec2> || std::same_as<V, Vec3> || std::same_as<V, Vec4>;

template <Vector V> [[nodiscard]] constexpr V operator*(float s, V v) noexcept {
    return v * s;
}

template <Vector V> constexpr V& operator+=(V& a, V b) noexcept {
    a = a + b;
    return a;
}

template <Vector V> constexpr V& operator-=(V& a, V b) noexcept {
    a = a - b;
    return a;
}

template <Vector V> constexpr V& operator*=(V& v, float s) noexcept {
    v = v * s;
    return v;
}

template <Vector V> [[nodiscard]] constexpr float length_squared(V v) noexcept {
    return dot(v, v);
}

template <Vector V> [[nodiscard]] inline float length(V v) noexcept {
    return std::sqrt(length_squared(v));
}

// Returns the zero vector for a zero-length input.
template <Vector V> [[nodiscard]] inline V normalized(V v) noexcept {
    const float size = length(v);
    return size > 0.0f ? v / size : V{};
}

// Works for floats and for any vector type.
template <typename T> [[nodiscard]] constexpr T lerp(T a, T b, float t) noexcept {
    return a + (b - a) * t;
}

} // namespace dream::core
