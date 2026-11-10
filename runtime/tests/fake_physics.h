#pragma once

#include "dream/core/handle.h"
#include "dream/physics2d/physics2d.h"
#include "dream/physics3d/physics3d.h"

#include <format>
#include <map>
#include <string>
#include <vector>

namespace dream::runtime::tests {

// Records what the runtime asks of a physics backend. Each step moves dynamic bodies one meter
// along +X and plays back any contact events the test queued. Lives outside the backend, because
// Physics3D takes ownership of it.
struct FakePhysics3DState {
    struct Body {
        physics3d::BodyDesc desc;
        physics3d::BodyState state;
    };

    std::map<std::uint32_t, Body> bodies;
    std::map<std::uint32_t, physics3d::JointDesc> joints;
    std::vector<std::string> calls;
    std::vector<physics3d::ContactEvent> next_contacts;
    std::optional<physics3d::RayHit> ray_hit;
    std::vector<physics3d::BodyHandle> overlapping;
    core::Vec3 gravity{0.0f, -9.81f, 0.0f};
    bool destroyed = false;
};

class FakePhysics3D final : public physics3d::PhysicsWorld {
public:
    explicit FakePhysics3D(FakePhysics3DState& state) : m_state(state) {}
    ~FakePhysics3D() override {
        m_state.destroyed = true;
    }

    void set_gravity(core::Vec3 gravity) override {
        m_state.gravity = gravity;
    }
    core::Vec3 gravity() const override {
        return m_state.gravity;
    }

    core::Result<physics3d::BodyHandle> create_body(const physics3d::BodyDesc& desc) override {
        if (core::Result<void> valid = physics3d::validate(desc); !valid.has_value()) {
            return valid.error();
        }
        const physics3d::BodyHandle handle{.index = m_next++, .generation = 1};
        m_state.bodies[handle.index] = {
            desc, physics3d::BodyState{.position = desc.position, .rotation = desc.rotation}};
        m_state.calls.push_back(std::format("create {}", handle.index));
        return handle;
    }

    void destroy_body(physics3d::BodyHandle body) override {
        if (m_state.bodies.erase(body.index) != 0) {
            m_state.calls.push_back(std::format("destroy {}", body.index));
        }
    }

    std::optional<physics3d::BodyState> body_state(physics3d::BodyHandle body) const override {
        const auto found = m_state.bodies.find(body.index);
        return found == m_state.bodies.end() ? std::nullopt
                                             : std::optional(found->second.state);
    }

    void set_transform(physics3d::BodyHandle body, core::Vec3 position, core::Quat rotation) override {
        if (const auto found = m_state.bodies.find(body.index); found != m_state.bodies.end()) {
            found->second.state.position = position;
            found->second.state.rotation = rotation;
        }
    }

    void move_kinematic(physics3d::BodyHandle body, core::Vec3 position, core::Quat,
                        float) override {
        m_state.calls.push_back(std::format("kinematic {} {}", body.index, position.x));
        set_transform(body, position, core::Quat{});
    }

    void set_linear_velocity(physics3d::BodyHandle body, core::Vec3 velocity) override {
        m_state.calls.push_back(std::format("velocity {} {}", body.index, velocity.x));
    }
    void set_angular_velocity(physics3d::BodyHandle body, core::Vec3) override {
        m_state.calls.push_back(std::format("spin {}", body.index));
    }
    void add_force(physics3d::BodyHandle body, core::Vec3 force) override {
        m_state.calls.push_back(std::format("force {} {}", body.index, force.y));
    }
    void add_impulse(physics3d::BodyHandle body, core::Vec3 impulse) override {
        m_state.calls.push_back(std::format("impulse {} {}", body.index, impulse.y));
    }

    core::Result<physics3d::JointHandle> create_joint(const physics3d::JointDesc& desc) override {
        if (!m_state.bodies.contains(desc.body_a.index)) {
            return core::Error{core::ErrorCode::InvalidArgument, "no body"};
        }
        const physics3d::JointHandle handle{.index = m_next++, .generation = 1};
        m_state.joints[handle.index] = desc;
        m_state.calls.push_back(std::format("joint {}", handle.index));
        return handle;
    }

    void destroy_joint(physics3d::JointHandle joint) override {
        if (m_state.joints.erase(joint.index) != 0) {
            m_state.calls.push_back(std::format("unjoint {}", joint.index));
        }
    }

    void step(float) override {
        for (auto& [index, body] : m_state.bodies) {
            if (body.desc.motion == physics3d::MotionType::Dynamic) {
                body.state.position.x += 1.0f;
            }
        }
        m_events = std::move(m_state.next_contacts);
        m_state.next_contacts.clear();
        m_state.calls.emplace_back("step");
    }

