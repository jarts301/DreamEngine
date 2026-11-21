// The 'forward' renderer (ADR 0030): one directional light with cascaded shadows (ADR 0029), drawn
// in HDR and tone-mapped with Khronos PBR Neutral (ADR 0028), then debug lines and the interface
// layers over the result. Parts out of the camera's view, or out of a shadow cascade, are skipped
// (ADR 0031). Surfaces are lit with glTF's metallic-roughness model and normal maps (ADR 0033).
// Opaque parts are drawn first, then cut-out ones, then blended ones, farthest first (ADR 0034).
// Skinned parts are moved by their joints in the vertex shader, one draw each (ADR 0036).
//
// Point and spot lights (ADR 0037) go to the shaders in a float texture rewritten each frame: the
// lights, then a list of them for each cluster of the view, a box of the screen at a range of
// depths, which every pixel reads only its own of. Built with DREAM_FORWARD_PLUS, this is the
// 'forward_plus' module, which splits the view into 16x9x24 clusters and takes 256 lights; without
// it, 'forward' keeps one cluster of the 16 nearest lights.

#include "dream/core/config.h"
#include "dream/core/log.h"
#include "dream/core/math/bounds.h"
#include "dream/core/module_registry.h"
#include "dream/core/process_stats.h"
#include "dream/core/profile.h"
#include "dream/renderer/scene_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace dream::modules {
namespace {

using core::Mat4;
using core::Vec3;
using core::Vec4;
using namespace renderer;

// This matches the block of the same name in the module's shaders: forward.slang and shadow.slang
// share it.
struct FrameUniforms {
    Mat4 view_projection;
    Vec4 camera_position;
    Vec4 direction_to_light;
    Vec4 light_color_and_ambient;
    Mat4 light_view_projection;
    // The shaders read these as 16 columns, not as an array of matrices, which Adreno drivers
    // misread from uniform blocks. The bytes are the same.
    std::array<Mat4, 4> cascades;
    Vec4 cascade_far;
    Vec4 cascade_normal_offset;
    Vec4 shadow_settings;
    Vec4 camera_forward;
    // The environment: intensity (0 for none), its last level, and its rotation's cosine and sine.
    Vec4 environment;
};

// One instance of a part, as forward.slang reads it per instance: the columns of its model
// matrix and of its normals' matrix, and its material's factors. shadow.slang reads the model
// matrix alone (ADR 0032, ADR 0033).
struct InstanceData {
    std::array<Vec4, 4> model;
    std::array<Vec4, 3> normals;
    Vec4 base_color;
    // Metallic, roughness, normal scale and occlusion strength.
    Vec4 material;
    // Emissive color, and in w which of the material's textures and emission the shader reads,
    // as MaterialFeature bits.
    Vec4 emissive;
};

// What forward.slang reads for a part. Leaving out what a material doesn't have skips texture
// reads, which the ground and plain props would otherwise pay for on every pixel (ADR 0033).
enum MaterialFeature : std::uint32_t {
    k_feature_normal_map = 1,
    k_feature_metallic_roughness_map = 2,
    k_feature_occlusion_map = 4,
    k_feature_emission = 8,
    k_feature_double_sided = 16,
    k_feature_blend = 32,
};

// The passes that draw parts, each with the parts of one alpha mode.
enum class Pass : std::uint8_t { Shadow, ShadowMasked, Opaque, Masked, Blend };

bool is_shadow(Pass pass) {
    return pass == Pass::Shadow || pass == Pass::ShadowMasked;
}

// Whether a pass draws a part.
bool in_pass(const MeshDraw& mesh, Pass pass) {
    const AlphaMode mode = mesh.material.alpha_mode;
    switch (pass) {
    case Pass::Shadow:
        return mesh.casts_shadows && mode == AlphaMode::Opaque;
    case Pass::ShadowMasked:
        return mesh.casts_shadows && mode == AlphaMode::Mask;
    case Pass::Opaque:
        return mode == AlphaMode::Opaque;
    case Pass::Masked:
        return mode == AlphaMode::Mask;
    case Pass::Blend:
        return mode == AlphaMode::Blend;
    }
    return false;
}

float material_features(const MeshMaterial& material) {
    std::uint32_t features = 0;
    if (material.normal_texture.is_valid() && material.normal_scale != 0.0f) {
        features |= k_feature_normal_map;
    }
    if (material.metallic_roughness_texture.is_valid()) {
        features |= k_feature_metallic_roughness_map;
    }
    if (material.occlusion_texture.is_valid() && material.occlusion_strength != 0.0f) {
        features |= k_feature_occlusion_map;
    }
    if (material.emissive.x > 0.0f || material.emissive.y > 0.0f || material.emissive.z > 0.0f) {
        features |= k_feature_emission;
    }
    if (material.double_sided) {
        features |= k_feature_double_sided;
    }
    if (material.alpha_mode == AlphaMode::Blend) {
        features |= k_feature_blend;
    }
    return static_cast<float>(features);
}

// Where forward.slang reads a material's textures, besides the base color at slot 0.
constexpr std::uint32_t k_normal_slot = 3;
constexpr std::uint32_t k_metallic_roughness_slot = 4;
constexpr std::uint32_t k_occlusion_slot = 5;
constexpr std::uint32_t k_emissive_slot = 6;
static_assert(k_emissive_slot < rhi::k_texture_slots);

struct ToneMapConstants {
    Vec4 settings;
    Vec4 bloom_texel;
};

struct UiConstants {
    core::Vec2 scale;
    core::Vec2 offset;
    float premultiplied = 0.0f;
};

// One particle, as particles.slang reads it per instance (ADR 0038): its middle and size, its color,
// and the cosine and sine of its rotation, with 1 in z if it adds its light.
struct ParticleInstance {
    Vec4 position_size;
    Vec4 color;
    Vec4 turn;
};

struct ParticleConstants {
    Vec4 right;
    Vec4 up;
    Vec4 settings;
};

// The most particles one frame draws: 1.5 MB of the frame's 4 MB of per-frame memory.
constexpr std::size_t k_max_particles_per_frame = 32768;
// How many particles one draw takes at most.
constexpr std::size_t k_max_particles_per_draw = 8192;

// The corners of a particle's square, and its two triangles, counter-clockwise from the camera.
constexpr std::array<core::Vec2, 4> k_particle_corners{
    core::Vec2{-0.5f, -0.5f}, core::Vec2{0.5f, -0.5f}, core::Vec2{0.5f, 0.5f},
    core::Vec2{-0.5f, 0.5f}};
constexpr std::array<std::uint32_t, 6> k_particle_indices{0, 1, 2, 0, 2, 3};

// One sprite, as sprites.slang reads it per instance (ADR 0039): position and size, texture
// rectangle, color, and pivot with the rotation's cosine and sine.
struct SpriteInstance {
    Vec4 position_size;
    Vec4 uv_rect;
    Vec4 color;
    Vec4 pivot_turn;
};

struct SpriteConstants {
    Vec4 settings;
};

// How many sprites one draw takes at most.
constexpr std::size_t k_max_sprites_per_draw = 8192;

static_assert(sizeof(ParticleInstance) == 48);
static_assert(sizeof(SpriteInstance) == 64);
static_assert(sizeof(FrameUniforms) <= rhi::k_max_uniform_size);
static_assert(sizeof(FrameUniforms) == 512);

// The environment's texture slot (ADR 0040), and the sky's and bloom's constants.
constexpr std::uint32_t k_environment_slot = 7;
static_assert(k_environment_slot < rhi::k_texture_slots);

struct SkyConstants {
    std::array<Vec4, 4> clip_to_world;
    Vec4 eye;
    Vec4 environment;
};

struct BloomConstants {
    Vec4 settings;
};

// The most halvings bloom makes: each spreads the glow twice as wide.
constexpr std::size_t k_max_bloom_levels = 6;
static_assert(sizeof(InstanceData) == 160);

// How many instances one draw takes at most, so each call's data stays small.
constexpr std::size_t k_max_instances_per_draw = 1024;

// Covers the window, with texture coordinates from the top left, for the tone-mapping pass.
struct ScreenVertex {
    float position[2];
    float uv[2];
};

constexpr std::array<ScreenVertex, 3> k_screen_triangle{{
    {{-1.0f, -1.0f}, {0.0f, 1.0f}},
    {{3.0f, -1.0f}, {2.0f, 1.0f}},
    {{-1.0f, 3.0f}, {0.0f, -1.0f}},
}};

// The scene's depth, beside the HDR color target, and the shadow map's.
constexpr rhi::TextureFormat k_depth_format = rhi::TextureFormat::Depth32Float;
constexpr std::uint32_t k_max_cascades = 4;

enum class ToneMapping : std::uint8_t { PbrNeutral, Clamp };

core::Error setting_error(std::string message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::move(message)};
}

std::string_view format_name(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::Rg11B10Float:
        return "R11G11B10 float";
    case rhi::TextureFormat::Rgba16Float:
        return "RGBA16 float";
    default:
        return "another format";
    }
}

Vec4 extend(Vec3 v, float w) {
    return Vec4{v.x, v.y, v.z, w};
}

// A device object, destroyed with its owner.
template <typename Handle, void (rhi::Device::*Destroy)(Handle)> class Owned {
public:
    Owned() = default;
    Owned(rhi::Device& device, Handle handle) : m_device(&device), m_handle(handle) {}
    Owned(Owned&& other) noexcept
        : m_device(other.m_device), m_handle(std::exchange(other.m_handle, Handle{})) {}
    Owned& operator=(Owned&& other) noexcept {
        if (this != &other) {
            reset();
            m_device = other.m_device;
            m_handle = std::exchange(other.m_handle, Handle{});
        }
        return *this;
    }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() {
        reset();
    }

    void reset() {
        if (m_handle.is_valid()) {
            (m_device->*Destroy)(m_handle);
            m_handle = {};
        }
    }

    [[nodiscard]] Handle get() const noexcept {
        return m_handle;
    }

    [[nodiscard]] bool exists() const noexcept {
        return m_handle.is_valid();
    }

private:
    rhi::Device* m_device = nullptr;
    Handle m_handle;
};

using OwnedTexture = Owned<rhi::TextureHandle, &rhi::Device::destroy_texture>;
using OwnedShader = Owned<rhi::ShaderHandle, &rhi::Device::destroy_shader>;
using OwnedPipeline = Owned<rhi::PipelineHandle, &rhi::Device::destroy_pipeline>;
using OwnedBuffer = Owned<rhi::BufferHandle, &rhi::Device::destroy_buffer>;

// What the project's [renderer] table and the device choose (ADR 0028, 0029).
// The lights' texture: 256 texels across, of four floats each, and as many rows as fit the
// lights, the clusters and their lists (ADR 0037). Its first four texels describe the rest:
//
//   0: light count, clusters across, clusters down, depth slices
//   1: the camera's near plane, the slices' logarithmic scale, where the clusters and the lists
//      start
//   2: the camera's forward direction
//
// From texel 4, each light takes four: position and range; color times intensity, and 1 for a
// spot; the spot's direction and the cosine of its outer angle; the cosine of its inner one.
// Each cluster has two floats, where its list starts and how many lights it has, two clusters a
// texel, and the lists hold four light indices a texel.
constexpr std::uint32_t k_light_texture_width = 256;
constexpr std::uint32_t k_light_texture_height = 32;
constexpr std::uint32_t k_light_header_texels = 4;
constexpr std::uint32_t k_texels_per_light = 4;
// Where forward.slang reads the lights: the slot the tone-mapping pass reads its input from, which
// the scene's pass doesn't use.
constexpr std::uint32_t k_light_slot = 1;

#if defined(DREAM_FORWARD_PLUS)
constexpr std::string_view k_module_name = "forward_plus";
constexpr std::uint32_t k_max_lights = 256;
constexpr std::array<std::uint32_t, 3> k_clusters{16, 9, 24};
#else
constexpr std::string_view k_module_name = "forward";
constexpr std::uint32_t k_max_lights = 16;
constexpr std::array<std::uint32_t, 3> k_clusters{1, 1, 1};
#endif
static_assert(k_light_header_texels + k_max_lights * k_texels_per_light +
                  (k_clusters[0] * k_clusters[1] * k_clusters[2] + 1) / 2 <
              k_light_texture_width * k_light_texture_height);

struct Settings {
    std::optional<rhi::TextureFormat> hdr_format;
    ToneMapping tone_mapping = ToneMapping::PbrNeutral;
    float exposure = 1.0f;
    std::uint32_t shadow_resolution = 0;
    std::uint32_t shadow_cascades = 0;
    float shadow_distance = 40.0f;
    // How much of each cascade's range, at its far end, blends into the next one's shadows.
    float shadow_blend = 0.1f;
    bool culling = true;
    // How many pixels a simpler version of a part may stray on screen; 0 always draws the full one.
    float lod_error = 1.0f;
    // At most this many point and spot lights, the nearest, up to the module's own limit.
    std::uint32_t max_lights = k_max_lights;
    // Glow around light brighter than the threshold, in HDR only (ADR 0040).
    bool bloom = true;
    float bloom_intensity = 0.6f;
    float bloom_threshold = 1.0f;
};

