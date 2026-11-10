#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/physics2d/physics2d.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace physics2d;
using core::Vec2;

b2Vec2 to_box2d(Vec2 v) {
    return b2Vec2{v.x, v.y};
}

Vec2 from_box2d(b2Vec2 v) {
    return Vec2{v.x, v.y};
}

b2BodyType to_box2d(MotionType motion) {
    switch (motion) {
    case MotionType::Static:
        return b2_staticBody;
    case MotionType::Kinematic:
        return b2_kinematicBody;
    case MotionType::Dynamic:
        return b2_dynamicBody;
    }
    return b2_staticBody;
}

// b2MakeRot and b2Rot_GetAngle trade accuracy for speed, by up to half a percent, which shows in
// games that set or read exact angles.
b2Rot exact_rotation(float angle) {
    return b2Rot{std::cos(angle), std::sin(angle)};
}

float exact_angle(b2Rot rotation) {
    return std::atan2(rotation.s, rotation.c);
}

// Box2D identifies shapes by slot and generation, like Handle.
std::uint64_t shape_key(b2ShapeId id) {
    return (std::uint64_t{static_cast<std::uint32_t>(id.index1)} << 16) | id.generation;
}

// Shape points for queries, in the shape's own frame.
struct QueryShape {
    b2Vec2 points[4];
    int count = 0;
    float radius = 0.0f;
};

QueryShape query_shape(const Shape& shape) {
    QueryShape result;
    if (const auto* box = std::get_if<BoxShape>(&shape)) {
        const float x = box->half_extents.x;
        const float y = box->half_extents.y;
        result.points[0] = {-x, -y};
        result.points[1] = {x, -y};
        result.points[2] = {x, y};
        result.points[3] = {-x, y};
        result.count = 4;
    } else if (const auto* circle = std::get_if<CircleShape>(&shape)) {
        result.points[0] = {0.0f, 0.0f};
        result.count = 1;
        result.radius = circle->radius;
    } else {
        const auto& capsule = std::get<CapsuleShape>(shape);
        result.points[0] = {0.0f, -capsule.half_height};
        result.points[1] = {0.0f, capsule.half_height};
        result.count = capsule.half_height > 0.0f ? 2 : 1;
        result.radius = capsule.radius;
    }
    return result;
}

struct BodyRecord {
    b2BodyId id{};
    b2ShapeId shape{};
    MotionType motion = MotionType::Dynamic;
};

struct JointRecord {
    b2JointId id{};
    BodyHandle body_a;
    BodyHandle body_b;
};

// What events need to know about a shape's body, kept for one step after the body is destroyed so
// contacts that end because of it can still be reported.
struct BodyIdentity {
    BodyHandle handle;
    std::uint64_t user_data = 0;
};

class Box2dPhysics final : public PhysicsWorld {
public:
    Box2dPhysics() {
        b2WorldDef def = b2DefaultWorldDef();
        def.gravity = b2Vec2{0.0f, -9.81f};
        m_world = b2CreateWorld(&def);

        // Joints to "the world" attach to this body, which has no shape.
        b2BodyDef ground = b2DefaultBodyDef();
        m_ground = b2CreateBody(m_world, &ground);
    }

    ~Box2dPhysics() override {
        b2DestroyWorld(m_world);
    }

    Box2dPhysics(const Box2dPhysics&) = delete;
    Box2dPhysics& operator=(const Box2dPhysics&) = delete;

    void set_gravity(Vec2 gravity) override {
        b2World_SetGravity(m_world, to_box2d(gravity));
    }

    Vec2 gravity() const override {
        return from_box2d(b2World_GetGravity(m_world));
    }

