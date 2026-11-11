#include "collide.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace impulse::detail {
namespace {

// A face is preferred over another, or over an edge, unless the other separates by this much more.
// Without it, stacked boxes would flip between equally good choices from one step to the next.
constexpr float k_face_tolerance = 0.0005f;
constexpr float k_edge_tolerance = 0.002f;

void add_point(Manifold& manifold, Vec3 position, float separation) {
    if (manifold.count < 4) {
        manifold.points[manifold.count++] = ManifoldPoint{position, separation};
    }
}

// --- Spheres and capsules: segments with a radius ---------------------------------------------

void rounded_pair(Segment a, float radius_a, Segment b, float radius_b, float margin,
                  Manifold& manifold) {
    float s = 0.0f;
    float t = 0.0f;
    closest_parameters(a, b, s, t);
    const Vec3 on_a = point_at(a, s);
    const Vec3 on_b = point_at(b, t);
    const Vec3 offset = on_b - on_a;
    const float distance = length(offset);

    Vec3 normal{0.0f, 1.0f, 0.0f};
    if (distance > 1e-6f) {
        normal = offset / distance;
    } else if (length_squared(a.end - a.start) > 1e-12f) {
        // The cores cross: any direction across segment a will do.
        Vec3 unused;
        tangents(normalized(a.end - a.start), normal, unused);
    }
    const float separation = distance - radius_a - radius_b;
    if (separation > margin) {
        return;
    }
    manifold.normal = normal;

    // Capsules lying along each other touch along a line; two points keep them steady.
    const Vec3 along_a = a.end - a.start;
    const Vec3 along_b = b.end - b.start;
    const float length_a = length(along_a);
    const float length_b = length(along_b);
    if (length_a > 1e-4f && length_b > 1e-4f &&
        std::abs(dot(along_a, along_b)) > 0.995f * length_a * length_b) {
        const float scale = 1.0f / (length_a * length_a);
        const float u0 = dot(b.start - a.start, along_a) * scale;
        const float u1 = dot(b.end - a.start, along_a) * scale;
        const float low = std::max(0.0f, std::min(u0, u1));
        const float high = std::min(1.0f, std::max(u0, u1));
        if (high - low > 1e-3f) {
            for (const float u : {low, high}) {
                const Vec3 p = point_at(a, u);
                const float v =
                    std::clamp(dot(p - b.start, along_b) / (length_b * length_b), 0.0f, 1.0f);
                const float gap = dot(point_at(b, v) - p, normal) - radius_a - radius_b;
                if (gap <= margin) {
                    add_point(manifold, p + normal * (radius_a + 0.5f * gap), gap);
                }
            }
            if (manifold.count > 0) {
                return;
            }
        }
    }
    add_point(manifold, on_a + normal * (radius_a + 0.5f * separation), separation);
}

// --- A sphere or capsule against a box ---------------------------------------------------------

Vec3 clamp_to_box(Vec3 p, Vec3 h) {
    return Vec3{std::clamp(p.x, -h.x, h.x), std::clamp(p.y, -h.y, h.y), std::clamp(p.z, -h.z, h.z)};
}

// Contacts of a rounded segment with a box, with the normal from the box to the segment.
void rounded_box(Segment segment, float radius, Vec3 h, const Transform& box, float margin,
                 Manifold& manifold) {
    const Vec3 p = box.to_local(segment.start);
    const Vec3 q = box.to_local(segment.end);
    const auto gap_squared = [&](float t) {
        const Vec3 point = p + (q - p) * t;
        return length_squared(point - clamp_to_box(point, h));
    };

    // The distance from the box along the segment has a single minimum, which a golden section
    // search finds.
    float t = 0.0f;
    if (length_squared(q - p) > 1e-12f) {
        constexpr float golden = 0.61803399f;
        float low = 0.0f;
        float high = 1.0f;
        float x1 = high - golden * (high - low);
        float x2 = low + golden * (high - low);
        float f1 = gap_squared(x1);
        float f2 = gap_squared(x2);
        for (int i = 0; i < 40; ++i) {
            if (f1 < f2) {
                high = x2;
                x2 = x1;
                f2 = f1;
                x1 = high - golden * (high - low);
                f1 = gap_squared(x1);
            } else {
                low = x1;
                x1 = x2;
                f1 = f2;
                x2 = low + golden * (high - low);
                f2 = gap_squared(x2);
            }
        }
        t = 0.5f * (low + high);
        for (const float end : {0.0f, 1.0f}) {
            if (gap_squared(end) <= gap_squared(t)) {
                t = end;
            }
        }
    }

    const Vec3 nearest = p + (q - p) * t;
    const Vec3 on_box = clamp_to_box(nearest, h);
    const float distance = length(nearest - on_box);

    Vec3 normal;
    if (distance > 1e-6f) {
        normal = (nearest - on_box) / distance;
        const float separation = distance - radius;
        if (separation > margin) {
            return;
        }
        add_point(manifold, on_box + normal * (0.5f * separation), separation);

        // A capsule lying on a face touches along its length, so its ends count too.
        for (const Vec3 end : {p, q}) {
            if (length_squared(end - nearest) < 1e-6f) {
                continue;
            }
            const Vec3 end_on_box = clamp_to_box(end, h);
            const float end_distance = length(end - end_on_box);
            if (end_distance <= 1e-6f) {
                continue;
            }
            const Vec3 end_normal = (end - end_on_box) / end_distance;
            const float end_separation = end_distance - radius;
            if (dot(end_normal, normal) > 0.95f && end_separation <= margin) {
                add_point(manifold, end_on_box + end_normal * (0.5f * end_separation),
                          end_separation);
            }
        }
    } else {
        // The segment passes through the box: the axis it overlaps least along pushes it out.
        Vec3 axes[6] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        int axis_count = 3;
        if (length_squared(q - p) > 1e-12f) {
            const Vec3 direction = normalized(q - p);
            for (int i = 0; i < 3; ++i) {
                const Vec3 across = cross(direction, axes[i]);
                if (length_squared(across) > 1e-8f) {
                    axes[axis_count++] = normalized(across);
                }
            }
        }
        float best = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < axis_count; ++i) {
            for (const float sign : {1.0f, -1.0f}) {
                const Vec3 n = axes[i] * sign;
                const float box_extent = dot(abs(n), h);
                const float separation = std::min(dot(p, n), dot(q, n)) - radius - box_extent;
                if (separation > best) {
                    best = separation;
                    normal = n;
                }
            }
        }
        const float box_extent = dot(abs(normal), h);
        const bool same = length_squared(q - p) < 1e-12f;
        for (const Vec3 end : {p, q}) {
            const float separation = dot(end, normal) - radius - box_extent;
            if (separation <= margin || manifold.count == 0) {
                add_point(manifold, end - normal * (radius + 0.5f * separation), separation);
            }
            if (same) {
                break;
            }
        }
    }