core::Result<Settings> read_settings(const core::Config& config, const rhi::Device& device) {
    Settings settings;

    const std::string_view hdr = config.get_string("renderer.hdr").value_or("auto");
    const auto usable = [&](rhi::TextureFormat format) {
        return device.supports_render_target(format) && device.supports_render_target(k_depth_format);
    };
    if (hdr == "auto") {
        // R11G11B10 takes half the bandwidth of RGBA16, which matters most on phones.
        for (const rhi::TextureFormat format :
             {rhi::TextureFormat::Rg11B10Float, rhi::TextureFormat::Rgba16Float}) {
            if (usable(format)) {
                settings.hdr_format = format;
                break;
            }
        }
    } else if (hdr == "r11g11b10" || hdr == "rgba16f") {
        const rhi::TextureFormat format = hdr == "r11g11b10" ? rhi::TextureFormat::Rg11B10Float
                                                             : rhi::TextureFormat::Rgba16Float;
        if (usable(format)) {
            settings.hdr_format = format;
        } else {
            DREAM_LOG_WARNING("The graphics device can't draw into the format renderer.hdr asks "
                              "for, so the scene is drawn without HDR");
        }
    } else if (hdr != "off") {
        return setting_error(std::format("Setting 'renderer.hdr' must be \"auto\", "
                                         "\"r11g11b10\", \"rgba16f\" or \"off\", not \"{}\"",
                                         hdr));
    }

    const std::string_view tone_mapping =
        config.get_string("renderer.tone_mapping").value_or("neutral");
    if (tone_mapping == "neutral") {
        settings.tone_mapping = ToneMapping::PbrNeutral;
    } else if (tone_mapping == "none") {
        settings.tone_mapping = ToneMapping::Clamp;
    } else {
        return setting_error(std::format("Setting 'renderer.tone_mapping' must be \"neutral\" or "
                                         "\"none\", not \"{}\"",
                                         tone_mapping));
    }

    const double exposure = config.get_float("renderer.exposure").value_or(1.0);
    if (!(exposure > 0.0 && exposure <= 1000.0)) {
        return setting_error(std::format("Setting 'renderer.exposure' must be above 0 and at most "
                                         "1000, not {}",
                                         exposure));
    }
    settings.exposure = static_cast<float>(exposure);

    // Shadows, per device unless the game says otherwise.
#if defined(__ANDROID__)
    constexpr std::uint32_t k_default_shadow_resolution = 1024;
    constexpr std::uint32_t k_default_cascades = 3;
    constexpr bool k_phone = true;
#else
    constexpr std::uint32_t k_default_shadow_resolution = 2048;
    constexpr std::uint32_t k_default_cascades = 4;
    constexpr bool k_phone = false;
#endif
    const std::int64_t cascades = config.get_int("renderer.shadows.cascades").value_or(0);
    if (cascades < 0 || cascades > k_max_cascades) {
        return setting_error(std::format("Setting 'renderer.shadows.cascades' must be from 0 to {}, "
                                         "not {}",
                                         k_max_cascades, cascades));
    }
    settings.shadow_cascades =
        cascades != 0 ? static_cast<std::uint32_t>(cascades) : k_default_cascades;
    const bool depth_targets = device.supports_render_target(k_depth_format);
    const std::string_view shadows = config.get_string("renderer.shadows.mode").value_or("auto");
    const std::int64_t resolution = config.get_int("renderer.shadows.resolution").value_or(0);
    if (resolution != 0 && (resolution < 256 || resolution > 8192)) {
        return setting_error(std::format("Setting 'renderer.shadows.resolution' must be 0 or from "
                                         "256 to 8192, not {}",
                                         resolution));
    }
    const auto chosen_resolution = resolution != 0 ? static_cast<std::uint32_t>(resolution)
                                                   : k_default_shadow_resolution;
    if (shadows == "auto") {
        // A phone with less than 4 GB has one of the weakest GPUs, where shadows cost too much.
        bool capable = depth_targets;
        if constexpr (k_phone) {
            if (capable) {
                const std::optional<core::SystemMemory> memory = core::system_memory();
                constexpr std::uint64_t k_enough = std::uint64_t{4} * 1000 * 1000 * 1000;
                capable = memory.has_value() && memory->total_physical_bytes >= k_enough;
            }
        }
        settings.shadow_resolution = capable ? chosen_resolution : 0;
    } else if (shadows == "on") {
        if (depth_targets) {
            settings.shadow_resolution = chosen_resolution;
        } else {
            DREAM_LOG_WARNING("The graphics device can't draw depth targets, so there are no "
                              "shadows");
        }
    } else if (shadows != "off") {
        return setting_error(std::format("Setting 'renderer.shadows.mode' must be \"auto\", "
                                         "\"on\" or \"off\", not \"{}\"",
                                         shadows));
    }
    const double distance = config.get_float("renderer.shadows.distance").value_or(40.0);
    if (!(distance >= 1.0 && distance <= 10000.0)) {
        return setting_error(std::format("Setting 'renderer.shadows.distance' must be from 1 to "
                                         "10000, not {}",
                                         distance));
    }
    settings.shadow_distance = static_cast<float>(distance);
    const double blend = config.get_float("renderer.shadows.blend").value_or(0.1);
    if (!(blend >= 0.0 && blend <= 0.5)) {
        return setting_error(std::format("Setting 'renderer.shadows.blend' must be from 0 to 0.5, "
                                         "not {}",
                                         blend));
    }
    settings.shadow_blend = static_cast<float>(blend);

    const double lod_error = config.get_float("renderer.lod_error").value_or(1.0);
    if (!(lod_error >= 0.0 && lod_error <= 100.0)) {
        return setting_error(std::format("Setting 'renderer.lod_error' must be from 0 to 100, "
                                         "not {}",
                                         lod_error));
    }
    settings.lod_error = static_cast<float>(lod_error);

    if (const std::optional<bool> culling = config.get_bool("renderer.culling")) {
        settings.culling = *culling;
    } else if (config.contains("renderer.culling")) {
        return setting_error("Setting 'renderer.culling' must be true or false");
    }
    if (settings.shadow_resolution == 0) {
        settings.shadow_cascades = 0;
    }
    if (config.contains("renderer.max_lights")) {
        const std::optional<std::int64_t> max_lights = config.get_int("renderer.max_lights");
        if (!max_lights.has_value() || *max_lights < 0 ||
            *max_lights > static_cast<std::int64_t>(k_max_lights)) {
            return setting_error(std::format("Setting 'renderer.max_lights' must be from 0 to {} "
                                             "for the {} renderer",
                                             k_max_lights, k_module_name));
        }
        settings.max_lights = static_cast<std::uint32_t>(*max_lights);
    }
    if (const std::optional<bool> bloom = config.get_bool("renderer.bloom")) {
        settings.bloom = *bloom;
    } else if (config.contains("renderer.bloom")) {
        return setting_error("Setting 'renderer.bloom' must be true or false");
    }
    const double intensity =
        config.get_float("renderer.bloom_intensity").value_or(settings.bloom_intensity);
    if (!(intensity >= 0.0 && intensity <= 10.0)) {
        return setting_error("Setting 'renderer.bloom_intensity' must be from 0 to 10");
    }
    settings.bloom_intensity = static_cast<float>(intensity);
    const double threshold =
        config.get_float("renderer.bloom_threshold").value_or(settings.bloom_threshold);
    if (!(threshold >= 0.0 && threshold <= 100.0)) {
        return setting_error("Setting 'renderer.bloom_threshold' must be from 0 to 100");
    }
    settings.bloom_threshold = static_cast<float>(threshold);
    if (!settings.hdr_format.has_value()) {
        settings.bloom = false;
    }
    return settings;
}

// Where cascade `index` lies in the shadow atlas, in cells: two across, then two down.
struct CascadeCell {
    std::uint32_t column = 0;
    std::uint32_t row = 0;
};

CascadeCell cascade_cell(std::uint32_t index) {
    return CascadeCell{index % 2, index / 2};
}

std::uint32_t atlas_columns(std::uint32_t cascades) {
    return cascades > 1 ? 2 : 1;
}

std::uint32_t atlas_rows(std::uint32_t cascades) {
    return cascades > 2 ? 2 : 1;
}

// The depth, in clip space, at `distance` in front of the camera, as perspective() maps it.
float clip_depth(const View& camera, float distance) {
    const float near = camera.z_near;
    const float far = camera.z_far;
    const float clamped = std::clamp(distance, near, far);
    return far * (clamped - near) / (clamped * (far - near));
}

