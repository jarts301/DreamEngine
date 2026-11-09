#include "dream/core/module_registry.h"
#include "dream/greeter/greeter.h"

#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace dream::modules {
namespace {

using greeter::Greeter;

class EnglishGreeter final : public Greeter {
public:
    [[nodiscard]] std::string greet(std::string_view name) const override {
        return std::format("Hello, {}!", name);
    }
};

core::Result<std::unique_ptr<Greeter>> create(const core::Config& /*config*/) {
    return std::unique_ptr<Greeter>{std::make_unique<EnglishGreeter>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_greeter_english(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<Greeter>{.name = "english", .create = &create});
}

} // namespace dream::modules
