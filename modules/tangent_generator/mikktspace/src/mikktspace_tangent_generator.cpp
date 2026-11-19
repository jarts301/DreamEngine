// Tangents by MikkTSpace (zlib license), the method glTF asks for when a file has none, and the
// one baking tools such as Blender use, so normal maps baked by them line up (ADR 0033).

#include "dream/core/module_registry.h"
#include "dream/tangent_generator/tangent_generator.h"

#include <mikktspace.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace dream::modules {
namespace {

using namespace tangent_generator;

struct Context {
    const Request* request = nullptr;
    std::vector<core::Vec4>* tangents = nullptr;
};

const Context& context_of(const SMikkTSpaceContext* context) {
    return *static_cast<const Context*>(context->m_pUserData);
}

template <std::size_t Count>
void read_floats(const SMikkTSpaceContext* context, int face, int corner, std::size_t offset,
                 float* out) {
    const Request& request = *context_of(context).request;
    const std::uint32_t vertex =
        request.indices[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(corner)];
    std::memcpy(out, request.vertices.data() + vertex * request.stride + offset,
                Count * sizeof(float));
}

int face_count(const SMikkTSpaceContext* context) {
    return static_cast<int>(context_of(context).request->indices.size() / 3);
}

int corners_of_face(const SMikkTSpaceContext* /*context*/, int /*face*/) {
    return 3;
}

void position(const SMikkTSpaceContext* context, float out[], int face, int corner) {
    read_floats<3>(context, face, corner, context_of(context).request->position_offset, out);
}

void normal(const SMikkTSpaceContext* context, float out[], int face, int corner) {
    read_floats<3>(context, face, corner, context_of(context).request->normal_offset, out);
}

// MikkTSpace's bitangent follows v, and glTF's v grows down the image, while its normal maps put
// +Y at the top. Flipping v here makes the bitangent point up the image, as Blender's do.
void texture_coordinates(const SMikkTSpaceContext* context, float out[], int face, int corner) {
    read_floats<2>(context, face, corner, context_of(context).request->uv_offset, out);
    out[1] = 1.0f - out[1];
}

void set_tangent(const SMikkTSpaceContext* context, const float tangent[], float sign, int face,
                 int corner) {
    std::vector<core::Vec4>& tangents = *context_of(context).tangents;
    tangents[static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(corner)] =
        core::Vec4{tangent[0], tangent[1], tangent[2], sign < 0.0f ? -1.0f : 1.0f};
}

class MikkTSpaceGenerator final : public TangentGenerator {
public:
    core::Result<std::vector<core::Vec4>> generate(const Request& request) override {
        const std::size_t needed = std::max({request.position_offset + 3 * sizeof(float),
                                             request.normal_offset + 3 * sizeof(float),
                                             request.uv_offset + 2 * sizeof(float)});
        if (request.indices.size() % 3 != 0 || request.stride < needed) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "Tangents need whole triangles and vertices with positions, normals "
                               "and texture coordinates"};
        }
        const std::size_t vertex_count = request.vertices.size() / request.stride;
        for (const std::uint32_t index : request.indices) {
            if (index >= vertex_count) {
                return core::Error{core::ErrorCode::InvalidArgument,
                                   "A triangle uses a vertex that doesn't exist"};
            }
        }

        std::vector<core::Vec4> tangents(request.indices.size());
        if (request.indices.empty()) {
            return tangents;
        }
        Context user{.request = &request, .tangents = &tangents};
        SMikkTSpaceInterface callbacks{};
        callbacks.m_getNumFaces = &face_count;
        callbacks.m_getNumVerticesOfFace = &corners_of_face;
        callbacks.m_getPosition = &position;
        callbacks.m_getNormal = &normal;
        callbacks.m_getTexCoord = &texture_coordinates;
        callbacks.m_setTSpaceBasic = &set_tangent;
        SMikkTSpaceContext context{.m_pInterface = &callbacks, .m_pUserData = &user};
        if (genTangSpaceDefault(&context) == 0) {
            return core::Error{core::ErrorCode::OutOfMemory, "MikkTSpace ran out of memory"};
        }
        return tangents;
    }
};

core::Result<std::unique_ptr<TangentGenerator>> create(const core::Config& /*config*/) {
    return std::unique_ptr<TangentGenerator>{std::make_unique<MikkTSpaceGenerator>()};
}

} // namespace

[[nodiscard]] core::Result<void> register_tangent_generator_mikktspace(core::ModuleRegistry& registry) {
    return registry.add(
        core::ModuleDescriptor<TangentGenerator>{.name = "mikktspace", .create = &create});
}

} // namespace dream::modules
