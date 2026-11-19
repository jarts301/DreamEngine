#pragma once

#include "dream/core/math/bounds.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/mesh_simplifier/mesh_simplifier.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/rhi/rhi.h"
#include "dream/tangent_generator/tangent_generator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dream::runtime {

// A graphics resource that is released when this object goes away.
template <typename Handle, void (rhi::Device::*Destroy)(Handle)> class GpuResource {
public:
    GpuResource(rhi::Device& device, Handle handle) : m_device(&device), m_handle(handle) {}

    GpuResource(GpuResource&& other) noexcept
        : m_device(other.m_device), m_handle(std::exchange(other.m_handle, Handle{})) {}

    GpuResource& operator=(GpuResource&&) = delete;

    ~GpuResource() {
        if (m_handle.is_valid()) {
            (m_device->*Destroy)(m_handle);
        }
    }

    [[nodiscard]] Handle get() const noexcept {
        return m_handle;
    }

private:
    rhi::Device* m_device;
    Handle m_handle;
};

using GpuBuffer = GpuResource<rhi::BufferHandle, &rhi::Device::destroy_buffer>;
using GpuTexture = GpuResource<rhi::TextureHandle, &rhi::Device::destroy_texture>;
using GpuShader = GpuResource<rhi::ShaderHandle, &rhi::Device::destroy_shader>;
using GpuPipeline = GpuResource<rhi::PipelineHandle, &rhi::Device::destroy_pipeline>;

// The vertices of every model, as the renderer module draws them.
using ModelVertex = renderer::MeshVertex;

// Four bytes per pixel, row by row from the top. A cooked image instead keeps its KTX2 file in
// `ktx2`, transcoded for the device when the model goes to the GPU, and the rest is left empty.
struct ImageData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> pixels{};
    std::vector<std::byte> ktx2{};
    // Whether it holds sRGB colors, like a base color; otherwise linear data, like a normal map.
    // An image used both ways is kept twice.
    bool color = true;
    // Its file name, or its place in the model, for messages.
    std::string name{};
};

// A material, in glTF's metallic-roughness model (ADR 0033). The textures are indices into
// ModelData::images, or -1 for none.
struct ModelMaterialData {
    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    core::Vec3 emissive{0.0f, 0.0f, 0.0f};

    int image = -1;
    int normal_image = -1;
    int metallic_roughness_image = -1;
    int occlusion_image = -1;
    int emissive_image = -1;
};

struct ModelPart {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = 0;
    // Around the part's vertices, in model space. create_model_asset works it out.
    core::Aabb bounds;
    // Simpler versions of the part, each simpler than the last, from generate_lods.
    std::vector<renderer::MeshLod> lods;
};

// A model in ordinary memory, before anything is sent to the GPU.
struct ModelData {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ModelPart> parts;
    std::vector<ModelMaterialData> materials;
    std::vector<ImageData> images;
};

// How generate_lods makes the simpler versions of a model.
struct LodOptions {
    // How many simpler versions each part gets, at most.
    std::uint32_t levels = 3;
    // Each version aims for this fraction of the last one's triangles.
    float reduction = 0.5f;
    // Parts with fewer triangles than this get none.
    std::uint32_t min_triangles = 64;
    // A version that keeps more than this fraction of the last one's triangles isn't worth having,
    // and ends the list.
    float min_saving = 0.8f;
};

// Adds simpler versions of each part to `data`, appending their indices after the others. Parts
// whose simplifier can't make them simpler enough get fewer, or none.
[[nodiscard]] core::Result<void> generate_lods(ModelData& data,
                                               mesh_simplifier::MeshSimplifier& simplifier,
                                               const LodOptions& options = {});

// Gives the parts whose material has a normal map tangents, where their file had none, splitting
// vertices that need different tangents in different triangles. Parts that have tangents, or no
// normal map, are left as they are.
[[nodiscard]] core::Result<void> generate_tangents(ModelData& data,
                                                   tangent_generator::TangentGenerator& generator);

// Reads a PNG or JPEG.
[[nodiscard]] core::Result<ImageData> decode_image(std::span<const std::byte> file);

// Reads a file a model refers to, such as a texture, by its path relative to the model's folder.
using ModelFileReader =
    std::function<core::Result<std::vector<std::byte>>(std::string_view relative_path)>;

// Reads a binary glTF (.glb) file. Node transforms are baked into the vertices, and only triangles
// with normals are accepted. Animation and skinning are ignored. Images are PNG, JPEG or KTX2,
// embedded or in files of their own, which `read_file` reads; textures with the KHR_texture_basisu
// extension use their KTX2 image. Materials keep their metallic-roughness factors and textures,
// normal map, occlusion and emission, with KHR_materials_emissive_strength; every texture is read
// with the first texture coordinates.
[[nodiscard]] core::Result<ModelData> parse_glb(std::span<const std::byte> file,
                                                const ModelFileReader& read_file = {});

// A material on the GPU, as the renderer module draws it. Missing textures are invalid handles.
using ModelMaterial = renderer::MeshMaterial;

// A model on the GPU, as used by runtime::MeshRenderer.
struct ModelAsset {
    GpuBuffer vertices;
    GpuBuffer indices;
    std::vector<GpuTexture> textures;
    std::vector<ModelMaterial> materials;
    std::vector<ModelPart> parts;
};

[[nodiscard]] core::Result<ModelAsset> create_model_asset(rhi::Device& device,
                                                          const ModelData& data);

} // namespace dream::runtime
