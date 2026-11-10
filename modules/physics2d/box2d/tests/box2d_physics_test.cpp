#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/collision_conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_physics2d_box2d(core::ModuleRegistry& registry);
}

std::unique_ptr<dream::physics2d::PhysicsWorld> make_world() {
    using namespace dream;
    core::ModuleRegistry registry;
    REQUIRE(modules::register_physics2d_box2d(registry).has_value());

    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"box2d"};
    auto created = registry.create<physics2d::PhysicsWorld>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}
