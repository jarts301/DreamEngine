// Simplifies meshes with meshoptimizer (MIT), which collapses edges in the order that changes the
// surface least, and reports how far the result strays.

#include "dream/core/module_registry.h"
#include "dream/mesh_simplifier/mesh_simplifier.h"

#include <meshoptimizer.h>

#include <cstddef>
#include <memory>
#include <string>

namespace dream::modules {
namespace {

using namespace mesh_simplifier;

core::Error invalid(std::string message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::move(message)};
}

class MeshoptimizerSimplifier final : public MeshSimplifier {
public:
    core::Result<Simplified> simplify(const Request& request) override {
        if (request.stride < 3 * sizeof(float) || request.vertices.size() % request.stride != 0) {
            return invalid("The vertices don't divide into positions of the stride given");
        }
        if (request.indices.size() % 3 != 0) {
            return invalid("A mesh to simplify must be made of triangles");
        }
        const std::size_t vertex_count = request.vertices.size() / request.stride;
        for (const std::uint32_t index : request.indices) {
            if (index >= vertex_count) {
                return invalid("A mesh to simplify has an index past its vertices");
            }
        }

        const auto* const positions = reinterpret_cast<const float*>(request.vertices.data());
        Simplified result;
        result.indices.resize(request.indices.size());
        float relative_error = 0.0f;
        const std::size_t count = meshopt_simplify(
            result.indices.data(), request.indices.data(), request.indices.size(), positions,
            vertex_count, request.stride, request.target_index_count, request.max_relative_error,
            request.keep_borders ? meshopt_SimplifyLockBorder : 0, &relative_error);
        result.indices.resize(count);
        // Relative to the mesh's size, which this turns back into its own units.
        result.error = relative_error *
                       meshopt_simplifyScale(positions, vertex_count, request.stride);
        return result;
    }
};

core::Result<std::unique_ptr<MeshSimplifier>> create(const core::Config& /*config*/) {
    return std::unique_ptr<MeshSimplifier>{std::make_unique<MeshoptimizerSimplifier>()};
}

} // namespace

[[nodiscard]] core::Result<void>
register_mesh_simplifier_meshoptimizer(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<MeshSimplifier>{.name = "meshoptimizer",
                                                                 .create = &create});
}

} // namespace dream::modules
