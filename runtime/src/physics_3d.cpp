#include "dream/runtime/physics_3d.h"

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/event_bus.h"
#include "dream/runtime/scene_loader.h"

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace dream::runtime {
namespace {

using core::Quat;
using core::Vec3;
using core::Vec4;
using namespace physics3d;

std::uint64_t key_of(BodyHandle body) {
    return (std::uint64_t{body.index} << 32) | body.generation;
}

std::uint64_t to_user_data(Entity entity) {
    return static_cast<std::uint64_t>(entt::to_integral(entity));
}

Entity from_user_data(std::uint64_t value) {
    return static_cast<Entity>(static_cast<entt::id_type>(value));
}

core::Error scene_error(std::string message) {
    return core::Error{core::ErrorCode::ParseError, std::move(message)};
}

core::Result<MotionType> read_motion(const ComponentData& data) {
    const core::Result<std::string> motion = data.text("motion", "dynamic");
    if (!motion.has_value()) {
        return motion.error();
    }
    if (motion.value() == "static") {
        return MotionType::Static;
    }
    if (motion.value() == "kinematic") {
        return MotionType::Kinematic;
    }
    if (motion.value() == "dynamic") {
        return MotionType::Dynamic;
    }
    return scene_error(std::format("unknown motion '{}'", motion.value()));
}

// Reads each setting into `target`, stopping at the first error.
struct Reader {
    const ComponentData& data;
    std::optional<core::Error> error;

    void number(std::string_view key, float& target) {
        if (!error.has_value()) {
            core::Result<float> value = data.number(key, target);
            if (value.has_value()) {
                target = value.value();
            } else {
                error = value.error();
            }
        }
    }

    void vec3(std::string_view key, Vec3& target) {
        if (!error.has_value()) {
            core::Result<Vec3> value = data.vec3(key, target);
            if (value.has_value()) {
                target = value.value();
            } else {
                error = value.error();
            }
        }
    }

    void flag(std::string_view key, bool& target) {
        if (!error.has_value()) {
            core::Result<bool> value = data.flag(key, target);
            if (value.has_value()) {
                target = value.value();
            } else {
                error = value.error();
            }
        }
    }

    void angle(std::string_view key, std::optional<float>& target) {
        if (!error.has_value() && data.has(key)) {
            float degrees = 0.0f;
            number(key, degrees);
            target = core::radians(degrees);
        }
    }
};

core::Result<void> read_rigid_body(ComponentContext& context) {
    const ComponentData& data = context.data;
    if (core::Result<void> known = data.check_keys(
            {"motion", "shape", "half_extents", "radius", "half_height", "mass", "friction",
             "restitution", "linear_damping", "angular_damping", "gravity_scale", "velocity",
             "angular_velocity", "sensor", "continuous"});
        !known.has_value()) {
        return known;
    }

    RigidBody3D body;
    const core::Result<MotionType> motion = read_motion(data);
    if (!motion.has_value()) {
        return motion.error();
    }
    body.desc.motion = motion.value();

    const core::Result<std::string> shape = data.text("shape", "box");
    if (!shape.has_value()) {
        return shape.error();
    }

    Reader read{data, std::nullopt};
    if (shape.value() == "box") {
        BoxShape box;
        read.vec3("half_extents", box.half_extents);
        body.desc.shape = box;
    } else if (shape.value() == "sphere") {
        SphereShape sphere;
        read.number("radius", sphere.radius);
        body.desc.shape = sphere;
    } else if (shape.value() == "capsule") {
        CapsuleShape capsule;
        read.number("radius", capsule.radius);
        read.number("half_height", capsule.half_height);
        body.desc.shape = capsule;
    } else {
        return scene_error(std::format("unknown shape '{}'", shape.value()));
    }

    read.number("mass", body.desc.mass);
    read.number("friction", body.desc.friction);
    read.number("restitution", body.desc.restitution);
    read.number("linear_damping", body.desc.linear_damping);
    read.number("angular_damping", body.desc.angular_damping);
    read.number("gravity_scale", body.desc.gravity_scale);
    read.vec3("velocity", body.desc.linear_velocity);
    read.vec3("angular_velocity", body.desc.angular_velocity);
    read.flag("sensor", body.desc.is_sensor);
    read.flag("continuous", body.desc.continuous_collision);
    if (read.error.has_value()) {
        return *read.error;
    }

    // Caught here, so the mistake is reported with the scene file's name.
    if (core::Result<void> valid = validate(body.desc); !valid.has_value()) {
        return valid;
    }
    context.world.registry().emplace_or_replace<RigidBody3D>(context.entity, body);
    return {};
}

core::Result<void> read_joint(ComponentContext& context) {
    const ComponentData& data = context.data;
    if (core::Result<void> known = data.check_keys(
            {"type", "connected", "anchor", "axis", "min_angle", "max_angle", "connected_anchor",
             "min_distance", "max_distance"});
        !known.has_value()) {
        return known;
    }

    Joint3D joint;
    const core::Result<std::string> type = data.text("type", "point");
    if (!type.has_value()) {
        return type.error();
    }
    if (type.value() == "fixed") {
        joint.type = Joint3D::Type::Fixed;
    } else if (type.value() == "point") {
        joint.type = Joint3D::Type::Point;
    } else if (type.value() == "hinge") {
        joint.type = Joint3D::Type::Hinge;
    } else if (type.value() == "distance") {
        joint.type = Joint3D::Type::Distance;
    } else {
        return scene_error(std::format("unknown joint type '{}'", type.value()));
    }

    if (data.has("connected")) {
        const core::Result<std::string> name = data.text("connected");
        if (!name.has_value()) {
            return name.error();
        }
        joint.connected = context.world.find(name.value());
        if (joint.connected == k_no_entity) {
            return scene_error(std::format("no entity named '{}'", name.value()));
        }
    }

    Reader read{data, std::nullopt};
    read.vec3("anchor", joint.anchor);
    read.vec3("axis", joint.axis);
    read.angle("min_angle", joint.min_angle_radians);
    read.angle("max_angle", joint.max_angle_radians);
    read.vec3("connected_anchor", joint.connected_anchor);
    read.number("min_distance", joint.min_distance);
    read.number("max_distance", joint.max_distance);
    if (read.error.has_value()) {
        return *read.error;
    }
    context.world.registry().emplace_or_replace<Joint3D>(context.entity, joint);
    return {};
}

Vec3 to_world(const Transform& transform, Vec3 local) {
    return transform.position + core::rotate(transform.rotation, local);
}

} // namespace

Physics3D::~Physics3D() {
    clear();
}

core::Result<void> Physics3D::initialize(App& app) {
    core::Result<std::unique_ptr<PhysicsWorld>> backend = app.create_module<PhysicsWorld>();
    if (!backend.has_value()) {
        return backend.error();
    }
    return initialize(app, std::move(backend).value());
}

core::Result<void> Physics3D::initialize(App& app, std::unique_ptr<PhysicsWorld> backend) {
    if (backend == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument, "Physics3D needs a backend"};
    }
    if (const auto gravity = app.config().get_float_list("physics3d.gravity");
        gravity.has_value()) {
        if (gravity->size() != 3) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Setting 'physics3d.gravity' must be a list of three numbers"};
        }
        backend->set_gravity(Vec3{static_cast<float>((*gravity)[0]),
                                  static_cast<float>((*gravity)[1]),
                                  static_cast<float>((*gravity)[2])});
    } else if (app.config().contains("physics3d.gravity")) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "Setting 'physics3d.gravity' must be a list of three numbers"};
    }

    clear();
    m_backend = std::move(backend);
    m_events = &app.events();
    app.scenes().register_component("rigid_body", &read_rigid_body);
    app.scenes().register_component("joint", &read_joint);
    return {};
}