// The sun's view for one cascade: an orthographic box around the part of the camera's view from
// `from` to `to` in front of it, reaching back toward the sun by `reach_back` more, so casters
// behind the camera still shadow what it sees. The box's size depends only on that part of the
// view, not on which way the camera turns, and it moves in whole texels of its map, so shadow edges
// hold still.
Mat4 fit_shadow_view(const View& camera, Vec3 light_direction, float from, float to,
                     float reach_back, std::uint32_t resolution) {
    const std::optional<Mat4> clip_to_world = core::inverse(camera.view_projection);
    if (!clip_to_world.has_value()) {
        return Mat4{};
    }
    std::array<Vec3, 8> corners{};
    std::size_t corner = 0;
    for (const float depth : {clip_depth(camera, from), clip_depth(camera, to)}) {
        for (const float y : {-1.0f, 1.0f}) {
            for (const float x : {-1.0f, 1.0f}) {
                const Vec4 point = *clip_to_world * Vec4{x, y, depth, 1.0f};
                corners[corner++] = Vec3{point.x, point.y, point.z} * (1.0f / point.w);
            }
        }
    }
    Vec3 center{};
    for (const Vec3& point : corners) {
        center = center + point * (1.0f / 8.0f);
    }
    float radius = 0.0f;
    for (const Vec3& point : corners) {
        radius = std::max(radius, core::length(point - center));
    }
    // Rounded up, so small changes in the view don't change the box's size.
    radius = std::ceil(radius * 16.0f) / 16.0f;

    const Vec3 direction = core::normalized(light_direction);
    const Vec3 up = std::abs(direction.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
    const float back = radius + reach_back;
    const Mat4 view = core::look_at(center - direction * back, center, up);
    Mat4 projection = core::orthographic(-radius, radius, -radius, radius, 0.0f, back + radius);

    // Moves the box so the world's origin falls on a texel, by less than one texel.
    const Vec4 origin = projection * view * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    const float half = static_cast<float>(resolution) * 0.5f;
    const float x = origin.x * half;
    const float y = origin.y * half;
    projection.columns[3].x += (std::round(x) - x) / half;
    projection.columns[3].y += (std::round(y) - y) / half;
    return projection * view;
}

void set_component(Vec4& vector, std::uint32_t index, float value) {
    switch (index) {
    case 0:
        vector.x = value;
        break;
    case 1:
        vector.y = value;
        break;
    case 2:
        vector.z = value;
        break;
    default:
        vector.w = value;
        break;
    }
}

// Splits the shadowed part of the view among the cascades, near ones short and far ones long, as a
// blend of even and logarithmic steps, and fits each one's box into its cell of the atlas.
// `bounds` gets what each cascade's box sees, for skipping casters outside it.
void fit_cascades(FrameUniforms& frame, const View& camera, Vec3 light_direction,
                  const Settings& settings, std::array<core::Frustum, k_max_cascades>& bounds) {
    const std::uint32_t count = settings.shadow_cascades;
    const std::uint32_t resolution = settings.shadow_resolution;
    const float near = camera.z_near;
    const float reach = std::clamp(settings.shadow_distance, near * 2.0f, camera.z_far);
    constexpr float k_logarithmic = 0.7f;

    const std::uint32_t columns = atlas_columns(count);
    const std::uint32_t rows = atlas_rows(count);
    float from = near;
    for (std::uint32_t cascade = 0; cascade < count; ++cascade) {
        const float step = static_cast<float>(cascade + 1) / static_cast<float>(count);
        const float even = near + (reach - near) * step;
        const float logarithmic = near * std::pow(reach / near, step);
        const float to = cascade + 1 == count ? reach
                                              : k_logarithmic * logarithmic +
                                                    (1.0f - k_logarithmic) * even;

        const Mat4 view = fit_shadow_view(camera, light_direction, from, to, reach, resolution);
        bounds[cascade] = core::frustum_from(view);
        // The world's size of one texel sets how far a surface moves along its normal.
        const Vec4 x_axis = view.columns[0];
        const float world_texel =
            2.0f / (static_cast<float>(resolution) *
                    std::max(core::length(Vec3{x_axis.x, x_axis.y, x_axis.z}), 1e-6f));

        // Squeezed into its cell: a quarter of clip space, or a half with two cascades.
        const CascadeCell cell = cascade_cell(cascade);
        const float scale_x = 1.0f / static_cast<float>(columns);
        const float scale_y = 1.0f / static_cast<float>(rows);
        const float offset_x = -1.0f + static_cast<float>(2 * cell.column + 1) * scale_x;
        const float offset_y = 1.0f - static_cast<float>(2 * cell.row + 1) * scale_y;
        frame.cascades[cascade] = core::translation(Vec3{offset_x, offset_y, 0.0f}) *
                                  core::scaling(Vec3{scale_x, scale_y, 1.0f}) * view;
        set_component(frame.cascade_far, cascade, to);
        set_component(frame.cascade_normal_offset, cascade, world_texel * 1.5f);
        from = to;
    }
    frame.shadow_settings =
        Vec4{static_cast<float>(count), 1.0f / static_cast<float>(resolution * columns), 0.0005f,
             1.0f / static_cast<float>(resolution * rows)};

    // The camera's forward direction: through the middle of the near plane.
    if (const std::optional<Mat4> clip_to_world = core::inverse(camera.view_projection)) {
        const Vec4 middle = *clip_to_world * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        const Vec3 point = Vec3{middle.x, middle.y, middle.z} * (1.0f / middle.w);
        frame.camera_forward =
            extend(core::normalized(point - camera.eye), settings.shadow_blend);
    }
}

// Every shader and pipeline, made from the compiled shader files together, and replaced together.
// A pipeline for parts drawn one side only, and one for double-sided parts.
struct SidedPipelines {
    OwnedPipeline one_sided;
    OwnedPipeline double_sided;
};

struct Pipelines {
    std::vector<OwnedShader> shaders;
    SidedPipelines forward;
    // Cut-out materials, with forward_masked.slang's discard.
    SidedPipelines masked;
    // Blended materials: premultiplied alpha, testing depth without writing it.
    SidedPipelines blended;
    // The same three, for skinned parts.
    SidedPipelines forward_skinned;
    SidedPipelines masked_skinned;
    SidedPipelines blended_skinned;
    OwnedPipeline lines;
    // Particles: premultiplied alpha, testing depth without writing it (ADR 0038).
    OwnedPipeline particles;
    // Sprites: straight alpha, into the window without depth (ADR 0039).
    OwnedPipeline sprites;
    OwnedPipeline ui;
    OwnedPipeline ui_premultiplied;
    OwnedPipeline shadow;        // Only with shadows.
    OwnedPipeline shadow_masked; // Only with shadows.
    OwnedPipeline shadow_skinned;        // Only with shadows.
    OwnedPipeline shadow_masked_skinned; // Only with shadows.
    OwnedPipeline tone_map;      // Only with HDR.
    // The environment behind the scene (ADR 0040).
    OwnedPipeline sky;
    // Bloom's halving and adding back, only with HDR (ADR 0040).
    OwnedPipeline bloom_down;
    OwnedPipeline bloom_up;
};

class ForwardRenderer final : public SceneRenderer {
public:
    explicit ForwardRenderer(core::Config config) : m_config(std::move(config)) {}

    ForwardRenderer(const ForwardRenderer&) = delete;
    ForwardRenderer& operator=(const ForwardRenderer&) = delete;

    ~ForwardRenderer() override {
        // Before the device's own objects, in no particular order: the device outlives this.
        m_pipelines.reset();
        m_shadow_map.reset();
        m_light_texture.reset();
        m_flat_normal.reset();
        m_particle_corners.reset();
        m_particle_indices.reset();
        m_hdr_color.reset();
        m_hdr_depth.reset();
        m_bloom.clear();
    }

    core::Result<void> start(rhi::Device& device) override {
        m_device = &device;
        core::Result<Settings> settings = read_settings(m_config, device);
        if (!settings.has_value()) {
            return settings.error();
        }
        m_settings = settings.value();

        constexpr std::array<std::uint8_t, 4> k_flat{128, 128, 255, 255};
        const core::Result<rhi::TextureHandle> flat = device.create_texture(
            rhi::TextureDesc{.width = 1, .height = 1, .format = rhi::TextureFormat::Rgba8Unorm},
            std::as_bytes(std::span(k_flat)));
        if (!flat.has_value()) {
            return flat.error();
        }
        m_flat_normal = OwnedTexture(device, flat.value());

        // Every particle is this one square, placed and sized per instance.
        const core::Result<rhi::BufferHandle> corners = device.create_buffer(
            rhi::BufferUsage::Vertex, std::as_bytes(std::span(k_particle_corners)));
        if (!corners.has_value()) {
            return corners.error();
        }
        m_particle_corners = OwnedBuffer(device, corners.value());
        const core::Result<rhi::BufferHandle> corner_indices = device.create_buffer(
            rhi::BufferUsage::Index, std::as_bytes(std::span(k_particle_indices)));
        if (!corner_indices.has_value()) {
            return corner_indices.error();
        }
        m_particle_indices = OwnedBuffer(device, corner_indices.value());

        // The lights' texture, rewritten each frame. Without it, only the sun lights the scene.
        const rhi::TextureDesc light_desc{.width = k_light_texture_width,
                                          .height = k_light_texture_height,
                                          .format = rhi::TextureFormat::Rgba32Float,
                                          .generate_mipmaps = false,
                                          .dynamic = true};
        m_light_data.assign(std::size_t{k_light_texture_width} * k_light_texture_height, Vec4{});
        if (device.supports_texture_format(rhi::TextureFormat::Rgba32Float)) {
            const core::Result<rhi::TextureHandle> lights =
                device.create_texture(light_desc, std::as_bytes(std::span(m_light_data)));
            if (lights.has_value()) {
                m_light_texture = OwnedTexture(device, lights.value());
            } else {
                DREAM_LOG_WARNING("No point or spot lights: {}", lights.error().message);
            }
        } else {
            DREAM_LOG_WARNING("No point or spot lights: the device has no float textures");
        }

        std::string description;
        if (m_settings.hdr_format.has_value()) {
            description = std::format("The scene is drawn in HDR, into {}, with {} tone mapping",
                                      format_name(*m_settings.hdr_format),
                                      m_settings.tone_mapping == ToneMapping::PbrNeutral
                                          ? "PBR Neutral"
                                          : "no");
        } else {
            description = "The scene is drawn straight to the window, without HDR";
        }
        if (m_settings.shadow_resolution > 0) {
            // The cascades side by side in one map, since shaders read one shadow texture.
            const core::Result<rhi::TextureHandle> shadow_map = device.create_texture(
                rhi::TextureDesc{
                    .width = m_settings.shadow_resolution * atlas_columns(m_settings.shadow_cascades),
                    .height = m_settings.shadow_resolution * atlas_rows(m_settings.shadow_cascades),
                    .format = k_depth_format,
                    .render_target = true},
                {});
            if (!shadow_map.has_value()) {
                DREAM_LOG_WARNING("Could not make the shadow map, so there are no shadows: {}",
                                  shadow_map.error().message);
                m_settings.shadow_resolution = 0;
                m_settings.shadow_cascades = 0;
            } else {
                m_shadow_map = OwnedTexture(device, shadow_map.value());
            }
        }
        if (m_settings.shadow_resolution > 0) {
            description += std::format("; shadows: {0} cascades of {1}x{1}, reaching {2} from the "
                                       "camera",
                                       m_settings.shadow_cascades, m_settings.shadow_resolution,
                                       m_settings.shadow_distance);
        } else {
            description += "; shadows are off";
        }
        description += m_settings.culling ? "; parts out of view are skipped" : "; culling is off";
        description += m_settings.lod_error > 0.0f
                           ? std::format("; simpler versions of parts within {} pixels",
                                         m_settings.lod_error)
                           : std::string("; parts are always drawn in full");
        description += "; materials are metallic-roughness PBR with normal maps, lit by the "
                       "environment when the scene has one";
        description += m_settings.bloom ? std::format("; bloom above {} at {}",
                                                      m_settings.bloom_threshold,
                                                      m_settings.bloom_intensity)
                                        : std::string("; no bloom");
        if (m_light_texture.exists()) {
            description += std::format(
                "; up to {} point and spot lights{}", m_settings.max_lights,
                k_clusters[0] * k_clusters[1] * k_clusters[2] > 1
                    ? std::format(", in {}x{}x{} clusters", k_clusters[0], k_clusters[1],
                                  k_clusters[2])
                    : std::string());
        }
        m_description = std::move(description);
        return {};
    }

    core::Result<void> load_shaders(const FileReader& read_file) override {
        core::Result<Pipelines> made = make_pipelines(read_file);
        if (!made.has_value()) {
            return made.error();
        }
        // Between frames, as hot reloading calls this, so nothing still draws with the old ones.
        m_pipelines = std::make_unique<Pipelines>(std::move(made).value());
        return {};
    }

    std::string_view describe() const override {
        return m_description;
    }

    FrameStats render(rhi::CommandList& commands, const Frame& frame) override;

private:
    core::Result<OwnedShader> load_shader(const FileReader& read_file, std::string_view name,
                                          rhi::ShaderStage stage) {
        const auto code =
            read_file(std::format("assets:/shaders/{}{}", name, m_device->shader_file_suffix()));
        if (!code.has_value()) {
            return core::Error{code.error().code,
                               std::format("{} (does the game list a renderer module, which "
                                           "compiles its shaders?)",
                                           code.error().message)};
        }
        const auto shader = m_device->create_shader(stage, code.value());
        if (!shader.has_value()) {
            return shader.error();
        }
        return OwnedShader(*m_device, shader.value());
    }

    core::Result<Pipelines> make_pipelines(const FileReader& read_file);
    void fit_hdr_targets(platform::SurfaceSize size);
    // Works out what every part draws this frame: its box in the world, the version of it for its
    // size on screen, and its instance data.
    void prepare_meshes(const Frame& frame, bool boxes);
    // Fills the lights' texture for this frame's view, and says whether any light reaches it.
    bool prepare_lights(const Frame& frame, FrameStats& stats);

    // Draws the parts, or the shadow casters among them, that `view` sees, when one is given,
    // with one instanced draw for the parts that share a range of indices and a texture.
    void draw_meshes(rhi::CommandList& commands, std::span<const MeshDraw> meshes, Pass pass,
                     const core::Frustum* view, FrameStats& stats);
    void draw_lines(rhi::CommandList& commands, std::span<const LineVertex> lines,
                    FrameStats& stats);
    // Draws the frame's sprites with the 2D camera, batch after batch, into the window.
    void draw_sprites(rhi::CommandList& commands, const Frame& frame, FrameStats& stats);
    // Draws the frame's particles over the scene: blended batches farthest first, each of their
    // particles farthest first, then additive ones.
    void draw_particles(rhi::CommandList& commands, const Frame& frame, FrameStats& stats);
    void draw_ui(rhi::CommandList& commands, const Frame& frame, FrameStats& stats);
    // Draws the environment behind everything, into the pass begun. Says whether it drew.
    bool draw_sky(rhi::CommandList& commands, const Frame& frame);
    // Adds the glow of the HDR target's brightest light back into it, between its pass and tone
    // mapping.
    void draw_bloom(rhi::CommandList& commands);

    core::Config m_config;
    rhi::Device* m_device = nullptr;
    Settings m_settings;
    std::string m_description;
    std::unique_ptr<Pipelines> m_pipelines;
    OwnedTexture m_shadow_map;
    // The point and spot lights of the frame, with their clusters (ADR 0037).
    OwnedTexture m_light_texture;
    std::vector<Vec4> m_light_data;
    std::uint32_t m_light_rows_used = 0;
    struct LightReach {
        std::uint32_t light = 0;
        std::array<std::uint32_t, 3> low{};
        std::array<std::uint32_t, 3> high{};
    };
    std::vector<LightReach> m_light_reach;
    std::vector<std::uint32_t> m_cluster_counts;
    std::vector<std::uint32_t> m_cluster_starts;
    std::vector<std::uint32_t> m_cluster_limits;
    std::vector<std::pair<float, std::uint32_t>> m_light_order;
    // One texel pointing straight out of the surface, for parts without a normal map.
    OwnedTexture m_flat_normal;
    // The square every particle is drawn as, and what draw_particles reuses between frames.
    OwnedBuffer m_particle_corners;
    OwnedBuffer m_particle_indices;
    std::vector<ParticleInstance> m_particle_instances;
    std::vector<std::pair<float, std::uint32_t>> m_particle_order;
    std::vector<std::pair<float, std::uint32_t>> m_batch_order;
    std::vector<SpriteInstance> m_sprite_instances;
    OwnedTexture m_hdr_color;
    OwnedTexture m_hdr_depth;
    // Bloom's images, each half the size of the last, starting at half the HDR target's.
    std::vector<OwnedTexture> m_bloom;
    std::vector<platform::SurfaceSize> m_bloom_sizes;
    platform::SurfaceSize m_hdr_size;
    // What prepare_meshes works out for each part this frame, reused between frames.
    struct Chosen {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        bool simplified = false;
    };
    std::vector<core::Aabb> m_world_bounds;
    std::vector<Chosen> m_chosen;
    std::vector<InstanceData> m_instances;
    // The parts one pass draws, in draw order, and the instance data of one draw.
    std::vector<std::uint32_t> m_order;
    std::vector<InstanceData> m_batch;
    // Each blended part's squared distance from the eye, for drawing the farthest first.
    std::vector<float> m_distances;
    // A skinned part's joints, as the per-draw block takes them.
    std::vector<Vec4> m_joint_rows;
    Vec3 m_eye;
    bool m_warned_instances = false;
    bool m_warned_light_lists = false;
};

core::Result<Pipelines> ForwardRenderer::make_pipelines(const FileReader& read_file) {
    rhi::Device& device = *m_device;
    const std::optional<rhi::TextureFormat> hdr = m_settings.hdr_format;
    Pipelines made;
    const auto shader = [&](std::string_view name,
                            rhi::ShaderStage stage) -> core::Result<rhi::ShaderHandle> {
        core::Result<OwnedShader> loaded = load_shader(read_file, name, stage);
        if (!loaded.has_value()) {
            return loaded.error();
        }
        made.shaders.push_back(std::move(loaded).value());
        return made.shaders.back().get();
    };
    const auto pipeline = [&](const rhi::PipelineDesc& desc) -> core::Result<OwnedPipeline> {
        const core::Result<rhi::PipelineHandle> created = device.create_pipeline(desc);
        if (!created.has_value()) {
            return created.error();
        }
        return OwnedPipeline(device, created.value());
    };

    const core::Result<rhi::ShaderHandle> vertex = shader("forward.vert", rhi::ShaderStage::Vertex);
    if (!vertex.has_value()) {
        return vertex.error();
    }
    const core::Result<rhi::ShaderHandle> fragment =
        shader("forward.frag", rhi::ShaderStage::Fragment);
    if (!fragment.has_value()) {
        return fragment.error();
    }
    constexpr std::array k_attributes{
        rhi::VertexAttribute{0, rhi::VertexFormat::Float3, offsetof(MeshVertex, position)},
        rhi::VertexAttribute{1, rhi::VertexFormat::Float3, offsetof(MeshVertex, normal)},
        rhi::VertexAttribute{2, rhi::VertexFormat::Float2, offsetof(MeshVertex, uv)},
        rhi::VertexAttribute{3, rhi::VertexFormat::Float4, offsetof(MeshVertex, tangent)},
    };
    // Each instance's model matrix, normals' matrix and material, after the vertex's own inputs.
    constexpr std::size_t k_instance_vec4s = sizeof(InstanceData) / sizeof(Vec4);
    constexpr auto instance_attributes = [](std::uint32_t first_location, std::size_t count) {
        std::array<rhi::VertexAttribute, k_instance_vec4s> attributes{};
        for (std::size_t index = 0; index < count; ++index) {
            attributes[index] = rhi::VertexAttribute{
                first_location + static_cast<std::uint32_t>(index), rhi::VertexFormat::Float4,
                static_cast<std::uint32_t>(index * sizeof(Vec4))};
        }
        return attributes;
    };
    static constexpr std::array k_instance_attributes = instance_attributes(4, k_instance_vec4s);
    static constexpr std::array k_shadow_instance_attributes = instance_attributes(1, 4);
    const core::Result<rhi::ShaderHandle> masked_fragment =
        shader("forward_masked.frag", rhi::ShaderStage::Fragment);
    if (!masked_fragment.has_value()) {
        return masked_fragment.error();
    }
    // Skinned parts' vertices also have their joints and weights, after the instance's inputs.
    static constexpr std::array k_skinned_attributes{
        k_attributes[0], k_attributes[1], k_attributes[2], k_attributes[3],
        rhi::VertexAttribute{14, rhi::VertexFormat::UByte4Norm, offsetof(MeshVertex, joints)},
        rhi::VertexAttribute{15, rhi::VertexFormat::UByte4Norm, offsetof(MeshVertex, weights)},
    };
    const core::Result<rhi::ShaderHandle> skinned_vertex =
        shader("forward_skinned.vert", rhi::ShaderStage::Vertex);
    if (!skinned_vertex.has_value()) {
        return skinned_vertex.error();
    }
    const core::Result<rhi::ShaderHandle> skinned_fragment =
        shader("forward_skinned.frag", rhi::ShaderStage::Fragment);
    if (!skinned_fragment.has_value()) {
        return skinned_fragment.error();
    }
    const core::Result<rhi::ShaderHandle> masked_skinned_fragment =
        shader("forward_masked_skinned.frag", rhi::ShaderStage::Fragment);
    if (!masked_skinned_fragment.has_value()) {
        return masked_skinned_fragment.error();
    }
    // One-sided and double-sided versions of a lit pipeline (ADR 0034), for static or skinned
    // parts.
    const auto sided = [&](rhi::ShaderHandle fragment_shader, rhi::BlendMode blend, bool skinned,
                           SidedPipelines& out) -> core::Result<void> {
        rhi::PipelineDesc desc{
            .vertex_shader = skinned ? skinned_vertex.value() : vertex.value(),
            .fragment_shader = fragment_shader,
            .vertex_stride = sizeof(MeshVertex),
            .attributes = skinned ? std::span<const rhi::VertexAttribute>(k_skinned_attributes)
                                  : std::span<const rhi::VertexAttribute>(k_attributes),
            .instance_stride = sizeof(InstanceData),
            .instance_attributes = k_instance_attributes,
            .blend = blend,
            // Blended surfaces don't hide what is behind them, even each other.
            .depth_write = blend == rhi::BlendMode::Opaque,
            .color_target = hdr,
            .depth_target = hdr.has_value() ? std::optional(k_depth_format) : std::nullopt,
        };
        core::Result<OwnedPipeline> one_sided = pipeline(desc);
        if (!one_sided.has_value()) {
            return one_sided.error();
        }
        desc.cull = rhi::CullMode::None;
        core::Result<OwnedPipeline> double_sided = pipeline(desc);
        if (!double_sided.has_value()) {
            return double_sided.error();
        }
        out.one_sided = std::move(one_sided).value();
        out.double_sided = std::move(double_sided).value();
        return {};
    };
    struct LitKind {
        rhi::ShaderHandle fragment;
        rhi::BlendMode blend;
        bool skinned;
        SidedPipelines* out;
    };
    for (const LitKind& kind : {
             LitKind{fragment.value(), rhi::BlendMode::Opaque, false, &made.forward},
             LitKind{masked_fragment.value(), rhi::BlendMode::Opaque, false, &made.masked},
             LitKind{fragment.value(), rhi::BlendMode::PremultipliedAlpha, false, &made.blended},
             LitKind{skinned_fragment.value(), rhi::BlendMode::Opaque, true, &made.forward_skinned},
             LitKind{masked_skinned_fragment.value(), rhi::BlendMode::Opaque, true,
                     &made.masked_skinned},
             LitKind{skinned_fragment.value(), rhi::BlendMode::PremultipliedAlpha, true,
                     &made.blended_skinned},
         }) {
        if (auto made_kind = sided(kind.fragment, kind.blend, kind.skinned, *kind.out);
            !made_kind.has_value()) {
            return made_kind.error();
        }
    }

    const core::Result<rhi::ShaderHandle> line_vertex =
        shader("debug_lines.vert", rhi::ShaderStage::Vertex);
    if (!line_vertex.has_value()) {
        return line_vertex.error();
    }
    const core::Result<rhi::ShaderHandle> line_fragment =
        shader("debug_lines.frag", rhi::ShaderStage::Fragment);
    if (!line_fragment.has_value()) {
        return line_fragment.error();
    }
    constexpr std::array k_line_attributes{
        rhi::VertexAttribute{0, rhi::VertexFormat::Float3, offsetof(LineVertex, position)},
        rhi::VertexAttribute{1, rhi::VertexFormat::Float4, offsetof(LineVertex, color)},
    };
    core::Result<OwnedPipeline> lines = pipeline(rhi::PipelineDesc{
        .vertex_shader = line_vertex.value(),
        .fragment_shader = line_fragment.value(),
        .vertex_stride = sizeof(LineVertex),
        .attributes = k_line_attributes,
        .topology = rhi::PrimitiveTopology::Lines,
        .depth_test = false,
        .cull = rhi::CullMode::None,
    });
    if (!lines.has_value()) {
        return lines.error();
    }
    made.lines = std::move(lines).value();

    const core::Result<rhi::ShaderHandle> particle_vertex =
        shader("particles.vert", rhi::ShaderStage::Vertex);
    if (!particle_vertex.has_value()) {
        return particle_vertex.error();
    }
    const core::Result<rhi::ShaderHandle> particle_fragment =
        shader("particles.frag", rhi::ShaderStage::Fragment);
    if (!particle_fragment.has_value()) {
        return particle_fragment.error();
    }
    constexpr std::array k_corner_attributes{
        rhi::VertexAttribute{0, rhi::VertexFormat::Float2, 0},
    };
    constexpr std::array k_particle_attributes{
        rhi::VertexAttribute{1, rhi::VertexFormat::Float4, offsetof(ParticleInstance, position_size)},
        rhi::VertexAttribute{2, rhi::VertexFormat::Float4, offsetof(ParticleInstance, color)},
        rhi::VertexAttribute{3, rhi::VertexFormat::Float4, offsetof(ParticleInstance, turn)},
    };
    core::Result<OwnedPipeline> particles = pipeline(rhi::PipelineDesc{
        .vertex_shader = particle_vertex.value(),
        .fragment_shader = particle_fragment.value(),
        .vertex_stride = sizeof(core::Vec2),
        .attributes = k_corner_attributes,
        .instance_stride = sizeof(ParticleInstance),
        .instance_attributes = k_particle_attributes,
        .blend = rhi::BlendMode::PremultipliedAlpha,
        .depth_write = false,
        .cull = rhi::CullMode::None,
        .color_target = hdr,
        .depth_target = hdr.has_value() ? std::optional(k_depth_format) : std::nullopt,
    });
    if (!particles.has_value()) {
        return particles.error();
    }
    made.particles = std::move(particles).value();

    const core::Result<rhi::ShaderHandle> sprite_vertex =
        shader("sprites.vert", rhi::ShaderStage::Vertex);
    if (!sprite_vertex.has_value()) {
        return sprite_vertex.error();
    }
    const core::Result<rhi::ShaderHandle> sprite_fragment =
        shader("sprites.frag", rhi::ShaderStage::Fragment);
    if (!sprite_fragment.has_value()) {
        return sprite_fragment.error();
    }
    constexpr std::array k_sprite_attributes{
        rhi::VertexAttribute{1, rhi::VertexFormat::Float4, offsetof(SpriteInstance, position_size)},
        rhi::VertexAttribute{2, rhi::VertexFormat::Float4, offsetof(SpriteInstance, uv_rect)},
        rhi::VertexAttribute{3, rhi::VertexFormat::Float4, offsetof(SpriteInstance, color)},
        rhi::VertexAttribute{4, rhi::VertexFormat::Float4, offsetof(SpriteInstance, pivot_turn)},
    };
    core::Result<OwnedPipeline> sprites = pipeline(rhi::PipelineDesc{
        .vertex_shader = sprite_vertex.value(),
        .fragment_shader = sprite_fragment.value(),
        .vertex_stride = sizeof(core::Vec2),
        .attributes = k_corner_attributes,
        .instance_stride = sizeof(SpriteInstance),
        .instance_attributes = k_sprite_attributes,
        .blend = rhi::BlendMode::Alpha,
        .depth_test = false,
        .cull = rhi::CullMode::None,
    });
    if (!sprites.has_value()) {
        return sprites.error();
    }
    made.sprites = std::move(sprites).value();

    const core::Result<rhi::ShaderHandle> ui_vertex = shader("ui.vert", rhi::ShaderStage::Vertex);
    if (!ui_vertex.has_value()) {
        return ui_vertex.error();
    }
    const core::Result<rhi::ShaderHandle> ui_fragment =
        shader("ui.frag", rhi::ShaderStage::Fragment);
    if (!ui_fragment.has_value()) {
        return ui_fragment.error();
    }
    constexpr std::array k_ui_attributes{
        rhi::VertexAttribute{0, rhi::VertexFormat::Float2, offsetof(ui::Vertex, position)},
        rhi::VertexAttribute{1, rhi::VertexFormat::Float2, offsetof(ui::Vertex, uv)},
        rhi::VertexAttribute{2, rhi::VertexFormat::UByte4Norm, offsetof(ui::Vertex, color)},
    };
    rhi::PipelineDesc ui_desc{
        .vertex_shader = ui_vertex.value(),
        .fragment_shader = ui_fragment.value(),
        .vertex_stride = sizeof(ui::Vertex),
        .attributes = k_ui_attributes,
        .blend = rhi::BlendMode::Alpha,
        .depth_test = false,
        .cull = rhi::CullMode::None,
    };
    core::Result<OwnedPipeline> ui = pipeline(ui_desc);
    if (!ui.has_value()) {
        return ui.error();
    }
    made.ui = std::move(ui).value();
    ui_desc.blend = rhi::BlendMode::PremultipliedAlpha;
    core::Result<OwnedPipeline> ui_premultiplied = pipeline(ui_desc);
    if (!ui_premultiplied.has_value()) {
        return ui_premultiplied.error();
    }
    made.ui_premultiplied = std::move(ui_premultiplied).value();

    if (m_settings.shadow_resolution > 0) {
        const core::Result<rhi::ShaderHandle> shadow_vertex =
            shader("shadow.vert", rhi::ShaderStage::Vertex);
        if (!shadow_vertex.has_value()) {
            return shadow_vertex.error();
        }
        // Depth only. Both faces, since a model's faces needn't make a closed shape, with a bias
        // that grows with the slope, against surfaces shadowing themselves.
        core::Result<OwnedPipeline> shadow = pipeline(rhi::PipelineDesc{
            .vertex_shader = shadow_vertex.value(),
            .vertex_stride = sizeof(MeshVertex),
            .attributes = std::span(k_attributes).first(1),
            .instance_stride = sizeof(InstanceData),
            .instance_attributes = std::span(k_shadow_instance_attributes).first(4),
            .cull = rhi::CullMode::None,
            .depth_bias = 1.0f,
            .slope_depth_bias = 1.5f,
            .depth_target = k_depth_format,
        });
        if (!shadow.has_value()) {
            return shadow.error();
        }
        made.shadow = std::move(shadow).value();

        // Cut-out casters: the position and texture coordinates, and of each instance its model
        // matrix, its alpha cutoff in the normals' first column, and its base color.
        const core::Result<rhi::ShaderHandle> masked_vertex =
            shader("shadow_masked.vert", rhi::ShaderStage::Vertex);
        if (!masked_vertex.has_value()) {
            return masked_vertex.error();
        }
        const core::Result<rhi::ShaderHandle> masked_shadow_fragment =
            shader("shadow_masked.frag", rhi::ShaderStage::Fragment);
        if (!masked_shadow_fragment.has_value()) {
            return masked_shadow_fragment.error();
        }
        static constexpr std::array k_masked_attributes{
            rhi::VertexAttribute{0, rhi::VertexFormat::Float3, offsetof(MeshVertex, position)},
            rhi::VertexAttribute{1, rhi::VertexFormat::Float2, offsetof(MeshVertex, uv)},
        };
        static constexpr std::array k_masked_instance_attributes{
            rhi::VertexAttribute{2, rhi::VertexFormat::Float4, offsetof(InstanceData, model)},
            rhi::VertexAttribute{3, rhi::VertexFormat::Float4,
                                 offsetof(InstanceData, model) + sizeof(Vec4)},
            rhi::VertexAttribute{4, rhi::VertexFormat::Float4,
                                 offsetof(InstanceData, model) + 2 * sizeof(Vec4)},
            rhi::VertexAttribute{5, rhi::VertexFormat::Float4,
                                 offsetof(InstanceData, model) + 3 * sizeof(Vec4)},
            rhi::VertexAttribute{6, rhi::VertexFormat::Float4, offsetof(InstanceData, normals)},
            rhi::VertexAttribute{7, rhi::VertexFormat::Float4, offsetof(InstanceData, base_color)},
        };
        core::Result<OwnedPipeline> shadow_masked = pipeline(rhi::PipelineDesc{
            .vertex_shader = masked_vertex.value(),
            .fragment_shader = masked_shadow_fragment.value(),
            .vertex_stride = sizeof(MeshVertex),
            .attributes = k_masked_attributes,
            .instance_stride = sizeof(InstanceData),
            .instance_attributes = k_masked_instance_attributes,
            .cull = rhi::CullMode::None,
            .depth_bias = 1.0f,
            .slope_depth_bias = 1.5f,
            .depth_target = k_depth_format,
        });
        if (!shadow_masked.has_value()) {
            return shadow_masked.error();
        }
        made.shadow_masked = std::move(shadow_masked).value();

        // Both again for skinned casters, with the vertices' joints and weights after the
        // instance's inputs.
        const core::Result<rhi::ShaderHandle> skinned_shadow_vertex =
            shader("shadow_skinned.vert", rhi::ShaderStage::Vertex);
        if (!skinned_shadow_vertex.has_value()) {
            return skinned_shadow_vertex.error();
        }
        static constexpr std::array k_shadow_skinned_attributes{
            k_attributes[0],
            rhi::VertexAttribute{5, rhi::VertexFormat::UByte4Norm, offsetof(MeshVertex, joints)},
            rhi::VertexAttribute{6, rhi::VertexFormat::UByte4Norm, offsetof(MeshVertex, weights)},
        };
        core::Result<OwnedPipeline> shadow_skinned = pipeline(rhi::PipelineDesc{
            .vertex_shader = skinned_shadow_vertex.value(),
            .vertex_stride = sizeof(MeshVertex),
            .attributes = k_shadow_skinned_attributes,
            .instance_stride = sizeof(InstanceData),
            .instance_attributes = std::span(k_shadow_instance_attributes).first(4),
            .cull = rhi::CullMode::None,
            .depth_bias = 1.0f,
            .slope_depth_bias = 1.5f,
            .depth_target = k_depth_format,
        });
        if (!shadow_skinned.has_value()) {
            return shadow_skinned.error();
        }
        made.shadow_skinned = std::move(shadow_skinned).value();

        const core::Result<rhi::ShaderHandle> masked_skinned_shadow_vertex =
            shader("shadow_masked_skinned.vert", rhi::ShaderStage::Vertex);
        if (!masked_skinned_shadow_vertex.has_value()) {
            return masked_skinned_shadow_vertex.error();
        }
        const core::Result<rhi::ShaderHandle> masked_skinned_shadow_fragment =
            shader("shadow_masked_skinned.frag", rhi::ShaderStage::Fragment);
        if (!masked_skinned_shadow_fragment.has_value()) {
            return masked_skinned_shadow_fragment.error();
        }
        static constexpr std::array k_masked_skinned_attributes{
            k_masked_attributes[0], k_masked_attributes[1],
            rhi::VertexAttribute{8, rhi::VertexFormat::UByte4Norm, offsetof(MeshVertex, joints)},
            rhi::VertexAttribute{9, rhi::VertexFormat::UByte4Norm, offsetof(MeshVertex, weights)},
        };
        core::Result<OwnedPipeline> shadow_masked_skinned = pipeline(rhi::PipelineDesc{
            .vertex_shader = masked_skinned_shadow_vertex.value(),
            .fragment_shader = masked_skinned_shadow_fragment.value(),
            .vertex_stride = sizeof(MeshVertex),
            .attributes = k_masked_skinned_attributes,
            .instance_stride = sizeof(InstanceData),
            .instance_attributes = k_masked_instance_attributes,
            .cull = rhi::CullMode::None,
            .depth_bias = 1.0f,
            .slope_depth_bias = 1.5f,
            .depth_target = k_depth_format,
        });
        if (!shadow_masked_skinned.has_value()) {
            return shadow_masked_skinned.error();
        }
        made.shadow_masked_skinned = std::move(shadow_masked_skinned).value();
    }

    if (hdr.has_value()) {
        const core::Result<rhi::ShaderHandle> tone_vertex =
            shader("tonemap.vert", rhi::ShaderStage::Vertex);
        if (!tone_vertex.has_value()) {
            return tone_vertex.error();
        }
        const core::Result<rhi::ShaderHandle> tone_fragment =
            shader("tonemap.frag", rhi::ShaderStage::Fragment);
        if (!tone_fragment.has_value()) {
            return tone_fragment.error();
        }
        constexpr std::array k_screen_attributes{
            rhi::VertexAttribute{0, rhi::VertexFormat::Float2, offsetof(ScreenVertex, position)},
            rhi::VertexAttribute{1, rhi::VertexFormat::Float2, offsetof(ScreenVertex, uv)},
        };
        core::Result<OwnedPipeline> tone_map = pipeline(rhi::PipelineDesc{
            .vertex_shader = tone_vertex.value(),
            .fragment_shader = tone_fragment.value(),
            .vertex_stride = sizeof(ScreenVertex),
            .attributes = k_screen_attributes,
            .depth_test = false,
            .cull = rhi::CullMode::None,
        });
        if (!tone_map.has_value()) {
            return tone_map.error();
        }
        made.tone_map = std::move(tone_map).value();

        if (m_settings.bloom) {
            for (const auto& [name, blend, out] :
                 {std::tuple{"bloom_down", rhi::BlendMode::Opaque, &made.bloom_down},
                  std::tuple{"bloom_up", rhi::BlendMode::PremultipliedAlpha, &made.bloom_up}}) {
                const core::Result<rhi::ShaderHandle> vertex_shader =
                    shader(std::format("{}.vert", name), rhi::ShaderStage::Vertex);
                if (!vertex_shader.has_value()) {
                    return vertex_shader.error();
                }
                const core::Result<rhi::ShaderHandle> fragment_shader =
                    shader(std::format("{}.frag", name), rhi::ShaderStage::Fragment);
                if (!fragment_shader.has_value()) {
                    return fragment_shader.error();
                }
                core::Result<OwnedPipeline> bloom = pipeline(rhi::PipelineDesc{
                    .vertex_shader = vertex_shader.value(),
                    .fragment_shader = fragment_shader.value(),
                    .vertex_stride = sizeof(ScreenVertex),
                    .attributes = k_screen_attributes,
                    .blend = blend,
                    .depth_test = false,
                    .cull = rhi::CullMode::None,
                    .color_target = hdr,
                });
                if (!bloom.has_value()) {
                    return bloom.error();
                }
                *out = std::move(bloom).value();
            }
        }
    }

    const core::Result<rhi::ShaderHandle> sky_vertex =
        shader("sky.vert", rhi::ShaderStage::Vertex);
    if (!sky_vertex.has_value()) {
        return sky_vertex.error();
    }
    const core::Result<rhi::ShaderHandle> sky_fragment =
        shader("sky.frag", rhi::ShaderStage::Fragment);
    if (!sky_fragment.has_value()) {
        return sky_fragment.error();
    }
    constexpr std::array k_sky_attributes{
        rhi::VertexAttribute{0, rhi::VertexFormat::Float2, offsetof(ScreenVertex, position)},
        rhi::VertexAttribute{1, rhi::VertexFormat::Float2, offsetof(ScreenVertex, uv)},
    };
    core::Result<OwnedPipeline> sky = pipeline(rhi::PipelineDesc{
        .vertex_shader = sky_vertex.value(),
        .fragment_shader = sky_fragment.value(),
        .vertex_stride = sizeof(ScreenVertex),
        .attributes = k_sky_attributes,
        // Behind everything, so only pixels nothing covers are shaded.
        .depth_test = true,
        .depth_write = false,
        .cull = rhi::CullMode::None,
        .color_target = hdr,
        .depth_target = hdr.has_value() ? std::optional(k_depth_format) : std::nullopt,
    });
    if (!sky.has_value()) {
        return sky.error();
    }
    made.sky = std::move(sky).value();
    return made;
}

void ForwardRenderer::fit_hdr_targets(platform::SurfaceSize size) {
    if (size == m_hdr_size && m_hdr_color.exists()) {
        return;
    }
    // Made between passes, before anything this frame reads the old ones.
    m_hdr_color.reset();
    m_hdr_depth.reset();
    m_hdr_size = {};
    if (size.width == 0 || size.height == 0) {
        return;
    }
    const auto make = [&](rhi::TextureFormat format) -> OwnedTexture {
        const core::Result<rhi::TextureHandle> texture = m_device->create_texture(
            rhi::TextureDesc{.width = size.width, .height = size.height, .format = format,
                             .render_target = true},
            {});
        if (!texture.has_value()) {
            DREAM_LOG_ERROR("Could not make the renderer's HDR target: {}",
                            texture.error().message);
            return {};
        }
        return OwnedTexture(*m_device, texture.value());
    };
    m_hdr_color = make(*m_settings.hdr_format);
    m_hdr_depth = make(k_depth_format);
    if (!m_hdr_color.exists() || !m_hdr_depth.exists()) {
        m_hdr_color.reset();
        m_hdr_depth.reset();
        return;
    }
    m_hdr_size = size;

    m_bloom.clear();
    m_bloom_sizes.clear();
    if (m_settings.bloom) {
        // The first image is a quarter of the scene's size, which halves bloom's cost on phones
        // against starting at half, with no difference to see in a glow this soft.
        platform::SurfaceSize level{size.width * 2, size.height * 2};
        while (m_bloom.size() < k_max_bloom_levels && level.width >= 16 && level.height >= 16) {
            level = platform::SurfaceSize{level.width / (m_bloom.empty() ? 8 : 2),
                                          level.height / (m_bloom.empty() ? 8 : 2)};
            const core::Result<rhi::TextureHandle> texture = m_device->create_texture(
                rhi::TextureDesc{.width = level.width,
                                 .height = level.height,
                                 .format = *m_settings.hdr_format,
                                 .render_target = true},
                {});
            if (!texture.has_value()) {
                DREAM_LOG_WARNING("No bloom: {}", texture.error().message);
                m_bloom.clear();
                m_bloom_sizes.clear();
                break;
            }
            m_bloom.emplace_back(*m_device, texture.value());
            m_bloom_sizes.push_back(level);
        }
    }
}

bool ForwardRenderer::draw_sky(rhi::CommandList& commands, const Frame& frame) {
    if (!frame.environment.has_value() || !frame.environment->background ||
        !frame.environment->radiance.is_valid() || !m_pipelines->sky.exists()) {
        return false;
    }
    const std::optional<Mat4> clip_to_world = core::inverse(frame.view->view_projection);
    if (!clip_to_world.has_value()) {
        return false;
    }
    const Environment& environment = *frame.environment;
    const SkyConstants constants{
        .clip_to_world = clip_to_world->columns,
        .eye = extend(frame.view->eye, 1.0f),
        .environment = Vec4{environment.intensity, 0.0f, std::cos(environment.rotation),
                            std::sin(environment.rotation)}};
    commands.bind_pipeline(m_pipelines->sky.get());
    commands.bind_texture(k_environment_slot, environment.radiance);
    commands.push_constants(std::as_bytes(std::span(&constants, 1)));
    commands.draw_vertices(std::as_bytes(std::span(k_screen_triangle)),
                           static_cast<std::uint32_t>(k_screen_triangle.size()));
    return true;
}

void ForwardRenderer::draw_bloom(rhi::CommandList& commands) {
    if (m_bloom.empty() || !m_pipelines->bloom_down.exists() || !m_pipelines->bloom_up.exists()) {
        return;
    }
    DREAM_PROFILE_ZONE("Bloom");
    const auto texel = [](platform::SurfaceSize size) {
        return core::Vec2{1.0f / static_cast<float>(size.width),
                          1.0f / static_cast<float>(size.height)};
    };
    const auto pass = [&](rhi::TextureHandle target, bool keep, rhi::PipelineHandle pipeline,
                          rhi::TextureHandle source, Vec4 settings) {
        commands.begin_render_pass(rhi::RenderTargets{
            .color = target,
            .clear_color = keep ? std::nullopt : std::optional(rhi::ClearColor{}),
            .clear_depth = std::nullopt});
        commands.bind_pipeline(pipeline);
        commands.bind_texture(1, source);
        const BloomConstants constants{.settings = settings};
        commands.push_constants(std::as_bytes(std::span(&constants, 1)));
        commands.draw_vertices(std::as_bytes(std::span(k_screen_triangle)),
                               static_cast<std::uint32_t>(k_screen_triangle.size()));
        commands.end_render_pass();
    };
    // Halving, from the scene down, keeping only what is above the threshold at the first step,
    // which quarters it: its taps are twice as far apart, to cover the scene.
    for (std::size_t level = 0; level < m_bloom.size(); ++level) {
        const bool first = level == 0;
        const core::Vec2 step = first ? texel(m_hdr_size) * 2.0f : texel(m_bloom_sizes[level - 1]);
        pass(m_bloom[level].get(), false, m_pipelines->bloom_down.get(),
             first ? m_hdr_color.get() : m_bloom[level - 1].get(),
             Vec4{step.x, step.y, m_settings.bloom_threshold, first ? 1.0f : 0.0f});
    }
    // Each smaller image added to the next larger. Tone mapping adds the largest to the scene.
    for (std::size_t level = m_bloom.size() - 1; level > 0; --level) {
        const core::Vec2 step = texel(m_bloom_sizes[level]);
        pass(m_bloom[level - 1].get(), true, m_pipelines->bloom_up.get(), m_bloom[level].get(),
             Vec4{step.x, step.y, 1.0f, 0.0f});
    }
}

void ForwardRenderer::prepare_meshes(const Frame& frame, bool boxes) {
    const std::span<const MeshDraw> meshes = frame.meshes;
    m_chosen.resize(meshes.size());
    m_instances.resize(meshes.size());
    m_world_bounds.resize(boxes ? meshes.size() : 0);

    // For a part's size on screen: the projection's vertical scale, which is the length of the
    // view-projection matrix's second row, as look_at's rows are unit length.
    const bool lods = m_settings.lod_error > 0.0f && frame.view.has_value() && boxes;
    float pixels_per_unit = 0.0f;
    if (lods) {
        const Mat4& matrix = frame.view->view_projection;
        const Vec3 row{matrix.columns[0].y, matrix.columns[1].y, matrix.columns[2].y};
        pixels_per_unit = core::length(row) * static_cast<float>(frame.size.height) * 0.5f;
    }

    std::optional<Mat4> normals_for;
    Mat4 normals;
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        const MeshDraw& mesh = meshes[index];
        if (boxes) {
            m_world_bounds[index] = core::transformed(mesh.model, mesh.bounds);
        }

        // The parts of one model share its matrix, so its normals are worked out once.
        if (!normals_for.has_value() || std::memcmp(&*normals_for, &mesh.model, sizeof(Mat4)) != 0) {
            const std::optional<Mat4> inverted = core::inverse(mesh.model);
            normals = inverted.has_value() ? core::transpose(*inverted) : mesh.model;
            normals_for = mesh.model;
        }
        m_instances[index] = InstanceData{
            .model = mesh.model.columns,
            // The first column's w, otherwise unused, carries the alpha cutoff.
            .normals = {Vec4{normals.columns[0].x, normals.columns[0].y, normals.columns[0].z,
                             mesh.material.alpha_cutoff},
                        normals.columns[1], normals.columns[2]},
            .base_color = mesh.material.base_color,
            .material = Vec4{mesh.material.metallic, mesh.material.roughness,
                             mesh.material.normal_scale, mesh.material.occlusion_strength},
            .emissive = extend(mesh.material.emissive, material_features(mesh.material)),
        };

        // The simplest version whose error stays under the limit on screen, at the part's nearest
        // point to the camera.
        Chosen chosen{mesh.first_index, mesh.index_count, false};
        const core::Aabb& box = boxes ? m_world_bounds[index] : mesh.bounds;
        if (lods && !mesh.lods.empty() && !box.empty()) {
            const Vec3 eye = frame.view->eye;
            const Vec3 nearest{std::clamp(eye.x, box.min.x, box.max.x),
                               std::clamp(eye.y, box.min.y, box.max.y),
                               std::clamp(eye.z, box.min.z, box.max.z)};
            const float distance = core::length(nearest - eye);
            if (distance > 0.0f) {
                float scale = 0.0f;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const Vec4& column = mesh.model.columns[axis];
                    scale = std::max(scale, core::length(Vec3{column.x, column.y, column.z}));
                }
                for (const MeshLod& lod : mesh.lods) {
                    if (lod.error * scale * pixels_per_unit / distance > m_settings.lod_error) {
                        break;
                    }
                    chosen = Chosen{lod.first_index, lod.index_count, true};
                }
            }
        }
        m_chosen[index] = chosen;
    }
}

