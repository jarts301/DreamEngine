#pragma once

#include "dream/core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace dream::mesh_simplifier {

// Makes simpler versions of triangle meshes, for levels of detail that are drawn when a model is
// small on screen (ADR 0032). The runtime asks for each level when a model loads.

// A simpler version of a mesh, over the same vertices.
struct Simplified {
    // Three per triangle, indexing the vertices given.
    std::vector<std::uint32_t> indices;
    // How far, at most, the simpler surface strays from the original, in the vertices' units.
    float error = 0.0f;
};

struct Request {
    // The vertices' positions, three floats each at the start of every `stride` bytes.
    std::span<const std::byte> vertices;
    std::size_t stride = 0;
    std::span<const std::uint32_t> indices;
    // How many indices to aim for, which the simplifier may not reach.
    std::size_t target_index_count = 0;
    // The most error to allow, as a fraction of the mesh's size.
    float max_relative_error = 0.1f;
    // Keeps the vertices on the mesh's open edges where they are, so pieces of one model that meet
    // there don't open gaps between them.
    bool keep_borders = true;
};

class MeshSimplifier {
public:
    static constexpr std::string_view k_module_interface = "mesh_simplifier";

    virtual ~MeshSimplifier() = default;

    // A simpler version of the request's mesh, or the same indices if it can't be simplified.
    [[nodiscard]] virtual core::Result<Simplified> simplify(const Request& request) = 0;
};

} // namespace dream::mesh_simplifier