void Physics3D::clear() {
    if (m_backend != nullptr) {
        for (const auto& [entity, joint] : m_joints) {
            m_backend->destroy_joint(joint.joint);
        }
        for (const auto& [entity, body] : m_bodies) {
            m_backend->destroy_body(body);
        }
    }
    m_joints.clear();
    m_bodies.clear();
    m_entities.clear();
    m_contact_points.clear();
}

void Physics3D::attach(World* world) {
    clear();
    m_world = world;
}

BodyHandle Physics3D::body_of(Entity entity) const {
    const auto found = m_bodies.find(entity);
    return found == m_bodies.end() ? BodyHandle{} : found->second;
}

Entity Physics3D::entity_of(BodyHandle body) const {
    const auto found = m_entities.find(key_of(body));
    return found == m_entities.end() ? k_no_entity : found->second;
}

void Physics3D::remove_stale() {
    Registry& registry = m_world->registry();

    for (auto it = m_bodies.begin(); it != m_bodies.end();) {
        const RigidBody3D* const body =
            registry.valid(it->first) ? registry.try_get<RigidBody3D>(it->first) : nullptr;
        if (body == nullptr || body->body != it->second) {
            m_backend->destroy_body(it->second);
            m_entities.erase(key_of(it->second));
            if (registry.valid(it->first)) {
                registry.remove<PhysicsPose3D>(it->first);
            }
            it = m_bodies.erase(it);
        } else {
            ++it;
        }
    }

    // After the bodies, so a joint whose body was just rebuilt is rebuilt too.
    for (auto it = m_joints.begin(); it != m_joints.end();) {
        const Joint3D* const joint =
            registry.valid(it->first) ? registry.try_get<Joint3D>(it->first) : nullptr;
        const TrackedJoint& tracked = it->second;
        const bool bodies_changed =
            joint == nullptr || body_of(it->first) != tracked.body_a ||
            (joint->connected != k_no_entity && body_of(joint->connected) != tracked.body_b);
        if (joint == nullptr || joint->joint != tracked.joint || bodies_changed) {
            m_backend->destroy_joint(tracked.joint);
            it = m_joints.erase(it);
        } else {
            ++it;
        }
    }
}