    core::Result<BodyHandle> create_body(const BodyDesc& desc) override {
        if (core::Result<void> valid = validate(desc); !valid.has_value()) {
            return valid.error();
        }
        const bool moves = desc.motion != MotionType::Static;

        b2BodyDef body_def = b2DefaultBodyDef();
        body_def.type = to_box2d(desc.motion);
        body_def.position = to_box2d(desc.position);
        body_def.rotation = exact_rotation(desc.angle);
        if (moves) {
            body_def.linearVelocity = to_box2d(desc.linear_velocity);
            body_def.angularVelocity = desc.fixed_rotation ? 0.0f : desc.angular_velocity;
        }
        body_def.linearDamping = desc.linear_damping;
        body_def.angularDamping = desc.angular_damping;
        body_def.gravityScale = desc.gravity_scale;
        body_def.fixedRotation = desc.fixed_rotation;
        body_def.isBullet = desc.continuous_collision;

        const core::Handle<BodyRecord> handle = m_bodies.create(BodyRecord{});
        const BodyHandle public_handle = handle_cast<BodyTag>(handle);
        const b2BodyId body = b2CreateBody(m_world, &body_def);

        b2ShapeDef shape_def = b2DefaultShapeDef();
        shape_def.density = 1.0f;
        shape_def.material.friction = desc.friction;
        shape_def.material.restitution = desc.restitution;
        shape_def.isSensor = desc.is_sensor;
        shape_def.enableSensorEvents = true;
        shape_def.enableContactEvents = true;

        b2ShapeId shape{};
        if (const auto* box = std::get_if<BoxShape>(&desc.shape)) {
            const b2Polygon polygon = b2MakeBox(box->half_extents.x, box->half_extents.y);
            shape = b2CreatePolygonShape(body, &shape_def, &polygon);
        } else if (const auto* circle = std::get_if<CircleShape>(&desc.shape)) {
            const b2Circle geometry{b2Vec2{0.0f, 0.0f}, circle->radius};
            shape = b2CreateCircleShape(body, &shape_def, &geometry);
        } else {
            const auto& capsule = std::get<CapsuleShape>(desc.shape);
            if (capsule.half_height > 0.0f) {
                const b2Capsule geometry{b2Vec2{0.0f, -capsule.half_height},
                                         b2Vec2{0.0f, capsule.half_height}, capsule.radius};
                shape = b2CreateCapsuleShape(body, &shape_def, &geometry);
            } else {
                const b2Circle geometry{b2Vec2{0.0f, 0.0f}, capsule.radius};
                shape = b2CreateCircleShape(body, &shape_def, &geometry);
            }
        }

        // Box2D derives mass from density, so the result is scaled to the mass asked for.
        if (desc.motion == MotionType::Dynamic) {
            b2MassData mass = b2Body_GetMassData(body);
            if (mass.mass > 0.0f) {
                mass.rotationalInertia *= desc.mass / mass.mass;
            }
            mass.mass = desc.mass;
            b2Body_SetMassData(body, mass);
        }

        BodyRecord& record = *m_bodies.get(handle);
        record.id = body;
        record.shape = shape;
        record.motion = desc.motion;
        m_identities[shape_key(shape)] = BodyIdentity{public_handle, desc.user_data};
        return public_handle;
    }

    void destroy_body(BodyHandle body) override {
        const BodyRecord* const record = find(body);
        if (record == nullptr) {
            return;
        }
        // Box2D destroys the body's joints with it.
        m_joints.for_each([&](core::Handle<JointRecord> handle, const JointRecord& joint) {
            if (joint.body_a == body || joint.body_b == body) {
                m_joints.destroy(handle);
            }
        });
        m_forgotten.push_back(shape_key(record->shape));
        b2DestroyBody(record->id);
        m_bodies.destroy(handle_cast<BodyRecord>(body));
    }

    std::optional<BodyState> body_state(BodyHandle body) const override {
        const BodyRecord* const record = find(body);
        if (record == nullptr) {
            return std::nullopt;
        }
        return BodyState{
            .position = from_box2d(b2Body_GetPosition(record->id)),
            .angle = exact_angle(b2Body_GetRotation(record->id)),
            .linear_velocity = from_box2d(b2Body_GetLinearVelocity(record->id)),
            .angular_velocity = b2Body_GetAngularVelocity(record->id),
            .is_awake = b2Body_IsAwake(record->id),
        };
    }

    void set_transform(BodyHandle body, Vec2 position, float angle) override {
        if (const BodyRecord* const record = find(body)) {
            b2Body_SetTransform(record->id, to_box2d(position), exact_rotation(angle));
            if (record->motion != MotionType::Static) {
                b2Body_SetAwake(record->id, true);
            }
        }
    }