    manifold.normal = box.basis * normal;
    for (int i = 0; i < manifold.count; ++i) {
        manifold.points[i].position = box.to_world(manifold.points[i].position);
    }
}

// --- Box against box -----------------------------------------------------------------------------

// Keeps the part of a polygon where dot(p, normal) <= offset.
int clip(const Vec3* in, int count, Vec3 normal, float offset, Vec3* out) {
    int written = 0;
    for (int i = 0; i < count; ++i) {
        const Vec3 p = in[i];
        const Vec3 q = in[(i + 1) % count];
        const float dp = dot(p, normal) - offset;
        const float dq = dot(q, normal) - offset;
        if (dp <= 0.0f) {
            out[written++] = p;
        }
        if ((dp < 0.0f && dq > 0.0f) || (dp > 0.0f && dq < 0.0f)) {
            out[written++] = p + (q - p) * (dp / (dp - dq));
        }
    }
    return written;
}

// Picks up to four points that cover the contact area well: the deepest, the one farthest from
// it, and the two that make the largest triangles with those on either side.
void reduce(const Vec3* points, const float* separations, int count, Vec3 normal,
            Manifold& manifold) {
    if (count <= 4) {
        for (int i = 0; i < count; ++i) {
            add_point(manifold, points[i], separations[i]);
        }
        return;
    }
    int chosen[4] = {0, -1, -1, -1};
    for (int i = 1; i < count; ++i) {
        if (separations[i] < separations[chosen[0]]) {
            chosen[0] = i;
        }
    }
    const Vec3 first = points[chosen[0]];
    float farthest = -1.0f;
    for (int i = 0; i < count; ++i) {
        const float d = length_squared(points[i] - first);
        if (d > farthest) {
            farthest = d;
            chosen[1] = i;
        }
    }
    const Vec3 line = points[chosen[1]] - first;
    float most = 0.0f;
    float least = 0.0f;
    for (int i = 0; i < count; ++i) {
        const float area = dot(cross(line, points[i] - first), normal);
        if (area > most) {
            most = area;
            chosen[2] = i;
        }
        if (area < least) {
            least = area;
            chosen[3] = i;
        }
    }
    for (int i = 0; i < 4; ++i) {
        const int index = chosen[i];
        const bool repeated = std::find(chosen, chosen + i, index) != chosen + i;
        if (index >= 0 && !repeated) {
            add_point(manifold, points[index], separations[index]);
        }
    }
}

