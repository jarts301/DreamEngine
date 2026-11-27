#pragma once

// The camera the 3D feature samples share: it circles the point its scene's camera looks at, turned
// and moved nearer by the orbit and zoom actions, by keys, a stick or a finger dragged across the
// screen, and turning by itself when `auto_turn` is set. It keeps the height and distance the
// scene gave it in proportion, so zooming keeps the view's angle.

#include "dream/runtime/app.h"
#include "dream/runtime/renderer.h"
#include "dream/runtime/world.h"

#include <algorithm>
#include <cmath>

namespace dream::samples {

class SampleCamera {
public:
    // Starts from the scene's active camera and the point it looks at.
    void attach(runtime::World& world) {
        m_world = &world;
        m_camera = runtime::active_camera(world);
        if (m_camera == runtime::k_no_entity) {
            return;
        }
        runtime::Registry& registry = world.registry();
        const runtime::Camera& lens = registry.get<runtime::Camera>(m_camera);
        m_target = lens.target.value_or(core::Vec3{});
        const core::Vec3 offset = registry.get<runtime::Transform>(m_camera).position - m_target;
        m_distance = core::length(offset);
        m_start_distance = m_distance;
        m_height = offset.y;
        m_yaw = std::atan2(offset.x, offset.z);
    }

    // Radians per second it turns by itself, such as 0.1 for a slow drift; 0 to stand still.
    float auto_turn = 0.0f;
    float min_distance = 3.0f;
    float max_distance = 60.0f;

    void update(runtime::App& app, float seconds) {
        if (m_world == nullptr || m_camera == runtime::k_no_entity) {
            return;
        }
        const runtime::InputActions& input = app.input();
        float turn = input.axis("orbit_left", "orbit_right");
        const float zoom = input.axis("zoom_out", "zoom_in");
        // A finger or the mouse dragged sideways turns the view too.
        if (input.is_down("drag")) {
            const auto pointer = input.pointer_position();
            if (m_dragging) {
                turn += (pointer.x - m_last_pointer) * 0.01f / std::max(seconds, 0.001f);
            }
            m_last_pointer = pointer.x;
            m_dragging = true;
        } else {
            m_dragging = false;
        }
        constexpr float k_turn_speed = 1.5f; // Radians per second.
        constexpr float k_zoom_speed = 0.8f; // Of the distance per second.
        m_yaw -= (turn * k_turn_speed - auto_turn) * seconds;
        const float scale = std::clamp(1.0f - zoom * k_zoom_speed * seconds, 0.5f, 1.5f);
        m_distance = std::clamp(m_distance * scale, min_distance, max_distance);

        const float height = m_height * (m_distance / std::max(m_start_distance, 0.001f));
        const float across = std::sqrt(std::max(m_distance * m_distance - height * height, 0.0f));
        m_world->registry().get<runtime::Transform>(m_camera).position =
            m_target + core::Vec3{std::sin(m_yaw) * across, height, std::cos(m_yaw) * across};
    }

private:
    runtime::World* m_world = nullptr;
    runtime::Entity m_camera = runtime::k_no_entity;
    core::Vec3 m_target;
    float m_distance = 1.0f;
    float m_start_distance = 1.0f;
    float m_height = 0.0f;
    float m_yaw = 0.0f;
    float m_last_pointer = 0.0f;
    bool m_dragging = false;
};

} // namespace dream::samples