    void move_kinematic(BodyHandle body, Vec2 position, float angle, float step_seconds) override {
        const BodyRecord* const record = find(body);
        if (record != nullptr && record->motion == MotionType::Kinematic && step_seconds > 0.0f) {
            b2Body_SetTargetTransform(record->id, b2Transform{to_box2d(position), exact_rotation(angle)},
                                      step_seconds);
        }
    }

    void set_linear_velocity(BodyHandle body, Vec2 velocity) override {
        if (const BodyRecord* const record = find_moving(body)) {
            b2Body_SetLinearVelocity(record->id, to_box2d(velocity));
            b2Body_SetAwake(record->id, true);
        }
    }

    void set_angular_velocity(BodyHandle body, float velocity) override {
        if (const BodyRecord* const record = find_moving(body)) {
            b2Body_SetAngularVelocity(record->id, velocity);
            b2Body_SetAwake(record->id, true);
        }
    }

    void add_force(BodyHandle body, Vec2 force) override {
        if (const BodyRecord* const record = find_dynamic(body)) {
            b2Body_ApplyForceToCenter(record->id, to_box2d(force), true);
        }
    }

    void add_impulse(BodyHandle body, Vec2 impulse) override {
        if (const BodyRecord* const record = find_dynamic(body)) {
            b2Body_ApplyLinearImpulseToCenter(record->id, to_box2d(impulse), true);
        }
    }

    core::Result<JointHandle> create_joint(const JointDesc& desc) override {
        const BodyRecord* const a = find(desc.body_a);
        const BodyRecord* const b = desc.body_b.is_valid() ? find(desc.body_b) : nullptr;
        if (a == nullptr || (desc.body_b.is_valid() && b == nullptr) || desc.body_a == desc.body_b) {
            return invalid("A joint needs two different bodies that exist");
        }

        // Box2D measures its body B against body A, and the interface measures body_a against
        // body_b, so the interface's body_b is Box2D's body A.
        const b2BodyId first = b != nullptr ? b->id : m_ground;
        const b2BodyId second = a->id;
        const float reference = exact_angle(b2Body_GetRotation(second)) -
                                exact_angle(b2Body_GetRotation(first));

        b2JointId joint{};
        if (std::holds_alternative<FixedJoint>(desc.type)) {
            b2WeldJointDef def = b2DefaultWeldJointDef();
            def.bodyIdA = first;
            def.bodyIdB = second;
            const b2Vec2 middle = b2Body_GetPosition(second);
            def.localAnchorA = b2Body_GetLocalPoint(first, middle);
            def.localAnchorB = b2Body_GetLocalPoint(second, middle);
            def.referenceAngle = reference;
            joint = b2CreateWeldJoint(m_world, &def);
        } else if (const auto* hinge = std::get_if<HingeJoint>(&desc.type)) {
            const float lowest = hinge->min_angle_radians.value_or(-core::k_pi);
            const float highest = hinge->max_angle_radians.value_or(core::k_pi);
            if (!(lowest <= 0.0f && highest >= 0.0f && std::isfinite(lowest) &&
                  std::isfinite(highest))) {
                return invalid("A hinge's limits must include zero");
            }
            b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
            def.bodyIdA = first;
            def.bodyIdB = second;
            def.localAnchorA = b2Body_GetLocalPoint(first, to_box2d(hinge->anchor));
            def.localAnchorB = b2Body_GetLocalPoint(second, to_box2d(hinge->anchor));
            def.referenceAngle = reference;
            def.enableLimit =
                hinge->min_angle_radians.has_value() || hinge->max_angle_radians.has_value();
            if (def.enableLimit) {
                // Box2D's limits stop just short of a half turn each way.
                constexpr float k_widest = 0.99f * core::k_pi;
                def.lowerAngle = std::max(lowest, -k_widest);
                def.upperAngle = std::min(highest, k_widest);
            }
            joint = b2CreateRevoluteJoint(m_world, &def);
        } else {
            const auto& distance = std::get<DistanceJoint>(desc.type);
            if (!(distance.min_distance >= 0.0f && distance.max_distance >= distance.min_distance)) {
                return invalid("A distance joint needs 0 <= min_distance <= max_distance");
            }
            b2DistanceJointDef def = b2DefaultDistanceJointDef();
            def.bodyIdA = first;
            def.bodyIdB = second;
            def.localAnchorA = b2Body_GetLocalPoint(first, to_box2d(distance.anchor_b));
            def.localAnchorB = b2Body_GetLocalPoint(second, to_box2d(distance.anchor_a));
            // A spring with no stiffness lets the length vary freely between the limits.
            def.length = std::clamp(core::length(distance.anchor_a - distance.anchor_b),
                                    distance.min_distance, distance.max_distance);
            def.enableSpring = true;
            def.hertz = 0.0f;
            def.enableLimit = true;
            def.minLength = distance.min_distance;
            def.maxLength = distance.max_distance;
            joint = b2CreateDistanceJoint(m_world, &def);
        }

        b2Body_SetAwake(a->id, true);
        if (b != nullptr) {
            b2Body_SetAwake(b->id, true);
        }
        return handle_cast<JointTag>(m_joints.create(JointRecord{joint, desc.body_a, desc.body_b}));
    }

