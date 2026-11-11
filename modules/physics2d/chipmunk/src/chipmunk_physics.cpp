#include "dream/core/handle.h"
#include "dream/core/module_registry.h"
#include "dream/physics2d/physics2d.h"

#include <chipmunk/chipmunk.h>
// For the velocity callback, which must change velocities without the setters: they wake the body,
// which would keep it from ever falling asleep.
#include <chipmunk/chipmunk_structs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace physics2d;
using core::Vec2;

// Chipmunk leaves shapes up to this far inside each other, so resting bodies sit this much lower.
// Its default, 10 cm, is meant for pixel units.
constexpr cpFloat k_collision_slop = 0.005;

// A body at rest this long falls asleep.
constexpr cpFloat k_sleep_seconds = 0.5;

// How much of a limit's overshoot is corrected per 60th of a second. Chipmunk corrects by adding
// velocity, which stays after the limit lets go, so a strong correction would send a body bouncing
// back from its limit; a gentle one leaves only a slow drift.
constexpr cpFloat k_limit_correction = 0.02;

// Queries skip sensors by their category.
constexpr cpBitmask k_solid_category = 1u << 0;
constexpr cpBitmask k_sensor_category = 1u << 1;

cpVect to_chipmunk(Vec2 v) {
    return cpv(v.x, v.y);
}

Vec2 from_chipmunk(cpVect v) {
    return Vec2{static_cast<float>(v.x), static_cast<float>(v.y)};
}

// Chipmunk's own helper for this needs the shape's body in a space; this works on any shape.
cpShape* make_shape(cpBody* body, const Shape& shape) {
    if (const auto* box = std::get_if<BoxShape>(&shape)) {
        return cpBoxShapeNew(body, 2.0 * box->half_extents.x, 2.0 * box->half_extents.y, 0.0);
    }
    if (const auto* circle = std::get_if<CircleShape>(&shape)) {
        return cpCircleShapeNew(body, circle->radius, cpvzero);
    }
    // A capsule is a segment with a radius.
    const auto& capsule = std::get<CapsuleShape>(shape);
    return cpSegmentShapeNew(body, cpv(0.0, -capsule.half_height), cpv(0.0, capsule.half_height),
                             capsule.radius);
}

cpFloat moment_of(const Shape& shape, cpFloat mass) {
    if (const auto* box = std::get_if<BoxShape>(&shape)) {
        return cpMomentForBox(mass, 2.0 * box->half_extents.x, 2.0 * box->half_extents.y);
    }
    if (const auto* circle = std::get_if<CircleShape>(&shape)) {
        return cpMomentForCircle(mass, 0.0, circle->radius, cpvzero);
    }
    const auto& capsule = std::get<CapsuleShape>(shape);
    return cpMomentForSegment(mass, cpv(0.0, -capsule.half_height), cpv(0.0, capsule.half_height),
                              capsule.radius);
}

// Everything the engine knows about a body. Chipmunk's body and shape point back to it, so it lives
// on the heap and never moves.
struct BodyRecord {
    cpBody* body = nullptr;
    cpShape* shape = nullptr;
    BodyHandle handle;
    MotionType motion = MotionType::Dynamic;
    std::uint64_t user_data = 0;
    float linear_damping = 0.0f;
    float angular_damping = 0.0f;
    float gravity_scale = 1.0f;
    bool fixed_rotation = false;

    // Set by move_kinematic: the body stops once it arrives.
    bool stops_after_step = false;
};

struct JointRecord {
    // A joint may be made of up to two Chipmunk constraints, such as a pivot and a limit.
    std::array<cpConstraint*, 2> constraints{};
    BodyHandle body_a;
    BodyHandle body_b;
};

// Applies gravity, forces and damping the way the interface describes them: Chipmunk has one
// damping and one gravity for the whole space.
void update_velocity(cpBody* body, cpVect gravity, cpFloat /*space_damping*/, cpFloat dt) {
    const auto* const record = static_cast<const BodyRecord*>(cpBodyGetUserData(body));
    cpBodyUpdateVelocity(body, cpvmult(gravity, record->gravity_scale), 1.0, dt);
    body->v = cpvmult(body->v, 1.0 / (1.0 + dt * record->linear_damping));
    body->w *= 1.0 / (1.0 + dt * record->angular_damping);
}

