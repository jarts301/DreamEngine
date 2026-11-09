#pragma once

#include "dream/core/math/matrix.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/runtime/ecs.h"

#include <string>
#include <string_view>

namespace dream::runtime {

struct Name {
    std::string value;
};

// Position, rotation and scale relative to the parent, or to the world without one.
struct Transform {
    core::Vec3 position;
    core::Quat rotation;
    core::Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Written by World::update_transforms.
struct WorldTransform {
    core::Mat4 matrix;
};

// Set through World::set_parent, which keeps the hierarchy free of loops.
struct Parent {
    Entity entity = k_no_entity;
};

// A set of entities: one level, or one screen. Components other than those above belong to the game
// and are added through registry(). Not thread-safe.
class World {
public:
    // The entity starts with a Transform and a WorldTransform, plus a Name if one is given.
    Entity create_entity(std::string_view name = {});

    // Also destroys the entity's children. Does nothing if the entity no longer exists.
    void destroy_entity(Entity entity);

    // The first entity with this name, or k_no_entity. Searches every entity, so avoid it per frame.
    [[nodiscard]] Entity find(std::string_view name) const;

    // Pass k_no_entity to remove the parent. Fails if an entity doesn't exist, or if this would
    // make an entity its own ancestor.
    [[nodiscard]] core::Result<void> set_parent(Entity child, Entity parent);

    // Recalculates every WorldTransform. Call after moving things and before drawing.
    void update_transforms();

    [[nodiscard]] Registry& registry() noexcept {
        return m_registry;
    }

    [[nodiscard]] const Registry& registry() const noexcept {
        return m_registry;
    }

private:
    Registry m_registry;
};

} // namespace dream::runtime