    void destroy_joint(JointHandle joint) override {
        if (const JointRecord* const record = m_joints.get(handle_cast<JointRecord>(joint))) {
            if (b2Joint_IsValid(record->id)) {
                b2DestroyJoint(record->id);
            }
            m_joints.destroy(handle_cast<JointRecord>(joint));
        }
    }

    void step(float step_seconds) override {
        if (step_seconds <= 0.0f) {
            return;
        }
        b2World_Step(m_world, step_seconds, 4);

        m_events.clear();
        const b2ContactEvents contacts = b2World_GetContactEvents(m_world);
        for (int i = 0; i < contacts.beginCount; ++i) {
            const b2ContactBeginTouchEvent& event = contacts.beginEvents[i];
            const b2Manifold& manifold = event.manifold;
            add_event(ContactEvent::Kind::Began, event.shapeIdA, event.shapeIdB,
                      manifold.pointCount > 0 ? from_box2d(manifold.points[0].point) : Vec2{},
                      from_box2d(manifold.normal));
        }
        for (int i = 0; i < contacts.endCount; ++i) {
            const b2ContactEndTouchEvent& event = contacts.endEvents[i];
            add_event(ContactEvent::Kind::Ended, event.shapeIdA, event.shapeIdB, Vec2{}, Vec2{});
        }

        const b2SensorEvents sensors = b2World_GetSensorEvents(m_world);
        for (int i = 0; i < sensors.beginCount; ++i) {
            const b2SensorBeginTouchEvent& event = sensors.beginEvents[i];
            add_event(ContactEvent::Kind::Began, event.sensorShapeId, event.visitorShapeId, Vec2{},
                      Vec2{});
        }
        for (int i = 0; i < sensors.endCount; ++i) {
            const b2SensorEndTouchEvent& event = sensors.endEvents[i];
            add_event(ContactEvent::Kind::Ended, event.sensorShapeId, event.visitorShapeId, Vec2{},
                      Vec2{});
        }

        for (const std::uint64_t key : m_forgotten) {
            m_identities.erase(key);
        }
        m_forgotten.clear();

        // Joints that went away with a body are already gone from the records; this catches any
        // that Box2D removed some other way.
        m_joints.for_each([&](core::Handle<JointRecord> handle, const JointRecord& joint) {
            if (!b2Joint_IsValid(joint.id)) {
                m_joints.destroy(handle);
            }
        });
    }

    std::span<const ContactEvent> contact_events() const override {
        return m_events;
    }