class ChipmunkPhysics final : public PhysicsWorld {
public:
    ChipmunkPhysics() : m_space(cpSpaceNew()) {
        cpSpaceSetGravity(m_space, cpv(0.0, -9.81));
        cpSpaceSetCollisionSlop(m_space, k_collision_slop);
        cpSpaceSetSleepTimeThreshold(m_space, k_sleep_seconds);

        cpCollisionHandler* const handler = cpSpaceAddDefaultCollisionHandler(m_space);
        handler->beginFunc = &on_begin;
        handler->preSolveFunc = &on_pre_solve;
        handler->separateFunc = &on_separate;
        handler->userData = this;
    }

    ~ChipmunkPhysics() override {
        // Removing shapes reports their contacts as ended, which no one will read now.
        m_closing = true;
        m_joints.for_each(
            [&](core::Handle<JointRecord>, JointRecord& joint) { free_constraints(joint); });
        m_bodies.for_each([&](core::Handle<std::unique_ptr<BodyRecord>>,
                              std::unique_ptr<BodyRecord>& record) { free_body(*record); });
        cpSpaceFree(m_space);
    }

    ChipmunkPhysics(const ChipmunkPhysics&) = delete;
    ChipmunkPhysics& operator=(const ChipmunkPhysics&) = delete;

    void set_gravity(Vec2 gravity) override {
        cpSpaceSetGravity(m_space, to_chipmunk(gravity));
        wake_all();
    }

    Vec2 gravity() const override {
        return from_chipmunk(cpSpaceGetGravity(m_space));
    }

    core::Result<BodyHandle> create_body(const BodyDesc& desc) override {
        if (core::Result<void> valid = validate(desc); !valid.has_value()) {
            return valid.error();
        }

        auto record = std::make_unique<BodyRecord>();
        record->motion = desc.motion;
        record->user_data = desc.user_data;
        record->linear_damping = desc.linear_damping;
        record->angular_damping = desc.angular_damping;
        record->gravity_scale = desc.gravity_scale;
        record->fixed_rotation = desc.fixed_rotation;

        switch (desc.motion) {
        case MotionType::Static:
            record->body = cpBodyNewStatic();
            break;
        case MotionType::Kinematic:
            record->body = cpBodyNewKinematic();
            break;
        case MotionType::Dynamic:
            record->body =
                cpBodyNew(desc.mass, desc.fixed_rotation ? std::numeric_limits<cpFloat>::infinity()
                                                         : moment_of(desc.shape, desc.mass));
            cpBodySetVelocityUpdateFunc(record->body, &update_velocity);
            break;
        }
        cpBody* const body = record->body;
        cpBodySetUserData(body, record.get());
        cpBodySetPosition(body, to_chipmunk(desc.position));
        cpBodySetAngle(body, desc.angle);
        if (desc.motion != MotionType::Static) {
            cpBodySetVelocity(body, to_chipmunk(desc.linear_velocity));
            cpBodySetAngularVelocity(body, desc.fixed_rotation ? 0.0 : desc.angular_velocity);
        }
        cpSpaceAddBody(m_space, body);

        cpShape* const shape = make_shape(body, desc.shape);
        record->shape = shape;
        cpShapeSetUserData(shape, record.get());
        cpShapeSetFriction(shape, desc.friction);
        cpShapeSetElasticity(shape, desc.restitution);
        cpShapeSetSensor(shape, desc.is_sensor ? cpTrue : cpFalse);
        cpShapeSetFilter(shape,
                         cpShapeFilterNew(CP_NO_GROUP,
                                          desc.is_sensor ? k_sensor_category : k_solid_category,
                                          CP_ALL_CATEGORIES));
        cpSpaceAddShape(m_space, shape);
        // Chipmunk has no continuous collision, so desc.continuous_collision has no effect.

        BodyRecord& stored = *record;
        const auto handle = m_bodies.create(std::move(record));
        stored.handle = handle_cast<BodyTag>(handle);
        return stored.handle;
    }

    void destroy_body(BodyHandle body) override {
        const auto key = handle_cast<std::unique_ptr<BodyRecord>>(body);
        std::unique_ptr<BodyRecord>* const record = m_bodies.get(key);
        if (record == nullptr) {
            return;
        }
        m_joints.for_each([&](core::Handle<JointRecord> handle, JointRecord& joint) {
            if (joint.body_a == body || joint.body_b == body) {
                free_constraints(joint);
                m_joints.destroy(handle);
            }
        });
        // Ending contacts are reported now, while the record can still name the body, and
        // delivered with the next step's events.
        free_body(**record);
        m_bodies.destroy(key);
    }

