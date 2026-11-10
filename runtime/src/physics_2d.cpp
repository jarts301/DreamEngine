#include "dream/runtime/physics_2d.h"

#include "dream/core/log.h"
#include "dream/runtime/app.h"
#include "dream/runtime/event_bus.h"
#include "dream/runtime/scene_loader.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace dream::runtime {
namespace {

using core::Quat;
using core::Vec2;
using core::Vec3;
using core::Vec4;
using namespace physics2d;

constexpr Vec3 k_z_axis{0.0f, 0.0f, 1.0f};

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

Vec2 flat(Vec3 v) {
    return Vec2{v.x, v.y};
}

Vec2 rotated(Vec2 v, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return Vec2{c * v.x - s * v.y, s * v.x + c * v.y};
}

Vec2 to_world(const Transform& transform, Vec2 local) {
    return flat(transform.position) + rotated(local, angle_around_z(transform.rotation));
}

// Reads each setting into `target`, stopping at the first error.
struct Reader {
    const ComponentData& data;
    std::optional<core::Error> error;

    template <typename T, typename Get> void read(std::string_view key, T& target, Get get) {
        if (error.has_value()) {
            return;
        }
        core::Result<T> value = (data.*get)(key, target);
        if (value.has_value()) {
            target = value.value();
        } else {
            error = value.error();
        }
    }

    void number(std::string_view key, float& target) {
        read(key, target, &ComponentData::number);
    }
    void vec2(std::string_view key, Vec2& target) {
        read(key, target, &ComponentData::vec2);
    }
    void flag(std::string_view key, bool& target) {
        read(key, target, &ComponentData::flag);
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
             "angular_velocity", "fixed_rotation", "sensor", "continuous"});
        !known.has_value()) {
        return known;
    }

    RigidBody2D body;
    const core::Result<std::string> motion = data.text("motion", "dynamic");
    if (!motion.has_value()) {
        return motion.error();
    }
    if (motion.value() == "static") {
        body.desc.motion = MotionType::Static;
    } else if (motion.value() == "kinematic") {
        body.desc.motion = MotionType::Kinematic;
    } else if (motion.value() == "dynamic") {
        body.desc.motion = MotionType::Dynamic;
    } else {
        return scene_error(std::format("unknown motion '{}'", motion.value()));
    }

    const core::Result<std::string> shape = data.text("shape", "box");
    if (!shape.has_value()) {
        return shape.error();
    }
    Reader read{data, std::nullopt};
    if (shape.value() == "box") {
        BoxShape box;
        read.vec2("half_extents", box.half_extents);
        body.desc.shape = box;
    } else if (shape.value() == "circle") {
        CircleShape circle;
        read.number("radius", circle.radius);
        body.desc.shape = circle;
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
    read.vec2("velocity", body.desc.linear_velocity);
    read.number("angular_velocity", body.desc.angular_velocity);
    read.flag("fixed_rotation", body.desc.fixed_rotation);
    read.flag("sensor", body.desc.is_sensor);
    read.flag("continuous", body.desc.continuous_collision);
    if (read.error.has_value()) {
        return *read.error;
    }
    if (core::Result<void> valid = validate(body.desc); !valid.has_value()) {
        return valid;
    }
    context.world.registry().emplace_or_replace<RigidBody2D>(context.entity, body);
    return {};
}

