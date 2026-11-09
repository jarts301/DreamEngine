#include "dream/core/module_registry.h"

#include "dream/core/log.h"

#include <algorithm>
#include <format>

namespace dream::core {

Result<void> ModuleRegistry::add_entry(Entry entry) {
    if (entry.name.empty() || entry.factory == nullptr) {
        return Error{ErrorCode::InvalidArgument,
                     std::format("A '{}' module needs both a name and a factory",
                                 entry.interface_name)};
    }

    for (const Entry& existing : m_entries) {
        if (existing.interface_name != entry.interface_name) {
            continue;
        }
        if (existing.type != entry.type) {
            return Error{ErrorCode::InvalidArgument,
                         std::format("Two different interfaces are both named '{}'",
                                     entry.interface_name)};
        }
        if (existing.name == entry.name) {
            return Error{ErrorCode::InvalidArgument,
                         std::format("Module '{}' is already registered for interface '{}'",
                                     entry.name, entry.interface_name)};
        }
    }

    m_entries.push_back(std::move(entry));
    return {};
}

bool ModuleRegistry::contains(std::string_view interface_name, std::string_view name) const {
    return std::ranges::any_of(m_entries, [&](const Entry& entry) {
        return entry.interface_name == interface_name && entry.name == name;
    });
}

std::vector<ModuleId> ModuleRegistry::modules() const {
    std::vector<ModuleId> result;
    result.reserve(m_entries.size());
    for (const Entry& entry : m_entries) {
        result.push_back(ModuleId{entry.interface_name, entry.name});
    }
    return result;
}

const ModuleRegistry::Entry* ModuleRegistry::find_usable(const void* type,
                                                         std::string_view interface_name,
                                                         std::string_view name,
                                                         std::string& rejected) const {
    const auto entry = std::ranges::find_if(m_entries, [&](const Entry& candidate) {
        return candidate.type == type && candidate.name == name;
    });

    if (entry == m_entries.end()) {
        record_rejection(interface_name, name, "it is not linked into this build", rejected);
        return nullptr;
    }
    if (entry->is_supported != nullptr && !entry->is_supported()) {
        record_rejection(interface_name, name, "it is not supported on this machine", rejected);
        return nullptr;
    }
    return &*entry;
}

void ModuleRegistry::record_rejection(std::string_view interface_name, std::string_view name,
                                      std::string_view reason, std::string& rejected) {
    DREAM_LOG_WARNING("Skipping module '{}' for interface '{}': {}", name, interface_name, reason);
    rejected += std::format(" '{}': {}.", name, reason);
}

void ModuleRegistry::log_selected(std::string_view interface_name, std::string_view name) {
    DREAM_LOG_INFO("Using module '{}' for interface '{}'", name, interface_name);
}

Error ModuleRegistry::no_usable_module(std::string_view interface_name, std::string_view rejected) {
    return Error{ErrorCode::NotFound,
                 rejected.empty()
                     ? std::format("No modules are listed for interface '{}'", interface_name)
                     : std::format("No usable module for interface '{}':{}", interface_name,
                                   rejected)};
}

} // namespace dream::core