    std::optional<BodyState> body_state(BodyHandle body) const override {
        const BodyRecord* const record = find(body);
        if (record == nullptr) {
            return std::nullopt;
        }
        return BodyState{
            .position = from_chipmunk(cpBodyGetPosition(record->body)),
            .angle = static_cast<float>(cpBodyGetAngle(record->body)),
            .linear_velocity = from_chipmunk(cpBodyGetVelocity(record->body)),
            .angular_velocity = static_cast<float>(cpBodyGetAngularVelocity(record->body)),
            .is_awake = record->motion != MotionType::Static && !cpBodyIsSleeping(record->body),
        };
    }

    void set_transform(BodyHandle body, Vec2 position, float angle) override {
        BodyRecord* const record = find(body);
        if (record == nullptr) {
            return;
        }
        if (record->motion != MotionType::Static) {
            cpBodyActivate(record->body);
        }
        cpBodySetPosition(record->body, to_chipmunk(position));
        cpBodySetAngle(record->body, angle);
        if (record->motion == MotionType::Static) {
            cpSpaceReindexShapesForBody(m_space, record->body);
        }
    }

    void move_kinematic(BodyHandle body, Vec2 position, float angle, float step_seconds) override {
        BodyRecord* const record = find(body);
        if (record == nullptr || record->motion != MotionType::Kinematic ||
            !(step_seconds > 0.0f)) {
            return;
        }
        const cpVect from = cpBodyGetPosition(record->body);
        cpBodyActivate(record->body);
        cpBodySetVelocity(record->body,
                          cpvmult(cpvsub(to_chipmunk(position), from), 1.0 / step_seconds));
        cpBodySetAngularVelocity(record->body,
                                 (angle - cpBodyGetAngle(record->body)) / step_seconds);
        record->stops_after_step = true;
    }

    void set_linear_velocity(BodyHandle body, Vec2 velocity) override {
        if (BodyRecord* const record = find_moving(body)) {
            cpBodyActivate(record->body);
            cpBodySetVelocity(record->body, to_chipmunk(velocity));
        }
    }

    void set_angular_velocity(BodyHandle body, float velocity) override {
        BodyRecord* const record = find_moving(body);
        if (record != nullptr && !record->fixed_rotation) {
            cpBodyActivate(record->body);
            cpBodySetAngularVelocity(record->body, velocity);
        }
    }

    void add_force(BodyHandle body, Vec2 force) override {
        if (BodyRecord* const record = find_dynamic(body)) {
            cpBodyActivate(record->body);
            cpBodySetForce(record->body, cpvadd(cpBodyGetForce(record->body), to_chipmunk(force)));
        }
    }

    void add_impulse(BodyHandle body, Vec2 impulse) override {
        if (BodyRecord* const record = find_dynamic(body)) {
            cpBodyActivate(record->body);
            cpBodyApplyImpulseAtWorldPoint(record->body, to_chipmunk(impulse),
                                           cpBodyGetPosition(record->body));
        }
    }

    core::Result<JointHandle> create_joint(const JointDesc& desc) override {
        BodyRecord* const a = find(desc.body_a);
        BodyRecord* const b = desc.body_b.is_valid() ? find(desc.body_b) : nullptr;
        if (a == nullptr || (desc.body_b.is_valid() && b == nullptr) ||
            desc.body_a == desc.body_b) {
            return invalid("A joint needs two different bodies that exist");
        }

        // Chipmunk measures its second body against its first, and the interface measures body_a
        // against body_b, so body_b comes first. A joint to the world uses the space's own body.
        cpBody* const first = b != nullptr ? b->body : cpSpaceGetStaticBody(m_space);
        cpBody* const second = a->body;
        const cpFloat relative = cpBodyGetAngle(second) - cpBodyGetAngle(first);

        JointRecord joint{.body_a = desc.body_a, .body_b = desc.body_b};
        if (std::holds_alternative<FixedJoint>(desc.type)) {
            const cpVect center = cpBodyGetPosition(second);
            joint.constraints[0] = cpPivotJointNew(first, second, center);
            // Keeps the second body's angle minus the first's, as it is now.
            joint.constraints[1] = cpGearJointNew(first, second, relative, 1.0);
        } else if (const auto* hinge = std::get_if<HingeJoint>(&desc.type)) {
            const float lowest = hinge->min_angle_radians.value_or(-core::k_pi);
            const float highest = hinge->max_angle_radians.value_or(core::k_pi);
            if (!(lowest <= 0.0f && highest >= 0.0f && std::isfinite(lowest) &&
                  std::isfinite(highest))) {
                return invalid("A hinge's limits must include zero");
            }
            joint.constraints[0] = cpPivotJointNew(first, second, to_chipmunk(hinge->anchor));
            if (hinge->min_angle_radians.has_value() || hinge->max_angle_radians.has_value()) {
                joint.constraints[1] =
                    cpRotaryLimitJointNew(first, second, relative + lowest, relative + highest);
                soften(joint.constraints[1]);
            }
        } else {
            const auto& distance = std::get<DistanceJoint>(desc.type);
            if (!(distance.min_distance >= 0.0f &&
                  distance.max_distance >= distance.min_distance)) {
                return invalid("A distance joint needs 0 <= min_distance <= max_distance");
            }
            joint.constraints[0] = cpSlideJointNew(
                first, second, cpBodyWorldToLocal(first, to_chipmunk(distance.anchor_b)),
                cpBodyWorldToLocal(second, to_chipmunk(distance.anchor_a)), distance.min_distance,
                distance.max_distance);
        }

        for (cpConstraint* const constraint : joint.constraints) {
            if (constraint != nullptr) {
                cpSpaceAddConstraint(m_space, constraint);
            }
        }
        cpBodyActivate(a->body);
        if (b != nullptr && b->motion != MotionType::Static) {
            cpBodyActivate(b->body);
        }
        return handle_cast<JointTag>(m_joints.create(joint));
    }