bool ForwardRenderer::prepare_lights(const Frame& frame, FrameStats& stats) {
    const View& view = *frame.view;
    const core::Frustum bounds = core::frustum_from(view.view_projection);

    // The camera's forward direction, through the middle of the near plane, which depth is
    // measured along.
    Vec3 forward{0.0f, 0.0f, -1.0f};
    if (const std::optional<Mat4> clip_to_world = core::inverse(view.view_projection)) {
        const Vec4 middle = *clip_to_world * Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        const Vec3 point = Vec3{middle.x, middle.y, middle.z} * (1.0f / middle.w);
        if (core::length(point - view.eye) > 0.0f) {
            forward = core::normalized(point - view.eye);
        }
    }

    // The lights whose reach is in view, nearest first, up to the limit.
    m_light_order.clear();
    for (std::uint32_t index = 0; index < frame.lights.size(); ++index) {
        const Light& light = frame.lights[index];
        if (!(light.range > 0.0f) || !(light.intensity > 0.0f)) {
            continue;
        }
        const Vec3 reach{light.range, light.range, light.range};
        if (!core::intersects(bounds, core::Aabb{light.position - reach, light.position + reach})) {
            continue;
        }
        const float distance =
            std::max(core::length(light.position - view.eye) - light.range, 0.0f);
        m_light_order.emplace_back(distance, index);
    }
    std::ranges::stable_sort(m_light_order, {}, &std::pair<float, std::uint32_t>::first);
    const auto count = static_cast<std::uint32_t>(
        std::min<std::size_t>(m_light_order.size(), m_settings.max_lights));
    stats.lights = count;
    stats.lights_dropped = static_cast<std::uint32_t>(m_light_order.size()) - count;

    const std::uint32_t cluster_count = k_clusters[0] * k_clusters[1] * k_clusters[2];
    const std::uint32_t cluster_start = k_light_header_texels + k_max_lights * k_texels_per_light;
    const std::uint32_t index_start = cluster_start + (cluster_count + 1) / 2;
    const float near = std::max(view.z_near, 0.001f);
    const float far = std::max(view.z_far, near * 2.0f);
    // Slices grow with depth, as the screen boxes do: slice = log(depth / near) * scale.
    const float slice_scale = static_cast<float>(k_clusters[2]) / std::log(far / near);

    // Each light's box of clusters, conservatively: its sphere's corners on screen and its depths.
    m_light_reach.clear();
    for (std::uint32_t order = 0; order < count; ++order) {
        const Light& light = frame.lights[m_light_order[order].second];
        LightReach reach{.light = order};
        bool behind = false;
        float low_x = 1.0f;
        float low_y = 1.0f;
        float high_x = -1.0f;
        float high_y = -1.0f;
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 point = light.position + Vec3{(corner & 1) != 0 ? light.range : -light.range,
                                                     (corner & 2) != 0 ? light.range : -light.range,
                                                     (corner & 4) != 0 ? light.range : -light.range};
            const Vec4 clip = view.view_projection * Vec4{point.x, point.y, point.z, 1.0f};
            if (clip.w <= near * 0.5f) {
                behind = true;
                break;
            }
            low_x = std::min(low_x, clip.x / clip.w);
            high_x = std::max(high_x, clip.x / clip.w);
            low_y = std::min(low_y, clip.y / clip.w);
            high_y = std::max(high_y, clip.y / clip.w);
        }
        const auto tile = [](float ndc, std::uint32_t tiles) {
            const float at = (std::clamp(ndc, -1.0f, 1.0f) * 0.5f + 0.5f) * static_cast<float>(tiles);
            return std::min(static_cast<std::uint32_t>(std::max(at, 0.0f)), tiles - 1);
        };
        // A light around or behind the camera may reach anywhere on screen.
        reach.low[0] = behind ? 0 : tile(low_x, k_clusters[0]);
        reach.high[0] = behind ? k_clusters[0] - 1 : tile(high_x, k_clusters[0]);
        reach.low[1] = behind ? 0 : tile(low_y, k_clusters[1]);
        reach.high[1] = behind ? k_clusters[1] - 1 : tile(high_y, k_clusters[1]);
        const float depth = core::dot(light.position - view.eye, forward);
        const auto slice = [&](float z) {
            const float at = std::log(std::max(z, near) / near) * slice_scale;
            return std::min(static_cast<std::uint32_t>(std::max(at, 0.0f)), k_clusters[2] - 1);
        };
        reach.low[2] = slice(depth - light.range);
        reach.high[2] = slice(depth + light.range);
        m_light_reach.push_back(reach);
    }

    // How many lights each cluster has, where its list starts, then the lists, as long as they
    // fit in the texture.
    const auto cluster_of = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return (z * k_clusters[1] + y) * k_clusters[0] + x;
    };
    m_cluster_counts.assign(cluster_count, 0);
    for (const LightReach& reach : m_light_reach) {
        for (std::uint32_t z = reach.low[2]; z <= reach.high[2]; ++z) {
            for (std::uint32_t y = reach.low[1]; y <= reach.high[1]; ++y) {
                for (std::uint32_t x = reach.low[0]; x <= reach.high[0]; ++x) {
                    ++m_cluster_counts[cluster_of(x, y, z)];
                }
            }
        }
    }
    const std::uint32_t index_room =
        (k_light_texture_width * k_light_texture_height - index_start) * 4;
    m_cluster_starts.assign(cluster_count, 0);
    std::uint32_t total = 0;
    for (std::uint32_t cluster = 0; cluster < cluster_count; ++cluster) {
        m_cluster_starts[cluster] = total;
        m_cluster_counts[cluster] = std::min(m_cluster_counts[cluster], index_room - total);
        total += m_cluster_counts[cluster];
    }
    if (total == index_room && !m_warned_light_lists) {
        DREAM_LOG_WARNING("The lights' cluster lists are full, so some clusters miss lights");
        m_warned_light_lists = true;
    }

    std::ranges::fill(m_light_data, Vec4{});
    m_light_data[0] = Vec4{static_cast<float>(count), static_cast<float>(k_clusters[0]),
                           static_cast<float>(k_clusters[1]), static_cast<float>(k_clusters[2])};
    m_light_data[1] = Vec4{near, slice_scale, static_cast<float>(cluster_start),
                           static_cast<float>(index_start)};
    m_light_data[2] = extend(forward, 0.0f);
    for (std::uint32_t order = 0; order < count; ++order) {
        const Light& light = frame.lights[m_light_order[order].second];
        const bool spot = light.type == LightType::Spot;
        const std::size_t at = k_light_header_texels + std::size_t{order} * k_texels_per_light;
        m_light_data[at] = extend(light.position, light.range);
        m_light_data[at + 1] = extend(light.color * light.intensity, spot ? 1.0f : 0.0f);
        m_light_data[at + 2] = extend(spot ? core::normalized(light.direction) : Vec3{},
                                      std::cos(std::max(light.outer_angle, light.inner_angle)));
        m_light_data[at + 3] = Vec4{std::cos(light.inner_angle), 0.0f, 0.0f, 0.0f};
    }
    for (std::uint32_t cluster = 0; cluster < cluster_count; ++cluster) {
        Vec4& entry = m_light_data[cluster_start + cluster / 2];
        const auto start = static_cast<float>(m_cluster_starts[cluster]);
        const auto lights = static_cast<float>(m_cluster_counts[cluster]);
        if (cluster % 2 == 0) {
            entry.x = start;
            entry.y = lights;
        } else {
            entry.z = start;
            entry.w = lights;
        }
    }
    // The lists, filled in the order the counts were made, so each stays within its count.
    std::vector<std::uint32_t>& filled = m_cluster_counts;
    std::vector<std::uint32_t>& limits = m_cluster_limits;
    limits.assign(filled.begin(), filled.end());
    std::ranges::fill(filled, 0);
    for (const LightReach& reach : m_light_reach) {
        for (std::uint32_t z = reach.low[2]; z <= reach.high[2]; ++z) {
            for (std::uint32_t y = reach.low[1]; y <= reach.high[1]; ++y) {
                for (std::uint32_t x = reach.low[0]; x <= reach.high[0]; ++x) {
                    const std::uint32_t cluster = cluster_of(x, y, z);
                    if (filled[cluster] >= limits[cluster]) {
                        continue;
                    }
                    const std::uint32_t slot = m_cluster_starts[cluster] + filled[cluster]++;
                    Vec4& packed = m_light_data[index_start + slot / 4];
                    const auto value = static_cast<float>(reach.light);
                    switch (slot % 4) {
                    case 0:
                        packed.x = value;
                        break;
                    case 1:
                        packed.y = value;
                        break;
                    case 2:
                        packed.z = value;
                        break;
                    default:
                        packed.w = value;
                        break;
                    }
                }
            }
        }
    }
    const std::uint32_t texels_used = index_start + (total + 3) / 4;
    m_light_rows_used = (texels_used + k_light_texture_width - 1) / k_light_texture_width;
    return count > 0;
}

