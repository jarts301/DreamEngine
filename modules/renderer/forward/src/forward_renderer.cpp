// The 'forward' renderer (ADR 0030): one directional light with cascaded shadows (ADR 0029), drawn
// in HDR and tone-mapped with Khronos PBR Neutral (ADR 0028), then debug lines and the interface
// layers over the result. Parts out of the camera's view, or out of a shadow cascade, are skipped
// (ADR 0031). Surfaces are lit with glTF's metallic-roughness model and normal maps (ADR 0033).

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
    // Emissive color; w is unused.
    Vec4 emissive;
};

// Where forward.slang reads a material's textures, besides the base color at slot 0.
constexpr std::uint32_t k_normal_slot = 3;
constexpr std::uint32_t k_metallic_roughness_slot = 4;
constexpr std::uint32_t k_occlusion_slot = 5;
constexpr std::uint32_t k_emissive_slot = 6;
static_assert(k_emissive_slot < rhi::k_texture_slots);

struct ToneMapConstants {
    Vec4 settings;
};

struct UiConstants {
    core::Vec2 scale;
    core::Vec2 offset;
    float premultiplied = 0.0f;
};

static_assert(sizeof(FrameUniforms) <= rhi::k_max_uniform_size);
static_assert(sizeof(FrameUniforms) == 496);
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

// What the project's [renderer] table and the device choose (ADR 0028, 0029).
struct Settings {
    std::optional<rhi::TextureFormat> hdr_format;
    ToneMapping tone_mapping = ToneMapping::PbrNeutral;
    float exposure = 1.0f;
    std::uint32_t shadow_resolution = 0;
    std::uint32_t shadow_cascades = 0;
    float shadow_distance = 40.0f;
    bool culling = true;
    // How many pixels a simpler version of a part may stray on screen; 0 always draws the full one.
    float lod_error = 1.0f;
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
        frame.camera_forward = extend(core::normalized(point - camera.eye), 0.0f);
    }
}

// Every shader and pipeline, made from the compiled shader files together, and replaced together.
struct Pipelines {
    std::vector<OwnedShader> shaders;
    OwnedPipeline forward;
    OwnedPipeline lines;
    OwnedPipeline ui;
    OwnedPipeline ui_premultiplied;
    OwnedPipeline shadow;   // Only with shadows.
    OwnedPipeline tone_map; // Only with HDR.
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
        m_flat_normal.reset();
        m_hdr_color.reset();
        m_hdr_depth.reset();
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
        description += "; materials are metallic-roughness PBR with normal maps";
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

    // Draws the parts, or the shadow casters among them, that `view` sees, when one is given,
    // with one instanced draw for the parts that share a range of indices and a texture.
    void draw_meshes(rhi::CommandList& commands, std::span<const MeshDraw> meshes, bool casters,
                     const core::Frustum* view, FrameStats& stats);
    void draw_lines(rhi::CommandList& commands, std::span<const LineVertex> lines,
                    FrameStats& stats);
    void draw_ui(rhi::CommandList& commands, const Frame& frame, FrameStats& stats);

    core::Config m_config;
    rhi::Device* m_device = nullptr;
    Settings m_settings;
    std::string m_description;
    std::unique_ptr<Pipelines> m_pipelines;
    OwnedTexture m_shadow_map;
    // One texel pointing straight out of the surface, for parts without a normal map.
    OwnedTexture m_flat_normal;
    OwnedTexture m_hdr_color;
    OwnedTexture m_hdr_depth;
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
    bool m_warned_instances = false;
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
    core::Result<OwnedPipeline> forward = pipeline(rhi::PipelineDesc{
        .vertex_shader = vertex.value(),
        .fragment_shader = fragment.value(),
        .vertex_stride = sizeof(MeshVertex),
        .attributes = k_attributes,
        .instance_stride = sizeof(InstanceData),
        .instance_attributes = k_instance_attributes,
        .color_target = hdr,
        .depth_target = hdr.has_value() ? std::optional(k_depth_format) : std::nullopt,
    });
    if (!forward.has_value()) {
        return forward.error();
    }
    made.forward = std::move(forward).value();

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
    }
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
            .normals = {normals.columns[0], normals.columns[1], normals.columns[2]},
            .base_color = mesh.material.base_color,
            .material = Vec4{mesh.material.metallic, mesh.material.roughness,
                             mesh.material.normal_scale, mesh.material.occlusion_strength},
            .emissive = extend(mesh.material.emissive, 0.0f),
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

