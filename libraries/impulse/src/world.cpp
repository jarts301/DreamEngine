#include "impulse/world.h"

#include "collide.h"
#include "geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_map>
#include <utility>

namespace impulse {

using namespace detail;

namespace {

// Resting contacts are left overlapping by this much, so they stay in contact from step to step.
constexpr float k_linear_slop = 0.005f;

// Contacts are found this far apart, so the solver can stop bodies where they would meet.
constexpr float k_speculative_margin = 0.02f;

// The share of an overlap or joint error corrected each position iteration, and the most moved.
constexpr float k_baumgarte = 0.2f;
constexpr float k_max_linear_correction = 0.2f;
constexpr float k_max_angular_correction = 8.0f * std::numbers::pi_v<float> / 180.0f;

// Contacts closing faster than this bounce by their restitution.
constexpr float k_restitution_threshold = 1.0f;

// Contact points within this distance of last step's are the same point, keeping their impulses.
constexpr float k_same_point_squared = 0.05f * 0.05f;

std::uint64_t pair_key(std::uint32_t a, std::uint32_t b) {
    return a < b ? (std::uint64_t{a} << 32) | b : (std::uint64_t{b} << 32) | a;
}

// Rotates `q` by the small rotation vector `r`.
Quat rotated(Quat q, Vec3 r) {
    return normalized(from_rotation_vector(r) * q);
}

struct Body {
    std::uint32_t generation = 1;
    bool alive = false;

    Shape shape;
    Motion motion = Motion::Dynamic;
    bool sensor = false;
    bool continuous = false;
    std::uint64_t user_data = 0;

    Vec3 position;
    Quat rotation;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    Vec3 force;

    float inverse_mass = 0.0f;
    Vec3 inverse_inertia_local;
    Mat3 inverse_inertia = Mat3::zero();

    float friction = 0.5f;
    float restitution = 0.0f;
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    float gravity_scale = 1.0f;

    bool awake = false;
    float sleep_time = 0.0f;
    std::optional<std::pair<Vec3, Quat>> kinematic_target;

    // Whether the body moved in the last step, for kinematic bodies that keep others awake.
    bool moved = false;
    Aabb bounds;

    [[nodiscard]] Transform transform() const {
        return Transform::make(position, rotation);
    }
    [[nodiscard]] bool moves() const {
        return motion != Motion::Static;
    }
    [[nodiscard]] bool is_dynamic() const {
        return motion == Motion::Dynamic;
    }
    // Awake and able to move: what makes a pair worth looking at.
    [[nodiscard]] bool active() const {
        return awake && moves();
    }
    void update_inertia() {
        if (!is_dynamic()) {
            inverse_inertia = Mat3::zero();
            return;
        }
        const Mat3 r = rotation_matrix(rotation);
        inverse_inertia = r * Mat3::diagonal(inverse_inertia_local) * transpose(r);
    }
    [[nodiscard]] Vec3 velocity_at(Vec3 offset) const {
        return linear_velocity + cross(angular_velocity, offset);
    }
};

struct ContactPoint {
    // The point in each body's frame, for matching with last step's points and for measuring how
    // the overlap changes as the bodies move.
    Vec3 local_a;
    Vec3 local_b;
    float separation = 0.0f;
    float normal_impulse = 0.0f;
    float tangent_impulse[2] = {0.0f, 0.0f};

    // Solver data for this step.
    Vec3 offset_a;
    Vec3 offset_b;
    float normal_mass = 0.0f;
    float tangent_mass[2] = {0.0f, 0.0f};
    float approach_speed = 0.0f;
    float most_impulse = 0.0f;
};

struct Pair {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    Vec3 normal;
    ContactPoint points[4];
    int count = 0;
    bool touching = false;
    bool sensor = false;
    bool updated = false;
    float friction = 0.0f;
    float restitution = 0.0f;
    Vec3 tangents[2];
};

enum class JointKind : std::uint8_t { Fixed, Point, Hinge, Distance };

struct Joint {
    std::uint32_t generation = 1;
    bool alive = false;
    JointKind kind = JointKind::Point;
    std::uint32_t a = 0;
    std::optional<std::uint32_t> b;

    // Anchors in each body's frame; for the world, in world space. Likewise the hinge axis.
    Vec3 anchor_a;
    Vec3 anchor_b;
    Vec3 axis_a;
    Vec3 axis_b;

    // Body b's rotation relative to body a when created (fixed joints), or a's relative to b
    // (hinges).
    Quat reference;

    std::optional<float> lower;
    std::optional<float> upper;

    // Accumulated impulses, applied at the start of the next step.
    Vec3 point_impulse;
    Vec3 angular_impulse;
    float lower_impulse = 0.0f;
    float upper_impulse = 0.0f;
};

// What the solver needs about a joint this step.
struct JointFrame {
    Joint* joint = nullptr;
    Body* a = nullptr;
    Body* b = nullptr;
    Vec3 offset_a;
    Vec3 offset_b;
    Vec3 point_a;
    Vec3 point_b;
    Mat3 point_mass;
    Vec3 axis;
    Vec3 tangent[2];
    float angle = 0.0f;
    float axial_mass = 0.0f;
    Vec3 direction;
    float distance = 0.0f;
};

// Union-find over bodies, for sleeping whole groups of touching bodies at once.
struct Islands {
    std::vector<std::uint32_t> parent;

    std::uint32_t find(std::uint32_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }
    void join(std::uint32_t a, std::uint32_t b) {
        parent[find(a)] = find(b);
    }
};

} // namespace

struct World::Impl {
    Settings settings;
    std::vector<Body> bodies;
    std::vector<std::uint32_t> free_bodies;
    std::size_t body_count = 0;
    std::vector<Joint> joints;
    std::vector<std::uint32_t> free_joints;
    std::unordered_map<std::uint64_t, Pair> pairs;
    std::unordered_map<std::uint64_t, int> joined;
    std::vector<ContactEvent> events;
    std::vector<ContactEvent> pending;