void Physics3D::create_bodies() {
    Registry& registry = m_world->registry();
    for (const auto [entity, body, transform] :
         registry.view<RigidBody3D, const Transform>().each()) {
        if (body.failed || m_bodies.contains(entity)) {
            continue;
        }
        if (registry.all_of<Parent>(entity)) {
            DREAM_LOG_ERROR("Physics3D: entity {} has a parent, so it can't have a rigid body",
                            entt::to_integral(entity));
            body.failed = true;
            continue;
        }

        BodyDesc desc = body.desc;
        desc.position = transform.position;
        desc.rotation = transform.rotation;
        desc.user_data = to_user_data(entity);
        const core::Result<BodyHandle> created = m_backend->create_body(desc);
        if (!created.has_value()) {
            DREAM_LOG_ERROR("Physics3D: entity {}'s body: {}", entt::to_integral(entity),
                            created.error().message);
            body.failed = true;
            continue;
        }

        body.body = created.value();
        m_bodies.emplace(entity, created.value());
        m_entities.emplace(key_of(created.value()), entity);
        if (desc.motion == MotionType::Dynamic) {
            registry.emplace_or_replace<PhysicsPose3D>(entity, transform.position,
                                                       transform.rotation, transform.position,
                                                       transform.rotation);
        }
    }
}

void Physics3D::create_joints() {
    Registry& registry = m_world->registry();
    for (const auto [entity, joint] : registry.view<Joint3D>().each()) {
        if (joint.failed || m_joints.contains(entity)) {
            continue;
        }
        const BodyHandle body_a = body_of(entity);
        const BodyHandle body_b =
            joint.connected == k_no_entity ? BodyHandle{} : body_of(joint.connected);
        const bool connected_missing =
            joint.connected != k_no_entity &&
            (!registry.valid(joint.connected) || !registry.all_of<RigidBody3D>(joint.connected));
        if (!registry.all_of<RigidBody3D>(entity) || connected_missing) {
            DREAM_LOG_ERROR("Physics3D: entity {}'s joint needs a rigid body on both ends",
                            entt::to_integral(entity));
            joint.failed = true;
            continue;
        }
        // A body that failed is reported already; one not yet created waits for the next step.
        if (!body_a.is_valid() || (joint.connected != k_no_entity && !body_b.is_valid())) {
            const bool gave_up =
                registry.get<RigidBody3D>(entity).failed ||
                (joint.connected != k_no_entity && registry.get<RigidBody3D>(joint.connected).failed);
            joint.failed = gave_up;
            continue;
        }

        const Transform& own = registry.get<Transform>(entity);
        const Vec3 anchor = to_world(own, joint.anchor);
        JointType type;
        switch (joint.type) {
        case Joint3D::Type::Fixed:
            type = FixedJoint{};
            break;
        case Joint3D::Type::Point:
            type = PointJoint{anchor};
            break;
        case Joint3D::Type::Hinge:
            type = HingeJoint{anchor, core::rotate(own.rotation, joint.axis),
                              joint.min_angle_radians, joint.max_angle_radians};
            break;
        case Joint3D::Type::Distance: {
            const Vec3 other = joint.connected == k_no_entity
                                   ? joint.connected_anchor
                                   : to_world(registry.get<Transform>(joint.connected),
                                              joint.connected_anchor);
            type = DistanceJoint{anchor, other, joint.min_distance, joint.max_distance};
            break;
        }
        }

        const core::Result<JointHandle> created =
            m_backend->create_joint(JointDesc{body_a, body_b, type});
        if (!created.has_value()) {
            DREAM_LOG_ERROR("Physics3D: entity {}'s joint: {}", entt::to_integral(entity),
                            created.error().message);
            joint.failed = true;
            continue;
        }
        joint.joint = created.value();
        m_joints.emplace(entity, TrackedJoint{created.value(), body_a, body_b});
    }
}

