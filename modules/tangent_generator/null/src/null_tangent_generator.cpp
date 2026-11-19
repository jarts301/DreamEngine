// A generator that makes no tangents, so models without their own ignore their normal maps.

#include "dream/core/module_registry.h"
#include "dream/tangent_generator/tangent_generator.h"

#include <memory>
#include <vector>

namespace dream::modules {
namespace {

using namespace tangent_generator;

class NullTangentGenerator final : public TangentGenerator {
public:
    core::Result<std::vector<core::Vec4>> generate(const Request& request) override {
        return std::vector<core::Vec4>(request.indices.size());
    }
};

core::Result<std::unique_ptr<TangentGenerator>> create(const core::Config& /*config*/) {
    return std::unique_ptr<TangentGenerator>{std::make_unique<NullTangentGenerator>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_tangent_generator_null(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<TangentGenerator>{.name = "null", .create = &create});
}

} // namespace dream::modules