core::Result<void> read_joint(ComponentContext& context) {
    const ComponentData& data = context.data;
    if (core::Result<void> known =
            data.check_keys({"type", "connected", "anchor", "min_angle", "max_angle",
                             "connected_anchor", "min_distance", "max_distance"});
        !known.has_value()) {
        return known;
    }

    Joint2D joint;
    const core::Result<std::string> type = data.text("type", "hinge");
    if (!type.has_value()) {
        return type.error();
    }
    if (type.value() == "fixed") {
        joint.type = Joint2D::Type::Fixed;
    } else if (type.value() == "hinge") {
        joint.type = Joint2D::Type::Hinge;
    } else if (type.value() == "distance") {
        joint.type = Joint2D::Type::Distance;
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
    read.vec2("anchor", joint.anchor);
    read.angle("min_angle", joint.min_angle_radians);
    read.angle("max_angle", joint.max_angle_radians);
    read.vec2("connected_anchor", joint.connected_anchor);
    read.number("min_distance", joint.min_distance);
    read.number("max_distance", joint.max_distance);
    if (read.error.has_value()) {
        return *read.error;
    }
    context.world.registry().emplace_or_replace<Joint2D>(context.entity, joint);
    return {};
}

// Outlines in the plane z = `depth`.
void draw_box(DebugLines& lines, Vec2 center, float angle, Vec2 half, float depth, Vec4 color) {
    const Vec2 corners[] = {{-half.x, -half.y}, {half.x, -half.y}, {half.x, half.y},
                            {-half.x, half.y}};
    for (int i = 0; i < 4; ++i) {
        const Vec2 a = center + rotated(corners[i], angle);
        const Vec2 b = center + rotated(corners[(i + 1) % 4], angle);
        lines.line(Vec3{a.x, a.y, depth}, Vec3{b.x, b.y, depth}, color);
    }
}

void draw_arc(DebugLines& lines, Vec2 center, float radius, float from, float to, float depth,
              Vec4 color) {
    constexpr int k_segments = 16;
    Vec2 previous = center + Vec2{std::cos(from), std::sin(from)} * radius;
    for (int i = 1; i <= k_segments; ++i) {
        const float angle = from + (to - from) * static_cast<float>(i) / k_segments;
        const Vec2 next = center + Vec2{std::cos(angle), std::sin(angle)} * radius;
        lines.line(Vec3{previous.x, previous.y, depth}, Vec3{next.x, next.y, depth}, color);
        previous = next;
    }
}

} // namespace

float angle_around_z(Quat rotation) noexcept {
    return 2.0f * std::atan2(rotation.z, rotation.w);
}

Physics2D::~Physics2D() {
    clear();
}

core::Result<void> Physics2D::initialize(App& app) {
    core::Result<std::unique_ptr<PhysicsWorld>> backend = app.create_module<PhysicsWorld>();
    if (!backend.has_value()) {
        return backend.error();
    }
    return initialize(app, std::move(backend).value());
}

core::Result<void> Physics2D::initialize(App& app, std::unique_ptr<PhysicsWorld> backend) {
    if (backend == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument, "Physics2D needs a backend"};
    }
    if (app.config().contains("physics2d.gravity")) {
        const auto gravity = app.config().get_float_list("physics2d.gravity");
        if (!gravity.has_value() || gravity->size() != 2) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Setting 'physics2d.gravity' must be a list of two numbers"};
        }
        backend->set_gravity(
            Vec2{static_cast<float>((*gravity)[0]), static_cast<float>((*gravity)[1])});
    }

    clear();
    m_backend = std::move(backend);
    m_events = &app.events();
    app.scenes().register_component("rigid_body_2d", &read_rigid_body);
    app.scenes().register_component("joint_2d", &read_joint);
    return {};
}

