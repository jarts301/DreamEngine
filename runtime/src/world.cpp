#include "dream/runtime/world.h"

#include "dream/core/profile.h"

#include <unordered_set>
#include <vector>

namespace dream::runtime {
namespace {

core::Mat4 local_matrix(const Transform& transform) {
    return core::transform(transform.position, transform.rotation, transform.scale);
}

// Works out a parented entity's matrix, after its parent's. `done` avoids repeating shared parents.
const core::Mat4& resolve(Registry& registry, Entity entity, std::unordered_set<Entity>& done) {
    WorldTransform& world = registry.get<WorldTransform>(entity);
    if (!done.insert(entity).second) {
        return world.matrix;
    }

    const core::Mat4 local = local_matrix(registry.get<Transform>(entity));
    const Parent* const parent = registry.try_get<Parent>(entity);
    if (parent != nullptr && registry.valid(parent->entity) &&
        registry.all_of<Transform, WorldTransform>(parent->entity)) {
        world.matrix = resolve(registry, parent->entity, done) * local;
    } else {
        world.matrix = local;
    }
    return world.matrix;
}

} // namespace

Entity World::create_entity(std::string_view name) {
    const Entity entity = m_registry.create();
    m_registry.emplace<Transform>(entity);
    m_registry.emplace<WorldTransform>(entity);
    if (!name.empty()) {
        m_registry.emplace<Name>(entity, std::string(name));
    }
    return entity;
}

void World::destroy_entity(Entity entity) {
    if (!m_registry.valid(entity)) {
        return;
    }

    std::vector<Entity> children;
    for (const auto [child, parent] : m_registry.view<Parent>().each()) {
        if (parent.entity == entity) {
            children.push_back(child);
        }
    }
    for (const Entity child : children) {
        destroy_entity(child);
    }
    m_registry.destroy(entity);
}

Entity World::find(std::string_view name) const {
    for (const auto [entity, entity_name] : m_registry.view<const Name>().each()) {
        if (entity_name.value == name) {
            return entity;
        }
    }
    return k_no_entity;
}

core::Result<void> World::set_parent(Entity child, Entity parent) {
    if (!m_registry.valid(child)) {
        return core::Error{core::ErrorCode::InvalidArgument, "The child entity doesn't exist"};
    }
    if (parent == k_no_entity) {
        m_registry.remove<Parent>(child);
        return {};
    }
    if (!m_registry.valid(parent)) {
        return core::Error{core::ErrorCode::InvalidArgument, "The parent entity doesn't exist"};
    }

    for (Entity ancestor = parent; ancestor != k_no_entity;) {
        if (ancestor == child) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "An entity can't be its own ancestor"};
        }
        const Parent* const next = m_registry.try_get<Parent>(ancestor);
        ancestor = next != nullptr ? next->entity : k_no_entity;
    }

    m_registry.emplace_or_replace<Parent>(child, parent);
    return {};
}

void World::update_transforms() {
    DREAM_PROFILE_ZONE("Update transforms");
    const auto roots = m_registry.view<const Transform, WorldTransform>(entt::exclude<Parent>);
    for (const auto [entity, transform, world] : roots.each()) {
        world.matrix = local_matrix(transform);
    }

    std::unordered_set<Entity> done;
    for (const auto entity : m_registry.view<Parent, Transform, WorldTransform>()) {
        resolve(m_registry, entity, done);
    }
}

} // namespace dream::runtime
