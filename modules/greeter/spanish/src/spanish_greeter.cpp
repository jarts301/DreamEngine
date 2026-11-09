#include "dream/core/module_registry.h"
#include "dream/greeter/greeter.h"

#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace dream::modules {
namespace {

using greeter::Greeter;

class SpanishGreeter final : public Greeter {
public:
    [[nodiscard]] std::string greet(std::string_view name) const override {
        return std::format("Hola, {}!", name);
    }
};

core::Result<std::unique_ptr<Greeter>> create(const core::Config& config) {
    // Lets a project simulate a backend that fails to start.
    if (!config.get_bool("greeter.spanish.available").value_or(true)) {
        return core::Error{core::ErrorCode::Unknown, "disabled by greeter.spanish.available"};
    }
    return std::unique_ptr<Greeter>{std::make_unique<SpanishGreeter>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_greeter_spanish(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Greeter>{.name = "spanish", .create = &create});
}

} // namespace dream::modules
