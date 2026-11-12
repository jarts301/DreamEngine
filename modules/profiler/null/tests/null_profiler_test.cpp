#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tests/conformance.h"

#include "dream/core/config.h"
#include "dream/core/module_registry.h"

#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_profiler_null(core::ModuleRegistry& registry);
}

std::unique_ptr<dream::profiler::Profiler> make_profiler() {
    using namespace dream;
    core::ModuleRegistry registry;
    REQUIRE(modules::register_profiler_null(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"null"};
    auto created = registry.create<profiler::Profiler>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}