void Physics2D::clear() {
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

void Physics2D::attach(World* world) {
    clear();
    m_world = world;
}

BodyHandle Physics2D::body_of(Entity entity) const {
    const auto found = m_bodies.find(entity);
    return found == m_bodies.end() ? BodyHandle{} : found->second;
}

Entity Physics2D::entity_of(BodyHandle body) const {
    const auto found = m_entities.find(key_of(body));
    return found == m_entities.end() ? k_no_entity : found->second;
}

void Physics2D::remove_stale() {
    Registry& registry = m_world->registry();

    for (auto it = m_bodies.begin(); it != m_bodies.end();) {
        const RigidBody2D* const body =
            registry.valid(it->first) ? registry.try_get<RigidBody2D>(it->first) : nullptr;
        if (body == nullptr || body->body != it->second) {
            m_backend->destroy_body(it->second);
            m_entities.erase(key_of(it->second));
            if (registry.valid(it->first)) {
                registry.remove<PhysicsPose2D>(it->first);
            }
            it = m_bodies.erase(it);
        } else {
            ++it;
        }
    }

    // After the bodies, so a joint whose body was just rebuilt is rebuilt too.
    for (auto it = m_joints.begin(); it != m_joints.end();) {
        const Joint2D* const joint =
            registry.valid(it->first) ? registry.try_get<Joint2D>(it->first) : nullptr;
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

void Physics2D::create_bodies() {
    Registry& registry = m_world->registry();
    for (const auto [entity, body, transform] :
         registry.view<RigidBody2D, const Transform>().each()) {
        if (body.failed || m_bodies.contains(entity)) {
            continue;
        }
        if (registry.all_of<Parent>(entity)) {
            DREAM_LOG_ERROR("Physics2D: entity {} has a parent, so it can't have a rigid body",
                            entt::to_integral(entity));
            body.failed = true;
            continue;
        }

        BodyDesc desc = body.desc;
        desc.position = flat(transform.position);
        desc.angle = angle_around_z(transform.rotation);
        desc.user_data = to_user_data(entity);
        const core::Result<BodyHandle> created = m_backend->create_body(desc);
        if (!created.has_value()) {
            DREAM_LOG_ERROR("Physics2D: entity {}'s body: {}", entt::to_integral(entity),
                            created.error().message);
            body.failed = true;
            continue;
        }

        body.body = created.value();
        m_bodies.emplace(entity, created.value());
        m_entities.emplace(key_of(created.value()), entity);
        if (desc.motion == MotionType::Dynamic) {
            registry.emplace_or_replace<PhysicsPose2D>(entity, desc.position, desc.angle,
                                                       desc.position, desc.angle);
        }
    }
}

void Physics2D::create_joints() {
    Registry& registry = m_world->registry();
    for (const auto [entity, joint] : registry.view<Joint2D>().each()) {
        if (joint.failed || m_joints.contains(entity)) {
            continue;
        }
        const BodyHandle body_a = body_of(entity);
        const BodyHandle body_b =
            joint.connected == k_no_entity ? BodyHandle{} : body_of(joint.connected);
        const bool connected_missing =
            joint.connected != k_no_entity &&
            (!registry.valid(joint.connected) || !registry.all_of<RigidBody2D>(joint.connected));
        if (!registry.all_of<RigidBody2D>(entity) || connected_missing) {
            DREAM_LOG_ERROR("Physics2D: entity {}'s joint needs a rigid body on both ends",
                            entt::to_integral(entity));
            joint.failed = true;
            continue;
        }
        if (!body_a.is_valid() || (joint.connected != k_no_entity && !body_b.is_valid())) {
            joint.failed =
                registry.get<RigidBody2D>(entity).failed ||
                (joint.connected != k_no_entity && registry.get<RigidBody2D>(joint.connected).failed);
            continue;
        }

        const Transform& own = registry.get<Transform>(entity);
        const Vec2 anchor = to_world(own, joint.anchor);
        JointType type;
        switch (joint.type) {
        case Joint2D::Type::Fixed:
            type = FixedJoint{};
            break;
        case Joint2D::Type::Hinge:
            type = HingeJoint{anchor, joint.min_angle_radians, joint.max_angle_radians};
            break;
        case Joint2D::Type::Distance: {
            const Vec2 other = joint.connected == k_no_entity
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
            DREAM_LOG_ERROR("Physics2D: entity {}'s joint: {}", entt::to_integral(entity),
                            created.error().message);
            joint.failed = true;
            continue;
        }
        joint.joint = created.value();
        m_joints.emplace(entity, TrackedJoint{created.value(), body_a, body_b});
    }
}

void Physics2D::step(float step_seconds) {
    if (m_backend == nullptr || m_world == nullptr || !(step_seconds > 0.0f)) {
        return;
    }
    remove_stale();
    create_bodies();
    create_joints();

    Registry& registry = m_world->registry();
    for (const auto [entity, body, transform] :
         registry.view<const RigidBody2D, const Transform>().each()) {
        if (body.desc.motion == MotionType::Kinematic && m_bodies.contains(entity)) {
            m_backend->move_kinematic(body.body, flat(transform.position),
                                      angle_around_z(transform.rotation), step_seconds);
        }
    }

    m_backend->step(step_seconds);

    for (const auto [entity, body, pose] : registry.view<const RigidBody2D, PhysicsPose2D>().each()) {
        if (const std::optional<BodyState> state = m_backend->body_state(body.body)) {
            pose.previous_position = pose.position;
            pose.previous_angle = pose.angle;
            pose.position = state->position;
            pose.angle = state->angle;
        }
    }

    const std::span<const ContactEvent> raw = m_backend->contact_events();
    std::vector<Contact2D> contacts;
    contacts.reserve(raw.size());
    m_contact_points.clear();
    for (const ContactEvent& event : raw) {
        contacts.push_back(Contact2D{
            .kind = event.kind,
            .a = from_user_data(event.user_data_a),
            .b = from_user_data(event.user_data_b),
            .point = event.point,
            .normal = event.normal,
        });
        if (event.kind == ContactEvent::Kind::Began && core::length(event.normal) > 0.0f) {
            m_contact_points.push_back(event.point);
        }
    }
    if (m_events != nullptr) {
        for (const Contact2D& contact : contacts) {
            m_events->publish(contact);
        }
    }
}

void Physics2D::update_transforms(float alpha) {
    if (m_world == nullptr) {
        return;
    }
    const float t = std::clamp(alpha, 0.0f, 1.0f);
    for (const auto [entity, pose, transform] :
         m_world->registry().view<const PhysicsPose2D, Transform>().each()) {
        const Vec2 position = core::lerp(pose.previous_position, pose.position, t);
        // The shorter way round, in case the angle wrapped between steps.
        const float turn = std::remainder(pose.angle - pose.previous_angle, core::k_two_pi);
        transform.position.x = position.x;
        transform.position.y = position.y;
        transform.rotation = core::quat_from_axis_angle(k_z_axis, pose.previous_angle + turn * t);
    }
}

void Physics2D::debug_draw(DebugLines& lines) const {
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
        const RigidBody2D* const body = registry.try_get<RigidBody2D>(entity);
        const Transform* const transform = registry.try_get<Transform>(entity);
        if (!state.has_value() || body == nullptr || transform == nullptr) {
            continue;
        }
        const BodyDesc& desc = body->desc;
        const float depth = transform->position.z;
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

        const Vec2 center = state->position;
        const float angle = state->angle;
        if (const auto* box = std::get_if<BoxShape>(&desc.shape)) {
            draw_box(lines, center, angle, box->half_extents, depth, color);
        } else if (const auto* circle = std::get_if<CircleShape>(&desc.shape)) {
            draw_arc(lines, center, circle->radius, 0.0f, core::k_two_pi, depth, color);
            // A spoke, so turning shows.
            const Vec2 rim = center + rotated(Vec2{circle->radius, 0.0f}, angle);
            lines.line(Vec3{center.x, center.y, depth}, Vec3{rim.x, rim.y, depth}, color);
        } else {
            const auto& capsule = std::get<CapsuleShape>(desc.shape);
            const Vec2 up = rotated(Vec2{0.0f, 1.0f}, angle);
            const Vec2 side = rotated(Vec2{1.0f, 0.0f}, angle) * capsule.radius;
            const Vec2 top = center + up * capsule.half_height;
            const Vec2 bottom = center - up * capsule.half_height;
            draw_arc(lines, top, capsule.radius, angle, angle + core::k_pi, depth, color);
            draw_arc(lines, bottom, capsule.radius, angle + core::k_pi, angle + core::k_two_pi,
                     depth, color);
            for (const Vec2 offset : {side, -side}) {
                const Vec2 a = bottom + offset;
                const Vec2 b = top + offset;
                lines.line(Vec3{a.x, a.y, depth}, Vec3{b.x, b.y, depth}, color);
            }
        }
    }

    for (const auto& [entity, tracked] : m_joints) {
        const Joint2D* const joint = registry.try_get<Joint2D>(entity);
        const std::optional<BodyState> state = m_backend->body_state(body_of(entity));
        const Transform* const transform = registry.try_get<Transform>(entity);
        if (joint == nullptr || !state.has_value() || transform == nullptr) {
            continue;
        }
        const float depth = transform->position.z;
        const Vec2 anchor = state->position + rotated(joint->anchor, state->angle);
        lines.line(Vec3{state->position.x, state->position.y, depth},
                   Vec3{anchor.x, anchor.y, depth}, k_joint);
        lines.cross(Vec3{anchor.x, anchor.y, depth}, 0.1f, k_joint);
    }

    for (const Vec2 point : m_contact_points) {
        lines.cross(Vec3{point.x, point.y, 0.0f}, 0.15f, k_contact);
    }
}

std::optional<BodyState> Physics2D::body_state(Entity entity) const {
    const BodyHandle body = body_of(entity);
    return m_backend != nullptr && body.is_valid() ? m_backend->body_state(body) : std::nullopt;
}

void Physics2D::add_force(Entity entity, Vec2 force) {
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->add_force(body, force);
    }
}

void Physics2D::add_impulse(Entity entity, Vec2 impulse) {
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->add_impulse(body, impulse);
    }
}

void Physics2D::set_linear_velocity(Entity entity, Vec2 velocity) {
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->set_linear_velocity(body, velocity);
    }
}