void ForwardRenderer::draw_meshes(rhi::CommandList& commands, std::span<const MeshDraw> meshes,
                                  Pass pass, const core::Frustum* view, FrameStats& stats) {
    const bool casters = is_shadow(pass);
    m_order.clear();
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        if (!in_pass(meshes[index], pass)) {
            continue;
        }
        if (view != nullptr && !core::intersects(*view, m_world_bounds[index])) {
            ++(casters ? stats.shadow_culled : stats.culled);
            continue;
        }
        m_order.push_back(static_cast<std::uint32_t>(index));
    }
    if (m_order.empty()) {
        return;
    }

    // Parts that draw the same indices with the same textures and sides become one draw, whatever
    // their material's factors. Opaque shadows ignore textures; cut-out ones need the base color.
    const auto key = [&](std::uint32_t index) {
        const MeshDraw& mesh = meshes[index];
        const Chosen& chosen = m_chosen[index];
        const MeshMaterial& material = mesh.material;
        const auto texture = [&](rhi::TextureHandle handle) {
            return casters ? rhi::TextureHandle{} : handle;
        };
        // A skinned part has its own pose, so it is never drawn with another.
        const std::uint32_t own = mesh.joint_matrices.empty() ? 0 : index + 1;
        return std::make_tuple(
            !casters && material.double_sided, own, mesh.vertices, mesh.indices,
            chosen.first_index, chosen.index_count,
            pass == Pass::Shadow ? rhi::TextureHandle{} : material.base_color_texture,
            texture(material.normal_texture), texture(material.metallic_roughness_texture),
            texture(material.occlusion_texture), texture(material.emissive_texture));
    };
    if (pass == Pass::Blend) {
        // Farthest first, by the middle of each part's box, so nearer glass blends over farther.
        // Equal neighbours still share a draw.
        m_distances.resize(meshes.size());
        const Vec3 eye = m_eye;
        for (const std::uint32_t index : m_order) {
            const MeshDraw& mesh = meshes[index];
            const Vec3 middle = mesh.bounds.empty()
                                    ? Vec3{}
                                    : (mesh.bounds.min + mesh.bounds.max) * 0.5f;
            const Vec3 world = core::transform_point(mesh.model, middle);
            m_distances[index] = core::length_squared(world - eye);
        }
        std::ranges::stable_sort(m_order, [&](std::uint32_t a, std::uint32_t b) {
            return m_distances[a] > m_distances[b];
        });
    } else {
        std::ranges::stable_sort(m_order,
                                 [&](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });
    }

    // The pass's pipeline for a part: skinned or not, and for lit passes, one- or double-sided.
    const auto pipeline_for = [&](const MeshDraw& mesh) {
        const bool skinned = !mesh.joint_matrices.empty();
        const Pipelines& made = *m_pipelines;
        const SidedPipelines* sided = nullptr;
        switch (pass) {
        case Pass::Shadow:
            return (skinned ? made.shadow_skinned : made.shadow).get();
        case Pass::ShadowMasked:
            return (skinned ? made.shadow_masked_skinned : made.shadow_masked).get();
        case Pass::Opaque:
            sided = skinned ? &made.forward_skinned : &made.forward;
            break;
        case Pass::Masked:
            sided = skinned ? &made.masked_skinned : &made.masked;
            break;
        case Pass::Blend:
            sided = skinned ? &made.blended_skinned : &made.blended;
            break;
        }
        return (mesh.material.double_sided ? sided->double_sided : sided->one_sided).get();
    };
    rhi::PipelineHandle bound_pipeline;
    rhi::BufferHandle bound_vertices;
    rhi::BufferHandle bound_indices;
    for (std::size_t first = 0; first < m_order.size();) {
        std::size_t last = first + 1;
        while (last < m_order.size() && last - first < k_max_instances_per_draw &&
               key(m_order[last]) == key(m_order[first])) {
            ++last;
        }
        const MeshDraw& mesh = meshes[m_order[first]];
        const Chosen& chosen = m_chosen[m_order[first]];
        m_batch.clear();
        for (std::size_t at = first; at < last; ++at) {
            m_batch.push_back(m_instances[m_order[at]]);
        }
        const auto count = static_cast<std::uint32_t>(last - first);
        first = last;

        if (const rhi::PipelineHandle wanted = pipeline_for(mesh); wanted != bound_pipeline) {
            commands.bind_pipeline(wanted);
            bound_pipeline = wanted;
        }
        if (!mesh.joint_matrices.empty()) {
            // Each joint as three rows of a 3x4 matrix, as skinning.slang reads them.
            const std::size_t joints = std::min(mesh.joint_matrices.size(), k_max_joints);
            m_joint_rows.resize(joints * 3);
            for (std::size_t joint = 0; joint < joints; ++joint) {
                const Mat4& matrix = mesh.joint_matrices[joint];
                for (std::size_t row = 0; row < 3; ++row) {
                    const auto element = [&](std::size_t column) {
                        const Vec4& values = matrix.columns[column];
                        return row == 0 ? values.x : row == 1 ? values.y : values.z;
                    };
                    m_joint_rows[joint * 3 + row] = Vec4{element(0), element(1), element(2),
                                                         element(3)};
                }
            }
            commands.set_draw_uniforms(std::as_bytes(std::span(m_joint_rows)));
            if (!casters) {
                ++stats.skinned;
            }
        }
        if (mesh.vertices != bound_vertices) {
            commands.bind_vertex_buffer(mesh.vertices);
            bound_vertices = mesh.vertices;
        }
        if (mesh.indices != bound_indices) {
            commands.bind_index_buffer(mesh.indices, rhi::IndexFormat::UInt32);
            bound_indices = mesh.indices;
        }
        if (pass == Pass::ShadowMasked) {
            commands.bind_texture(mesh.material.base_color_texture);
        } else if (!casters) {
            const MeshMaterial& material = mesh.material;
            commands.bind_texture(material.base_color_texture);
            // Without a normal map, a flat one, which leaves the surface's normal as it is.
            commands.bind_texture(k_normal_slot, material.normal_texture.is_valid()
                                                     ? material.normal_texture
                                                     : m_flat_normal.get());
            commands.bind_texture(k_metallic_roughness_slot, material.metallic_roughness_texture);
            commands.bind_texture(k_occlusion_slot, material.occlusion_texture);
            commands.bind_texture(k_emissive_slot, material.emissive_texture);
        }
        if (!commands.bind_instances(std::as_bytes(std::span(m_batch)))) {
            if (!m_warned_instances) {
                DREAM_LOG_WARNING("The frame's vertex memory is used up, so some parts aren't "
                                  "drawn");
                m_warned_instances = true;
            }
            break;
        }
        commands.draw_indexed_instanced(chosen.first_index, chosen.index_count, count);
        ++stats.draw_calls;
        if (casters) {
            stats.shadow_casters += count;
        } else {
            stats.draws += count;
            if (pass == Pass::Blend) {
                stats.blended += count;
            }
            if (chosen.simplified) {
                stats.simplified += count;
            }
        }
    }
}

