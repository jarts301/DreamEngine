#include "dream/core/handle.h"
#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/physics3d/physics3d.h"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace physics3d;

// --- Process-wide setup -------------------------------------------------------------------------

void trace(const char* format, ...) {
    char message[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    DREAM_LOG_INFO("Jolt: {}", message);
}

#ifdef JPH_ENABLE_ASSERTS
bool assert_failed(const char* expression, const char* message, const char* file,
                   JPH::uint line) {
    DREAM_LOG_ERROR("Jolt assertion '{}' failed at {}:{}: {}", expression, file, line,
                    message != nullptr ? message : "");
    return true;
}
#endif

// Jolt's type registry and allocator are global, so they live as long as any world does.
std::mutex g_jolt_mutex;
int g_jolt_users = 0;

void acquire_jolt() {
    const std::lock_guard lock(g_jolt_mutex);
    if (g_jolt_users++ == 0) {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = &trace;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &assert_failed;)
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void release_jolt() {
    const std::lock_guard lock(g_jolt_mutex);
    if (--g_jolt_users == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

// The first base of every world, so the global state exists before the world's members are built
// and outlives them.
class JoltUser {
public:
    JoltUser() {
        acquire_jolt();
    }
    ~JoltUser() {
        release_jolt();
    }
    JoltUser(const JoltUser&) = delete;
    JoltUser& operator=(const JoltUser&) = delete;
};

// --- Collision layers ---------------------------------------------------------------------------

// Bodies that never move don't need testing against each other.
namespace layers {
constexpr JPH::ObjectLayer k_still = 0;
constexpr JPH::ObjectLayer k_moving = 1;
constexpr JPH::uint k_count = 2;
} // namespace layers

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override {
        return layers::k_count;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return static_cast<JPH::BroadPhaseLayer::Type>(layer) == layers::k_still ? "still"
                                                                                 : "moving";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase) const override {
        return layer == layers::k_moving ||
               static_cast<JPH::BroadPhaseLayer::Type>(broad_phase) == layers::k_moving;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == layers::k_moving || b == layers::k_moving;
    }
};

// Leaves sensors out of queries.
class SolidBodies final : public JPH::BodyFilter {
public:
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return !body.IsSensor();
    }
};

// --- Conversions --------------------------------------------------------------------------------

JPH::Vec3 to_jolt(core::Vec3 v) {
    return JPH::Vec3(v.x, v.y, v.z);
}

JPH::Quat to_jolt(core::Quat q) {
    return JPH::Quat(q.x, q.y, q.z, q.w).Normalized();
}

core::Vec3 from_jolt(JPH::Vec3 v) {
    return core::Vec3{v.GetX(), v.GetY(), v.GetZ()};
}

core::Quat from_jolt(JPH::Quat q) {
    return core::Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

JPH::EMotionType to_jolt(MotionType motion) {
    switch (motion) {
    case MotionType::Static:
        return JPH::EMotionType::Static;
    case MotionType::Kinematic:
        return JPH::EMotionType::Kinematic;
    case MotionType::Dynamic:
        return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Static;
}

std::uint64_t pack(BodyHandle handle) {
    return (std::uint64_t{handle.index} << 32) | handle.generation;
}

BodyHandle unpack(std::uint64_t value) {
    return BodyHandle{.index = static_cast<std::uint32_t>(value >> 32),
                      .generation = static_cast<std::uint32_t>(value)};
}

core::Result<JPH::RefConst<JPH::Shape>> make_shape(const Shape& shape) {
    JPH::Shape::ShapeResult result;
    if (const auto* box = std::get_if<BoxShape>(&shape)) {
        const core::Vec3 half = box->half_extents;
        // Rounded edges make collisions cheaper and steadier, but can't exceed half the box.
        const float rounding =
            std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({half.x, half.y, half.z}));
        result = JPH::BoxShapeSettings(to_jolt(half), rounding).Create();
    } else if (const auto* sphere = std::get_if<SphereShape>(&shape)) {
        result = JPH::SphereShapeSettings(sphere->radius).Create();
    } else {
        const auto& capsule = std::get<CapsuleShape>(shape);
        result = capsule.half_height > 0.0f
                     ? JPH::CapsuleShapeSettings(capsule.half_height, capsule.radius).Create()
                     : JPH::SphereShapeSettings(capsule.radius).Create();
    }
    if (result.HasError()) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::string("Jolt rejected the shape: ") + result.GetError().c_str()};
    }
    return JPH::RefConst<JPH::Shape>(result.Get());
}

// Any unit vector at right angles to `axis`.
JPH::Vec3 perpendicular(JPH::Vec3 axis) {
    return axis.GetNormalizedPerpendicular();
}

// --- Contacts -----------------------------------------------------------------------------------

struct RawContact {
    ContactEvent::Kind kind;
    JPH::BodyID body_a;
    JPH::BodyID body_b;
    core::Vec3 point;
    core::Vec3 normal;
};

// Jolt reports contacts from its worker threads, so they are collected under a lock and turned into
// events once the step is over.
class ContactCollector final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& body_a, const JPH::Body& body_b,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& /*settings*/) override {
        const JPH::Vec3 point = manifold.mRelativeContactPointsOn1.empty()
                                    ? JPH::Vec3(manifold.mBaseOffset)
                                    : JPH::Vec3(manifold.GetWorldSpaceContactPointOn1(0));
        const std::lock_guard lock(m_mutex);
        m_contacts.push_back(RawContact{ContactEvent::Kind::Began, body_a.GetID(), body_b.GetID(),
                                        from_jolt(point), from_jolt(manifold.mWorldSpaceNormal)});
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        const std::lock_guard lock(m_mutex);
        m_contacts.push_back(RawContact{ContactEvent::Kind::Ended, pair.GetBody1ID(),
                                        pair.GetBody2ID(), core::Vec3{}, core::Vec3{}});
    }

    // Called on the main thread, while no step is running.
    std::vector<RawContact> take() {
        const std::lock_guard lock(m_mutex);
        std::vector<RawContact> taken;
        taken.swap(m_contacts);
        return taken;
    }

