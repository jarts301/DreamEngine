// Picks a Greeter from the project configuration and uses it through the interface alone. Nothing
// here names a backend.

#include "dream/core/log.h"
#include "dream/core/module_project.h"
#include "dream/greeter/greeter.h"

#include <cstdio>
#include <format>
#include <string>

using namespace dream;

int main() {
    const core::Result<core::ModuleProject> project =
        core::ModuleProject::load(core::generated_module_project());
    if (!project.has_value()) {
        DREAM_LOG_ERROR("{}", project.error().message);
        return 1;
    }

    const core::Result<core::CreatedModule<greeter::Greeter>> selected =
        project.value().create<greeter::Greeter>();
    if (!selected.has_value()) {
        DREAM_LOG_ERROR("{}", selected.error().message);
        return 1;
    }

    std::string linked;
    for (const core::ModuleId& module : project.value().registry().modules()) {
        linked += (linked.empty() ? "" : ",") + module.name;
    }

    const std::string report =
        std::format("selected={} greeting={} linked={}", selected.value().name,
                    selected.value().instance->greet("Dream"), linked);
    std::puts(report.c_str());
    return 0;
}
