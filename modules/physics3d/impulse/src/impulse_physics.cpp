#include "dream/core/log.h"
#include "dream/core/module_registry.h"
#include "dream/physics3d/physics3d.h"

#include <impulse/world.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dream::modules {
namespace {

using namespace physics3d;

impulse::Vec3 to_impulse(core::Vec3 v) {
    return impulse::Vec3{v.x, v.y, v.z};
}

impulse::Quat to_impulse(core::Quat q) {
    return impulse::Quat{q.x, q.y, q.z, q.w};
}

core::Vec3 from_impulse(impulse::Vec3 v) {
    return core::Vec3{v.x, v.y, v.z};
}

core::Quat from_impulse(impulse::Quat q) {
    return core::Quat{q.x, q.y, q.z, q.w};
}

impulse::Shape to_impulse(const Shape& shape) {
    if (const auto* box = std::get_if<BoxShape>(&shape)) {
        return impulse::Box{to_impulse(box->half_extents)};
    }
    if (const auto* sphere = std::get_if<SphereShape>(&shape)) {
        return impulse::Sphere{sphere->radius};
    }
    const auto& capsule = std::get<CapsuleShape>(shape);
    return impulse::Capsule{capsule.half_height, capsule.radius};
}

impulse::Motion to_impulse(MotionType motion) {
    switch (motion) {
    case MotionType::Static:
        return impulse::Motion::Static;
    case MotionType::Kinematic:
        return impulse::Motion::Kinematic;
    case MotionType::Dynamic:
        return impulse::Motion::Dynamic;
    }
    return impulse::Motion::Static;
}

// The library's ids are slot and generation, like the engine's handles, so they convert directly.
impulse::BodyId to_impulse(BodyHandle body) {
    return impulse::BodyId{body.index, body.generation};
}

BodyHandle from_impulse(impulse::BodyId body) {
    return BodyHandle{.index = body.index, .generation = body.generation};
}

core::Error invalid(const char* message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
}

class ImpulsePhysics final : public PhysicsWorld {
public:
    explicit ImpulsePhysics(const impulse::Settings& settings) : m_world(settings) {}

    void set_gravity(core::Vec3 gravity) override {
        m_world.set_gravity(to_impulse(gravity));
    }

    core::Vec3 gravity() const override {
        return from_impulse(m_world.gravity());
    }

    core::Result<BodyHandle> create_body(const BodyDesc& desc) override {
        if (core::Result<void> valid = validate(desc); !valid.has_value()) {
            return valid.error();
        }
        const std::optional<impulse::BodyId> body = m_world.create_body(impulse::BodyDef{
            .shape = to_impulse(desc.shape),
            .motion = to_impulse(desc.motion),
            .position = to_impulse(desc.position),
            .rotation = to_impulse(desc.rotation),
            .linear_velocity = to_impulse(desc.linear_velocity),
            .angular_velocity = to_impulse(desc.angular_velocity),
            .mass = desc.mass,
            .friction = desc.friction,
            .restitution = desc.restitution,
            .linear_damping = desc.linear_damping,
            .angular_damping = desc.angular_damping,
            .gravity_scale = desc.gravity_scale,
            .sensor = desc.is_sensor,
            .continuous = desc.continuous_collision,
            .user_data = desc.user_data,
        });
        if (!body.has_value()) {
            return invalid("The physics library rejected the body");
        }
        return from_impulse(*body);
    }

    void destroy_body(BodyHandle body) override {
        m_world.destroy_body(to_impulse(body));
    }

    std::optional<BodyState> body_state(BodyHandle body) const override {
        const std::optional<impulse::BodyState> state = m_world.state(to_impulse(body));
        if (!state.has_value()) {
            return std::nullopt;
        }
        return BodyState{
            .position = from_impulse(state->position),
            .rotation = from_impulse(state->rotation),
            .linear_velocity = from_impulse(state->linear_velocity),
            .angular_velocity = from_impulse(state->angular_velocity),
            .is_awake = state->awake,
        };
    }

    void set_transform(BodyHandle body, core::Vec3 position, core::Quat rotation) override {
        m_world.set_transform(to_impulse(body), to_impulse(position), to_impulse(rotation));
    }

    void move_kinematic(BodyHandle body, core::Vec3 position, core::Quat rotation,
                        float step_seconds) override {
        m_world.move_kinematic(to_impulse(body), to_impulse(position), to_impulse(rotation),
                               step_seconds);
    }

    void set_linear_velocity(BodyHandle body, core::Vec3 velocity) override {
        m_world.set_linear_velocity(to_impulse(body), to_impulse(velocity));
    }

    void set_angular_velocity(BodyHandle body, core::Vec3 velocity) override {
        m_world.set_angular_velocity(to_impulse(body), to_impulse(velocity));
    }

    void add_force(BodyHandle body, core::Vec3 force) override {
        m_world.add_force(to_impulse(body), to_impulse(force));
    }

