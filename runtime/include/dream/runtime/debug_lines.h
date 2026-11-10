#pragma once

#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"

#include <cstddef>
#include <span>
#include <vector>

namespace dream::runtime {

// One end of a debug line. Matches the vertex layout of runtime/shaders/debug_lines.slang.
struct DebugVertex {
    core::Vec3 position;
    core::Vec4 color;
};

// Lines collected during a frame for Renderer to draw over the scene, such as physics shapes. They
// are drawn without depth testing, so nothing hides them, and cleared after each frame.
class DebugLines {
public:
    void line(core::Vec3 from, core::Vec3 to, core::Vec4 color);

    // The edges of a box centered on `center`.
    void box(core::Vec3 center, core::Quat rotation, core::Vec3 half_extents, core::Vec4 color);

    // A circle around `normal`, drawn with `segments` straight pieces.
    void circle(core::Vec3 center, core::Vec3 normal, float radius, core::Vec4 color,
                int segments = 24);

    // Three circles, one around each axis.
    void sphere(core::Vec3 center, core::Quat rotation, float radius, core::Vec4 color);

    // Upright along the rotated Y axis, like physics capsules.
    void capsule(core::Vec3 center, core::Quat rotation, float half_height, float radius,
                 core::Vec4 color);

    // Short lines along the three axes, for marking a point.
    void cross(core::Vec3 center, float size, core::Vec4 color);

    void clear() noexcept {
        m_vertices.clear();
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_vertices.empty();
    }

    // Two vertices per line.
    [[nodiscard]] std::span<const DebugVertex> vertices() const noexcept {
        return m_vertices;
    }

private:
    std::vector<DebugVertex> m_vertices;
};

} // namespace dream::runtime