    void destroy_joint(JointHandle joint) override {
        const auto key = handle_cast<JointRecord>(joint);
        if (JointRecord* const record = m_joints.get(key)) {
            free_constraints(*record);
            m_joints.destroy(key);
        }
    }

    void step(float step_seconds) override {
        if (!(step_seconds > 0.0f)) {
            return;
        }
        cpSpaceStep(m_space, step_seconds);

        m_bodies.for_each(
            [](core::Handle<std::unique_ptr<BodyRecord>>, std::unique_ptr<BodyRecord>& record) {
                if (record->stops_after_step) {
                    record->stops_after_step = false;
                    cpBodySetVelocity(record->body, cpvzero);
                    cpBodySetAngularVelocity(record->body, 0.0);
                }
            });

        // Events from this step, and from bodies destroyed since the last one.
        m_events.swap(m_pending);
        m_pending.clear();
    }

    std::span<const ContactEvent> contact_events() const override {
        return m_events;
    }

    std::optional<RayHit> cast_ray(Vec2 origin, Vec2 direction, float max_distance) const override {
        const float length = core::length(direction);
        if (length <= 0.0f || !(max_distance > 0.0f)) {
            return std::nullopt;
        }
        const cpVect start = to_chipmunk(origin);
        const cpVect end = to_chipmunk(origin + direction / length * max_distance);
        cpSegmentQueryInfo info{};
        const cpShape* const shape = cpSpaceSegmentQueryFirst(
            m_space, start, end, 0.0,
            cpShapeFilterNew(CP_NO_GROUP, CP_ALL_CATEGORIES, k_solid_category), &info);
        if (shape == nullptr) {
            return std::nullopt;
        }
        const auto* const record = static_cast<const BodyRecord*>(cpShapeGetUserData(shape));
        return RayHit{
            .body = record->handle,
            .user_data = record->user_data,
            .point = from_chipmunk(info.point),
            .normal = from_chipmunk(info.normal),
            .distance = static_cast<float>(info.alpha) * max_distance,
        };
    }

    void overlap(const Shape& shape, Vec2 position, float angle,
                 std::vector<BodyHandle>& bodies) const override {
        bodies.clear();
        if (!detail::is_valid_shape(shape)) {
            return;
        }
        // The query shape belongs to a body that is never added to the space.
        cpBody* const body = cpBodyNewStatic();
        cpBodySetPosition(body, to_chipmunk(position));
        cpBodySetAngle(body, angle);
        cpShape* const query = make_shape(body, shape);
        cpShapeCacheBB(query);

        const auto found = [](cpShape* hit, cpContactPointSet* /*points*/, void* data) {
            auto* const result = static_cast<std::vector<BodyHandle>*>(data);
            const auto* const record = static_cast<const BodyRecord*>(cpShapeGetUserData(hit));
            if (std::find(result->begin(), result->end(), record->handle) == result->end()) {
                result->push_back(record->handle);
            }
        };
        cpSpaceShapeQuery(m_space, query, found, &bodies);
        cpShapeFree(query);
        cpBodyFree(body);
    }

    std::size_t body_count() const override {
        return m_bodies.size();
    }

private:
    static core::Error invalid(const char* message) {
        return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
    }

    static void soften(cpConstraint* limit) {
        cpConstraintSetErrorBias(limit, std::pow(1.0 - k_limit_correction, 60.0));
    }

    // --- Contacts, from Chipmunk's callbacks ---

