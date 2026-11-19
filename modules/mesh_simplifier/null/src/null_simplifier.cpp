// A simplifier that never simplifies, so models get no levels of detail.

#include "dream/core/module_registry.h"
#include "dream/mesh_simplifier/mesh_simplifier.h"

#include <memory>

namespace dream::modules {
namespace {

using namespace mesh_simplifier;

class NullSimplifier final : public MeshSimplifier {
public:
    core::Result<Simplified> simplify(const Request& request) override {
        return Simplified{.indices = {request.indices.begin(), request.indices.end()},
                          .error = 0.0f};
    }
};

core::Result<std::unique_ptr<MeshSimplifier>> create(const core::Config& /*config*/) {
    return std::unique_ptr<MeshSimplifier>{std::make_unique<NullSimplifier>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_mesh_simplifier_null(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<MeshSimplifier>{.name = "null", .create = &create});
}

} // namespace dream::modules