void Physics3D::step(float step_seconds) {
    if (m_backend == nullptr || m_world == nullptr || !(step_seconds > 0.0f)) {
        return;
    }
    remove_stale();
    create_bodies();
    create_joints();

    Registry& registry = m_world->registry();
    for (const auto [entity, body, transform] :
         registry.view<const RigidBody3D, const Transform>().each()) {
        if (body.desc.motion == MotionType::Kinematic && m_bodies.contains(entity)) {
            m_backend->move_kinematic(body.body, transform.position, transform.rotation,
                                      step_seconds);
        }
    }

    m_backend->step(step_seconds);

    for (const auto [entity, body, pose] : registry.view<const RigidBody3D, PhysicsPose3D>().each()) {
        if (const std::optional<BodyState> state = m_backend->body_state(body.body)) {
            pose.previous_position = pose.position;
            pose.previous_rotation = pose.rotation;
            pose.position = state->position;
            pose.rotation = state->rotation;
        }
    }

    // Copied first, because a listener may add or remove bodies.
    const std::span<const ContactEvent> raw = m_backend->contact_events();
    std::vector<Contact3D> contacts;
    contacts.reserve(raw.size());
    m_contact_points.clear();
    for (const ContactEvent& event : raw) {
        contacts.push_back(Contact3D{
            .kind = event.kind,
            .a = from_user_data(event.user_data_a),
            .b = from_user_data(event.user_data_b),
            .point = event.point,
            .normal = event.normal,
        });
        if (event.kind == ContactEvent::Kind::Began) {
            m_contact_points.push_back(event.point);
        }
    }
    if (m_events != nullptr) {
        for (const Contact3D& contact : contacts) {
            m_events->publish(contact);
        }
    }
}

void Physics3D::update_transforms(float alpha) {
    if (m_world == nullptr) {
        return;
    }
    const float t = std::clamp(alpha, 0.0f, 1.0f);
    for (const auto [entity, pose, transform] :
         m_world->registry().view<const PhysicsPose3D, Transform>().each()) {
        transform.position = core::lerp(pose.previous_position, pose.position, t);
        transform.rotation = core::slerp(pose.previous_rotation, pose.rotation, t);
    }
}