    std::optional<RayHit> cast_ray(Vec2 origin, Vec2 direction, float max_distance) const override {
        const float length = core::length(direction);
        if (length <= 0.0f || !(max_distance > 0.0f)) {
            return std::nullopt;
        }

        struct Closest {
            const Box2dPhysics* self = nullptr;
            b2ShapeId shape{};
            b2Vec2 point{};
            b2Vec2 normal{};
            float fraction = 1.0f;
            bool hit = false;
        } closest{this};

        const auto callback = [](b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction,
                                 void* context) -> float {
            auto* const result = static_cast<Closest*>(context);
            if (b2Shape_IsSensor(shape)) {
                return -1.0f;
            }
            result->shape = shape;
            result->point = point;
            result->normal = normal;
            result->fraction = fraction;
            result->hit = true;
            return fraction;
        };
        b2World_CastRay(m_world, to_box2d(origin), to_box2d(direction / length * max_distance),
                        b2DefaultQueryFilter(), callback, &closest);
        if (!closest.hit) {
            return std::nullopt;
        }

        RayHit hit{
            .point = from_box2d(closest.point),
            .normal = from_box2d(closest.normal),
            .distance = closest.fraction * max_distance,
        };
        if (const BodyIdentity* const found = identity(closest.shape)) {
            hit.body = found->handle;
            hit.user_data = found->user_data;
        }
        return hit;
    }

    void overlap(const Shape& shape, Vec2 position, float angle,
                 std::vector<BodyHandle>& bodies) const override {
        bodies.clear();
        if (!detail::is_valid_shape(shape)) {
            return;
        }
        const QueryShape points = query_shape(shape);
        const b2ShapeProxy proxy = b2MakeOffsetProxy(points.points, points.count, points.radius,
                                                     to_box2d(position), exact_rotation(angle));

        struct Found {
            const Box2dPhysics* self;
            std::vector<BodyHandle>* bodies;
        } found{this, &bodies};
        const auto callback = [](b2ShapeId hit, void* context) -> bool {
            auto* const result = static_cast<Found*>(context);
            if (const BodyIdentity* const body = result->self->identity(hit)) {
                if (std::find(result->bodies->begin(), result->bodies->end(), body->handle) ==
                    result->bodies->end()) {
                    result->bodies->push_back(body->handle);
                }
            }
            return true;
        };
        b2World_OverlapShape(m_world, &proxy, b2DefaultQueryFilter(), callback, &found);
    }

    std::size_t body_count() const override {
        return m_bodies.size();
    }

private:
    static core::Error invalid(const char* message) {
        return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
    }

    void add_event(ContactEvent::Kind kind, b2ShapeId shape_a, b2ShapeId shape_b, Vec2 point,
                   Vec2 normal) {
        const BodyIdentity* const a = identity(shape_a);
        const BodyIdentity* const b = identity(shape_b);
        if (a == nullptr || b == nullptr) {
            return;
        }
        m_events.push_back(ContactEvent{
            .kind = kind,
            .body_a = a->handle,
            .body_b = b->handle,
            .user_data_a = a->user_data,
            .user_data_b = b->user_data,
            .point = point,
            .normal = normal,
        });
    }

    const BodyIdentity* identity(b2ShapeId shape) const {
        const auto found = m_identities.find(shape_key(shape));
        return found == m_identities.end() ? nullptr : &found->second;
    }

    const BodyRecord* find(BodyHandle body) const {
        return m_bodies.get(handle_cast<BodyRecord>(body));
    }

    const BodyRecord* find_moving(BodyHandle body) const {
        const BodyRecord* const record = find(body);
        return record != nullptr && record->motion != MotionType::Static ? record : nullptr;
    }

    const BodyRecord* find_dynamic(BodyHandle body) const {
        const BodyRecord* const record = find(body);
        return record != nullptr && record->motion == MotionType::Dynamic ? record : nullptr;
    }

    b2WorldId m_world{};
    b2BodyId m_ground{};
    core::HandlePool<BodyRecord> m_bodies;
    core::HandlePool<JointRecord> m_joints;
    std::unordered_map<std::uint64_t, BodyIdentity> m_identities;
    std::vector<std::uint64_t> m_forgotten;
    std::vector<ContactEvent> m_events;
};

core::Result<std::unique_ptr<PhysicsWorld>> create(const core::Config& /*config*/) {
    return std::unique_ptr<PhysicsWorld>{std::make_unique<Box2dPhysics>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_physics2d_box2d(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<PhysicsWorld>{.name = "box2d", .create = &create});
}

} // namespace dream::modules
