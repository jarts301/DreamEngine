#include "dream/runtime/model.h"

#include "dream/core/log.h"
#include "dream/core/math/matrix.h"
#include "dream/runtime/texture.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace dream::runtime {
namespace {

core::Error model_error(std::string message) {
    return core::Error{core::ErrorCode::ParseError, std::move(message)};
}

// The same format, read as sRGB colors or as linear data.
rhi::TextureFormat with_color_space(rhi::TextureFormat format, bool srgb) {
    using enum rhi::TextureFormat;
    switch (format) {
    case Rgba8Srgb:
    case Rgba8Unorm:
        return srgb ? Rgba8Srgb : Rgba8Unorm;
    case Bc7Srgb:
    case Bc7Unorm:
        return srgb ? Bc7Srgb : Bc7Unorm;
    case Astc4x4Srgb:
    case Astc4x4Unorm:
        return srgb ? Astc4x4Srgb : Astc4x4Unorm;
    default:
        return format;
    }
}

struct CgltfDeleter {
    void operator()(cgltf_data* data) const {
        cgltf_free(data);
    }
};

using GltfPointer = std::unique_ptr<cgltf_data, CgltfDeleter>;

// Each image the model has read, by its glTF image and whether it is read as colors.
using KnownImages = std::map<std::pair<const cgltf_image*, bool>, int>;

// Decodes an image once, however many materials use it the same way, and returns its index in
// `model.images`. A KTX2 image is kept as it is, to be transcoded once the device is known.
core::Result<int> add_image(const cgltf_image& image, bool color, ModelData& model,
                            KnownImages& known, const ModelFileReader& read_file) {
    if (const auto existing = known.find({&image, color}); existing != known.end()) {
        return existing->second;
    }

    std::vector<std::byte> file;
    if (image.buffer_view != nullptr && image.buffer_view->buffer->data != nullptr) {
        const auto* const start = static_cast<const std::byte*>(image.buffer_view->buffer->data) +
                                  image.buffer_view->offset;
        file.assign(start, start + image.buffer_view->size);
    } else if (image.uri != nullptr && std::string_view(image.uri).starts_with("data:")) {
        return model_error("Images in data URIs aren't supported; embed them or use files");
    } else if (image.uri != nullptr && read_file) {
        std::string path(image.uri);
        cgltf_decode_uri(path.data());
        path.resize(std::strlen(path.c_str()));
        core::Result<std::vector<std::byte>> read = read_file(path);
        if (!read.has_value()) {
            return core::Error{read.error().code,
                               std::format("The model's image '{}': {}", path, read.error().message)};
        }
        file = std::move(read).value();
    } else {
        return model_error("The model's image is in a file of its own, which can't be read here");
    }

    ImageData data;
    if (is_ktx2(file)) {
        data.ktx2 = std::move(file);
    } else {
        core::Result<ImageData> decoded = decode_image(file);
        if (!decoded.has_value()) {
            return decoded.error();
        }
        data = std::move(decoded).value();
    }

    data.color = color;
    data.name = image.uri != nullptr  ? std::string(image.uri)
                : image.name != nullptr ? std::string(image.name)
                                        : std::format("image {}", model.images.size());
    const int index = static_cast<int>(model.images.size());
    model.images.push_back(std::move(data));
    known.emplace(std::make_pair(&image, color), index);
    return index;
}

// The image a texture reads, or -1 for none. KHR_texture_basisu gives a texture a KTX2 image,
// preferred over its plain one.
core::Result<int> add_texture(const cgltf_texture_view& view, bool color, ModelData& model,
                              KnownImages& images, const ModelFileReader& read_file) {
    const cgltf_texture* const texture = view.texture;
    const cgltf_image* const source =
        texture == nullptr ? nullptr
        : texture->has_basisu && texture->basisu_image != nullptr ? texture->basisu_image
                                                                   : texture->image;
    if (source == nullptr) {
        return -1;
    }
    if (view.texcoord != 0) {
        DREAM_LOG_WARNING("A material's texture uses texture coordinates {}, but only the first "
                          "set is read; it is read with those",
                          view.texcoord);
    }
    return add_image(*source, color, model, images, read_file);
}

core::Result<std::uint32_t> add_material(const cgltf_material* material, ModelData& model,
                                         std::map<const cgltf_material*, std::uint32_t>& known,
                                         KnownImages& images, const ModelFileReader& read_file) {
    if (const auto existing = known.find(material); existing != known.end()) {
        return existing->second;
    }

    // glTF's defaults, which a primitive without a material also gets: white, fully metallic and
    // fully rough.
    ModelMaterialData data;
    data.metallic = 1.0f;
    data.roughness = 1.0f;
    if (material != nullptr) {
        switch (material->alpha_mode) {
        case cgltf_alpha_mode_mask:
            data.alpha_mode = renderer::AlphaMode::Mask;
            break;
        case cgltf_alpha_mode_blend:
            data.alpha_mode = renderer::AlphaMode::Blend;
            break;
        default:
            data.alpha_mode = renderer::AlphaMode::Opaque;
            break;
        }
        data.alpha_cutoff = material->alpha_cutoff;
        data.double_sided = material->double_sided != 0;
        const auto texture = [&](const cgltf_texture_view& view, bool color,
                                 int& image) -> core::Result<void> {
            const core::Result<int> added = add_texture(view, color, model, images, read_file);
            if (!added.has_value()) {
                return added.error();
            }
            image = added.value();
            return {};
        };
        if (material->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
            data.base_color = core::Vec4{pbr.base_color_factor[0], pbr.base_color_factor[1],
                                         pbr.base_color_factor[2], pbr.base_color_factor[3]};
            data.metallic = pbr.metallic_factor;
            data.roughness = pbr.roughness_factor;
            if (auto added = texture(pbr.base_color_texture, true, data.image); !added.has_value()) {
                return added.error();
            }
            if (auto added = texture(pbr.metallic_roughness_texture, false,
                                     data.metallic_roughness_image);
                !added.has_value()) {
                return added.error();
            }
        }
        if (auto added = texture(material->normal_texture, false, data.normal_image);
            !added.has_value()) {
            return added.error();
        }
        data.normal_scale = data.normal_image >= 0 ? material->normal_texture.scale : 1.0f;
        if (auto added = texture(material->occlusion_texture, false, data.occlusion_image);
            !added.has_value()) {
            return added.error();
        }
        data.occlusion_strength = data.occlusion_image >= 0 ? material->occlusion_texture.scale
                                                            : 1.0f;
        if (auto added = texture(material->emissive_texture, true, data.emissive_image);
            !added.has_value()) {
            return added.error();
        }
        const float strength =
            material->has_emissive_strength ? material->emissive_strength.emissive_strength : 1.0f;
        data.emissive = core::Vec3{material->emissive_factor[0], material->emissive_factor[1],
                                   material->emissive_factor[2]} *
                        strength;
    }

    const auto index = static_cast<std::uint32_t>(model.materials.size());
    model.materials.push_back(data);
    known.emplace(material, index);
    return index;
}

core::Result<void> add_primitive(const cgltf_primitive& primitive, const core::Mat4& world,
                                 const core::Mat4& normal_matrix, ModelData& model,
                                 std::map<const cgltf_material*, std::uint32_t>& materials,
                                 KnownImages& images, const ModelFileReader& read_file) {
    if (primitive.type != cgltf_primitive_type_triangles) {
        return model_error("Only triangles are supported");
    }

    const cgltf_accessor* position = nullptr;
    const cgltf_accessor* normal = nullptr;
    const cgltf_accessor* uv = nullptr;
    const cgltf_accessor* tangent = nullptr;
    for (cgltf_size index = 0; index < primitive.attributes_count; ++index) {
        const cgltf_attribute& attribute = primitive.attributes[index];
        if (attribute.type == cgltf_attribute_type_position) {
            position = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_normal) {
            normal = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) {
            uv = attribute.data;
        } else if (attribute.type == cgltf_attribute_type_tangent) {
            tangent = attribute.data;
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
    // Tangents move with the surface, and a mirroring transform turns their bitangents around.
    const core::Vec3 x_axis{world.columns[0].x, world.columns[0].y, world.columns[0].z};
    const core::Vec3 y_axis{world.columns[1].x, world.columns[1].y, world.columns[1].z};
    const core::Vec3 z_axis{world.columns[2].x, world.columns[2].y, world.columns[2].z};
    const float handedness = core::dot(core::cross(x_axis, y_axis), z_axis) < 0.0f ? -1.0f : 1.0f;

    const auto base = static_cast<std::uint32_t>(model.vertices.size());
    for (cgltf_size index = 0; index < position->count; ++index) {
        std::array<cgltf_float, 3> p{};
        std::array<cgltf_float, 3> n{};
        std::array<cgltf_float, 2> t{};
        std::array<cgltf_float, 4> tangent_value{};
        cgltf_accessor_read_float(position, index, p.data(), 3);
        cgltf_accessor_read_float(normal, index, n.data(), 3);
        if (uv != nullptr) {
            cgltf_accessor_read_float(uv, index, t.data(), 2);
        }
        core::Vec4 world_tangent;
        if (tangent != nullptr) {
            cgltf_accessor_read_float(tangent, index, tangent_value.data(), 4);
            const core::Vec3 moved = core::transform_direction(
                world, core::Vec3{tangent_value[0], tangent_value[1], tangent_value[2]});
            if (core::length(moved) > 0.0f) {
                const core::Vec3 unit = core::normalized(moved);
                world_tangent = core::Vec4{unit.x, unit.y, unit.z,
                                           (tangent_value[3] < 0.0f ? -1.0f : 1.0f) * handedness};
            }
        }
        model.vertices.push_back(ModelVertex{
            .position = core::transform_point(world, core::Vec3{p[0], p[1], p[2]}),
            .normal = core::normalized(
                core::transform_direction(normal_matrix, core::Vec3{n[0], n[1], n[2]})),
            .uv = core::Vec2{t[0], t[1]},
            .tangent = world_tangent,
        });
    }

    core::Result<std::uint32_t> material =
        add_material(primitive.material, model, materials, images, read_file);
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
                    std::vector<std::byte>(start, start + size), {}};
    stbi_image_free(pixels);
    return image;
}

core::Result<ModelData> parse_glb(std::span<const std::byte> file,
                                  const ModelFileReader& read_file) {
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
    KnownImages images;

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
            if (core::Result<void> added =
                    add_primitive(node.mesh->primitives[index], world, normal_matrix, model,
                                  materials, images, read_file);
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

core::Result<void> generate_tangents(ModelData& data,
                                     tangent_generator::TangentGenerator& generator) {
    for (const ModelPart& part : data.parts) {
        if (part.material >= data.materials.size() ||
            data.materials[part.material].normal_image < 0 ||
            std::size_t{part.first_index} + part.index_count > data.indices.size()) {
            continue;
        }
        const std::span<std::uint32_t> indices =
            std::span(data.indices).subspan(part.first_index, part.index_count);
        // A part whose file gave tangents keeps them.
        const bool has_tangents = std::ranges::any_of(indices, [&](std::uint32_t index) {
            return index < data.vertices.size() && data.vertices[index].tangent.w != 0.0f;
        });
        if (has_tangents) {
            continue;
        }

        const core::Result<std::vector<core::Vec4>> generated =
            generator.generate(tangent_generator::Request{
                // Again for each part, since earlier parts may have added vertices.
                .vertices = std::as_bytes(std::span(data.vertices)),
                .stride = sizeof(ModelVertex),
                .position_offset = offsetof(ModelVertex, position),
                .normal_offset = offsetof(ModelVertex, normal),
                .uv_offset = offsetof(ModelVertex, uv),
                .indices = indices,
            });
        if (!generated.has_value()) {
            return generated.error();
        }
        const std::vector<core::Vec4>& tangents = generated.value();
        if (tangents.size() != indices.size()) {
            return core::Error{core::ErrorCode::InvalidArgument,
                               "The tangent generator gave the wrong number of tangents"};
        }

        // Each corner's vertex takes its tangent. A vertex that already has a different one, on a
        // seam of the texture, is copied for this corner, or a copy made earlier is reused.
        const auto same = [](const core::Vec4& a, const core::Vec4& b) {
            return a.w == b.w && core::dot(core::Vec3{a.x, a.y, a.z}, core::Vec3{b.x, b.y, b.z}) >
                                     0.999f;
        };
        std::map<std::uint32_t, std::vector<std::uint32_t>> copies;
        for (std::size_t corner = 0; corner < indices.size(); ++corner) {
            const core::Vec4& tangent = tangents[corner];
            std::uint32_t& index = indices[corner];
            ModelVertex& vertex = data.vertices[index];
            if (vertex.tangent.w == 0.0f) {
                vertex.tangent = tangent;
                continue;
            }
            if (same(vertex.tangent, tangent)) {
                continue;
            }
            std::vector<std::uint32_t>& made = copies[index];
            const auto found = std::ranges::find_if(made, [&](std::uint32_t copy) {
                return same(data.vertices[copy].tangent, tangent);
            });
            if (found != made.end()) {
                index = *found;
                continue;
            }
            if (data.vertices.size() >= std::numeric_limits<std::uint32_t>::max()) {
                return model_error("The model has too many vertices");
            }
            ModelVertex copy = data.vertices[index];
            copy.tangent = tangent;
            const auto copy_index = static_cast<std::uint32_t>(data.vertices.size());
            data.vertices.push_back(copy);
            made.push_back(copy_index);
            index = copy_index;
        }
    }
    return {};
}

core::Result<void> generate_lods(ModelData& data, mesh_simplifier::MeshSimplifier& simplifier,
                                 const LodOptions& options) {
    const std::span<const std::byte> vertices = std::as_bytes(std::span(data.vertices));
    for (ModelPart& part : data.parts) {
        part.lods.clear();
        if (part.index_count < options.min_triangles * 3 ||
            std::size_t{part.first_index} + part.index_count > data.indices.size()) {
            continue;
        }
        // Each version is made from the full part, aiming lower each time, which keeps errors from
        // adding up.
        const std::vector<std::uint32_t> full(
            data.indices.begin() + part.first_index,
            data.indices.begin() + part.first_index + part.index_count);
        std::size_t last_count = full.size();
        for (std::uint32_t level = 0; level < options.levels; ++level) {
            const auto target = static_cast<std::size_t>(static_cast<float>(last_count) *
                                                         options.reduction) /
                                3 * 3;
            const core::Result<mesh_simplifier::Simplified> simplified =
                simplifier.simplify(mesh_simplifier::Request{
                    .vertices = vertices,
                    .stride = sizeof(ModelVertex),
                    .indices = full,
                    .target_index_count = target,
                    .max_relative_error = 0.2f,
                    .keep_borders = true,
                });
            if (!simplified.has_value()) {
                return simplified.error();
            }
            const std::vector<std::uint32_t>& indices = simplified.value().indices;
            if (indices.empty() ||
                static_cast<float>(indices.size()) > static_cast<float>(last_count) * options.min_saving) {
                break;
            }
            part.lods.push_back(renderer::MeshLod{
                .first_index = static_cast<std::uint32_t>(data.indices.size()),
                .index_count = static_cast<std::uint32_t>(indices.size()),
                .error = simplified.value().error,
            });
            data.indices.insert(data.indices.end(), indices.begin(), indices.end());
            last_count = indices.size();
        }
    }
    return {};
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
        core::Result<rhi::TextureHandle> texture = rhi::TextureHandle{};
        if (!image.ktx2.empty()) {
            core::Result<TextureData> loaded = load_ktx2(image.ktx2, device);
            if (!loaded.has_value()) {
                return loaded.error();
            }
            // How the material uses it decides how it is read. The bytes are the same either way,
            // but the cooking made its smaller levels for the color space its name asked for.
            rhi::TextureDesc desc = loaded.value().desc;
            if (rhi::is_srgb(desc.format) != image.color) {
                DREAM_LOG_WARNING("'{}' was cooked as {}, but a material reads it as {}; see "
                                  "docs/guides/materials.md on naming textures",
                                  image.name, image.color ? "linear data" : "colors",
                                  image.color ? "colors" : "linear data");
                desc.format = with_color_space(desc.format, image.color);
            }
            texture = device.create_texture(desc, loaded.value().pixels);
        } else {
            texture = device.create_texture(
                rhi::TextureDesc{.width = image.width,
                                 .height = image.height,
                                 .format = image.color ? rhi::TextureFormat::Rgba8Srgb
                                                       : rhi::TextureFormat::Rgba8Unorm},
                image.pixels);
        }
        if (!texture.has_value()) {
            return texture.error();
        }
        textures.emplace_back(device, texture.value());
    }

    const auto texture_of = [&](int image) {
        return image >= 0 && static_cast<std::size_t>(image) < textures.size()
                   ? textures[static_cast<std::size_t>(image)].get()
                   : rhi::TextureHandle{};
    };
    std::vector<ModelMaterial> materials;
    materials.reserve(data.materials.size());
    for (const ModelMaterialData& material : data.materials) {
        materials.push_back(ModelMaterial{
            .base_color = material.base_color,
            .metallic = material.metallic,
            .roughness = material.roughness,
            .normal_scale = material.normal_scale,
            .occlusion_strength = material.occlusion_strength,
            .emissive = material.emissive,
            .alpha_mode = material.alpha_mode,
            .alpha_cutoff = material.alpha_cutoff,
            .double_sided = material.double_sided,
            .base_color_texture = texture_of(material.image),
            .normal_texture = texture_of(material.normal_image),
            .metallic_roughness_texture = texture_of(material.metallic_roughness_image),
            .occlusion_texture = texture_of(material.occlusion_image),
            .emissive_texture = texture_of(material.emissive_image),
        });
    }

    // Each part's bounds, for the renderer to skip parts out of view.
    std::vector<ModelPart> parts = data.parts;
    for (ModelPart& part : parts) {
        part.bounds = {};
        const std::size_t end =
            std::min<std::size_t>(std::size_t{part.first_index} + part.index_count,
                                  data.indices.size());
        for (std::size_t index = part.first_index; index < end; ++index) {
            const std::uint32_t vertex = data.indices[index];
            if (vertex < data.vertices.size()) {
                part.bounds.add(data.vertices[vertex].position);
            }
        }
    }

    return ModelAsset{std::move(vertex_buffer), std::move(index_buffer), std::move(textures),
                      std::move(materials), std::move(parts)};
}

} // namespace dream::runtime