    Body* find(BodyId id) {
        if (id.index >= bodies.size() || !bodies[id.index].alive ||
            bodies[id.index].generation != id.generation) {
            return nullptr;
        }
        return &bodies[id.index];
    }
    const Body* find(BodyId id) const {
        return const_cast<Impl*>(this)->find(id);
    }
    BodyId id_of(std::uint32_t index) const {
        return BodyId{index, bodies[index].generation};
    }

    void wake(Body& body) {
        if (body.moves()) {
            body.awake = true;
            body.sleep_time = 0.0f;
        }
    }

    // Wakes everything touching a body, such as a stack when a box is pulled out of it.
    void wake_touching(std::uint32_t index) {
        for (auto& [key, pair] : pairs) {
            if (pair.a == index || pair.b == index) {
                wake(bodies[pair.a]);
                wake(bodies[pair.b]);
            }
        }
    }

    ContactEvent event(const Pair& pair, ContactEvent::Kind kind) const {
        ContactEvent result{
            .kind = kind,
            .a = id_of(pair.a),
            .b = id_of(pair.b),
            .user_data_a = bodies[pair.a].user_data,
            .user_data_b = bodies[pair.b].user_data,
        };
        if (kind == ContactEvent::Kind::Began && pair.count > 0) {
            const ContactPoint& first = pair.points[0];
            result.point = bodies[pair.a].transform().to_world(first.local_a);
            result.normal = pair.normal;
        }
        return result;
    }

    // --- Collision detection ---

    void find_contacts(float dt) {
        for (Body& body : bodies) {
            if (!body.alive) {
                continue;
            }
            body.bounds = bounds(body.shape, body.transform());
            float grow = k_speculative_margin;
            if (body.continuous && body.active()) {
                grow += length(body.linear_velocity) * dt;
            }
            const Vec3 margin{grow, grow, grow};
            body.bounds.min -= margin;
            body.bounds.max += margin;
        }

        // Sort and sweep along x.
        std::vector<std::uint32_t> order;
        order.reserve(body_count);
        for (std::uint32_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].alive) {
                order.push_back(i);
            }
        }
        std::sort(order.begin(), order.end(), [&](std::uint32_t l, std::uint32_t r) {
            return bodies[l].bounds.min.x < bodies[r].bounds.min.x;
        });

        for (auto& [key, pair] : pairs) {
            pair.updated = false;
        }
        for (std::size_t i = 0; i < order.size(); ++i) {
            const Body& first = bodies[order[i]];
            for (std::size_t j = i + 1; j < order.size(); ++j) {
                const Body& second = bodies[order[j]];
                if (second.bounds.min.x > first.bounds.max.x) {
                    break;
                }
                if (worth_testing(first, second) && first.bounds.overlaps(second.bounds) &&
                    !joined.contains(pair_key(order[i], order[j]))) {
                    update_pair(std::min(order[i], order[j]), std::max(order[i], order[j]), dt);
                }
            }
        }