void Physics3D::debug_draw(DebugLines& lines) const {
    if (m_backend == nullptr || m_world == nullptr) {
        return;
    }
    constexpr Vec4 k_static{0.55f, 0.55f, 0.55f, 1.0f};
    constexpr Vec4 k_kinematic{0.25f, 0.55f, 1.0f, 1.0f};
    constexpr Vec4 k_awake{0.2f, 1.0f, 0.3f, 1.0f};
    constexpr Vec4 k_asleep{0.1f, 0.45f, 0.15f, 1.0f};
    constexpr Vec4 k_sensor{1.0f, 0.85f, 0.1f, 1.0f};
    constexpr Vec4 k_contact{1.0f, 0.2f, 0.2f, 1.0f};
    constexpr Vec4 k_joint{1.0f, 0.4f, 1.0f, 1.0f};

    const Registry& registry = m_world->registry();
    for (const auto& [entity, handle] : m_bodies) {
        const std::optional<BodyState> state = m_backend->body_state(handle);
        const RigidBody3D* const body = registry.try_get<RigidBody3D>(entity);
        if (!state.has_value() || body == nullptr) {
            continue;
        }
        const BodyDesc& desc = body->desc;
        Vec4 color = k_awake;
        if (desc.is_sensor) {
            color = k_sensor;
        } else if (desc.motion == MotionType::Static) {
            color = k_static;
        } else if (desc.motion == MotionType::Kinematic) {
            color = k_kinematic;
        } else if (!state->is_awake) {
            color = k_asleep;
        }

        if (const auto* box = std::get_if<BoxShape>(&desc.shape)) {
            lines.box(state->position, state->rotation, box->half_extents, color);
        } else if (const auto* sphere = std::get_if<SphereShape>(&desc.shape)) {
            lines.sphere(state->position, state->rotation, sphere->radius, color);
        } else {
            const auto& capsule = std::get<CapsuleShape>(desc.shape);
            lines.capsule(state->position, state->rotation, capsule.half_height, capsule.radius,
                          color);
        }
    }

    // A line from each jointed body to its anchor's current place.
    for (const auto& [entity, tracked] : m_joints) {
        const Joint3D* const joint = registry.try_get<Joint3D>(entity);
        const std::optional<BodyState> state = m_backend->body_state(body_of(entity));
        if (joint == nullptr || !state.has_value()) {
            continue;
        }
        const Vec3 anchor = state->position + core::rotate(state->rotation, joint->anchor);
        lines.line(state->position, anchor, k_joint);
        lines.cross(anchor, 0.1f, k_joint);
    }

    for (const Vec3& point : m_contact_points) {
        lines.cross(point, 0.15f, k_contact);
    }
}

std::optional<BodyState> Physics3D::body_state(Entity entity) const {
    const BodyHandle body = body_of(entity);
    return m_backend != nullptr && body.is_valid() ? m_backend->body_state(body) : std::nullopt;
}

void Physics3D::add_force(Entity entity, Vec3 force) {
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->add_force(body, force);
    }
}

void Physics3D::add_impulse(Entity entity, Vec3 impulse) {
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->add_impulse(body, impulse);
    }
}

void Physics3D::set_linear_velocity(Entity entity, Vec3 velocity) {
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->set_linear_velocity(body, velocity);
    }
}

void Physics3D::set_angular_velocity(Entity entity, Vec3 velocity) {
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->set_angular_velocity(body, velocity);
    }
}

void Physics3D::teleport(Entity entity, Vec3 position, Quat rotation) {
    if (m_world == nullptr || !m_world->registry().valid(entity)) {
        return;
    }
    Registry& registry = m_world->registry();
    if (Transform* const transform = registry.try_get<Transform>(entity)) {
        transform->position = position;
        transform->rotation = rotation;
    }
    if (PhysicsPose3D* const pose = registry.try_get<PhysicsPose3D>(entity)) {
        *pose = PhysicsPose3D{position, rotation, position, rotation};
    }
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->set_transform(body, position, rotation);
    }
}

std::optional<RayHit3D> Physics3D::cast_ray(Vec3 origin, Vec3 direction, float max_distance) const {
    if (m_backend == nullptr) {
        return std::nullopt;
    }
    const std::optional<RayHit> hit = m_backend->cast_ray(origin, direction, max_distance);
    if (!hit.has_value()) {
        return std::nullopt;
    }
    return RayHit3D{entity_of(hit->body), hit->point, hit->normal, hit->distance};
}

void Physics3D::overlap(const Shape& shape, Vec3 position, Quat rotation,
                        std::vector<Entity>& entities) const {
    entities.clear();
    if (m_backend == nullptr) {
        return;
    }
    std::vector<BodyHandle> bodies;
    m_backend->overlap(shape, position, rotation, bodies);
    for (const BodyHandle body : bodies) {
        if (const Entity entity = entity_of(body); entity != k_no_entity) {
            entities.push_back(entity);
        }
    }
}

void Physics3D::set_gravity(Vec3 gravity) {
    if (m_backend != nullptr) {
        m_backend->set_gravity(gravity);
    }
}

} // namespace dream::runtime
