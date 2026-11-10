#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/collision_conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_physics3d_jolt(core::ModuleRegistry& registry);
}

std::unique_ptr<dream::physics3d::PhysicsWorld> make_world() {
    using namespace dream;
    core::ModuleRegistry registry;
    REQUIRE(modules::register_physics3d_jolt(registry).has_value());

    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"jolt"};
    auto created = registry.create<physics3d::PhysicsWorld>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

TEST_CASE("Jolt worlds can coexist and be created again") {
    auto first = make_world();
    auto second = make_world();
    REQUIRE(first->create_body(conformance::floating_ball()).has_value());
    first.reset();
    second->step(conformance::k_step);
    second.reset();

    const auto third = make_world();
    CHECK(third->create_body(conformance::floating_ball()).has_value());
}
