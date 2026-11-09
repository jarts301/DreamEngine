#pragma once

#include <entt/entity/registry.hpp>

namespace dream::runtime {

// The entity system is EnTT, used directly. Like Core, it isn't swappable: it is the vocabulary
// that games and the engine share. See docs/adr/0001-layered-architecture.md.
using Entity = entt::entity;
using Registry = entt::registry;

inline constexpr Entity k_no_entity = entt::null;

} // namespace dream::runtime