        // Pairs not looked at this step are kept if both bodies rest, and otherwise have drifted
        // apart.
        for (auto it = pairs.begin(); it != pairs.end();) {
            Pair& pair = it->second;
            const bool resting = !bodies[pair.a].active() && !bodies[pair.b].active();
            if (!pair.updated && !resting) {
                if (pair.touching) {
                    events.push_back(event(pair, ContactEvent::Kind::Ended));
                }
                it = pairs.erase(it);
            } else {
                ++it;
            }
        }
    }

    static bool worth_testing(const Body& a, const Body& b) {
        if (!a.active() && !b.active()) {
            return false;
        }
        // Only dynamic bodies respond to contacts, but sensors notice anything that moves.
        return a.is_dynamic() || b.is_dynamic() || a.sensor || b.sensor;
    }

    void update_pair(std::uint32_t ia, std::uint32_t ib, float dt) {
        Body& a = bodies[ia];
        Body& b = bodies[ib];
        const bool sensor = a.sensor || b.sensor;
        float margin = 0.0f;
        if (!sensor) {
            margin = k_speculative_margin;
            if (a.continuous || b.continuous) {
                margin += length(b.linear_velocity - a.linear_velocity) * dt;
            }
        }

        const Transform ta = a.transform();
        const Transform tb = b.transform();
        Manifold manifold;
        collide(a.shape, ta, b.shape, tb, margin, manifold);

        const std::uint64_t key = pair_key(ia, ib);
        auto found = pairs.find(key);
        if (manifold.count == 0) {
            if (found != pairs.end()) {
                // Leaves the pair in place, not updated, so the cleanup ends it.
                found->second.count = 0;
            }
            return;
        }
        if (found == pairs.end()) {
            found = pairs.emplace(key, Pair{.a = ia, .b = ib}).first;
        }
        Pair& pair = found->second;
        pair.updated = true;
        pair.sensor = sensor;
        pair.normal = manifold.normal;
        pair.friction = std::sqrt(a.friction * b.friction);
        pair.restitution = std::max(a.restitution, b.restitution);

        // Carries impulses over from matching points of the last step.
        ContactPoint points[4];
        float deepest = std::numeric_limits<float>::infinity();
        for (int i = 0; i < manifold.count; ++i) {
            const ManifoldPoint& source = manifold.points[i];
            ContactPoint& point = points[i];
            point.local_a = ta.to_local(source.position);
            point.local_b = tb.to_local(source.position);
            point.separation = source.separation;
            deepest = std::min(deepest, source.separation);
            for (int j = 0; j < pair.count; ++j) {
                if (length_squared(pair.points[j].local_a - point.local_a) < k_same_point_squared) {
                    point.normal_impulse = pair.points[j].normal_impulse;
                    point.tangent_impulse[0] = pair.points[j].tangent_impulse[0];
                    point.tangent_impulse[1] = pair.points[j].tangent_impulse[1];
                    break;
                }
            }
        }
        std::copy(points, points + manifold.count, pair.points);
        pair.count = manifold.count;

        const bool touching = sensor ? deepest <= 0.0f : deepest <= k_linear_slop;
        if (touching != pair.touching) {
            pair.touching = touching;
            events.push_back(
                event(pair, touching ? ContactEvent::Kind::Began : ContactEvent::Kind::Ended));
        }
        // Something moving into a sleeping body wakes it.
        if (!sensor && deepest <= k_linear_slop) {
            if (a.active() && !b.awake && b.is_dynamic()) {
                wake(b);
            } else if (b.active() && !a.awake && a.is_dynamic()) {
                wake(a);
            }
        }
    }

    // --- Solving ---

    std::vector<Pair*> contacts_to_solve() {
        std::vector<std::pair<std::uint64_t, Pair*>> sorted;
        for (auto& [key, pair] : pairs) {
            const Body& a = bodies[pair.a];
            const Body& b = bodies[pair.b];
            if (!pair.sensor && pair.count > 0 && pair.updated &&
                ((a.is_dynamic() && a.awake) || (b.is_dynamic() && b.awake))) {
                sorted.emplace_back(key, &pair);
            }
        }
        // A fixed order makes the simulation repeat exactly.
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& l, const auto& r) { return l.first < r.first; });
        std::vector<Pair*> result;
        result.reserve(sorted.size());
        for (const auto& entry : sorted) {
            result.push_back(entry.second);
        }
        return result;
    }

    void prepare_contacts(const std::vector<Pair*>& contacts) {
        for (Pair* const pair : contacts) {
            Body& a = bodies[pair->a];
            Body& b = bodies[pair->b];
            tangents(pair->normal, pair->tangents[0], pair->tangents[1]);
            for (int i = 0; i < pair->count; ++i) {
                ContactPoint& point = pair->points[i];
                const Vec3 world = a.transform().to_world(point.local_a);
                point.offset_a = world - a.position;
                point.offset_b = world - b.position;
                point.normal_mass = 1.0f / effective_mass(a, b, point, pair->normal);
                for (int t = 0; t < 2; ++t) {
                    point.tangent_mass[t] = 1.0f / effective_mass(a, b, point, pair->tangents[t]);
                }
                point.approach_speed = dot(
                    b.velocity_at(point.offset_b) - a.velocity_at(point.offset_a), pair->normal);
                point.most_impulse = 0.0f;
            }
        }
    }

    static float effective_mass(const Body& a, const Body& b, const ContactPoint& point,
                                Vec3 direction) {
        const Vec3 ra = cross(point.offset_a, direction);
        const Vec3 rb = cross(point.offset_b, direction);
        const float mass = a.inverse_mass + b.inverse_mass + dot(ra, a.inverse_inertia * ra) +
                           dot(rb, b.inverse_inertia * rb);
        return mass > 0.0f ? mass : 1.0f;
    }

    static void apply(Body& a, Body& b, Vec3 offset_a, Vec3 offset_b, Vec3 impulse) {
        a.linear_velocity -= impulse * a.inverse_mass;
        a.angular_velocity -= a.inverse_inertia * cross(offset_a, impulse);
        b.linear_velocity += impulse * b.inverse_mass;
        b.angular_velocity += b.inverse_inertia * cross(offset_b, impulse);
    }

    void warm_start_contacts(const std::vector<Pair*>& contacts) {
        for (Pair* const pair : contacts) {
            Body& a = bodies[pair->a];
            Body& b = bodies[pair->b];
            for (int i = 0; i < pair->count; ++i) {
                const ContactPoint& point = pair->points[i];
                const Vec3 impulse = pair->normal * point.normal_impulse +
                                     pair->tangents[0] * point.tangent_impulse[0] +
                                     pair->tangents[1] * point.tangent_impulse[1];
                apply(a, b, point.offset_a, point.offset_b, impulse);
            }
        }
    }

    template <typename Pairs> void solve_contacts(const Pairs& contacts, float dt) {
        for (Pair* const pair : contacts) {
            Body& a = bodies[pair->a];
            Body& b = bodies[pair->b];
            for (int i = 0; i < pair->count; ++i) {
                ContactPoint& point = pair->points[i];

                // Friction, limited by the normal impulse.
                const float limit = pair->friction * point.normal_impulse;
                for (int t = 0; t < 2; ++t) {
                    const Vec3 direction = pair->tangents[t];
                    const float speed = dot(
                        b.velocity_at(point.offset_b) - a.velocity_at(point.offset_a), direction);
                    const float previous = point.tangent_impulse[t];
                    point.tangent_impulse[t] =
                        std::clamp(previous - speed * point.tangent_mass[t], -limit, limit);
                    apply(a, b, point.offset_a, point.offset_b,
                          direction * (point.tangent_impulse[t] - previous));
                }

                // No approach, or for points still apart, no more than closes the gap this step.
                // Gaps within the slop count as touching: letting those close while neighboring
                // points push back sets stacked boxes rocking.
                const float speed = dot(
                    b.velocity_at(point.offset_b) - a.velocity_at(point.offset_a), pair->normal);
                const float allowed =
                    point.separation > k_linear_slop ? point.separation / dt : 0.0f;
                const float previous = point.normal_impulse;
                point.normal_impulse =
                    std::max(previous - (speed + allowed) * point.normal_mass, 0.0f);
                point.most_impulse = std::max(point.most_impulse, point.normal_impulse);
                apply(a, b, point.offset_a, point.offset_b,
                      pair->normal * (point.normal_impulse - previous));
            }
        }
    }

    void apply_restitution(const std::vector<Pair*>& contacts) {
        for (Pair* const pair : contacts) {
            if (pair->restitution == 0.0f) {
                continue;
            }
            Body& a = bodies[pair->a];
            Body& b = bodies[pair->b];
            for (int i = 0; i < pair->count; ++i) {
                ContactPoint& point = pair->points[i];
                if (point.approach_speed > -k_restitution_threshold || point.most_impulse <= 0.0f) {
                    continue;
                }
                const float speed = dot(
                    b.velocity_at(point.offset_b) - a.velocity_at(point.offset_a), pair->normal);
                const float previous = point.normal_impulse;
                point.normal_impulse =
                    std::max(previous - (speed + pair->restitution * point.approach_speed) *
                                            point.normal_mass,
                             0.0f);
                apply(a, b, point.offset_a, point.offset_b,
                      pair->normal * (point.normal_impulse - previous));
            }
        }
    }

    static void push(Body& body, Vec3 offset, Vec3 impulse, float sign) {
        body.position += impulse * (sign * body.inverse_mass);
        body.rotation =
            rotated(body.rotation, body.inverse_inertia * cross(offset, impulse * sign));
    }

    void correct_contacts(const std::vector<Pair*>& contacts) {
        for (Pair* const pair : contacts) {
            Body& a = bodies[pair->a];
            Body& b = bodies[pair->b];
            for (int i = 0; i < pair->count; ++i) {
                const ContactPoint& point = pair->points[i];
                const Vec3 offset_a = rotate(a.rotation, point.local_a);
                const Vec3 offset_b = rotate(b.rotation, point.local_b);
                const float separation =
                    point.separation +
                    dot((b.position + offset_b) - (a.position + offset_a), pair->normal);
                const float correction = std::clamp(k_baumgarte * (separation + k_linear_slop),
                                                    -k_max_linear_correction, 0.0f);
                if (correction >= 0.0f) {
                    continue;
                }
                const Vec3 ra = cross(offset_a, pair->normal);
                const Vec3 rb = cross(offset_b, pair->normal);
                const float mass = a.inverse_mass + b.inverse_mass +
                                   dot(ra, a.inverse_inertia * ra) +
                                   dot(rb, b.inverse_inertia * rb);
                if (mass <= 0.0f) {
                    continue;
                }
                const Vec3 impulse = pair->normal * (-correction / mass);
                push(a, offset_a, impulse, -1.0f);
                push(b, offset_b, impulse, 1.0f);
            }
        }
    }

    // --- Joints ---

    static Mat3 point_mass(const Body& a, const Body* b, Vec3 offset_a, Vec3 offset_b) {
        const float masses = a.inverse_mass + (b != nullptr ? b->inverse_mass : 0.0f);
        Mat3 k = Mat3::diagonal(Vec3{masses, masses, masses});
        const Mat3 sa = skew(offset_a);
        const Mat3 ta = sa * a.inverse_inertia * sa;
        for (int r = 0; r < 3; ++r) {
            k.rows[r] -= ta.rows[r];
        }
        if (b != nullptr) {
            const Mat3 sb = skew(offset_b);
            const Mat3 tb = sb * b->inverse_inertia * sb;
            for (int r = 0; r < 3; ++r) {
                k.rows[r] -= tb.rows[r];
            }
        }
        return k;
    }

    static Mat3 angular_mass(const Body& a, const Body* b) {
        return b != nullptr ? a.inverse_inertia + b->inverse_inertia : a.inverse_inertia;
    }

    // The hinge angle: body a's rotation relative to body b around the axis, since creation.
    static float hinge_angle(const Joint& joint, const Body& a, const Body* b) {
        const Quat b_rotation = b != nullptr ? b->rotation : Quat{};
        Quat change = conjugate(b_rotation) * a.rotation * conjugate(joint.reference);
        if (change.w < 0.0f) {
            change = Quat{-change.x, -change.y, -change.z, -change.w};
        }
        return 2.0f * std::atan2(dot(Vec3{change.x, change.y, change.z}, joint.axis_b), change.w);
    }

    JointFrame frame(Joint& joint) {
        JointFrame f;
        f.joint = &joint;
        f.a = &bodies[joint.a];
        f.b = joint.b.has_value() ? &bodies[*joint.b] : nullptr;
        const Body& a = *f.a;
        f.offset_a = rotate(a.rotation, joint.anchor_a);
        f.point_a = a.position + f.offset_a;
        if (f.b != nullptr) {
            f.offset_b = rotate(f.b->rotation, joint.anchor_b);
            f.point_b = f.b->position + f.offset_b;
        } else {
            f.point_b = joint.anchor_b;
        }
        if (joint.kind == JointKind::Fixed || joint.kind == JointKind::Point ||
            joint.kind == JointKind::Hinge) {
            f.point_mass = point_mass(a, f.b, f.offset_a, f.offset_b);
        }
        if (joint.kind == JointKind::Hinge) {
            f.axis = f.b != nullptr ? rotate(f.b->rotation, joint.axis_b) : joint.axis_b;
            tangents(normalized(rotate(a.rotation, joint.axis_a)), f.tangent[0], f.tangent[1]);
            f.angle = hinge_angle(joint, a, f.b);
            const float mass = dot(f.axis, angular_mass(a, f.b) * f.axis);
            f.axial_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
        }
        if (joint.kind == JointKind::Distance) {
            const Vec3 between = f.point_b - f.point_a;
            f.distance = length(between);
            f.direction = f.distance > 1e-6f ? between / f.distance : Vec3{0.0f, 1.0f, 0.0f};
            const Vec3 ra = cross(f.offset_a, f.direction);
            float mass = a.inverse_mass + dot(ra, a.inverse_inertia * ra);
            if (f.b != nullptr) {
                const Vec3 rb = cross(f.offset_b, f.direction);
                mass += f.b->inverse_mass + dot(rb, f.b->inverse_inertia * rb);
            }
            f.axial_mass = mass > 0.0f ? 1.0f / mass : 0.0f;
        }
        return f;
    }

    static void apply_linear(JointFrame& f, Vec3 impulse) {
        f.a->linear_velocity -= impulse * f.a->inverse_mass;
        f.a->angular_velocity -= f.a->inverse_inertia * cross(f.offset_a, impulse);
        if (f.b != nullptr) {
            f.b->linear_velocity += impulse * f.b->inverse_mass;
            f.b->angular_velocity += f.b->inverse_inertia * cross(f.offset_b, impulse);
        }
    }

    static void apply_angular(JointFrame& f, Vec3 impulse) {
        f.a->angular_velocity -= f.a->inverse_inertia * impulse;
        if (f.b != nullptr) {
            f.b->angular_velocity += f.b->inverse_inertia * impulse;
        }
    }

    static Vec3 relative_point_velocity(const JointFrame& f) {
        const Vec3 vb = f.b != nullptr ? f.b->velocity_at(f.offset_b) : Vec3{};
        return vb - f.a->velocity_at(f.offset_a);
    }

    static Vec3 relative_angular_velocity(const JointFrame& f) {
        const Vec3 wb = f.b != nullptr ? f.b->angular_velocity : Vec3{};
        return wb - f.a->angular_velocity;
    }

    static void warm_start_joint(JointFrame& f) {
        Joint& joint = *f.joint;
        apply_linear(f, joint.point_impulse);
        apply_angular(f, joint.angular_impulse);
        if (joint.kind == JointKind::Hinge) {
            apply_angular(f, f.axis * (joint.lower_impulse + joint.upper_impulse));
        } else if (joint.kind == JointKind::Distance) {
            apply_linear(f, f.direction * (joint.lower_impulse + joint.upper_impulse));
        }
    }

    static void solve_joint(JointFrame& f, float dt) {
        Joint& joint = *f.joint;
        switch (joint.kind) {
        case JointKind::Fixed: {
            const Vec3 angular = solve(angular_mass(*f.a, f.b), -relative_angular_velocity(f));
            joint.angular_impulse += angular;
            apply_angular(f, angular);
            break;
        }
        case JointKind::Hinge: {
            // Only turning around the axis is allowed.
            const Vec3 spin = relative_angular_velocity(f);
            const Mat3 mass = angular_mass(*f.a, f.b);
            const Vec3 t0 = f.tangent[0];
            const Vec3 t1 = f.tangent[1];
            const float k00 = dot(t0, mass * t0);
            const float k01 = dot(t0, mass * t1);
            const float k11 = dot(t1, mass * t1);
            const float determinant = k00 * k11 - k01 * k01;
            if (std::abs(determinant) > 1e-12f) {
                const float c0 = -dot(spin, t0);
                const float c1 = -dot(spin, t1);
                const float l0 = (k11 * c0 - k01 * c1) / determinant;
                const float l1 = (k00 * c1 - k01 * c0) / determinant;
                const Vec3 angular = t0 * l0 + t1 * l1;
                joint.angular_impulse += angular;
                apply_angular(f, angular);
            }
            // Limits stop the turn where it would pass them during this step.
            const float axial = dot(relative_angular_velocity(f), f.axis);
            if (joint.upper.has_value()) {
                const float room = std::max(*joint.upper - f.angle, 0.0f) / dt;
                const float previous = joint.upper_impulse;
                joint.upper_impulse = std::max(previous - (axial + room) * f.axial_mass, 0.0f);
                apply_angular(f, f.axis * (joint.upper_impulse - previous));
            }
            if (joint.lower.has_value()) {
                const float axial_now = dot(relative_angular_velocity(f), f.axis);
                const float room = std::max(f.angle - *joint.lower, 0.0f) / dt;
                const float previous = joint.lower_impulse;
                joint.lower_impulse = std::min(previous - (axial_now - room) * f.axial_mass, 0.0f);
                apply_angular(f, f.axis * (joint.lower_impulse - previous));
            }
            break;
        }
        case JointKind::Distance: {
            const float speed = dot(relative_point_velocity(f), f.direction);
            const float stretch_room = (joint.upper.value_or(0.0f) - f.distance) / dt;
            // The upper limit pulls, with a negative impulse along the direction from a to b.
            {
                const float room = std::max(stretch_room, 0.0f);
                const float previous = joint.upper_impulse;
                joint.upper_impulse = std::min(previous - (speed - room) * f.axial_mass, 0.0f);
                apply_linear(f, f.direction * (joint.upper_impulse - previous));
            }
            if (joint.lower.value_or(0.0f) > 0.0f) {
                const float speed_now = dot(relative_point_velocity(f), f.direction);
                const float room = std::max(f.distance - *joint.lower, 0.0f) / dt;
                const float previous = joint.lower_impulse;
                joint.lower_impulse = std::max(previous - (speed_now + room) * f.axial_mass, 0.0f);
                apply_linear(f, f.direction * (joint.lower_impulse - previous));
            }
            return;
        }
        case JointKind::Point:
            break;
        }
        // The anchors stay together.
        const Vec3 impulse = solve(f.point_mass, -relative_point_velocity(f));
        joint.point_impulse += impulse;
        apply_linear(f, impulse);
    }

    // Moves the bodies a share of the way to where the joint wants them.
    void correct_joint(Joint& joint) {
        JointFrame f = frame(joint);
        Body& a = *f.a;
        Body* const b = f.b;

        const auto push_angular = [&](Vec3 impulse) {
            a.rotation = rotated(a.rotation, -(a.inverse_inertia * impulse));
            if (b != nullptr) {
                b->rotation = rotated(b->rotation, b->inverse_inertia * impulse);
            }
        };
        const auto push_linear = [&](Vec3 impulse, Vec3 offset_a, Vec3 offset_b) {
            push(a, offset_a, impulse, -1.0f);
            if (b != nullptr) {
                push(*b, offset_b, impulse, 1.0f);
            }
        };

        switch (joint.kind) {
        case JointKind::Fixed: {
            const Quat target = (b != nullptr ? b->rotation : Quat{}) * conjugate(joint.reference);
            Vec3 error = to_rotation_vector(target * conjugate(a.rotation));
            const float size = length(error);
            if (size > k_max_angular_correction) {
                error *= k_max_angular_correction / size;
            }
            // Turns a toward its target, and b the other way, in proportion to how easily each
            // turns.
            push_angular(-solve(angular_mass(a, b), error));
            break;
        }
        case JointKind::Hinge: {
            const Vec3 axis_a = rotate(a.rotation, joint.axis_a);
            const Vec3 error = cross(axis_a, f.axis);
            const Mat3 mass = angular_mass(a, b);
            const Vec3 t0 = f.tangent[0];
            const Vec3 t1 = f.tangent[1];
            const float k00 = dot(t0, mass * t0);
            const float k01 = dot(t0, mass * t1);
            const float k11 = dot(t1, mass * t1);
            const float determinant = k00 * k11 - k01 * k01;
            if (std::abs(determinant) > 1e-12f) {
                const float c0 = -dot(error, t0);
                const float c1 = -dot(error, t1);
                const float l0 = (k11 * c0 - k01 * c1) / determinant;
                const float l1 = (k00 * c1 - k01 * c0) / determinant;
                push_angular(t0 * l0 + t1 * l1);
            }
            float over = 0.0f;
            if (joint.upper.has_value() && f.angle > *joint.upper) {
                over = f.angle - *joint.upper;
            } else if (joint.lower.has_value() && f.angle < *joint.lower) {
                over = f.angle - *joint.lower;
            }
            if (over != 0.0f && f.axial_mass > 0.0f) {
                const float step = std::clamp(k_baumgarte * over, -k_max_angular_correction,
                                              k_max_angular_correction);
                push_angular(f.axis * (step * f.axial_mass));
            }
            break;
        }
        case JointKind::Distance: {
            float over = 0.0f;
            if (joint.upper.has_value() && f.distance > *joint.upper) {
                over = f.distance - *joint.upper;
            } else if (joint.lower.has_value() && f.distance < *joint.lower) {
                over = f.distance - *joint.lower;
            }
            if (over != 0.0f && f.axial_mass > 0.0f) {
                const float step = std::clamp(k_baumgarte * over, -k_max_linear_correction,
                                              k_max_linear_correction);
                push_linear(f.direction * (-step * f.axial_mass), f.offset_a, f.offset_b);
            }
            return;
        }
        case JointKind::Point:
            break;
        }

        // Refreshes the frame for the rotations just made.
        f = frame(joint);
        Vec3 error = f.point_b - f.point_a;
        const float size = length(error);
        if (size > k_max_linear_correction) {
            error *= k_max_linear_correction / size;
        }
        push_linear(solve(f.point_mass, -error), f.offset_a, f.offset_b);
    }

    // --- Sleeping ---

    void update_sleep(float dt) {
        const float linear_limit = settings.sleep_linear_speed * settings.sleep_linear_speed;
        const float angular_limit = settings.sleep_angular_speed * settings.sleep_angular_speed;
        Islands islands;
        islands.parent.resize(bodies.size());
        for (std::uint32_t i = 0; i < bodies.size(); ++i) {
            islands.parent[i] = i;
            Body& body = bodies[i];
            if (!body.alive || !body.active()) {
                continue;
            }
            const bool still = body.motion == Motion::Kinematic
                                   ? !body.moved
                                   : length_squared(body.linear_velocity) < linear_limit &&
                                         length_squared(body.angular_velocity) < angular_limit;
            body.sleep_time = still ? body.sleep_time + dt : 0.0f;
        }
        for (const auto& [key, pair] : pairs) {
            const Body& a = bodies[pair.a];
            const Body& b = bodies[pair.b];
            if (!pair.sensor && pair.touching && a.is_dynamic() && b.is_dynamic()) {
                islands.join(pair.a, pair.b);
            }
            // Something moving keeps what it touches awake.
            if (!pair.sensor && pair.touching) {
                if (a.motion == Motion::Kinematic && a.moved) {
                    bodies[pair.b].sleep_time = 0.0f;
                }
                if (b.motion == Motion::Kinematic && b.moved) {
                    bodies[pair.a].sleep_time = 0.0f;
                }
            }
        }
        for (const Joint& joint : joints) {
            if (joint.alive && joint.b.has_value() && bodies[joint.a].is_dynamic() &&
                bodies[*joint.b].is_dynamic()) {
                islands.join(joint.a, *joint.b);
            }
        }

        std::vector<float> least(bodies.size(), std::numeric_limits<float>::infinity());
        for (std::uint32_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].alive && bodies[i].active()) {
                const std::uint32_t root = islands.find(i);
                least[root] = std::min(least[root], bodies[i].sleep_time);
            }
        }
        for (std::uint32_t i = 0; i < bodies.size(); ++i) {
            Body& body = bodies[i];
            if (body.alive && body.active() && least[islands.find(i)] >= settings.sleep_seconds) {
                body.awake = false;
                body.linear_velocity = Vec3{};
                body.angular_velocity = Vec3{};
            }
        }
    }

    // --- The step ---

    void step(float dt) {
        events.swap(pending);
        pending.clear();

        for (Body& body : bodies) {
            if (!body.alive || !body.is_dynamic() || !body.awake) {
                body.force = Vec3{};
                continue;
            }
            body.linear_velocity +=
                (settings.gravity * body.gravity_scale + body.force * body.inverse_mass) * dt;
            body.linear_velocity *= 1.0f / (1.0f + dt * body.linear_damping);
            body.angular_velocity *= 1.0f / (1.0f + dt * body.angular_damping);
            body.force = Vec3{};
        }
        for (Body& body : bodies) {
            if (body.alive) {
                body.update_inertia();
            }
        }

        find_contacts(dt);

        const std::vector<Pair*> contacts = contacts_to_solve();
        std::vector<JointFrame> frames;
        for (Joint& joint : joints) {
            if (!joint.alive) {
                continue;
            }
            const Body& a = bodies[joint.a];
            const Body* const b = joint.b.has_value() ? &bodies[*joint.b] : nullptr;
            if ((a.is_dynamic() && a.awake) || (b != nullptr && b->is_dynamic() && b->awake)) {
                frames.push_back(frame(joint));
            }
        }

        prepare_contacts(contacts);
        for (JointFrame& f : frames) {
            warm_start_joint(f);
        }
        warm_start_contacts(contacts);
        // Alternating the order contacts are solved in keeps any one order from biasing the
        // result, which lets stacks settle in far fewer iterations.
        const std::vector<Pair*> reversed(contacts.rbegin(), contacts.rend());
        for (int i = 0; i < settings.velocity_iterations; ++i) {
            for (JointFrame& f : frames) {
                solve_joint(f, dt);
            }
            solve_contacts(i % 2 == 0 ? contacts : reversed, dt);
        }
        apply_restitution(contacts);

        for (Body& body : bodies) {
            body.moved = false;
            if (!body.alive || !body.active()) {
                continue;
            }
            body.moved = length_squared(body.linear_velocity) > 0.0f ||
                         length_squared(body.angular_velocity) > 0.0f;
            body.position += body.linear_velocity * dt;
            body.rotation = rotated(body.rotation, body.angular_velocity * dt);
            if (body.kinematic_target.has_value()) {
                body.position = body.kinematic_target->first;
                body.rotation = normalized(body.kinematic_target->second);
            }
        }

        for (int i = 0; i < settings.position_iterations; ++i) {
            for (const JointFrame& f : frames) {
                correct_joint(*f.joint);
            }
            correct_contacts(contacts);
        }

        for (Body& body : bodies) {
            if (body.alive && body.kinematic_target.has_value()) {
                body.kinematic_target.reset();
                body.linear_velocity = Vec3{};
                body.angular_velocity = Vec3{};
            }
        }
        update_sleep(dt);
    }
};

