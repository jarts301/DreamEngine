#pragma once

#include "dream/animation/animation.h"
#include "dream/core/config.h"
#include "dream/core/math/bounds.h"
#include "dream/core/math/vector.h"
#include "dream/core/result.h"
#include "dream/mesh_simplifier/mesh_simplifier.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/rhi/rhi.h"
#include "dream/tangent_generator/tangent_generator.h"
#include "dream/texture_decoder/texture_decoder.h"

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
    renderer::AlphaMode alpha_mode = renderer::AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;
    bool double_sided = false;

    int image = -1;
    int normal_image = -1;
    int metallic_roughness_image = -1;
    int occlusion_image = -1;
    int emissive_image = -1;
};

// A box around the vertices one joint of a skin moves, in the skin's bind space.
struct JointBounds {
    std::uint32_t joint = 0;
    core::Aabb bounds;
};

struct ModelPart {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    std::uint32_t material = 0;
    // Around the part's vertices, in model space. create_model_asset works it out.
    core::Aabb bounds;
    // Simpler versions of the part, each simpler than the last, from generate_lods.
    std::vector<renderer::MeshLod> lods;
    // The skin that moves the part, an index into ModelData::skins, or -1 for a static part.
    std::int32_t skin = -1;
    // For a skinned part, a box per joint that moves any of its vertices, from which the posed
    // part's box is worked out each frame. create_model_asset works it out.
    std::vector<JointBounds> joint_bounds{};
};

// The joints a model's skins hang from (ADR 0036): every glTF node a skin uses as a joint, and
// every node above those, so an armature's own transform poses the model as glTF says. Empty for a
// model without skins.
struct ModelSkeleton {
    std::vector<std::int32_t> parents;
    std::vector<animation::JointTransform> rest_pose;
    std::vector<std::string> names;
};

// The joints one skinned mesh is bound to.
struct ModelSkin {
    // Each skin joint's index in the skeleton. The vertices' joint indices index this list.
    std::vector<std::uint32_t> joints;
    // From the mesh's space to each joint's, as the mesh was bound.
    std::vector<core::Mat4> inverse_bind;
};

// One of a model's animations, as keyframes per skeleton joint.
struct ModelClip {
    std::string name;
    float duration = 0.0f;
    std::vector<animation::JointTrack> tracks;
};

// A model in ordinary memory, before anything is sent to the GPU.
struct ModelData {
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ModelPart> parts;
    std::vector<ModelMaterialData> materials;
    std::vector<ImageData> images;
    ModelSkeleton skeleton{};
    std::vector<ModelSkin> skins{};
    std::vector<ModelClip> clips{};
};

// Each skeleton joint's transform in the model's space, posed at rest.
[[nodiscard]] std::vector<core::Mat4> rest_model_space(const ModelSkeleton& skeleton);

// A skin's skinning matrices for a posed skeleton, whose joints' model-space transforms are
// `model_space`, written into `out`, one per skin joint.
void skinning_matrices(const ModelSkin& skin, std::span<const core::Mat4> model_space,
                       std::vector<core::Mat4>& out);

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
    // Whether the build makes them, with the model cook tool, instead of the runtime as models
    // load (ADR 0042). Only the build reads it.
    bool cook = true;
};

// Reads the [lods] table of a project's configuration:
//
//     [lods]
//     levels = 3          # Versions per part, at most; 0 makes none.
//     reduction = 0.5     # Each aims for this fraction of the last one's triangles.
//     min_triangles = 64  # Smaller parts get none.
//     cook = true         # Made at build time, where the build can.
[[nodiscard]] core::Result<LodOptions> read_lod_options(const core::Config& config);

// Levels of detail made at build time (ADR 0042): a model's simpler versions, cooked by the model
// cook tool into a file beside it, which the runtime reads in place of making them as the model
// loads. The file names the model it was made from, by its size and a hash of its bytes, and the
// options and geometry it was made with, so a model or setting changed since is noticed, and its
// versions are made again as it loads.
inline constexpr std::uint32_t k_cooked_lods_version = 1;

// Where a model's cooked versions are: "models/ball.glb" has "models/ball.lods".
[[nodiscard]] std::string cooked_lods_path(std::string_view model_path);

// The cooked versions of `data`, which generate_lods has given its versions, made from the model
// file `source` with `options`.
[[nodiscard]] std::vector<std::byte> write_cooked_lods(const ModelData& data,
                                                      std::span<const std::byte> source,
                                                      const LodOptions& options);

// Gives `data`, parsed from `source` and with its tangents made, the versions cooked for it, as
// generate_lods would have. Fails, leaving `data` as it was, if they were made from another file,
// other options, or other geometry, which a change to the engine's model reading can cause.
[[nodiscard]] core::Result<void> apply_cooked_lods(ModelData& data,
                                                   std::span<const std::byte> cooked,
                                                   std::span<const std::byte> source,
                                                   const LodOptions& options);

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

// Reads a binary glTF (.glb) file. Node transforms are baked into the vertices of static meshes,
// and only triangles with normals are accepted. Skins, joint weights and animations are read into
// a skeleton, skins and clips (ADR 0036): skinned meshes keep their own space, as glTF says, and
// meshes that hang from a joint without a skin follow that joint. Morph targets are ignored. Images are PNG, JPEG or KTX2,
// embedded or in files of their own, which `read_file` reads; textures with the KHR_texture_basisu
// extension use their KTX2 image. Materials keep their metallic-roughness factors and textures,
// normal map, occlusion and emission, with KHR_materials_emissive_strength, and their alpha mode,
// alpha cutoff and double-sidedness; every texture is read with the first texture coordinates.
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
    // For animation (ADR 0036). The runtime's Animation builds its own form of these.
    ModelSkeleton skeleton{};
    std::vector<ModelSkin> skins{};
    std::vector<ModelClip> clips{};
    // Each skin's matrices at rest, for drawing a skinned model nothing animates.
    std::vector<std::vector<core::Mat4>> rest_skinning{};
};

// Puts a model on the GPU. Its cooked images, if any, are read with `decoder`, the project's
// texture decoder, and fail without one.
[[nodiscard]] core::Result<ModelAsset>
create_model_asset(rhi::Device& device, const ModelData& data,
                   texture_decoder::TextureDecoder* decoder = nullptr);

} // namespace dream::runtime
