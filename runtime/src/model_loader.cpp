#include "dream/runtime/model.h"

#include "dream/core/math/matrix.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <string>

namespace dream::runtime {
namespace {

core::Error model_error(std::string message) {
    return core::Error{core::ErrorCode::ParseError, std::move(message)};
}

struct CgltfDeleter {
    void operator()(cgltf_data* data) const {
        cgltf_free(data);
    }
};

using GltfPointer = std::unique_ptr<cgltf_data, CgltfDeleter>;

// Decodes an image once, however many materials use it, and returns its index in `model.images`.
core::Result<int> add_image(const cgltf_image& image, ModelData& model,
                            std::map<const cgltf_image*, int>& known) {
    if (const auto existing = known.find(&image); existing != known.end()) {
        return existing->second;
    }
    if (image.buffer_view == nullptr || image.buffer_view->buffer->data == nullptr) {
        return model_error("Only images embedded in the file are supported");
    }

    const auto* const start = static_cast<const std::byte*>(image.buffer_view->buffer->data) +
                              image.buffer_view->offset;
    core::Result<ImageData> decoded =
        decode_image(std::span<const std::byte>(start, image.buffer_view->size));
    if (!decoded.has_value()) {
        return decoded.error();
    }

    const int index = static_cast<int>(model.images.size());
    model.images.push_back(std::move(decoded).value());
    known.emplace(&image, index);
    return index;
}

core::Result<std::uint32_t> add_material(const cgltf_material* material, ModelData& model,
                                         std::map<const cgltf_material*, std::uint32_t>& known,
                                         std::map<const cgltf_image*, int>& images) {
    if (const auto existing = known.find(material); existing != known.end()) {
        return existing->second;
    }

    ModelMaterialData data;
    if (material != nullptr && material->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
        data.base_color = core::Vec4{pbr.base_color_factor[0], pbr.base_color_factor[1],
                                     pbr.base_color_factor[2], pbr.base_color_factor[3]};

        const cgltf_texture* const texture = pbr.base_color_texture.texture;
        if (texture != nullptr && texture->image != nullptr) {
            core::Result<int> image = add_image(*texture->image, model, images);
            if (!image.has_value()) {
                return image.error();
            }
            data.image = image.value();
        }
    }

    const auto index = static_cast<std::uint32_t>(model.materials.size());
    model.materials.push_back(data);
    known.emplace(material, index);
    return index;
}

core::Result<void> add_primitive(const cgltf_primitive& primitive, const core::Mat4& world,
                                 const core::Mat4& normal_matrix, ModelData& model,
                                 std::map<const cgltf_material*, std::uint32_t>& materials,
                                 std::map<const cgltf_image*, int>& images) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        return model_error("Only triangles are supported");
    }

    const cgltf_accessor* position = nullptr;
    const cgltf_accessor* normal = nullptr;
    const cgltf_accessor* uv = nullptr;
    for (cgltf_size index = 0; index < primitive.attributes_count; ++index) {
        const cgltf_attribute& attribute = primitive.attributes[index];
        if (attribute.type == cgltf_attribute_type_position) {
            position = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_normal) {
            normal = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) {
            uv = attribute.data;
        }
    }
    if (position == nullptr) {
        return model_error("A mesh has no positions");
    }
    if (normal == nullptr) {
        return model_error("A mesh has no normals, so export it with normals enabled");
    }

    if (model.vertices.size() + position->count > std::numeric_limits<std::uint32_t>::max()) {
        return model_error("The model has too many vertices");
    }
    const auto base = static_cast<std::uint32_t>(model.vertices.size());
    for (cgltf_size index = 0; index < position->count; ++index) {
        std::array<cgltf_float, 3> p{};
        std::array<cgltf_float, 3> n{};
        std::array<cgltf_float, 2> t{};
        cgltf_accessor_read_float(position, index, p.data(), 3);
        cgltf_accessor_read_float(normal, index, n.data(), 3);
        if (uv != nullptr) {
            cgltf_accessor_read_float(uv, index, t.data(), 2);
        }
        model.vertices.push_back(ModelVertex{
            core::transform_point(world, core::Vec3{p[0], p[1], p[2]}),
            core::normalized(core::transform_direction(normal_matrix, core::Vec3{n[0], n[1], n[2]})),
            core::Vec2{t[0], t[1]},
        });
    }

    core::Result<std::uint32_t> material =
        add_material(primitive.material, model, materials, images);
    if (!material.has_value()) {
        return material.error();
    }