// --- The public interface ------------------------------------------------------------------------

World::World(const Settings& settings) : m_impl(std::make_unique<Impl>()) {
    m_impl->settings = settings;
}

World::~World() = default;

void World::set_gravity(Vec3 gravity) {
    m_impl->settings.gravity = gravity;
    for (Body& body : m_impl->bodies) {
        if (body.alive) {
            m_impl->wake(body);
        }
    }
}

Vec3 World::gravity() const {
    return m_impl->settings.gravity;
}

std::optional<BodyId> World::create_body(const BodyDef& def) {
    const bool valid_material = def.friction >= 0.0f && def.restitution >= 0.0f &&
                                def.restitution <= 1.0f && def.linear_damping >= 0.0f &&
                                def.angular_damping >= 0.0f && std::isfinite(def.gravity_scale);
    if (!is_valid(def.shape) || !is_finite(def.position) || !valid_material ||
        (def.motion == Motion::Dynamic && !(def.mass > 0.0f && std::isfinite(def.mass)))) {
        return std::nullopt;
    }

    Impl& impl = *m_impl;
    std::uint32_t index = 0;
    if (impl.free_bodies.empty()) {
        index = static_cast<std::uint32_t>(impl.bodies.size());
        impl.bodies.emplace_back();
    } else {
        index = impl.free_bodies.back();
        impl.free_bodies.pop_back();
    }
    Body& body = impl.bodies[index];
    const std::uint32_t generation = body.generation;
    body = Body{};
    body.generation = generation;
    body.alive = true;
    body.shape = def.shape;
    body.motion = def.motion;
    body.sensor = def.sensor;
    body.continuous = def.continuous;
    body.user_data = def.user_data;
    body.position = def.position;
    body.rotation = normalized(def.rotation);
    body.friction = def.friction;
    body.restitution = def.restitution;
    body.linear_damping = def.linear_damping;
    body.angular_damping = def.angular_damping;
    body.gravity_scale = def.gravity_scale;
    if (def.motion != Motion::Static) {
        body.linear_velocity = def.linear_velocity;
        body.angular_velocity = def.angular_velocity;
        body.awake = true;
    }
    if (def.motion == Motion::Dynamic) {
        body.inverse_mass = 1.0f / def.mass;
        body.inverse_inertia_local = inverse_inertia(def.shape, def.mass);
    }
    body.update_inertia();
    ++impl.body_count;
    return BodyId{index, generation};
}

