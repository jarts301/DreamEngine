#pragma once

#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/rhi/rhi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
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
    std::vector<std::byte> pixels;
    std::vector<std::byte> ktx2;
};

struct ModelMaterialData {
    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};

    // An index into ModelData::images, or -1 for no texture.
    int image = -1;
};

struct ModelPart {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = 0;
};

// A model in ordinary memory, before anything is sent to the GPU.
struct ModelData {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ModelPart> parts;
    std::vector<ModelMaterialData> materials;
    std::vector<ImageData> images;
};

// Reads a PNG or JPEG.
[[nodiscard]] core::Result<ImageData> decode_image(std::span<const std::byte> file);

// Reads a file a model refers to, such as a texture, by its path relative to the model's folder.
using ModelFileReader =
    std::function<core::Result<std::vector<std::byte>>(std::string_view relative_path)>;

// Reads a binary glTF (.glb) file. Node transforms are baked into the vertices, and only triangles
// with normals are accepted. Animation and skinning are ignored. Images are PNG, JPEG or KTX2,
// embedded or in files of their own, which `read_file` reads; textures with the KHR_texture_basisu
// extension use their KTX2 image.
[[nodiscard]] core::Result<ModelData> parse_glb(std::span<const std::byte> file,
                                                const ModelFileReader& read_file = {});

struct ModelMaterial {
    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};

    // Invalid for a material with no texture, which draws with plain white.
    rhi::TextureHandle texture;
};

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
