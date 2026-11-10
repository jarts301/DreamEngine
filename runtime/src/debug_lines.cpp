#include "dream/runtime/debug_lines.h"

#include <array>
#include <cmath>

namespace dream::runtime {

using core::Quat;
using core::Vec3;
using core::Vec4;

namespace {

// Two unit vectors at right angles to `normal` and each other.
void basis(Vec3 normal, Vec3& u, Vec3& v) {
    const Vec3 n = core::normalized(normal);
    const Vec3 helper = std::abs(n.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    u = core::normalized(core::cross(helper, n));
    v = core::cross(n, u);
}

} // namespace

void DebugLines::line(Vec3 from, Vec3 to, Vec4 color) {
    m_vertices.push_back(DebugVertex{from, color});
    m_vertices.push_back(DebugVertex{to, color});
}

void DebugLines::box(Vec3 center, Quat rotation, Vec3 half_extents, Vec4 color) {
    std::array<Vec3, 8> corners;
    for (int i = 0; i < 8; ++i) {
        const Vec3 local{(i & 1) != 0 ? half_extents.x : -half_extents.x,
                         (i & 2) != 0 ? half_extents.y : -half_extents.y,
                         (i & 4) != 0 ? half_extents.z : -half_extents.z};
        corners[static_cast<std::size_t>(i)] = center + core::rotate(rotation, local);
    }
    // Each edge joins two corners whose indices differ in one bit.
    for (int i = 0; i < 8; ++i) {
        for (const int bit : {1, 2, 4}) {
            if ((i & bit) == 0) {
                line(corners[static_cast<std::size_t>(i)],
                     corners[static_cast<std::size_t>(i | bit)], color);
            }
        }
    }
}

void DebugLines::circle(Vec3 center, Vec3 normal, float radius, Vec4 color, int segments) {
    if (segments < 3 || !(core::length(normal) > 0.0f)) {
        return;
    }
    Vec3 u;
    Vec3 v;
    basis(normal, u, v);
    Vec3 previous = center + u * radius;
    for (int i = 1; i <= segments; ++i) {
        const float angle = core::k_two_pi * static_cast<float>(i) / static_cast<float>(segments);
        const Vec3 next = center + (u * std::cos(angle) + v * std::sin(angle)) * radius;
        line(previous, next, color);
        previous = next;
    }
}

void DebugLines::sphere(Vec3 center, Quat rotation, float radius, Vec4 color) {
    circle(center, core::rotate(rotation, Vec3{1.0f, 0.0f, 0.0f}), radius, color);
    circle(center, core::rotate(rotation, Vec3{0.0f, 1.0f, 0.0f}), radius, color);
    circle(center, core::rotate(rotation, Vec3{0.0f, 0.0f, 1.0f}), radius, color);
}

void DebugLines::capsule(Vec3 center, Quat rotation, float half_height, float radius,
                         Vec4 color) {
    const Vec3 up = core::rotate(rotation, Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 side_x = core::rotate(rotation, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 side_z = core::rotate(rotation, Vec3{0.0f, 0.0f, 1.0f});
    const Vec3 top = center + up * half_height;
    const Vec3 bottom = center - up * half_height;

    circle(top, up, radius, color);
    circle(bottom, up, radius, color);
    for (const Vec3 side : {side_x, -side_x, side_z, -side_z}) {
        line(bottom + side * radius, top + side * radius, color);
    }

    // Half circles for the rounded ends, in the two upright planes.
    constexpr int k_segments = 12;
    for (const Vec3 side : {side_x, side_z}) {
        for (const float direction : {1.0f, -1.0f}) {
            const Vec3 end = direction > 0.0f ? top : bottom;
            Vec3 previous = end + side * radius;
            for (int i = 1; i <= k_segments; ++i) {
                const float angle = core::k_pi * static_cast<float>(i) / k_segments;
                const Vec3 next =
                    end + (side * std::cos(angle) + up * (direction * std::sin(angle))) * radius;
                line(previous, next, color);
                previous = next;
            }
        }
    }
}

void DebugLines::cross(Vec3 center, float size, Vec4 color) {
    line(center - Vec3{size, 0.0f, 0.0f}, center + Vec3{size, 0.0f, 0.0f}, color);
    line(center - Vec3{0.0f, size, 0.0f}, center + Vec3{0.0f, size, 0.0f}, color);
    line(center - Vec3{0.0f, 0.0f, size}, center + Vec3{0.0f, 0.0f, size}, color);
}

} // namespace dream::runtime