void World::destroy_body(BodyId id) {
    Impl& impl = *m_impl;
    if (impl.find(id) == nullptr) {
        return;
    }
    impl.wake_touching(id.index);
    for (auto it = impl.pairs.begin(); it != impl.pairs.end();) {
        const Pair& pair = it->second;
        if (pair.a == id.index || pair.b == id.index) {
            if (pair.touching) {
                impl.pending.push_back(impl.event(pair, ContactEvent::Kind::Ended));
            }
            it = impl.pairs.erase(it);
        } else {
            ++it;
        }
    }
    for (std::uint32_t i = 0; i < impl.joints.size(); ++i) {
        const Joint& joint = impl.joints[i];
        if (joint.alive && (joint.a == id.index || joint.b == std::optional(id.index))) {
            destroy_joint(JointId{i, joint.generation});
        }
    }
    Body& body = impl.bodies[id.index];
    body.alive = false;
    ++body.generation;
    impl.free_bodies.push_back(id.index);
    --impl.body_count;
}

std::optional<BodyState> World::state(BodyId id) const {
    const Body* const body = m_impl->find(id);
    if (body == nullptr) {
        return std::nullopt;
    }
    return BodyState{body->position, body->rotation, body->linear_velocity, body->angular_velocity,
                     body->active()};
}