void ForwardRenderer::draw_lines(rhi::CommandList& commands, std::span<const LineVertex> lines,
                                 FrameStats& stats) {
    if (lines.empty()) {
        return;
    }
    commands.bind_pipeline(m_pipelines->lines.get());
    // Drawn in pieces that fit the frame's vertex memory, until it is used up.
    constexpr std::size_t k_vertices_per_draw = 8192;
    for (std::size_t first = 0; first < lines.size(); first += k_vertices_per_draw) {
        const std::span<const LineVertex> piece =
            lines.subspan(first, std::min(k_vertices_per_draw, lines.size() - first));
        if (!commands.draw_vertices(std::as_bytes(piece), static_cast<std::uint32_t>(piece.size()))) {
            break;
        }
        stats.lines += static_cast<std::uint32_t>(piece.size() / 2);
    }
}

void ForwardRenderer::draw_sprites(rhi::CommandList& commands, const Frame& frame,
                                   FrameStats& stats) {
    if (frame.sprites.empty() || !m_pipelines->sprites.exists()) {
        return;
    }
    DREAM_PROFILE_ZONE("Sprites");
    FrameUniforms camera{};
    camera.view_projection = *frame.sprite_view;
    commands.bind_pipeline(m_pipelines->sprites.get());
    commands.set_uniforms(std::as_bytes(std::span(&camera, 1)));
    commands.bind_vertex_buffer(m_particle_corners.get());
    commands.bind_index_buffer(m_particle_indices.get(), rhi::IndexFormat::UInt32);
    for (const SpriteBatch& batch : frame.sprites) {
        if (batch.sprites.empty()) {
            continue;
        }
        const SpriteConstants constants{
            .settings = Vec4{batch.filter == SpriteFilter::Pixel ? 1.0f : 0.0f,
                             static_cast<float>(std::max(batch.texture_width, 1u)),
                             static_cast<float>(std::max(batch.texture_height, 1u)), 0.0f}};
        commands.push_constants(std::as_bytes(std::span(&constants, 1)));
        commands.bind_texture(batch.texture);
        for (std::size_t first = 0; first < batch.sprites.size(); first += k_max_sprites_per_draw) {
            const std::span<const SpriteQuad> piece = batch.sprites.subspan(
                first, std::min(k_max_sprites_per_draw, batch.sprites.size() - first));
            m_sprite_instances.clear();
            for (const SpriteQuad& sprite : piece) {
                m_sprite_instances.push_back(SpriteInstance{
                    .position_size = Vec4{sprite.position.x, sprite.position.y, sprite.size.x,
                                          sprite.size.y},
                    .uv_rect = sprite.uv,
                    .color = sprite.color,
                    .pivot_turn = Vec4{sprite.pivot.x, sprite.pivot.y, std::cos(sprite.rotation),
                                       std::sin(sprite.rotation)}});
            }
            if (!commands.bind_instances(std::as_bytes(std::span(m_sprite_instances)))) {
                return;
            }
            commands.draw_indexed_instanced(0, static_cast<std::uint32_t>(k_particle_indices.size()),
                                            static_cast<std::uint32_t>(piece.size()));
            ++stats.draw_calls;
            stats.sprites += static_cast<std::uint32_t>(piece.size());
        }
    }
    DREAM_PROFILE_VALUE("Sprites", stats.sprites);
}