private:
    std::mutex m_mutex;
    std::vector<RawContact> m_contacts;
};

// --- The world ----------------------------------------------------------------------------------

struct BodyRecord {
    JPH::BodyID id;
    MotionType motion = MotionType::Dynamic;
};

struct JointRecord {
    JPH::Ref<JPH::Constraint> constraint;
    BodyHandle body_a;
    BodyHandle body_b;
};

// What contacts need to know about a body, kept for one step after it is destroyed so contacts
// that end because of it can still be reported.
struct BodyIdentity {
    BodyHandle handle;
    std::uint64_t user_data = 0;
};

struct Limits {
    JPH::uint max_bodies = 65536;
    JPH::uint max_body_pairs = 65536;
    JPH::uint max_contact_constraints = 16384;
    int threads = 0;
};

class JoltPhysics final : private JoltUser, public PhysicsWorld {
public:
    explicit JoltPhysics(const Limits& limits)
        : m_temp_allocator(16 * 1024 * 1024),
          m_job_system(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, limits.threads) {
        m_system.Init(limits.max_bodies, 0, limits.max_body_pairs, limits.max_contact_constraints,
                      m_broad_phase_layers, m_object_vs_broad_phase, m_object_pairs);
        m_system.SetContactListener(&m_contacts);
        m_system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    }

    ~JoltPhysics() override {
        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        m_joints.for_each([&](core::Handle<JointRecord>, JointRecord& joint) {
            m_system.RemoveConstraint(joint.constraint);
        });
        m_bodies.for_each([&](core::Handle<BodyRecord>, BodyRecord& body) {
            bodies.RemoveBody(body.id);
            bodies.DestroyBody(body.id);
        });
    }

    JoltPhysics(const JoltPhysics&) = delete;
    JoltPhysics& operator=(const JoltPhysics&) = delete;

    void set_gravity(core::Vec3 gravity) override {
        m_system.SetGravity(to_jolt(gravity));
    }

