#include "dream/runtime/model.h"

#include "dream/core/log.h"
#include "dream/runtime/texture.h"

#include <span>
#include <utility>
#include <vector>

namespace dream::runtime {
namespace {

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

} // namespace

core::Result<ModelAsset> create_model_asset(rhi::Device& device, const ModelData& data,
                                            texture_decoder::TextureDecoder* decoder) {
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
            core::Result<TextureData> loaded =
                decode_cooked_texture(decoder, image.ktx2, device, image.name);
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

    // Each part's bounds, for the renderer to skip parts out of view. A skinned part also gets a
    // box per joint that moves its vertices, which the posed part's box is made from.
    std::vector<ModelPart> parts = data.parts;
    for (ModelPart& part : parts) {
        part.bounds = {};
        part.joint_bounds.clear();
        const bool skinned = part.skin >= 0 && static_cast<std::size_t>(part.skin) < data.skins.size();
        std::map<std::uint32_t, core::Aabb> by_joint;
        const std::size_t end =
            std::min<std::size_t>(std::size_t{part.first_index} + part.index_count,
                                  data.indices.size());
        for (std::size_t index = part.first_index; index < end; ++index) {
            const std::uint32_t vertex = data.indices[index];
            if (vertex >= data.vertices.size()) {
                continue;
            }
            const ModelVertex& v = data.vertices[vertex];
            part.bounds.add(v.position);
            if (skinned) {
                for (std::size_t slot = 0; slot < 4; ++slot) {
                    if (v.weights[slot] > 0) {
                        by_joint[v.joints[slot]].add(v.position);
                    }
                }
            }
        }
        for (const auto& [joint, bounds] : by_joint) {
            part.joint_bounds.push_back(JointBounds{.joint = joint, .bounds = bounds});
        }
    }

    ModelAsset asset{std::move(vertex_buffer), std::move(index_buffer), std::move(textures),
                     std::move(materials), std::move(parts)};
    asset.skeleton = data.skeleton;
    asset.skins = data.skins;
    asset.clips = data.clips;
    const std::vector<core::Mat4> rest = rest_model_space(data.skeleton);
    for (const ModelSkin& skin : data.skins) {
        skinning_matrices(skin, rest, asset.rest_skinning.emplace_back());
    }
    return asset;
}

} // namespace dream::runtime