void Physics2D::set_angular_velocity(Entity entity, float velocity) {
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->set_angular_velocity(body, velocity);
    }
}

void Physics2D::teleport(Entity entity, Vec2 position, float angle) {
    if (m_world == nullptr || !m_world->registry().valid(entity)) {
        return;
    }
    Registry& registry = m_world->registry();
    if (Transform* const transform = registry.try_get<Transform>(entity)) {
        transform->position.x = position.x;
        transform->position.y = position.y;
        transform->rotation = core::quat_from_axis_angle(k_z_axis, angle);
    }
    if (PhysicsPose2D* const pose = registry.try_get<PhysicsPose2D>(entity)) {
        *pose = PhysicsPose2D{position, angle, position, angle};
    }
    if (const BodyHandle body = body_of(entity); body.is_valid()) {
        m_backend->set_transform(body, position, angle);
    }
}

std::optional<RayHit2D> Physics2D::cast_ray(Vec2 origin, Vec2 direction, float max_distance) const {
    if (m_backend == nullptr) {
        return std::nullopt;
    }
    const std::optional<RayHit> hit = m_backend->cast_ray(origin, direction, max_distance);
    if (!hit.has_value()) {
        return std::nullopt;
    }
    return RayHit2D{entity_of(hit->body), hit->point, hit->normal, hit->distance};
}

void Physics2D::overlap(const Shape& shape, Vec2 position, float angle,
                        std::vector<Entity>& entities) const {
    entities.clear();
    if (m_backend == nullptr) {
        return;
    }
    std::vector<BodyHandle> bodies;
    m_backend->overlap(shape, position, angle, bodies);
    for (const BodyHandle body : bodies) {
        if (const Entity entity = entity_of(body); entity != k_no_entity) {
            entities.push_back(entity);
        }
    }
}

void Physics2D::set_gravity(Vec2 gravity) {
    if (m_backend != nullptr) {
        m_backend->set_gravity(gravity);
    }
}

} // namespace dream::runtime