    core::Vec3 gravity() const override {
        return from_jolt(m_system.GetGravity());
    }

    core::Result<BodyHandle> create_body(const BodyDesc& desc) override {
        if (core::Result<void> valid = validate(desc); !valid.has_value()) {
            return valid.error();
        }
        core::Result<JPH::RefConst<JPH::Shape>> shape = make_shape(desc.shape);
        if (!shape.has_value()) {
            return shape.error();
        }

        const bool moves = desc.motion != MotionType::Static;
        JPH::BodyCreationSettings settings(shape.value(), JPH::RVec3(to_jolt(desc.position)),
                                           to_jolt(desc.rotation), to_jolt(desc.motion),
                                           moves ? layers::k_moving : layers::k_still);
        settings.mFriction = desc.friction;
        settings.mRestitution = desc.restitution;
        settings.mLinearDamping = desc.linear_damping;
        settings.mAngularDamping = desc.angular_damping;
        settings.mGravityFactor = desc.gravity_scale;
        settings.mIsSensor = desc.is_sensor;
        settings.mMotionQuality = desc.continuous_collision ? JPH::EMotionQuality::LinearCast
                                                            : JPH::EMotionQuality::Discrete;
        if (moves) {
            settings.mLinearVelocity = to_jolt(desc.linear_velocity);
            settings.mAngularVelocity = to_jolt(desc.angular_velocity);
        }
        if (desc.motion == MotionType::Dynamic) {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = desc.mass;
        }
        // A kinematic sensor still has to notice static bodies, such as a trigger on a moving door.
        settings.mCollideKinematicVsNonDynamic = desc.is_sensor;

        const core::Handle<BodyRecord> handle = m_bodies.create(BodyRecord{});
        settings.mUserData = pack(handle_cast<BodyTag>(handle));

        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        JPH::Body* const body = bodies.CreateBody(settings);
        if (body == nullptr) {
            m_bodies.destroy(handle);
            return core::Error{core::ErrorCode::OutOfMemory,
                               "The physics world has reached its body limit"};
        }
        bodies.AddBody(body->GetID(),
                       moves ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

        BodyRecord& record = *m_bodies.get(handle);
        record.id = body->GetID();
        record.motion = desc.motion;
        m_identities[body->GetID().GetIndexAndSequenceNumber()] =
            BodyIdentity{handle_cast<BodyTag>(handle), desc.user_data};
        return handle_cast<BodyTag>(handle);
    }

    void destroy_body(BodyHandle body) override {
        const BodyRecord* const record = m_bodies.get(handle_cast<BodyRecord>(body));
        if (record == nullptr) {
            return;
        }
        m_joints.for_each([&](core::Handle<JointRecord> handle, JointRecord& joint) {
            if (joint.body_a == body || joint.body_b == body) {
                m_system.RemoveConstraint(joint.constraint);
                m_joints.destroy(handle);
            }
        });

        JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        bodies.RemoveBody(record->id);
        bodies.DestroyBody(record->id);
        m_forgotten.push_back(record->id.GetIndexAndSequenceNumber());
        m_bodies.destroy(handle_cast<BodyRecord>(body));
    }

    std::optional<BodyState> body_state(BodyHandle body) const override {
        const BodyRecord* const record = m_bodies.get(handle_cast<BodyRecord>(body));
        if (record == nullptr) {
            return std::nullopt;
        }
        const JPH::BodyInterface& bodies = m_system.GetBodyInterface();
        JPH::RVec3 position;
        JPH::Quat rotation;
        bodies.GetPositionAndRotation(record->id, position, rotation);
        return BodyState{
            .position = from_jolt(JPH::Vec3(position)),
            .rotation = from_jolt(rotation),
            .linear_velocity = from_jolt(bodies.GetLinearVelocity(record->id)),
            .angular_velocity = from_jolt(bodies.GetAngularVelocity(record->id)),
            .is_awake = bodies.IsActive(record->id),
        };
    }

    void set_transform(BodyHandle body, core::Vec3 position, core::Quat rotation) override {
        if (const BodyRecord* const record = find(body)) {
            m_system.GetBodyInterface().SetPositionAndRotation(
                record->id, JPH::RVec3(to_jolt(position)), to_jolt(rotation),
                record->motion == MotionType::Static ? JPH::EActivation::DontActivate
                                                     : JPH::EActivation::Activate);
        }
    }

    void move_kinematic(BodyHandle body, core::Vec3 position, core::Quat rotation,
                        float step_seconds) override {
        const BodyRecord* const record = find(body);
        if (record != nullptr && record->motion == MotionType::Kinematic && step_seconds > 0.0f) {
            m_system.GetBodyInterface().MoveKinematic(record->id, JPH::RVec3(to_jolt(position)),
                                                      to_jolt(rotation), step_seconds);
        }
    }

    void set_linear_velocity(BodyHandle body, core::Vec3 velocity) override {
        if (const BodyRecord* const record = find_moving(body)) {
            JPH::BodyInterface& bodies = m_system.GetBodyInterface();
            bodies.SetLinearVelocity(record->id, to_jolt(velocity));
            bodies.ActivateBody(record->id);
        }
    }

    void set_angular_velocity(BodyHandle body, core::Vec3 velocity) override {
        if (const BodyRecord* const record = find_moving(body)) {
            JPH::BodyInterface& bodies = m_system.GetBodyInterface();
            bodies.SetAngularVelocity(record->id, to_jolt(velocity));
            bodies.ActivateBody(record->id);
        }
    }

    void add_force(BodyHandle body, core::Vec3 force) override {
        if (const BodyRecord* const record = find_dynamic(body)) {
            m_system.GetBodyInterface().AddForce(record->id, to_jolt(force));
        }
    }

    void add_impulse(BodyHandle body, core::Vec3 impulse) override {
        if (const BodyRecord* const record = find_dynamic(body)) {
            m_system.GetBodyInterface().AddImpulse(record->id, to_jolt(impulse));
        }
    }

    core::Result<JointHandle> create_joint(const JointDesc& desc) override {
        const BodyRecord* const a = find(desc.body_a);
        const BodyRecord* const b = desc.body_b.is_valid() ? find(desc.body_b) : nullptr;
        if (a == nullptr || (desc.body_b.is_valid() && b == nullptr) || desc.body_a == desc.body_b) {
            return invalid("A joint needs two different bodies that exist");
        }

        core::Result<JPH::Ref<JPH::TwoBodyConstraintSettings>> settings = make_joint(desc.type);
        if (!settings.has_value()) {
            return settings.error();
        }

        // Jolt needs both bodies locked while it reads their starting positions. Jolt measures its
        // second body against its first, and the interface measures body_a against body_b, so body_b
        // goes first.
        const JPH::BodyID ids[] = {a->id, b != nullptr ? b->id : JPH::BodyID()};
        JPH::BodyLockMultiWrite lock(m_system.GetBodyLockInterface(), ids, b != nullptr ? 2 : 1);
        JPH::Body* const body_a = lock.GetBody(0);
        JPH::Body* const body_b = b != nullptr ? lock.GetBody(1) : &JPH::Body::sFixedToWorld;
        if (body_a == nullptr || body_b == nullptr) {
            return invalid("A joint's bodies could not be locked");
        }
        JPH::Ref<JPH::Constraint> constraint = settings.value()->Create(*body_b, *body_a);
        lock.ReleaseLocks();

        m_system.AddConstraint(constraint);
        m_system.GetBodyInterface().ActivateBody(a->id);
        if (b != nullptr) {
            m_system.GetBodyInterface().ActivateBody(b->id);
        }
        return handle_cast<JointTag>(
            m_joints.create(JointRecord{std::move(constraint), desc.body_a, desc.body_b}));
    }

    void destroy_joint(JointHandle joint) override {
        if (JointRecord* const record = m_joints.get(handle_cast<JointRecord>(joint))) {
            m_system.RemoveConstraint(record->constraint);
            m_joints.destroy(handle_cast<JointRecord>(joint));
        }
    }

    void step(float step_seconds) override {
        if (step_seconds <= 0.0f) {
            return;
        }
        // Collision steps keep fast bodies stable when the game's fixed step is long.
        const int collision_steps = std::max(1, static_cast<int>(std::ceil(step_seconds * 60.0f - 0.01f)));
        const JPH::EPhysicsUpdateError error =
            m_system.Update(step_seconds, collision_steps, &m_temp_allocator, &m_job_system);
        if (error != JPH::EPhysicsUpdateError::None) {
            DREAM_LOG_WARNING("Jolt: the step ran out of space for contacts or body pairs (error "
                              "{}); raise the limits under [physics3d.jolt]",
                              static_cast<unsigned>(error));
        }

        m_events.clear();
        for (const RawContact& contact : m_contacts.take()) {
            const BodyIdentity* const a = identity(contact.body_a);
            const BodyIdentity* const b = identity(contact.body_b);
            if (a == nullptr || b == nullptr) {
                continue;
            }
            m_events.push_back(ContactEvent{
                .kind = contact.kind,
                .body_a = a->handle,
                .body_b = b->handle,
                .user_data_a = a->user_data,
                .user_data_b = b->user_data,
                .point = contact.point,
                .normal = contact.normal,
            });
        }

        for (const std::uint32_t id : m_forgotten) {
            m_identities.erase(id);
        }
        m_forgotten.clear();
    }

    std::span<const ContactEvent> contact_events() const override {
        return m_events;
    }

    std::optional<RayHit> cast_ray(core::Vec3 origin, core::Vec3 direction,
                                   float max_distance) const override {
        const float length = core::length(direction);
        if (length <= 0.0f || !(max_distance > 0.0f)) {
            return std::nullopt;
        }
        const JPH::RRayCast ray{JPH::RVec3(to_jolt(origin)),
                                to_jolt(direction / length * max_distance)};
        JPH::RayCastResult hit;
        const SolidBodies solid;
        if (!m_system.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, solid)) {
            return std::nullopt;
        }

        const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
        RayHit result{
            .point = from_jolt(JPH::Vec3(point)),
            .distance = hit.mFraction * max_distance,
        };
        const JPH::BodyLockRead lock(m_system.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            result.normal = from_jolt(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
            result.body = unpack(body.GetUserData());
        }
        if (const BodyIdentity* const found = identity(hit.mBodyID)) {
            result.user_data = found->user_data;
        }
        return result;
    }

    void overlap(const Shape& shape, core::Vec3 position, core::Quat rotation,
                 std::vector<BodyHandle>& bodies) const override {
        bodies.clear();
        if (!detail::is_valid_shape(shape)) {
            return;
        }
        const core::Result<JPH::RefConst<JPH::Shape>> query = make_shape(shape);
        if (!query.has_value()) {
            return;
        }

        JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
        m_system.GetNarrowPhaseQuery().CollideShape(
            query.value(), JPH::Vec3::sReplicate(1.0f),
            JPH::RMat44::sRotationTranslation(to_jolt(rotation), JPH::RVec3(to_jolt(position))),
            JPH::CollideShapeSettings(), JPH::RVec3::sZero(), collector);

        for (const JPH::CollideShapeResult& hit : collector.mHits) {
            if (const BodyIdentity* const found = identity(hit.mBodyID2)) {
                if (std::find(bodies.begin(), bodies.end(), found->handle) == bodies.end()) {
                    bodies.push_back(found->handle);
                }
            }
        }
    }

    std::size_t body_count() const override {
        return m_bodies.size();
    }

private:
    static core::Error invalid(const char* message) {
        return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
    }

    static core::Result<JPH::Ref<JPH::TwoBodyConstraintSettings>>
    make_joint(const JointType& type) {
        if (std::holds_alternative<FixedJoint>(type)) {
            auto settings = new JPH::FixedConstraintSettings();
            settings->mAutoDetectPoint = true;
            return JPH::Ref<JPH::TwoBodyConstraintSettings>(settings);
        }
        if (const auto* point = std::get_if<PointJoint>(&type)) {
            auto settings = new JPH::PointConstraintSettings();
            settings->mPoint1 = settings->mPoint2 = JPH::RVec3(to_jolt(point->anchor));
            return JPH::Ref<JPH::TwoBodyConstraintSettings>(settings);
        }
        if (const auto* hinge = std::get_if<HingeJoint>(&type)) {
            if (!(core::length(hinge->axis) > 0.0f)) {
                return invalid("A hinge needs an axis");
            }
            const float lowest = hinge->min_angle_radians.value_or(-core::k_pi);
            const float highest = hinge->max_angle_radians.value_or(core::k_pi);
            if (!(lowest <= 0.0f && highest >= 0.0f && lowest >= -core::k_pi &&
                  highest <= core::k_pi)) {
                return invalid("A hinge's limits must be within -pi and pi, around zero");
            }
            auto settings = new JPH::HingeConstraintSettings();
            const JPH::Vec3 axis = to_jolt(hinge->axis).Normalized();
            settings->mPoint1 = settings->mPoint2 = JPH::RVec3(to_jolt(hinge->anchor));
            settings->mHingeAxis1 = settings->mHingeAxis2 = axis;
            settings->mNormalAxis1 = settings->mNormalAxis2 = perpendicular(axis);
            settings->mLimitsMin = lowest;
            settings->mLimitsMax = highest;
            return JPH::Ref<JPH::TwoBodyConstraintSettings>(settings);
        }
        const auto& distance = std::get<DistanceJoint>(type);
        if (!(distance.min_distance >= 0.0f && distance.max_distance >= distance.min_distance)) {
            return invalid("A distance joint needs 0 <= min_distance <= max_distance");
        }
        auto settings = new JPH::DistanceConstraintSettings();
        settings->mPoint1 = JPH::RVec3(to_jolt(distance.anchor_b));
        settings->mPoint2 = JPH::RVec3(to_jolt(distance.anchor_a));
        settings->mMinDistance = distance.min_distance;
        settings->mMaxDistance = distance.max_distance;
        return JPH::Ref<JPH::TwoBodyConstraintSettings>(settings);
    }

    const BodyIdentity* identity(JPH::BodyID id) const {
        const auto found = m_identities.find(id.GetIndexAndSequenceNumber());
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

    BroadPhaseLayers m_broad_phase_layers;
    ObjectVsBroadPhase m_object_vs_broad_phase;
    ObjectPairs m_object_pairs;
    JPH::TempAllocatorImpl m_temp_allocator;
    JPH::JobSystemThreadPool m_job_system;
    ContactCollector m_contacts;
    JPH::PhysicsSystem m_system;

    core::HandlePool<BodyRecord> m_bodies;
    core::HandlePool<JointRecord> m_joints;
    std::unordered_map<std::uint32_t, BodyIdentity> m_identities;
    std::vector<std::uint32_t> m_forgotten;
    std::vector<ContactEvent> m_events;
};

core::Result<std::unique_ptr<PhysicsWorld>> create(const core::Config& config) {
    Limits limits;
    const auto read = [&](const char* key, JPH::uint& value) {
        if (const std::optional<std::int64_t> set = config.get_int(key); set.has_value()) {
            value = static_cast<JPH::uint>(std::clamp<std::int64_t>(*set, 16, 1 << 22));
        }
    };
    read("physics3d.jolt.max_bodies", limits.max_bodies);
    read("physics3d.jolt.max_body_pairs", limits.max_body_pairs);
    read("physics3d.jolt.max_contact_constraints", limits.max_contact_constraints);

    // Worker threads besides the caller's. The default leaves cores for rendering and the OS.
    const int cores = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    limits.threads = static_cast<int>(
        config.get_int("physics3d.jolt.threads").value_or(std::clamp(cores - 2, 1, 4)));
    limits.threads = std::clamp(limits.threads, 0, 64);

    return std::unique_ptr<PhysicsWorld>{std::make_unique<JoltPhysics>(limits)};
}

} // namespace

[[nodiscard]] core::Result<void> register_physics3d_jolt(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<PhysicsWorld>{.name = "jolt", .create = &create});
}

} // namespace dream::modules
