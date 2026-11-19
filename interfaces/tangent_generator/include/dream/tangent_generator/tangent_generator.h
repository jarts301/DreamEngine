#pragma once

#include "dream/core/math/vector.h"
#include "dream/core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace dream::tangent_generator {

// Works out tangents for meshes whose files don't have them, so their normal maps can be placed on
// the surface (ADR 0033). The runtime asks for them as a model loads, for the parts whose material
// has a normal map.

struct Request {
    // The vertices, `stride` bytes each, with three floats of position and of normal and two of
    // texture coordinates at these offsets.
    std::span<const std::byte> vertices;
    std::size_t stride = 0;
    std::size_t position_offset = 0;
    std::size_t normal_offset = 0;
    std::size_t uv_offset = 0;
    // Three per triangle.
    std::span<const std::uint32_t> indices;
};

class TangentGenerator {
public:
    static constexpr std::string_view k_module_interface = "tangent_generator";

    virtual ~TangentGenerator() = default;

    // One tangent per index, so each corner of each triangle gets its own: xyz along the direction
    // in which the texture's u grows, w = 1 or -1 so that cross(normal, xyz) * w points toward the
    // top of the texture, as glTF's normal maps expect. Texture coordinates are glTF's, with v
    // growing down the image. All zero where none can be found, or from a generator that makes
    // none.
    [[nodiscard]] virtual core::Result<std::vector<core::Vec4>> generate(const Request& request) = 0;
};

} // namespace dream::tangent_generator