    void add_impulse(BodyHandle body, core::Vec3 impulse) override {
        m_world.add_impulse(to_impulse(body), to_impulse(impulse));
    }

    core::Result<JointHandle> create_joint(const JointDesc& desc) override {
        if (!m_world.state(to_impulse(desc.body_a)).has_value() ||
            (desc.body_b.is_valid() && !m_world.state(to_impulse(desc.body_b)).has_value()) ||
            desc.body_a == desc.body_b) {
            return invalid("A joint needs two different bodies that exist");
        }
        impulse::JointDef def{.a = to_impulse(desc.body_a)};
        if (desc.body_b.is_valid()) {
            def.b = to_impulse(desc.body_b);
        }
        if (std::holds_alternative<FixedJoint>(desc.type)) {
            def.type = impulse::FixedJoint{};
        } else if (const auto* point = std::get_if<PointJoint>(&desc.type)) {
            def.type = impulse::PointJoint{to_impulse(point->anchor)};
        } else if (const auto* hinge = std::get_if<HingeJoint>(&desc.type)) {
            def.type = impulse::HingeJoint{to_impulse(hinge->anchor), to_impulse(hinge->axis),
                                           hinge->min_angle_radians, hinge->max_angle_radians};
        } else {
            const auto& distance = std::get<DistanceJoint>(desc.type);
            def.type =
                impulse::DistanceJoint{to_impulse(distance.anchor_a), to_impulse(distance.anchor_b),
                                       distance.min_distance, distance.max_distance};
        }
        const std::optional<impulse::JointId> joint = m_world.create_joint(def);
        if (!joint.has_value()) {
            return invalid("The joint's settings are invalid: a hinge needs an axis and limits "
                           "within -pi and pi around zero, and a distance joint needs "
                           "0 <= min_distance <= max_distance");
        }
        return JointHandle{.index = joint->index, .generation = joint->generation};
    }

    void destroy_joint(JointHandle joint) override {
        m_world.destroy_joint(impulse::JointId{joint.index, joint.generation});
    }

    void step(float step_seconds) override {
        m_world.step(step_seconds);
        m_events.clear();
        for (const impulse::ContactEvent& event : m_world.contact_events()) {
            m_events.push_back(ContactEvent{
                .kind = event.kind == impulse::ContactEvent::Kind::Began
                            ? ContactEvent::Kind::Began
                            : ContactEvent::Kind::Ended,
                .body_a = from_impulse(event.a),
                .body_b = from_impulse(event.b),
                .user_data_a = event.user_data_a,
                .user_data_b = event.user_data_b,
                .point = from_impulse(event.point),
                .normal = from_impulse(event.normal),
            });
        }
    }

    std::span<const ContactEvent> contact_events() const override {
        return m_events;
    }

    std::optional<RayHit> cast_ray(core::Vec3 origin, core::Vec3 direction,
                                   float max_distance) const override {
        const std::optional<impulse::RayHit> hit =
            m_world.cast_ray(to_impulse(origin), to_impulse(direction), max_distance);
        if (!hit.has_value()) {
            return std::nullopt;
        }
        return RayHit{
            .body = from_impulse(hit->body),
            .user_data = hit->user_data,
            .point = from_impulse(hit->point),
            .normal = from_impulse(hit->normal),
            .distance = hit->distance,
        };
    }

    void overlap(const Shape& shape, core::Vec3 position, core::Quat rotation,
                 std::vector<BodyHandle>& bodies) const override {
        bodies.clear();
        if (!detail::is_valid_shape(shape)) {
            return;
        }
        m_world.overlap(to_impulse(shape), to_impulse(position), to_impulse(rotation), m_found);
        for (const impulse::BodyId body : m_found) {
            bodies.push_back(from_impulse(body));
        }
    }

    std::size_t body_count() const override {
        return m_world.body_count();
    }

private:
    impulse::World m_world;
    std::vector<ContactEvent> m_events;
    mutable std::vector<impulse::BodyId> m_found;
};

// Settings under [physics3d.impulse]: velocity_iterations and position_iterations trade speed for
// steadier stacks and joints.
core::Result<std::unique_ptr<PhysicsWorld>> create(const core::Config& config) {
    impulse::Settings settings;
    settings.velocity_iterations = static_cast<int>(std::clamp<std::int64_t>(
        config.get_int("physics3d.impulse.velocity_iterations").value_or(10), 1, 100));
    settings.position_iterations = static_cast<int>(std::clamp<std::int64_t>(
        config.get_int("physics3d.impulse.position_iterations").value_or(4), 0, 100));
    return std::unique_ptr<PhysicsWorld>{std::make_unique<ImpulsePhysics>(settings)};
}

} // namespace

[[nodiscard]] core::Result<void> register_physics3d_impulse(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<PhysicsWorld>{.name = "impulse", .create = &create});
}

} // namespace dream::modules