    std::span<const physics3d::ContactEvent> contact_events() const override {
        return m_events;
    }

    std::optional<physics3d::RayHit> cast_ray(core::Vec3, core::Vec3, float) const override {
        return m_state.ray_hit;
    }

    void overlap(const physics3d::Shape&, core::Vec3, core::Quat,
                 std::vector<physics3d::BodyHandle>& bodies) const override {
        bodies = m_state.overlapping;
    }

    std::size_t body_count() const override {
        return m_state.bodies.size();
    }

private:
    FakePhysics3DState& m_state;
    std::uint32_t m_next = 1;
    std::vector<physics3d::ContactEvent> m_events;
};

// The same for 2D, with less recording.
struct FakePhysics2DState {
    struct Body {
        physics2d::BodyDesc desc;
        physics2d::BodyState state;
    };

    std::map<std::uint32_t, Body> bodies;
    std::map<std::uint32_t, physics2d::JointDesc> joints;
    std::vector<physics2d::ContactEvent> next_contacts;
    core::Vec2 gravity{0.0f, -9.81f};
};

class FakePhysics2D final : public physics2d::PhysicsWorld {
public:
    explicit FakePhysics2D(FakePhysics2DState& state) : m_state(state) {}

    void set_gravity(core::Vec2 gravity) override {
        m_state.gravity = gravity;
    }
    core::Vec2 gravity() const override {
        return m_state.gravity;
    }

    core::Result<physics2d::BodyHandle> create_body(const physics2d::BodyDesc& desc) override {
        if (core::Result<void> valid = physics2d::validate(desc); !valid.has_value()) {
            return valid.error();
        }
        const physics2d::BodyHandle handle{.index = m_next++, .generation = 1};
        m_state.bodies[handle.index] = {desc,
                                        physics2d::BodyState{.position = desc.position, .angle = desc.angle}};
        return handle;
    }

    void destroy_body(physics2d::BodyHandle body) override {
        m_state.bodies.erase(body.index);
    }

    std::optional<physics2d::BodyState> body_state(physics2d::BodyHandle body) const override {
        const auto found = m_state.bodies.find(body.index);
        return found == m_state.bodies.end() ? std::nullopt
                                             : std::optional(found->second.state);
    }

    void set_transform(physics2d::BodyHandle body, core::Vec2 position, float angle) override {
        if (const auto found = m_state.bodies.find(body.index); found != m_state.bodies.end()) {
            found->second.state.position = position;
            found->second.state.angle = angle;
        }
    }

    void move_kinematic(physics2d::BodyHandle body, core::Vec2 position, float angle,
                        float) override {
        set_transform(body, position, angle);
    }

    void set_linear_velocity(physics2d::BodyHandle, core::Vec2) override {}
    void set_angular_velocity(physics2d::BodyHandle, float) override {}
    void add_force(physics2d::BodyHandle, core::Vec2) override {}
    void add_impulse(physics2d::BodyHandle, core::Vec2) override {}

    core::Result<physics2d::JointHandle> create_joint(const physics2d::JointDesc& desc) override {
        const physics2d::JointHandle handle{.index = m_next++, .generation = 1};
        m_state.joints[handle.index] = desc;
        return handle;
    }

    void destroy_joint(physics2d::JointHandle joint) override {
        m_state.joints.erase(joint.index);
    }

    // Dynamic bodies move one meter along +X and turn a quarter turn each step.
    void step(float) override {
        for (auto& [index, body] : m_state.bodies) {
            if (body.desc.motion == physics2d::MotionType::Dynamic) {
                body.state.position.x += 1.0f;
                body.state.angle += core::k_pi / 2.0f;
            }
        }
        m_events = std::move(m_state.next_contacts);
        m_state.next_contacts.clear();
    }

    std::span<const physics2d::ContactEvent> contact_events() const override {
        return m_events;
    }

    std::optional<physics2d::RayHit> cast_ray(core::Vec2, core::Vec2, float) const override {
        return std::nullopt;
    }

    void overlap(const physics2d::Shape&, core::Vec2, float,
                 std::vector<physics2d::BodyHandle>& bodies) const override {
        bodies.clear();
    }

    std::size_t body_count() const override {
        return m_state.bodies.size();
    }

private:
    FakePhysics2DState& m_state;
    std::uint32_t m_next = 1;
    std::vector<physics2d::ContactEvent> m_events;
};

} // namespace dream::runtime::tests
