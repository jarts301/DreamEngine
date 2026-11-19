#pragma once

#include "dream/core/math/matrix.h"
#include "dream/core/math/vector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace dream::core {

// An axis-aligned box. Default-constructed empty, with min above max, which is how a box with
// nothing in it, or an unknown extent, is told apart.
struct Aabb {
    Vec3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    Vec3 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
             std::numeric_limits<float>::lowest()};

    [[nodiscard]] constexpr bool empty() const noexcept {
        return min.x > max.x || min.y > max.y || min.z > max.z;
    }

    // Grows the box to hold `point`.
    constexpr void add(Vec3 point) noexcept {
        min = Vec3{std::min(min.x, point.x), std::min(min.y, point.y), std::min(min.z, point.z)};
        max = Vec3{std::max(max.x, point.x), std::max(max.y, point.y), std::max(max.z, point.z)};
    }
};

// The box around `box` after `matrix` moves, turns and scales it, which may be larger than the
// moved shape itself. An empty box stays empty.
[[nodiscard]] inline Aabb transformed(const Mat4& matrix, const Aabb& box) noexcept {
    if (box.empty()) {
        return box;
    }
    const Vec4& t = matrix.columns[3];
    Aabb result;
    result.min = Vec3{t.x, t.y, t.z};
    result.max = result.min;
    // For each axis of the result, the smallest and largest contribution of each input axis.
    const std::array<float, 3> low{box.min.x, box.min.y, box.min.z};
    const std::array<float, 3> high{box.max.x, box.max.y, box.max.z};
    for (std::size_t column = 0; column < 3; ++column) {
        const Vec4& axis = matrix.columns[column];
        const std::array<float, 3> along{axis.x, axis.y, axis.z};
        std::array<float, 3> grow_min{};
        std::array<float, 3> grow_max{};
        for (std::size_t row = 0; row < 3; ++row) {
            const float a = along[row] * low[column];
            const float b = along[row] * high[column];
            grow_min[row] = std::min(a, b);
            grow_max[row] = std::max(a, b);
        }
        result.min = result.min + Vec3{grow_min[0], grow_min[1], grow_min[2]};
        result.max = result.max + Vec3{grow_max[0], grow_max[1], grow_max[2]};
    }
    return result;
}

// The six planes of what a view-projection matrix sees, as matrix.h's perspective and orthographic
// make them: clip x and y from -1 to 1 and depth from 0 to 1. Each plane is (normal, distance),
// with the normal pointing inward, so a point p is inside when dot(normal, p) + distance >= 0.
struct Frustum {
    std::array<Vec4, 6> planes{};
};

[[nodiscard]] inline Frustum frustum_from(const Mat4& view_projection) noexcept {
    const auto row = [&](std::size_t index) {
        const auto pick = [&](const Vec4& column) {
            return index == 0 ? column.x : index == 1 ? column.y : index == 2 ? column.z : column.w;
        };
        const auto& c = view_projection.columns;
        return Vec4{pick(c[0]), pick(c[1]), pick(c[2]), pick(c[3])};
    };
    const Vec4 x = row(0);
    const Vec4 y = row(1);
    const Vec4 z = row(2);
    const Vec4 w = row(3);
    Frustum frustum{{w + x, w - x, w + y, w - y, z, w - z}};
    // Normalized, so the distances are in world units.
    for (Vec4& plane : frustum.planes) {
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (length > 0.0f) {
            plane = plane * (1.0f / length);
        }
    }
    return frustum;
}

// Whether any of `box` may be inside. It errs toward true near the frustum's corners, where a box
// outside can still pass, which only costs a draw. An empty box, whose extent is unknown, is always
// treated as inside.
[[nodiscard]] inline bool intersects(const Frustum& frustum, const Aabb& box) noexcept {
    if (box.empty()) {
        return true;
    }
    for (const Vec4& plane : frustum.planes) {
        // The box's corner furthest along the plane's normal.
        const Vec3 corner{plane.x >= 0.0f ? box.max.x : box.min.x,
                          plane.y >= 0.0f ? box.max.y : box.min.y,
                          plane.z >= 0.0f ? box.max.z : box.min.z};
        if (plane.x * corner.x + plane.y * corner.y + plane.z * corner.z + plane.w < 0.0f) {
            return false;
        }
    }
    return true;
}

} // namespace dream::core
