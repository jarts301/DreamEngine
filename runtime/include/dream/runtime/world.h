#pragma once

#include "dream/core/math/matrix.h"
#include "dream/core/math/quaternion.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/runtime/ecs.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

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
    World();

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

    // A number no other world in the program has had, for systems that keep state per world and
    // entity, such as Animation and Particles. A world's address can be reused by the next one.
    [[nodiscard]] std::uint64_t id() const noexcept {
        return m_id;
    }

    // Expires when the world is destroyed, so those systems can release what they kept for it.
    [[nodiscard]] std::weak_ptr<const void> lifetime() const noexcept {
        return m_lifetime;
    }

private:
    Registry m_registry;
    std::uint64_t m_id = 0;
    std::shared_ptr<const void> m_lifetime;
    // For update_transforms: by entity index, the update in which each was last worked out.
    std::vector<std::uint32_t> m_resolved;
    std::uint32_t m_resolve_pass = 0;
};

// The worlds a system has seen, for one that keeps state per World::id to tell which have since
// been destroyed.
class WorldLifetimes {
public:
    void note(const World& world) {
        if (!m_worlds.contains(world.id())) {
            m_worlds.emplace(world.id(), world.lifetime());
        }
    }

    // Whether the world with this id was noted and has since been destroyed.
    [[nodiscard]] bool destroyed(std::uint64_t id) {
        const auto found = m_worlds.find(id);
        return found != m_worlds.end() && found->second.expired();
    }

    // Drops the destroyed worlds, once nothing is kept for them.
    void prune() {
        std::erase_if(m_worlds, [](const auto& entry) { return entry.second.expired(); });
    }

private:
    std::unordered_map<std::uint64_t, std::weak_ptr<const void>> m_worlds;
};

} // namespace dream::runtime