void ForwardRenderer::draw_particles(rhi::CommandList& commands, const Frame& frame,
                                     FrameStats& stats) {
    if (frame.particles.empty() || !frame.view.has_value() || !m_pipelines->particles.exists()) {
        return;
    }
    DREAM_PROFILE_ZONE("Particles");
    const View& view = *frame.view;
    // The camera's right and up directions: the view-projection matrix's first two rows, as
    // look_at's rows are the camera's axes and the projection only scales them.
    const Mat4& matrix = view.view_projection;
    const Vec3 right = core::normalized(
        Vec3{matrix.columns[0].x, matrix.columns[1].x, matrix.columns[2].x});
    const Vec3 up = core::normalized(
        Vec3{matrix.columns[0].y, matrix.columns[1].y, matrix.columns[2].y});

    // Blended batches farthest first, by their particles' middle, then the additive ones.
    m_batch_order.clear();
    std::size_t wanted = 0;
    for (std::uint32_t index = 0; index < frame.particles.size(); ++index) {
        const ParticleBatch& batch = frame.particles[index];
        if (batch.particles.empty()) {
            continue;
        }
        wanted += batch.particles.size();
        float key = -1.0f;
        if (batch.blend == ParticleBlend::Alpha) {
            Vec3 middle{};
            for (const ParticleQuad& particle : batch.particles) {
                middle = middle + particle.position;
            }
            middle = middle / static_cast<float>(batch.particles.size());
            key = core::length_squared(middle - view.eye);
        }
        m_batch_order.emplace_back(key, index);
    }
    std::ranges::stable_sort(m_batch_order, std::ranges::greater{},
                             &std::pair<float, std::uint32_t>::first);

    commands.bind_pipeline(m_pipelines->particles.get());
    commands.bind_vertex_buffer(m_particle_corners.get());
    commands.bind_index_buffer(m_particle_indices.get(), rhi::IndexFormat::UInt32);
    std::size_t budget = k_max_particles_per_frame;
    for (const auto& [key, index] : m_batch_order) {
        const ParticleBatch& batch = frame.particles[index];
        const bool additive = batch.blend == ParticleBlend::Additive;
        // Blended particles farthest first, so nearer ones cover them.
        m_particle_order.clear();
        for (std::uint32_t particle = 0; particle < batch.particles.size(); ++particle) {
            const float distance =
                additive ? 0.0f
                         : core::length_squared(batch.particles[particle].position - view.eye);
            m_particle_order.emplace_back(distance, particle);
        }
        if (!additive) {
            std::ranges::sort(m_particle_order, std::ranges::greater{},
                              &std::pair<float, std::uint32_t>::first);
        }
        // Over the frame's limit, the nearest are kept.
        const std::size_t taken = std::min(budget, m_particle_order.size());
        const std::size_t skipped = m_particle_order.size() - taken;
        budget -= taken;
        m_particle_instances.clear();
        for (std::size_t at = skipped; at < m_particle_order.size(); ++at) {
            const ParticleQuad& particle = batch.particles[m_particle_order[at].second];
            m_particle_instances.push_back(ParticleInstance{
                .position_size = extend(particle.position, particle.size),
                .color = particle.color,
                .turn = Vec4{std::cos(particle.rotation), std::sin(particle.rotation),
                             additive ? 1.0f : 0.0f, 0.0f}});
        }
        if (m_particle_instances.empty()) {
            continue;
        }
        const ParticleConstants constants{
            .right = extend(right, 0.0f),
            .up = extend(up, 0.0f),
            .settings = Vec4{batch.texture.is_valid() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}};
        commands.push_constants(std::as_bytes(std::span(&constants, 1)));
        commands.bind_texture(batch.texture);
        for (std::size_t first = 0; first < m_particle_instances.size();
             first += k_max_particles_per_draw) {
            const std::span<const ParticleInstance> piece = std::span(m_particle_instances)
                .subspan(first, std::min(k_max_particles_per_draw,
                                         m_particle_instances.size() - first));
            if (!commands.bind_instances(std::as_bytes(piece))) {
                budget = 0;
                break;
            }
            commands.draw_indexed_instanced(0, static_cast<std::uint32_t>(k_particle_indices.size()),
                                            static_cast<std::uint32_t>(piece.size()));
            ++stats.draw_calls;
            stats.particles += static_cast<std::uint32_t>(piece.size());
        }
    }
    stats.particles_dropped = static_cast<std::uint32_t>(wanted - stats.particles);
}

