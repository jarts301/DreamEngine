#pragma once

#include "dream/core/config.h"
#include "dream/core/result.h"

#include <concepts>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dream::core {

// A subsystem contract that backends can implement. `k_module_interface` is the name used in the
// project configuration, such as "physics".
template <typename T>
concept ModuleInterface = std::has_virtual_destructor_v<T> && requires {
    { T::k_module_interface } -> std::convertible_to<std::string_view>;
};

template <ModuleInterface Interface>
using ModuleFactory = Result<std::unique_ptr<Interface>> (*)(const Config& config);

// Describes one backend of `Interface`.
template <ModuleInterface Interface> struct ModuleDescriptor {
    // The name used in the project configuration, such as "jolt".
    std::string_view name;

    // Receives the whole project configuration, so a backend can read its own settings. Returning
    // an error makes selection move on to the next backend in the list.
    ModuleFactory<Interface> create = nullptr;

    // Optional check for whether this machine can run the backend. Null means it always can.
    bool (*is_supported)() = nullptr;
};

template <ModuleInterface Interface> struct CreatedModule {
    std::unique_ptr<Interface> instance;
    std::string name;
};

struct ModuleId {
    std::string interface_name;
    std::string name;
};

namespace detail {

// Identifies an interface type without RTTI. It is deliberately not const: linkers can merge
// identical read-only data, which would give two interfaces the same address.
template <typename Interface> inline char g_module_type_tag = 0;

} // namespace detail

// The backends linked into this program, grouped by interface.
//
// Fill it during startup, before any other thread reads it. The registry itself does no locking.
class ModuleRegistry {
public:
    // Fails if the descriptor is incomplete or this backend is already registered.
    template <ModuleInterface Interface>
    [[nodiscard]] Result<void> add(const ModuleDescriptor<Interface>& descriptor) {
        return add_entry(Entry{
            .type = &detail::g_module_type_tag<Interface>,
            .interface_name = std::string(Interface::k_module_interface),
            .name = std::string(descriptor.name),
            .factory = reinterpret_cast<ErasedFactory>(descriptor.create),
            .is_supported = descriptor.is_supported,
        });
    }

    [[nodiscard]] bool contains(std::string_view interface_name, std::string_view name) const;

    // Every registered backend, in registration order.
    [[nodiscard]] std::vector<ModuleId> modules() const;

    // Creates the first backend in `preference` that is registered, supported and starts
    // successfully. Backends that don't qualify are logged and skipped. The error lists why each
    // one was rejected.
    template <ModuleInterface Interface>
    [[nodiscard]] Result<CreatedModule<Interface>> create(std::span<const std::string> preference,
                                                          const Config& config) const {
        const std::string_view interface_name = Interface::k_module_interface;
        std::string rejected;

        for (const std::string& name : preference) {
            const Entry* const entry =
                find_usable(&detail::g_module_type_tag<Interface>, interface_name, name, rejected);
            if (entry == nullptr) {
                continue;
            }

            // Safe because find_usable matched the type this factory was registered with.
            const auto factory = reinterpret_cast<ModuleFactory<Interface>>(entry->factory);
            Result<std::unique_ptr<Interface>> created = factory(config);
            if (!created.has_value()) {
                record_rejection(interface_name, name, created.error().message, rejected);
            } else if (created.value() == nullptr) {
                record_rejection(interface_name, name, "its factory returned null", rejected);
            } else {
                log_selected(interface_name, name);
                return CreatedModule<Interface>{std::move(created).value(), name};
            }
        }

        return no_usable_module(interface_name, rejected);
    }

private:
    using ErasedFactory = void (*)();

    struct Entry {
        const void* type = nullptr;
        std::string interface_name;
        std::string name;
        ErasedFactory factory = nullptr;
        bool (*is_supported)() = nullptr;
    };

    [[nodiscard]] Result<void> add_entry(Entry entry);

    [[nodiscard]] const Entry* find_usable(const void* type, std::string_view interface_name,
                                           std::string_view name, std::string& rejected) const;

    static void record_rejection(std::string_view interface_name, std::string_view name,
                                 std::string_view reason, std::string& rejected);
    static void log_selected(std::string_view interface_name, std::string_view name);
    [[nodiscard]] static Error no_usable_module(std::string_view interface_name,
                                                std::string_view rejected);

    std::vector<Entry> m_entries;
};

} // namespace dream::core
