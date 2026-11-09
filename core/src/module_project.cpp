#include "dream/core/module_project.h"

#include <utility>

namespace dream::core {

ModuleProject::ModuleProject(Config config, ModuleRegistry registry, std::string_view platform)
    : m_config(std::move(config)), m_registry(std::move(registry)), m_platform(platform) {}

Result<ModuleProject> ModuleProject::load(const ModuleProjectDefinition& definition) {
    Result<Config> config = Config::parse(definition.config_text);
    if (!config.has_value()) {
        return Error{config.error().code,
                     std::format("Invalid project configuration: {}", config.error().message)};
    }

    ModuleRegistry registry;
    if (definition.register_modules != nullptr) {
        Result<void> registered = definition.register_modules(registry);
        if (!registered.has_value()) {
            return registered.error();
        }
    }

    return ModuleProject(std::move(config).value(), std::move(registry), definition.platform);
}

Error ModuleProject::missing_list(std::string_view key) {
    return Error{ErrorCode::NotFound,
                 std::format("The project configuration has no module list named '{}'", key)};
}

} // namespace dream::core