void ForwardRenderer::draw_meshes(rhi::CommandList& commands, std::span<const MeshDraw> meshes,
                                  bool casters, const core::Frustum* view, FrameStats& stats) {
    m_order.clear();
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        if (casters && !meshes[index].casts_shadows) {
            continue;
        }
        if (view != nullptr && !core::intersects(*view, m_world_bounds[index])) {
            ++(casters ? stats.shadow_culled : stats.culled);
            continue;
        }
        m_order.push_back(static_cast<std::uint32_t>(index));
    }

    // Parts that draw the same indices with the same textures become one draw, whatever their
    // material's factors. Shadows ignore textures.
    const auto key = [&](std::uint32_t index) {
        const MeshDraw& mesh = meshes[index];
        const Chosen& chosen = m_chosen[index];
        const MeshMaterial& material = mesh.material;
        const auto texture = [&](rhi::TextureHandle handle) {
            return casters ? rhi::TextureHandle{} : handle;
        };
        return std::make_tuple(mesh.vertices, mesh.indices, chosen.first_index, chosen.index_count,
                               texture(material.base_color_texture),
                               texture(material.normal_texture),
                               texture(material.metallic_roughness_texture),
                               texture(material.occlusion_texture),
                               texture(material.emissive_texture));
    };
    std::ranges::stable_sort(m_order, [&](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });

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

        if (mesh.vertices != bound_vertices) {
            commands.bind_vertex_buffer(mesh.vertices);
            bound_vertices = mesh.vertices;
        }
        if (mesh.indices != bound_indices) {
            commands.bind_index_buffer(mesh.indices, rhi::IndexFormat::UInt32);
            bound_indices = mesh.indices;
        }
        if (!casters) {
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
        };
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
        prepare_meshes(frame, culling || m_settings.lod_error > 0.0f);
        if (culling) {
            camera_bounds = core::frustum_from(frame.view->view_projection);
        }
    }

    if (shadows) {
        // Each cascade into its own part of the map, kept there by a scissor.
        DREAM_PROFILE_ZONE("Shadow map");
        commands.begin_render_pass(rhi::RenderTargets{.depth = m_shadow_map.get()});
        commands.bind_pipeline(m_pipelines->shadow.get());
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
            commands.set_uniforms(std::as_bytes(std::span(&cascade_uniforms, 1)));
            draw_meshes(commands, frame.meshes, true, culling ? &cascade_bounds[cascade] : nullptr,
                        stats);
        }
        commands.end_render_pass();
    }

    const bool hdr = m_settings.hdr_format.has_value() && m_pipelines->tone_map.exists();
    if (hdr) {
        fit_hdr_targets(frame.size);
    }
    const auto draw_lit_scene = [&] {
        if (!uniforms.has_value()) {
            return;
        }
        commands.bind_pipeline(m_pipelines->forward.get());
        commands.set_uniforms(uniform_bytes());
        if (shadows) {
            commands.bind_texture(rhi::k_shadow_slot, m_shadow_map.get());
        }
        draw_meshes(commands, frame.meshes, false, camera_bounds ? &*camera_bounds : nullptr, stats);
    };
    if (hdr && m_hdr_color.exists()) {
        // The scene in HDR, then tone-mapped onto the window, with the lines and interface over it.
        commands.begin_render_pass(rhi::RenderTargets{.color = m_hdr_color.get(),
                                                      .depth = m_hdr_depth.get(),
                                                      .clear_color = frame.clear_color});
        draw_lit_scene();
        commands.end_render_pass();

        commands.begin_render_pass(rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
        {
            DREAM_PROFILE_ZONE("Tone mapping");
            const ToneMapConstants constants{.settings = Vec4{
                m_settings.exposure,
                m_settings.tone_mapping == ToneMapping::PbrNeutral ? 1.0f : 0.0f, 0.0f, 0.0f}};
            commands.bind_pipeline(m_pipelines->tone_map.get());
            commands.bind_texture(1, m_hdr_color.get());
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

    if (uniforms.has_value()) {
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
    return stats;
}

core::Result<std::unique_ptr<SceneRenderer>> create(const core::Config& config) {
    return std::unique_ptr<SceneRenderer>{std::make_unique<ForwardRenderer>(config)};
}

} // namespace

[[nodiscard]] core::Result<void> register_renderer_forward(core::ModuleRegistry& registry) {
    return registry.add(core::ModuleDescriptor<SceneRenderer>{.name = "forward", .create = &create});
}

} // namespace dream::modules