    ModelPart part;
    part.first_index = static_cast<std::uint32_t>(model.indices.size());
    part.material = material.value();
    if (primitive.indices != nullptr) {
        part.index_count = static_cast<std::uint32_t>(primitive.indices->count);
        for (cgltf_size index = 0; index < primitive.indices->count; ++index) {
            model.indices.push_back(
                base + static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, index)));
        }
    } else {
        part.index_count = static_cast<std::uint32_t>(position->count);
        for (cgltf_size index = 0; index < position->count; ++index) {
            model.indices.push_back(base + static_cast<std::uint32_t>(index));
        }
    }
    model.parts.push_back(part);
    return {};
}

} // namespace

core::Result<ImageData> decode_image(std::span<const std::byte> file) {
    if (file.empty() || file.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return model_error("The image file is empty or too large");
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* const pixels =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file.data()),
                              static_cast<int>(file.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        return model_error(std::format("Could not read an image: {}", stbi_failure_reason()));
    }

    const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    const auto* const start = reinterpret_cast<const std::byte*>(pixels);
    ImageData image{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
                    std::vector<std::byte>(start, start + size)};
    stbi_image_free(pixels);
    return image;
}

core::Result<ModelData> parse_glb(std::span<const std::byte> file) {
    cgltf_options options{};
    options.type = cgltf_file_type_glb;

    cgltf_data* raw = nullptr;
    if (cgltf_parse(&options, file.data(), file.size(), &raw) != cgltf_result_success) {
        return model_error("The file is not a valid .glb model");
    }
    const GltfPointer gltf(raw);

    if (cgltf_load_buffers(&options, gltf.get(), nullptr) != cgltf_result_success) {
        return model_error("The model's data must be embedded in the .glb file");
    }
    if (cgltf_validate(gltf.get()) != cgltf_result_success) {
        return model_error("The model's contents are inconsistent");
    }

    ModelData model;
    std::map<const cgltf_material*, std::uint32_t> materials;
    std::map<const cgltf_image*, int> images;

    for (cgltf_size node_index = 0; node_index < gltf->nodes_count; ++node_index) {
        const cgltf_node& node = gltf->nodes[node_index];
        if (node.mesh == nullptr) {
            continue;
        }

        std::array<cgltf_float, 16> values{};
        cgltf_node_transform_world(&node, values.data());
        core::Mat4 world;
        std::memcpy(&world, values.data(), sizeof(world));

        // Normals need the inverse transpose, so that non-uniform scale doesn't skew them.
        const std::optional<core::Mat4> inverted = core::inverse(world);
        const core::Mat4 normal_matrix = inverted.has_value() ? core::transpose(*inverted) : world;

        for (cgltf_size index = 0; index < node.mesh->primitives_count; ++index) {
            if (core::Result<void> added = add_primitive(node.mesh->primitives[index], world,
                                                         normal_matrix, model, materials, images);
                !added.has_value()) {
                return added.error();
            }
        }
    }

    if (model.parts.empty()) {
        return model_error("The model has no meshes");
    }
    return model;
}

core::Result<ModelAsset> create_model_asset(rhi::Device& device, const ModelData& data) {
    const auto vertices =
        device.create_buffer(rhi::BufferUsage::Vertex, std::as_bytes(std::span(data.vertices)));
    if (!vertices.has_value()) {
        return vertices.error();
    }
    GpuBuffer vertex_buffer(device, vertices.value());

    const auto indices =
        device.create_buffer(rhi::BufferUsage::Index, std::as_bytes(std::span(data.indices)));
    if (!indices.has_value()) {
        return indices.error();
    }
    GpuBuffer index_buffer(device, indices.value());

    std::vector<GpuTexture> textures;
    textures.reserve(data.images.size());
    for (const ImageData& image : data.images) {
        const auto texture = device.create_texture(
            rhi::TextureDesc{.width = image.width, .height = image.height}, image.pixels);
        if (!texture.has_value()) {
            return texture.error();
        }
        textures.emplace_back(device, texture.value());
    }

    std::vector<ModelMaterial> materials;
    materials.reserve(data.materials.size());
    for (const ModelMaterialData& material : data.materials) {
        materials.push_back(ModelMaterial{
            material.base_color,
            material.image >= 0 ? textures[static_cast<std::size_t>(material.image)].get()
                                : rhi::TextureHandle{},
        });
    }

    return ModelAsset{std::move(vertex_buffer), std::move(index_buffer), std::move(textures),
                      std::move(materials), data.parts};
}

} // namespace dream::runtime
