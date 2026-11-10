#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_physics3d_null(core::ModuleRegistry& registry);
}

std::unique_ptr<dream::physics3d::PhysicsWorld> make_world() {
    using namespace dream;
    core::ModuleRegistry registry;
    REQUIRE(modules::register_physics3d_null(registry).has_value());

    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"null"};
    auto created = registry.create<physics3d::PhysicsWorld>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}