std::size_t World::body_count() const {
    return m_impl->body_count;
}

void World::set_transform(BodyId id, Vec3 position, Quat rotation) {
    Body* const body = m_impl->find(id);
    if (body == nullptr || !is_finite(position)) {
        return;
    }
    body->position = position;
    body->rotation = normalized(rotation);
    body->update_inertia();
    m_impl->wake(*body);
    m_impl->wake_touching(id.index);
}

void World::move_kinematic(BodyId id, Vec3 position, Quat rotation, float step_seconds) {
    Body* const body = m_impl->find(id);
    if (body == nullptr || body->motion != Motion::Kinematic || !(step_seconds > 0.0f)) {
        return;
    }
    rotation = normalized(rotation);
    body->linear_velocity = (position - body->position) / step_seconds;
    body->angular_velocity =
        to_rotation_vector(rotation * conjugate(body->rotation)) / step_seconds;
    body->kinematic_target = std::pair{position, rotation};
    m_impl->wake(*body);
}

void World::set_linear_velocity(BodyId id, Vec3 velocity) {
    Body* const body = m_impl->find(id);
    if (body != nullptr && body->moves()) {
        body->linear_velocity = velocity;
        m_impl->wake(*body);
    }
}

void World::set_angular_velocity(BodyId id, Vec3 velocity) {
    Body* const body = m_impl->find(id);
    if (body != nullptr && body->moves()) {
        body->angular_velocity = velocity;
        m_impl->wake(*body);
    }
}