    static ChipmunkPhysics& self(cpDataPointer data) {
        return *static_cast<ChipmunkPhysics*>(data);
    }

    static cpBool on_begin(cpArbiter* arbiter, cpSpace* /*space*/, cpDataPointer data) {
        const cpContactPointSet points = cpArbiterGetContactPointSet(arbiter);
        const Vec2 point =
            points.count > 0
                ? from_chipmunk(cpvlerp(points.points[0].pointA, points.points[0].pointB, 0.5))
                : Vec2{};
        self(data).record_event(arbiter, ContactEvent::Kind::Began, point,
                                from_chipmunk(points.normal));
        return cpTrue;
    }

    // Chipmunk multiplies the two shapes' elasticities, so a bouncy ball on a plain floor wouldn't
    // bounce. The interface follows the other backends: the bouncier of the two, and the geometric
    // mean of the frictions.
    static cpBool on_pre_solve(cpArbiter* arbiter, cpSpace* /*space*/, cpDataPointer /*data*/) {
        CP_ARBITER_GET_SHAPES(arbiter, a, b);
        cpArbiterSetRestitution(arbiter,
                                std::max(cpShapeGetElasticity(a), cpShapeGetElasticity(b)));
        cpArbiterSetFriction(arbiter, std::sqrt(cpShapeGetFriction(a) * cpShapeGetFriction(b)));
        return cpTrue;
    }

    static void on_separate(cpArbiter* arbiter, cpSpace* /*space*/, cpDataPointer data) {
        self(data).record_event(arbiter, ContactEvent::Kind::Ended, Vec2{}, Vec2{});
    }

    void record_event(cpArbiter* arbiter, ContactEvent::Kind kind, Vec2 point, Vec2 normal) {
        if (m_closing) {
            return;
        }
        CP_ARBITER_GET_SHAPES(arbiter, shape_a, shape_b);
        const auto* const a = static_cast<const BodyRecord*>(cpShapeGetUserData(shape_a));
        const auto* const b = static_cast<const BodyRecord*>(cpShapeGetUserData(shape_b));
        m_pending.push_back(ContactEvent{
            .kind = kind,
            .body_a = a->handle,
            .body_b = b->handle,
            .user_data_a = a->user_data,
            .user_data_b = b->user_data,
            .point = point,
            .normal = normal,
        });
    }

    // --- Bookkeeping ---

    void free_constraints(JointRecord& joint) {
        for (cpConstraint*& constraint : joint.constraints) {
            if (constraint != nullptr) {
                cpSpaceRemoveConstraint(m_space, constraint);
                cpConstraintFree(constraint);
                constraint = nullptr;
            }
        }
    }

    void free_body(BodyRecord& record) {
        if (record.motion != MotionType::Static) {
            cpBodyActivate(record.body);
        }
        cpSpaceRemoveShape(m_space, record.shape);
        cpShapeFree(record.shape);
        cpSpaceRemoveBody(m_space, record.body);
        cpBodyFree(record.body);
    }

    void wake_all() {
        m_bodies.for_each(
            [](core::Handle<std::unique_ptr<BodyRecord>>, std::unique_ptr<BodyRecord>& record) {
                if (record->motion != MotionType::Static) {
                    cpBodyActivate(record->body);
                }
            });
    }

    BodyRecord* find(BodyHandle body) const {
        const std::unique_ptr<BodyRecord>* const record =
            m_bodies.get(handle_cast<std::unique_ptr<BodyRecord>>(body));
        return record == nullptr ? nullptr : record->get();
    }

    BodyRecord* find_moving(BodyHandle body) const {
        BodyRecord* const record = find(body);
        return record != nullptr && record->motion != MotionType::Static ? record : nullptr;
    }

    BodyRecord* find_dynamic(BodyHandle body) const {
        BodyRecord* const record = find(body);
        return record != nullptr && record->motion == MotionType::Dynamic ? record : nullptr;
    }

    cpSpace* m_space = nullptr;
    core::HandlePool<std::unique_ptr<BodyRecord>> m_bodies;
    core::HandlePool<JointRecord> m_joints;
    std::vector<ContactEvent> m_pending;
    std::vector<ContactEvent> m_events;
    bool m_closing = false;
};

core::Result<std::unique_ptr<PhysicsWorld>> create(const core::Config& /*config*/) {
    return std::unique_ptr<PhysicsWorld>{std::make_unique<ChipmunkPhysics>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_physics2d_chipmunk(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<PhysicsWorld>{.name = "chipmunk", .create = &create});
}

} // namespace dream::modules
