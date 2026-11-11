#pragma once

// Contact generation between pairs of shapes.

#include "geometry.h"

namespace impulse::detail {

struct ManifoldPoint {
    // Halfway between the two surfaces.
    Vec3 position;

    // Distance between the surfaces along the normal: negative when they overlap.
    float separation = 0.0f;
};

struct Manifold {
    // From shape a to shape b.
    Vec3 normal;
    ManifoldPoint points[4];
    int count = 0;
};

// The contacts between two shapes that are closer than `margin`, or none. Nearby but separated
// points let the solver stop bodies exactly where they would meet.
void collide(const Shape& a, const Transform& ta, const Shape& b, const Transform& tb, float margin,
             Manifold& manifold);

} // namespace impulse::detail