void World::add_force(BodyId id, Vec3 force) {
    Body* const body = m_impl->find(id);
    if (body != nullptr && body->is_dynamic()) {
        body->force += force;
        m_impl->wake(*body);
    }
}

void World::add_impulse(BodyId id, Vec3 impulse) {
    Body* const body = m_impl->find(id);
    if (body != nullptr && body->is_dynamic()) {
        body->linear_velocity += impulse * body->inverse_mass;
        m_impl->wake(*body);
    }
}

std::optional<JointId> World::create_joint(const JointDef& def) {
    Impl& impl = *m_impl;
    Body* const a = impl.find(def.a);
    Body* const b = def.b.has_value() ? impl.find(*def.b) : nullptr;
    if (a == nullptr || (def.b.has_value() && b == nullptr) || a == b) {
        return std::nullopt;
    }

    Joint joint;
    joint.alive = true;
    joint.a = def.a.index;
    if (def.b.has_value()) {
        joint.b = def.b->index;
    }
    const Transform ta = a->transform();
    const auto local_b = [&](Vec3 world) {
        return b != nullptr ? b->transform().to_local(world) : world;
    };
    const auto axis_in_b = [&](Vec3 world) {
        return b != nullptr ? rotate(conjugate(b->rotation), world) : world;
    };
    const Quat b_rotation = b != nullptr ? b->rotation : Quat{};

    if (std::holds_alternative<FixedJoint>(def.type)) {
        joint.kind = JointKind::Fixed;
        joint.anchor_a = Vec3{};
        joint.anchor_b = local_b(a->position);
        joint.reference = conjugate(a->rotation) * b_rotation;
    } else if (const auto* point = std::get_if<PointJoint>(&def.type)) {
        joint.kind = JointKind::Point;
        joint.anchor_a = ta.to_local(point->anchor);
        joint.anchor_b = local_b(point->anchor);
    } else if (const auto* hinge = std::get_if<HingeJoint>(&def.type)) {
        const float lower = hinge->lower.value_or(-std::numbers::pi_v<float>);
        const float upper = hinge->upper.value_or(std::numbers::pi_v<float>);
        if (!(length(hinge->axis) > 0.0f) || !(lower <= 0.0f && upper >= 0.0f) ||
            lower < -std::numbers::pi_v<float> || upper > std::numbers::pi_v<float>) {
            return std::nullopt;
        }
        const Vec3 axis = normalized(hinge->axis);
        joint.kind = JointKind::Hinge;
        joint.anchor_a = ta.to_local(hinge->anchor);
        joint.anchor_b = local_b(hinge->anchor);
        joint.axis_a = rotate(conjugate(a->rotation), axis);
        joint.axis_b = axis_in_b(axis);
        joint.reference = conjugate(b_rotation) * a->rotation;
        joint.lower = hinge->lower;
        joint.upper = hinge->upper;
    } else {
        const auto& distance = std::get<DistanceJoint>(def.type);
        if (!(distance.min_distance >= 0.0f && distance.max_distance >= distance.min_distance)) {
            return std::nullopt;
        }
        joint.kind = JointKind::Distance;
        joint.anchor_a = ta.to_local(distance.anchor_a);
        joint.anchor_b = local_b(distance.anchor_b);
        joint.lower = distance.min_distance;
        joint.upper = distance.max_distance;
    }

    std::uint32_t index = 0;
    if (impl.free_joints.empty()) {
        index = static_cast<std::uint32_t>(impl.joints.size());
        impl.joints.emplace_back();
    } else {
        index = impl.free_joints.back();
        impl.free_joints.pop_back();
    }
    joint.generation = impl.joints[index].generation;
    impl.joints[index] = joint;
    if (joint.b.has_value()) {
        ++impl.joined[pair_key(joint.a, *joint.b)];
    }
    impl.wake(*a);
    if (b != nullptr) {
        impl.wake(*b);
    }
    return JointId{index, joint.generation};
}

