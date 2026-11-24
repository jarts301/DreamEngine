#pragma once

#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/core/result.h"

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::core {

// What the build knows about a project's modules.
struct ModuleProjectDefinition {
    // The section of the configuration that applies to this build, such as "windows".
    std::string_view platform;
    std::string_view config_text;
    Result<void> (*register_modules)(ModuleRegistry& registry) = nullptr;
};

// The definition for this program. It is defined in a source file that the CMake function
// dream_link_modules generates, so linking fails if the program's target didn't call it.
[[nodiscard]] ModuleProjectDefinition generated_module_project();

// A project's configuration together with the backends linked into it.
//
// A backend is chosen with the list under `[modules.<platform>]` that is named after its interface:
//
//     [modules.windows]
//     physics = ["jolt", "bullet"]
class ModuleProject {
public:
    [[nodiscard]] static Result<ModuleProject> load(const ModuleProjectDefinition& definition);

    // Creates the first usable backend from the configured list for `Interface`.
    template <ModuleInterface Interface>
    [[nodiscard]] Result<CreatedModule<Interface>> create() const {
        const std::string key =
            std::format("modules.{}.{}", m_platform, Interface::k_module_interface);
        const auto preference = m_config.get_string_list(key);
        if (!preference.has_value()) {
            return missing_list(key);
        }
        return m_registry.create<Interface>(*preference, m_config);
    }

    // Creates every backend in the configured list for `Interface`, in its order, for interfaces
    // of which a program uses several at once, such as editor panels. A backend that can't start
    // is logged and left out; an empty list, or none, gives none.
    template <ModuleInterface Interface>
    [[nodiscard]] std::vector<CreatedModule<Interface>> create_all() const {
        std::vector<CreatedModule<Interface>> created;
        const auto names = m_config.get_string_list(
            std::format("modules.{}.{}", m_platform, Interface::k_module_interface));
        if (!names.has_value()) {
            return created;
        }
        for (const std::string& name : *names) {
            Result<CreatedModule<Interface>> one =
                m_registry.create<Interface>(std::span<const std::string>(&name, 1), m_config);
            if (one.has_value()) {
                created.push_back(std::move(one).value());
            }
        }
        return created;
    }

    // Whether the configuration has a list for `Interface` on this platform. A subsystem a project
    // doesn't list is simply left out.
    template <ModuleInterface Interface> [[nodiscard]] bool is_configured() const {
        return m_config.contains(
            std::format("modules.{}.{}", m_platform, Interface::k_module_interface));
    }

    [[nodiscard]] const ModuleRegistry& registry() const noexcept {
        return m_registry;
    }

    [[nodiscard]] const Config& config() const noexcept {
        return m_config;
    }

    [[nodiscard]] std::string_view platform() const noexcept {
        return m_platform;
    }

private:
    ModuleProject(Config config, ModuleRegistry registry, std::string_view platform);

    [[nodiscard]] static Error missing_list(std::string_view key);

    Config m_config;
    ModuleRegistry m_registry;
    std::string m_platform;
};

} // namespace dream::core