void ForwardRenderer::draw_ui(rhi::CommandList& commands, const Frame& frame, FrameStats& stats) {
    if (frame.size.width == 0 || frame.size.height == 0) {
        return;
    }
    const float width = static_cast<float>(frame.size.width);
    const float height = static_cast<float>(frame.size.height);

    for (const UiLayer& layer : frame.ui_layers) {
        if (layer.list == nullptr) {
            continue;
        }
        const ui::DrawList& list = *layer.list;
        const core::Vec2 offset = layer.offset;
        if (list.commands.empty() || list.vertices.empty()) {
            continue;
        }
        commands.bind_pipeline(list.premultiplied_alpha ? m_pipelines->ui_premultiplied.get()
                                                        : m_pipelines->ui.get());
        if (!commands.bind_frame_geometry(std::as_bytes(std::span(list.vertices)), list.indices)) {
            continue;
        }
        for (const ui::DrawCommand& command : list.commands) {
            if (command.index_count == 0 ||
                command.first_index + command.index_count > list.indices.size()) {
                continue;
            }
            const UiConstants constants{
                .scale = core::Vec2{2.0f / width, -2.0f / height},
                .offset = core::Vec2{-1.0f + 2.0f * (command.translation.x + offset.x) / width,
                                     1.0f - 2.0f * (command.translation.y + offset.y) / height},
                .premultiplied = list.premultiplied_alpha ? 1.0f : 0.0f,
            };
            if (command.clip.has_value()) {
                const ui::ClipRect& clip = *command.clip;
                commands.set_scissor(rhi::ScissorRect{
                    .x = clip.x + static_cast<std::int32_t>(offset.x),
                    .y = clip.y + static_cast<std::int32_t>(offset.y),
                    .width = static_cast<std::uint32_t>(std::max(clip.width, 0)),
                    .height = static_cast<std::uint32_t>(std::max(clip.height, 0)),
                });
            } else {
                commands.set_scissor(std::nullopt);
            }
            commands.bind_texture(frame.ui_texture ? frame.ui_texture(command.texture)
                                                   : rhi::TextureHandle{});
            commands.push_constants(std::as_bytes(std::span(&constants, 1)));
            commands.draw_indexed_range(command.first_index, command.index_count);
            ++stats.ui_commands;
        }
        commands.set_scissor(std::nullopt);
    }
}

FrameStats ForwardRenderer::render(rhi::CommandList& commands, const Frame& frame) {
    DREAM_PROFILE_ZONE("Draw scene");
    FrameStats stats;
    if (m_pipelines == nullptr) {
        // No shaders yet: the window is only cleared.
        commands.begin_render_pass(frame.clear_color);
        commands.end_render_pass();
        return stats;
    }

    // The frame's uniforms, which every pass reads, or nothing without a camera.
    std::optional<FrameUniforms> uniforms;
    bool shadows = false;
    std::array<core::Frustum, k_max_cascades> cascade_bounds{};
    if (frame.view.has_value()) {
        const View& camera = *frame.view;
        FrameUniforms values{
            .view_projection = camera.view_projection,
            .camera_position = extend(camera.eye, 1.0f),
            .direction_to_light = Vec4{0.0f, 1.0f, 0.0f, 0.0f},
            .light_color_and_ambient = Vec4{0.0f, 0.0f, 0.0f, 0.2f},
            .light_view_projection = Mat4{},
            .cascades = {},
            .cascade_far = Vec4{},
            .cascade_normal_offset = Vec4{},
            .shadow_settings = Vec4{},
            .camera_forward = Vec4{},
            .environment = Vec4{},
        };
        if (frame.environment.has_value() && frame.environment->radiance.is_valid()) {
            const Environment& environment = *frame.environment;
            values.environment =
                Vec4{std::max(environment.intensity, 1e-6f),
                     static_cast<float>(std::max(environment.levels, 2u) - 1),
                     std::cos(environment.rotation), std::sin(environment.rotation)};
        }
        if (frame.sun.has_value()) {
            const Sunlight& sun = *frame.sun;
            values.direction_to_light = extend(core::normalized(-sun.direction), 0.0f);
            values.light_color_and_ambient = extend(sun.color * sun.intensity, sun.ambient);
            shadows = sun.casts_shadows && m_shadow_map.exists() && m_pipelines->shadow.exists();
            if (shadows) {
                fit_cascades(values, camera, sun.direction, m_settings, cascade_bounds);
            }
        }
        uniforms = values;
    }
    const auto uniform_bytes = [&] { return std::as_bytes(std::span(&*uniforms, 1)); };

    // Each part's box moved into the world once, for culling against the camera and every
    // cascade, and for choosing its version.
    const bool culling = m_settings.culling && frame.view.has_value();
    std::optional<core::Frustum> camera_bounds;
    if (frame.view.has_value()) {
        DREAM_PROFILE_ZONE("Prepare parts");
        m_eye = frame.view->eye;
        prepare_meshes(frame, culling || m_settings.lod_error > 0.0f);
        if (culling) {
            camera_bounds = core::frustum_from(frame.view->view_projection);
        }
    }
    // Before any pass, as textures are only updated between them.
    if (frame.view.has_value() && m_light_texture.exists()) {
        DREAM_PROFILE_ZONE("Prepare lights");
        prepare_lights(frame, stats);
        commands.update_texture(
            m_light_texture.get(), 0,
            std::as_bytes(std::span(m_light_data).first(std::size_t{m_light_rows_used} *
                                                        k_light_texture_width)));
    }

    if (shadows) {
        // Each cascade into its own part of the map, kept there by a scissor.
        DREAM_PROFILE_ZONE("Shadow map");
        commands.begin_render_pass(rhi::RenderTargets{.depth = m_shadow_map.get()});
        FrameUniforms cascade_uniforms = *uniforms;
        const std::uint32_t cell_size = m_settings.shadow_resolution;
        for (std::uint32_t cascade = 0; cascade < m_settings.shadow_cascades; ++cascade) {
            const CascadeCell cell = cascade_cell(cascade);
            commands.set_scissor(rhi::ScissorRect{
                .x = static_cast<std::int32_t>(cell.column * cell_size),
                .y = static_cast<std::int32_t>(cell.row * cell_size),
                .width = cell_size,
                .height = cell_size});
            cascade_uniforms.light_view_projection = uniforms->cascades[cascade];
            const core::Frustum* const bounds = culling ? &cascade_bounds[cascade] : nullptr;
            commands.bind_pipeline(m_pipelines->shadow.get());
            commands.set_uniforms(std::as_bytes(std::span(&cascade_uniforms, 1)));
            draw_meshes(commands, frame.meshes, Pass::Shadow, bounds, stats);
            // Cut-out casters, which need their base color's alpha.
            commands.bind_pipeline(m_pipelines->shadow_masked.get());
            commands.set_uniforms(std::as_bytes(std::span(&cascade_uniforms, 1)));
            draw_meshes(commands, frame.meshes, Pass::ShadowMasked, bounds, stats);
        }
        commands.end_render_pass();
    }

    // Without a 3D view, as in a 2D game, the window is cleared and drawn into directly, so the
    // clear color isn't tone-mapped.
    const bool hdr = m_settings.hdr_format.has_value() && m_pipelines->tone_map.exists() &&
                     frame.view.has_value();
    if (hdr) {
        fit_hdr_targets(frame.size);
    }
    const auto draw_lit_scene = [&] {
        if (!uniforms.has_value()) {
            return;
        }
        commands.bind_pipeline(m_pipelines->forward.one_sided.get());
        commands.set_uniforms(uniform_bytes());
        if (uniforms->environment.x > 0.0f) {
            commands.bind_texture(k_environment_slot, frame.environment->radiance);
        }
        if (shadows) {
            commands.bind_texture(rhi::k_shadow_slot, m_shadow_map.get());
        }
        // Without a lights' texture the slot keeps its default, which reads as no lights.
        if (m_light_texture.exists()) {
            commands.bind_texture(k_light_slot, m_light_texture.get());
        }
        // Opaque, then cut-out, then blended over both. Each pass binds its own pipelines.
        const core::Frustum* const bounds = camera_bounds ? &*camera_bounds : nullptr;
        draw_meshes(commands, frame.meshes, Pass::Opaque, bounds, stats);
        draw_meshes(commands, frame.meshes, Pass::Masked, bounds, stats);
        // The environment where no opaque surface is, before what blends over it.
        if (draw_sky(commands, frame)) {
            commands.set_uniforms(uniform_bytes());
        }
        draw_meshes(commands, frame.meshes, Pass::Blend, bounds, stats);
        draw_particles(commands, frame, stats);
    };
    if (hdr && m_hdr_color.exists()) {
        // The scene in HDR, then tone-mapped onto the window, with the lines and interface over it.
        commands.begin_render_pass(rhi::RenderTargets{.color = m_hdr_color.get(),
                                                      .depth = m_hdr_depth.get(),
                                                      .clear_color = frame.clear_color});
        draw_lit_scene();
        commands.end_render_pass();
        const bool bloom = m_settings.bloom && !m_bloom.empty() &&
                           m_pipelines->bloom_down.exists() && m_pipelines->bloom_up.exists();
        if (bloom) {
            draw_bloom(commands);
        }

        commands.begin_render_pass(rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
        {
            DREAM_PROFILE_ZONE("Tone mapping");
            // The bloom's levels are added up, so its intensity is shared among them.
            const float glow = bloom ? m_settings.bloom_intensity /
                                           static_cast<float>(m_bloom.size())
                                     : 0.0f;
            const ToneMapConstants constants{
                .settings = Vec4{m_settings.exposure,
                                 m_settings.tone_mapping == ToneMapping::PbrNeutral ? 1.0f : 0.0f,
                                 glow, 0.0f},
                .bloom_texel =
                    bloom ? Vec4{1.0f / static_cast<float>(m_bloom_sizes[0].width),
                                 1.0f / static_cast<float>(m_bloom_sizes[0].height), 0.0f, 0.0f}
                          : Vec4{}};
            commands.bind_pipeline(m_pipelines->tone_map.get());
            commands.bind_texture(1, m_hdr_color.get());
            if (bloom) {
                commands.bind_texture(3, m_bloom[0].get());
            }
            commands.push_constants(std::as_bytes(std::span(&constants, 1)));
            commands.draw_vertices(std::as_bytes(std::span(k_screen_triangle)),
                                   static_cast<std::uint32_t>(k_screen_triangle.size()));
        }
        // A pass starts without the scene's uniforms on some backends, and the lines read them.
        if (uniforms.has_value()) {
            commands.set_uniforms(uniform_bytes());
        }
    } else {
        commands.begin_render_pass(frame.clear_color);
        draw_lit_scene();
    }

    if (frame.sprite_view.has_value()) {
        draw_sprites(commands, frame, stats);
    }
    if (uniforms.has_value()) {
        // The sprites' camera replaced the scene's.
        if (frame.sprite_view.has_value() && !frame.sprites.empty()) {
            commands.set_uniforms(uniform_bytes());
        }
        draw_lines(commands, frame.lines, stats);
    } else if (frame.sprite_view.has_value()) {
        // A 2D game's lines, such as Physics2D's, in its 2D camera's view.
        FrameUniforms flat{};
        flat.view_projection = *frame.sprite_view;
        commands.set_uniforms(std::as_bytes(std::span(&flat, 1)));
        draw_lines(commands, frame.lines, stats);
    }
    draw_ui(commands, frame, stats);
    commands.end_render_pass();
    DREAM_PROFILE_VALUE("Draw calls", stats.draws);
    DREAM_PROFILE_VALUE("Shadow casters", stats.shadow_casters);
    DREAM_PROFILE_VALUE("Culled parts", stats.culled);
    DREAM_PROFILE_VALUE("Culled shadow casters", stats.shadow_culled);
    DREAM_PROFILE_VALUE("Instanced draw calls", stats.draw_calls);
    DREAM_PROFILE_VALUE("Simplified parts", stats.simplified);
    DREAM_PROFILE_VALUE("Debug lines", stats.lines);
    DREAM_PROFILE_VALUE("UI draw commands", stats.ui_commands);
    DREAM_PROFILE_VALUE("Lights", stats.lights);
    return stats;
}

core::Result<std::unique_ptr<SceneRenderer>> create(const core::Config& config) {
    return std::unique_ptr<SceneRenderer>{std::make_unique<ForwardRenderer>(config)};
}

} // namespace

#if defined(DREAM_FORWARD_PLUS)
[[nodiscard]] core::Result<void> register_renderer_forward_plus(core::ModuleRegistry& registry) {
#else
[[nodiscard]] core::Result<void> register_renderer_forward(core::ModuleRegistry& registry) {
#endif
    return registry.add(
        core::ModuleDescriptor<SceneRenderer>{.name = k_module_name, .create = &create});
}

} // namespace dream::modules