void box_box(Vec3 ha, const Transform& ta, Vec3 hb, const Transform& tb, float margin,
             Manifold& manifold) {
    const Vec3 a[3] = {ta.axis(0), ta.axis(1), ta.axis(2)};
    const Vec3 b[3] = {tb.axis(0), tb.axis(1), tb.axis(2)};
    const Vec3 d = tb.position - ta.position;

    float r[3][3];
    float abs_r[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r[i][j] = dot(a[i], b[j]);
            abs_r[i][j] = std::abs(r[i][j]) + 1e-6f;
        }
    }
    const Vec3 t{dot(d, a[0]), dot(d, a[1]), dot(d, a[2])};

    // Face axes. Indices 0 to 2 are a's faces, 3 to 5 b's.
    float face_separation = -std::numeric_limits<float>::infinity();
    int face = -1;
    for (int i = 0; i < 3; ++i) {
        const float extent_b = hb.x * abs_r[i][0] + hb.y * abs_r[i][1] + hb.z * abs_r[i][2];
        const float separation = std::abs(t[i]) - ha[i] - extent_b;
        if (separation > margin) {
            return;
        }
        if (separation > face_separation) {
            face_separation = separation;
            face = i;
        }
    }
    for (int j = 0; j < 3; ++j) {
        const float extent_a = ha.x * abs_r[0][j] + ha.y * abs_r[1][j] + ha.z * abs_r[2][j];
        const float along = t.x * r[0][j] + t.y * r[1][j] + t.z * r[2][j];
        const float separation = std::abs(along) - extent_a - hb[j];
        if (separation > margin) {
            return;
        }
        if (separation > face_separation + k_face_tolerance) {
            face_separation = separation;
            face = 3 + j;
        }
    }

    // Edge axes.
    float edge_separation = -std::numeric_limits<float>::infinity();
    int edge_a = -1;
    int edge_b = -1;
    Vec3 edge_normal;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const Vec3 axis = cross(a[i], b[j]);
            const float size = length(axis);
            if (size < 1e-5f) {
                continue;
            }
            const Vec3 n = axis / size;
            const float extent_a = ha.x * std::abs(dot(a[0], n)) + ha.y * std::abs(dot(a[1], n)) +
                                   ha.z * std::abs(dot(a[2], n));
            const float extent_b = hb.x * std::abs(dot(b[0], n)) + hb.y * std::abs(dot(b[1], n)) +
                                   hb.z * std::abs(dot(b[2], n));
            const float separation = std::abs(dot(d, n)) - extent_a - extent_b;
            if (separation > margin) {
                return;
            }
            if (separation > edge_separation) {
                edge_separation = separation;
                edge_a = i;
                edge_b = j;
                edge_normal = n;
            }
        }
    }

    if (edge_a >= 0 && edge_separation > face_separation + k_edge_tolerance) {
        // Two edges meet: one point, where they come closest.
        Vec3 n = edge_normal;
        if (dot(d, n) < 0.0f) {
            n = -n;
        }
        Vec3 on_a = ta.position;
        Vec3 on_b = tb.position;
        for (int k = 0; k < 3; ++k) {
            if (k != edge_a) {
                on_a += a[k] * (dot(n, a[k]) > 0.0f ? ha[k] : -ha[k]);
            }
            if (k != edge_b) {
                on_b += b[k] * (dot(n, b[k]) < 0.0f ? hb[k] : -hb[k]);
            }
        }
        const Segment segment_a{on_a - a[edge_a] * ha[edge_a], on_a + a[edge_a] * ha[edge_a]};
        const Segment segment_b{on_b - b[edge_b] * hb[edge_b], on_b + b[edge_b] * hb[edge_b]};
        float s = 0.0f;
        float u = 0.0f;
        closest_parameters(segment_a, segment_b, s, u);
        const Vec3 pa = point_at(segment_a, s);
        const Vec3 pb = point_at(segment_b, u);
        manifold.normal = n;
        add_point(manifold, 0.5f * (pa + pb), dot(pb - pa, n));
        return;
    }

    // A face of one box against the other: the other's most opposed face, clipped to the first.
    const bool reference_is_a = face < 3;
    const Transform& reference = reference_is_a ? ta : tb;
    const Transform& incident = reference_is_a ? tb : ta;
    const Vec3 hr = reference_is_a ? ha : hb;
    const Vec3 hi = reference_is_a ? hb : ha;
    const int axis = face % 3;

    Vec3 n = reference.axis(axis);
    if (dot(incident.position - reference.position, n) < 0.0f) {
        n = -n;
    }
    const Vec3 face_center = reference.position + n * hr[axis];
    const int ui = (axis + 1) % 3;
    const int vi = (axis + 2) % 3;
    const Vec3 u = reference.axis(ui);
    const Vec3 v = reference.axis(vi);

    int k = 0;
    float most_opposed = -1.0f;
    for (int i = 0; i < 3; ++i) {
        const float alignment = std::abs(dot(n, incident.axis(i)));
        if (alignment > most_opposed) {
            most_opposed = alignment;
            k = i;
        }
    }
    const float side = dot(n, incident.axis(k)) > 0.0f ? -1.0f : 1.0f;
    const Vec3 center = incident.position + incident.axis(k) * (side * hi[k]);
    const Vec3 e1 = incident.axis((k + 1) % 3) * hi[(k + 1) % 3];
    const Vec3 e2 = incident.axis((k + 2) % 3) * hi[(k + 2) % 3];

    Vec3 buffer_a[16] = {center + e1 + e2, center - e1 + e2, center - e1 - e2, center + e1 - e2};
    Vec3 buffer_b[16];
    int count = 4;
    count = clip(buffer_a, count, u, dot(face_center, u) + hr[ui], buffer_b);
    count = clip(buffer_b, count, -u, -dot(face_center, u) + hr[ui], buffer_a);
    count = clip(buffer_a, count, v, dot(face_center, v) + hr[vi], buffer_b);
    count = clip(buffer_b, count, -v, -dot(face_center, v) + hr[vi], buffer_a);

    Vec3 points[16];
    float separations[16];
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        const float separation = dot(buffer_a[i] - face_center, n);
        if (separation <= margin) {
            points[kept] = buffer_a[i] - n * (0.5f * separation);
            separations[kept] = separation;
            ++kept;
        }
    }
    manifold.normal = reference_is_a ? n : -n;
    reduce(points, separations, kept, n, manifold);
}

} // namespace

void collide(const Shape& a, const Transform& ta, const Shape& b, const Transform& tb, float margin,
             Manifold& manifold) {
    manifold.count = 0;
    const auto* const box_a = std::get_if<Box>(&a);
    const auto* const box_b = std::get_if<Box>(&b);
    if (box_a != nullptr && box_b != nullptr) {
        box_box(box_a->half_extents, ta, box_b->half_extents, tb, margin, manifold);
    } else if (box_a != nullptr) {
        rounded_box(core_segment(b, tb), core_radius(b), box_a->half_extents, ta, margin, manifold);
    } else if (box_b != nullptr) {
        rounded_box(core_segment(a, ta), core_radius(a), box_b->half_extents, tb, margin, manifold);
        manifold.normal = -manifold.normal;
    } else {
        rounded_pair(core_segment(a, ta), core_radius(a), core_segment(b, tb), core_radius(b),
                     margin, manifold);
    }
}

} // namespace impulse::detail
