#include "dream/core/handle.h"
#include "dream/core/module_registry.h"
#include "dream/physics3d/physics3d.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dream::modules {
namespace {

using namespace physics3d;
using core::Quat;
using core::Vec3;

struct Body {
    BodyDesc desc;
    BodyState state;
    Vec3 force;
    std::optional<Quat> target_rotation;
};

struct Joint {
    BodyHandle body_a;
    BodyHandle body_b;
};

// Moves bodies by their velocity and gravity, with no collisions, contacts or joint constraints.
// For automated runs, and for checking that engine code follows the interface's rules.
class NullPhysics final : public PhysicsWorld {
public:
    void set_gravity(Vec3 gravity) override {
        m_gravity = gravity;
    }

    Vec3 gravity() const override {
        return m_gravity;
    }

    core::Result<BodyHandle> create_body(const BodyDesc& desc) override {
        if (core::Result<void> valid = validate(desc); !valid.has_value()) {
            return valid.error();
        }
        const bool moves = desc.motion != MotionType::Static;
        return handle_cast<BodyTag>(m_bodies.create(Body{
            .desc = desc,
            .state =
                BodyState{
                    .position = desc.position,
                    .rotation = desc.rotation,
                    .linear_velocity = moves ? desc.linear_velocity : Vec3{},
                    .angular_velocity = moves ? desc.angular_velocity : Vec3{},
                    .is_awake = moves,
                },
            .force = Vec3{},
            .target_rotation = std::nullopt,
        }));
    }

    void destroy_body(BodyHandle body) override {
        if (!m_bodies.destroy(handle_cast<Body>(body))) {
            return;
        }
        m_joints.for_each([&](core::Handle<Joint> handle, const Joint& joint) {
            if (joint.body_a == body || joint.body_b == body) {
                m_joints.destroy(handle);
            }
        });
    }

    std::optional<BodyState> body_state(BodyHandle body) const override {
        const Body* const found = m_bodies.get(handle_cast<Body>(body));
        return found == nullptr ? std::nullopt : std::optional<BodyState>(found->state);
    }

    void set_transform(BodyHandle body, Vec3 position, Quat rotation) override {
        if (Body* const found = find(body)) {
            found->state.position = position;
            found->state.rotation = rotation;
        }
    }

    void move_kinematic(BodyHandle body, Vec3 position, Quat rotation,
                        float step_seconds) override {
        Body* const found = find(body);
        if (found == nullptr || found->desc.motion != MotionType::Kinematic || step_seconds <= 0.0f) {
            return;
        }
        found->state.linear_velocity = (position - found->state.position) / step_seconds;
        found->target_rotation = rotation;
        found->state.is_awake = true;
    }

    void set_linear_velocity(BodyHandle body, Vec3 velocity) override {
        if (Body* const found = find_moving(body)) {
            found->state.linear_velocity = velocity;
            found->state.is_awake = true;
        }
    }

    void set_angular_velocity(BodyHandle body, Vec3 velocity) override {
        if (Body* const found = find_moving(body)) {
            found->state.angular_velocity = velocity;
            found->state.is_awake = true;
        }
    }

    void add_force(BodyHandle body, Vec3 force) override {
        if (Body* const found = find_dynamic(body)) {
            found->force += force;
            found->state.is_awake = true;
        }
    }

    void add_impulse(BodyHandle body, Vec3 impulse) override {
        if (Body* const found = find_dynamic(body)) {
            found->state.linear_velocity += impulse / found->desc.mass;
            found->state.is_awake = true;
        }
    }

    core::Result<JointHandle> create_joint(const JointDesc& desc) override {
        if (find(desc.body_a) == nullptr ||
            (desc.body_b.is_valid() && find(desc.body_b) == nullptr) || desc.body_a == desc.body_b) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "A joint needs two different bodies that exist"};
        }
        return handle_cast<JointTag>(m_joints.create(Joint{desc.body_a, desc.body_b}));
    }

    void destroy_joint(JointHandle joint) override {
        m_joints.destroy(handle_cast<Joint>(joint));
    }

    void step(float step_seconds) override {
        m_bodies.for_each([&](core::Handle<Body> /*handle*/, Body& body) {
            BodyState& state = body.state;
            if (body.desc.motion == MotionType::Dynamic) {
                state.linear_velocity += (m_gravity * body.desc.gravity_scale +
                                          body.force / body.desc.mass) *
                                         step_seconds;
            }
            body.force = Vec3{};
            if (body.desc.motion == MotionType::Static) {
                return;
            }

            state.position += state.linear_velocity * step_seconds;
            const float spin = core::length(state.angular_velocity);
            if (spin > 0.0f) {
                state.rotation = core::normalized(
                    core::quat_from_axis_angle(state.angular_velocity / spin, spin * step_seconds) *
                    state.rotation);
            }
            if (body.target_rotation.has_value()) {
                state.rotation = *body.target_rotation;
                body.target_rotation.reset();
                state.linear_velocity = Vec3{};
            }
        });
    }

    std::span<const ContactEvent> contact_events() const override {
        return {};
    }

    std::optional<RayHit> cast_ray(Vec3 /*origin*/, Vec3 /*direction*/,
                                   float /*max_distance*/) const override {
        return std::nullopt;
    }

    void overlap(const Shape& /*shape*/, Vec3 /*position*/, Quat /*rotation*/,
                 std::vector<BodyHandle>& bodies) const override {
        bodies.clear();
    }

    std::size_t body_count() const override {
        return m_bodies.size();
    }

private:
    Body* find(BodyHandle body) {
        return m_bodies.get(handle_cast<Body>(body));
    }

    Body* find_moving(BodyHandle body) {
        Body* const found = find(body);
        return found != nullptr && found->desc.motion != MotionType::Static ? found : nullptr;
    }

    Body* find_dynamic(BodyHandle body) {
        Body* const found = find(body);
        return found != nullptr && found->desc.motion == MotionType::Dynamic ? found : nullptr;
    }

    core::HandlePool<Body> m_bodies;
    core::HandlePool<Joint> m_joints;
    Vec3 m_gravity{0.0f, -9.81f, 0.0f};
};

core::Result<std::unique_ptr<PhysicsWorld>> create(const core::Config& /*config*/) {
    return std::unique_ptr<PhysicsWorld>{std::make_unique<NullPhysics>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_physics3d_null(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<PhysicsWorld>{.name = "null", .create = &create});
}

} // namespace dream::modules