void World::destroy_joint(JointId id) {
    Impl& impl = *m_impl;
    if (id.index >= impl.joints.size() || !impl.joints[id.index].alive ||
        impl.joints[id.index].generation != id.generation) {
        return;
    }
    Joint& joint = impl.joints[id.index];
    impl.wake(impl.bodies[joint.a]);
    if (joint.b.has_value()) {
        impl.wake(impl.bodies[*joint.b]);
        const std::uint64_t key = pair_key(joint.a, *joint.b);
        if (--impl.joined[key] == 0) {
            impl.joined.erase(key);
        }
    }
    joint.alive = false;
    ++joint.generation;
    impl.free_joints.push_back(id.index);
}

void World::step(float seconds) {
    if (seconds > 0.0f && std::isfinite(seconds)) {
        m_impl->step(seconds);
    }
}

std::span<const ContactEvent> World::contact_events() const {
    return m_impl->events;
}

std::optional<RayHit> World::cast_ray(Vec3 origin, Vec3 direction, float max_distance) const {
    const Vec3 unit = normalized(direction);
    if (length_squared(unit) == 0.0f || !(max_distance > 0.0f) || !is_finite(origin)) {
        return std::nullopt;
    }
    std::optional<RayHit> best;
    float nearest = max_distance;
    const Impl& impl = *m_impl;
    for (std::uint32_t i = 0; i < impl.bodies.size(); ++i) {
        const Body& body = impl.bodies[i];
        if (!body.alive || body.sensor) {
            continue;
        }
        const std::optional<RayResult> hit =
            detail::cast_ray(body.shape, body.transform(), origin, unit, nearest);
        if (hit.has_value() && hit->distance <= nearest) {
            nearest = hit->distance;
            best = RayHit{impl.id_of(i), body.user_data, origin + unit * hit->distance, hit->normal,
                          hit->distance};
        }
    }
    return best;
}

void World::overlap(const Shape& shape, Vec3 position, Quat rotation,
                    std::vector<BodyId>& found) const {
    found.clear();
    if (!is_valid(shape) || !is_finite(position)) {
        return;
    }
    const Transform query = Transform::make(position, normalized(rotation));
    const Aabb query_bounds = bounds(shape, query);
    const Impl& impl = *m_impl;
    for (std::uint32_t i = 0; i < impl.bodies.size(); ++i) {
        const Body& body = impl.bodies[i];
        if (!body.alive) {
            continue;
        }
        const Transform transform = body.transform();
        if (!query_bounds.overlaps(bounds(body.shape, transform))) {
            continue;
        }
        Manifold manifold;
        collide(shape, query, body.shape, transform, 0.0f, manifold);
        for (int p = 0; p < manifold.count; ++p) {
            if (manifold.points[p].separation <= 0.0f) {
                found.push_back(impl.id_of(i));
                break;
            }
        }
    }
}

} // namespace impulse
