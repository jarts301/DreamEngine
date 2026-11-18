#include "dream/runtime/renderer.h"

#include "dream/core/assert.h"
#include "dream/core/log.h"
#include "dream/core/process_stats.h"
#include "dream/core/profile.h"
#include "dream/runtime/app.h"
#include "dream/runtime/texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dream::runtime {
namespace {

using core::Mat4;
using core::Vec3;
using core::Vec4;

// These two match the blocks of the same names in runtime/shaders/forward.slang.
struct FrameUniforms {
    Mat4 view_projection;
    Vec4 camera_position;
    Vec4 direction_to_light;
    Vec4 light_color_and_ambient;
    Mat4 light_view_projection;
    Vec4 shadow_settings;
};

struct DrawConstants {
    Mat4 model;
    Vec4 normal_x;
    Vec4 normal_y;
    Vec4 normal_z;
    Vec4 base_color;
};

// Matches ToneMapConstants in runtime/shaders/tonemap.slang.
struct ToneMapConstants {
    Vec4 settings;
};

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

// The scene's depth, beside the HDR color target.
constexpr rhi::TextureFormat k_hdr_depth_format = rhi::TextureFormat::Depth32Float;

core::Error setting_error(std::string_view message) {
    return core::Error{core::ErrorCode::InvalidArgument, std::string(message)};
}

// Matches UiConstants in runtime/shaders/ui.slang.
struct UiConstants {
    core::Vec2 scale;
    core::Vec2 offset;
    float premultiplied = 0.0f;
};

static_assert(sizeof(FrameUniforms) <= rhi::k_max_uniform_size);

static_assert(sizeof(DrawConstants) <= rhi::k_max_push_constant_size);

Vec4 multiply(Vec4 a, Vec4 b) {
    return Vec4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

Vec4 extend(Vec3 v, float w) {
    return Vec4{v.x, v.y, v.z, w};
}

core::Result<GpuShader> load_shader(VirtualFileSystem& files, rhi::Device& device,
                                    std::string_view name, rhi::ShaderStage stage) {
    const auto code =
        files.read(std::format("assets:/shaders/{}{}", name, device.shader_file_suffix()));
    if (!code.has_value()) {
        return core::Error{code.error().code,
                           std::format("{} (does the game use the RENDERER option?)",
                                       code.error().message)};
    }
    const auto shader = device.create_shader(stage, code.value());
    if (!shader.has_value()) {
        return shader.error();
    }
    return GpuShader(device, shader.value());
}

core::Result<void> read_mesh(ComponentContext& context) {
    const core::Result<std::string> path = context.data.text("model");
    if (!path.has_value()) {
        return path.error();
    }
    const core::Result<Vec3> tint = context.data.vec3("tint", Vec3{1.0f, 1.0f, 1.0f});
    if (!tint.has_value()) {
        return tint.error();
    }
    core::Result<AssetRef<ModelAsset>> model = context.assets.load<ModelAsset>(path.value());
    if (!model.has_value()) {
        return model.error();
    }
    const core::Result<bool> casts_shadows = context.data.flag("casts_shadows", true);
    if (!casts_shadows.has_value()) {
        return casts_shadows.error();
    }
    context.world.registry().emplace<MeshRenderer>(context.entity, std::move(model).value(),
                                                   extend(tint.value(), 1.0f),
                                                   casts_shadows.value());
    return {};
}

core::Result<void> read_camera(ComponentContext& context) {
    Camera camera;
    const core::Result<float> fov = context.data.number("vertical_fov", camera.vertical_fov_degrees);
    if (!fov.has_value()) {
        return fov.error();
    }
    camera.vertical_fov_degrees = fov.value();

    if (context.data.has("target")) {
        const core::Result<Vec3> target = context.data.vec3("target", Vec3{});
        if (!target.has_value()) {
            return target.error();
        }
        camera.target = target.value();
    }
    context.world.registry().emplace<Camera>(context.entity, camera);
    return {};
}

core::Result<void> read_directional_light(ComponentContext& context) {
    DirectionalLight light;
    const core::Result<Vec3> direction = context.data.vec3("direction", light.direction);
    if (!direction.has_value()) {
        return direction.error();
    }
    const core::Result<Vec3> color = context.data.vec3("color", light.color);
    if (!color.has_value()) {
        return color.error();
    }
    const core::Result<float> intensity = context.data.number("intensity", light.intensity);
    if (!intensity.has_value()) {
        return intensity.error();
    }
    const core::Result<float> ambient = context.data.number("ambient", light.ambient);
    if (!ambient.has_value()) {
        return ambient.error();
    }
    const core::Result<bool> casts_shadows = context.data.flag("casts_shadows", true);
    if (!casts_shadows.has_value()) {
        return casts_shadows.error();
    }
    context.world.registry().emplace<DirectionalLight>(context.entity, direction.value(),
                                                       color.value(), intensity.value(),
                                                       ambient.value(), casts_shadows.value());
    return {};
}

} // namespace

// Owns the textures interfaces create, by the ids they were given.
class Renderer::UiTextures final : public ui::Host {
public:
    UiTextures(App& app, rhi::Device& device) : m_app(app), m_device(device) {}

    core::Result<std::vector<std::byte>> read_file(std::string_view path) override {
        return m_app.files().read(path);
    }

    core::Result<ui::TextureId> create_texture(std::uint32_t width, std::uint32_t height,
                                               std::span<const std::byte> rgba,
                                               ui::TextureKind kind) override {
        const core::Result<rhi::TextureHandle> texture = m_device.create_texture(
            rhi::TextureDesc{
                .width = width,
                .height = height,
                .format = kind == ui::TextureKind::Color ? rhi::TextureFormat::Rgba8Srgb
                                                         : rhi::TextureFormat::Rgba8Unorm,
                .generate_mipmaps = false,
            },
            rgba);
        if (!texture.has_value()) {
            return texture.error();
        }
        const ui::TextureId id = ++m_next_id;
        m_textures.emplace(id, GpuTexture(m_device, texture.value()));
        return id;
    }

    core::Result<ui::LoadedTexture> load_texture(std::string_view path,
                                                 bool premultiply_alpha) override {
        const auto file = m_app.files().read(path);
        if (!file.has_value()) {
            return file.error();
        }
        core::Result<ImageData> image = decode_image(file.value());
        if (!image.has_value()) {
            return core::Error{image.error().code,
                               std::format("'{}': {}", path, image.error().message)};
        }
        ImageData& data = image.value();
        if (premultiply_alpha) {
            for (std::size_t i = 0; i + 3 < data.pixels.size(); i += 4) {
                const auto alpha = static_cast<unsigned>(data.pixels[i + 3]);
                for (std::size_t c = 0; c < 3; ++c) {
                    data.pixels[i + c] = static_cast<std::byte>(
                        (static_cast<unsigned>(data.pixels[i + c]) * alpha + 127) / 255);
                }
            }
        }
        const core::Result<ui::TextureId> id =
            create_texture(data.width, data.height, data.pixels, ui::TextureKind::Color);
        if (!id.has_value()) {
            return id.error();
        }
        return ui::LoadedTexture{id.value(), data.width, data.height};
    }

    void release_texture(ui::TextureId texture) override {
        if (m_textures.contains(texture)) {
            m_released.push_back(texture);
        }
    }

    void release_pending() {
        for (const ui::TextureId texture : m_released) {
            m_textures.erase(texture);
        }
        m_released.clear();
    }

    [[nodiscard]] rhi::TextureHandle find(ui::TextureId texture) const {
        const auto found = m_textures.find(texture);
        return found == m_textures.end() ? rhi::TextureHandle{} : found->second.get();
    }

    [[nodiscard]] std::size_t count() const noexcept {
        return m_textures.size();
    }

private:
    App& m_app;
    rhi::Device& m_device;
    std::unordered_map<ui::TextureId, GpuTexture> m_textures;
    std::vector<ui::TextureId> m_released;
    ui::TextureId m_next_id = 0;
};

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

void Renderer::set_ui_layer(UiLayer layer, const ui::DrawList& list, core::Vec2 offset) {
    if (layer < UiLayer::Count) {
        m_ui_offsets[static_cast<std::size_t>(layer)] = offset;
        ui::DrawList& target = m_ui_layers[static_cast<std::size_t>(layer)];
        target.vertices.assign(list.vertices.begin(), list.vertices.end());
        target.indices.assign(list.indices.begin(), list.indices.end());
        target.commands.assign(list.commands.begin(), list.commands.end());
        target.premultiplied_alpha = list.premultiplied_alpha;
    }
}

void Renderer::clear_ui_layer(UiLayer layer) {
    if (layer < UiLayer::Count) {
        m_ui_layers[static_cast<std::size_t>(layer)].clear();
    }
}

ui::Host& Renderer::ui_host() noexcept {
    DREAM_ASSERT(m_ui_textures != nullptr, "The renderer isn't initialized");
    return *m_ui_textures;
}

void Renderer::release_ui_textures() {
    if (m_ui_textures != nullptr) {
        m_ui_textures->release_pending();
    }
}

void Renderer::draw_ui(rhi::CommandList& commands, platform::SurfaceSize size) {
    if (m_pipelines == nullptr || size.width == 0 || size.height == 0) {
        return;
    }
    const float width = static_cast<float>(size.width);
    const float height = static_cast<float>(size.height);

    for (std::size_t layer = 0; layer < m_ui_layers.size(); ++layer) {
        const ui::DrawList& list = m_ui_layers[layer];
        const core::Vec2 offset = m_ui_offsets[layer];
        if (list.commands.empty() || list.vertices.empty()) {
            continue;
        }
        commands.bind_pipeline(list.premultiplied_alpha
                                   ? m_pipelines->ui_premultiplied_pipeline.get()
                                   : m_pipelines->ui_pipeline.get());
        if (!commands.bind_frame_geometry(std::as_bytes(std::span(list.vertices)),
                                          list.indices)) {
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
            commands.bind_texture(m_ui_textures->find(command.texture));
            commands.push_constants(std::as_bytes(std::span(&constants, 1)));
            commands.draw_indexed_range(command.first_index, command.index_count);
            ++m_last_ui_command_count;
        }
        commands.set_scissor(std::nullopt);
    }
}

float fitted_vertical_fov(float vertical_fov_radians, float aspect) noexcept {
    if (aspect >= 1.0f || aspect <= 0.0f) {
        return vertical_fov_radians;
    }
    return 2.0f * std::atan(std::tan(vertical_fov_radians * 0.5f) / aspect);
}

std::optional<CameraView> camera_view(const World& world, platform::SurfaceSize size) {
    const Registry& registry = world.registry();
    const auto cameras = registry.view<const Camera, const WorldTransform>();
    const Entity camera_entity = cameras.front();
    if (camera_entity == k_no_entity || size.width == 0 || size.height == 0) {
        return std::nullopt;
    }
    const Camera& camera = cameras.get<const Camera>(camera_entity);
    const Mat4& camera_world = cameras.get<const WorldTransform>(camera_entity).matrix;
    const Vec3 eye = core::transform_point(camera_world, Vec3{});

    Mat4 view;
    if (camera.target.has_value()) {
        view = core::look_at(eye, *camera.target, Vec3{0.0f, 1.0f, 0.0f});
    } else if (const std::optional<Mat4> inverted = core::inverse(camera_world)) {
        view = *inverted;
    }

    const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
    const float fov = fitted_vertical_fov(core::radians(camera.vertical_fov_degrees), aspect);
    const Mat4 projection = core::perspective(fov, aspect, camera.z_near, camera.z_far);
    return CameraView{.view_projection = projection * view,
                      .eye = eye,
                      .z_near = camera.z_near,
                      .z_far = camera.z_far};
}

std::optional<CameraRay> camera_ray(const World& world, platform::SurfaceSize size,
                                    core::Vec2 pixel) {
    const std::optional<CameraView> camera = camera_view(world, size);
    if (!camera.has_value()) {
        return std::nullopt;
    }
    const std::optional<Mat4> clip_to_world = core::inverse(camera->view_projection);
    if (!clip_to_world.has_value()) {
        return std::nullopt;
    }
    // Two points under the pixel, at the near plane and halfway into the depth range, which differ
    // only in distance from the eye.
    const float x = 2.0f * pixel.x / static_cast<float>(size.width) - 1.0f;
    const float y = 1.0f - 2.0f * pixel.y / static_cast<float>(size.height);
    const auto unproject = [&](float depth) {
        const Vec4 point = *clip_to_world * Vec4{x, y, depth, 1.0f};
        return Vec3{point.x, point.y, point.z} * (1.0f / point.w);
    };
    const Vec3 direction = core::normalized(unproject(0.5f) - unproject(0.0f));
    return CameraRay{.origin = camera->eye, .direction = direction};
}

core::Result<RendererSettings> RendererSettings::from_config(const core::Config& config,
                                                             const rhi::Device& device) {
    RendererSettings settings;

    const std::string_view hdr = config.get_string("renderer.hdr").value_or("auto");
    const auto usable = [&](rhi::TextureFormat format) {
        return device.supports_render_target(format) &&
               device.supports_render_target(k_hdr_depth_format);
    };
    if (hdr == "auto") {
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
    constexpr bool k_phone = true;
#else
    constexpr std::uint32_t k_default_shadow_resolution = 2048;
    constexpr bool k_phone = false;
#endif
    const bool depth_targets = device.supports_render_target(k_hdr_depth_format);
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
    return settings;
}

// The renderer's shaders and pipelines, made from the compiled shader files. The asset manager
// loads them like any asset, so hot reloading rebuilds them all when one of the files changes.
static core::Result<RendererPipelines> create_pipelines(VirtualFileSystem& files,
                                                     rhi::Device& device_ref,
                                                     const RendererSettings& settings) {
    rhi::Device* const device = &device_ref;
    const std::optional<rhi::TextureFormat> hdr = settings.hdr_format;

    core::Result<GpuShader> vertex =
        load_shader(files, *device, "forward.vert", rhi::ShaderStage::Vertex);
    if (!vertex.has_value()) {
        return vertex.error();
    }
    core::Result<GpuShader> fragment =
        load_shader(files, *device, "forward.frag", rhi::ShaderStage::Fragment);
    if (!fragment.has_value()) {
        return fragment.error();
    }

    constexpr std::array k_attributes{
        rhi::VertexAttribute{0, rhi::VertexFormat::Float3, offsetof(ModelVertex, position)},
        rhi::VertexAttribute{1, rhi::VertexFormat::Float3, offsetof(ModelVertex, normal)},
        rhi::VertexAttribute{2, rhi::VertexFormat::Float2, offsetof(ModelVertex, uv)},
    };
    const auto pipeline = device->create_pipeline(rhi::PipelineDesc{
        .vertex_shader = vertex.value().get(),
        .fragment_shader = fragment.value().get(),
        .vertex_stride = sizeof(ModelVertex),
        .attributes = k_attributes,
        .color_target = hdr,
        .depth_target = hdr.has_value() ? std::optional(k_hdr_depth_format) : std::nullopt,
    });
    if (!pipeline.has_value()) {
        return pipeline.error();
    }

    GpuPipeline forward_pipeline(*device, pipeline.value());

    core::Result<GpuShader> line_vertex =
        load_shader(files, *device, "debug_lines.vert", rhi::ShaderStage::Vertex);
    if (!line_vertex.has_value()) {
        return line_vertex.error();
    }
    core::Result<GpuShader> line_fragment =
        load_shader(files, *device, "debug_lines.frag", rhi::ShaderStage::Fragment);
    if (!line_fragment.has_value()) {
        return line_fragment.error();
    }
    constexpr std::array k_line_attributes{
        rhi::VertexAttribute{0, rhi::VertexFormat::Float3, offsetof(DebugVertex, position)},
        rhi::VertexAttribute{1, rhi::VertexFormat::Float4, offsetof(DebugVertex, color)},
    };
    const auto line_pipeline = device->create_pipeline(rhi::PipelineDesc{
        .vertex_shader = line_vertex.value().get(),
        .fragment_shader = line_fragment.value().get(),
        .vertex_stride = sizeof(DebugVertex),
        .attributes = k_line_attributes,
        .topology = rhi::PrimitiveTopology::Lines,
        .depth_test = false,
        .cull = rhi::CullMode::None,
    });
    if (!line_pipeline.has_value()) {
        return line_pipeline.error();
    }
    GpuPipeline lines(*device, line_pipeline.value());

    core::Result<GpuShader> ui_vertex =
        load_shader(files, *device, "ui.vert", rhi::ShaderStage::Vertex);
    if (!ui_vertex.has_value()) {
        return ui_vertex.error();
    }
    core::Result<GpuShader> ui_fragment =
        load_shader(files, *device, "ui.frag", rhi::ShaderStage::Fragment);
    if (!ui_fragment.has_value()) {
        return ui_fragment.error();
    }
    constexpr std::array k_ui_attributes{
        rhi::VertexAttribute{0, rhi::VertexFormat::Float2, offsetof(ui::Vertex, position)},
        rhi::VertexAttribute{1, rhi::VertexFormat::Float2, offsetof(ui::Vertex, uv)},
        rhi::VertexAttribute{2, rhi::VertexFormat::UByte4Norm, offsetof(ui::Vertex, color)},
    };
    rhi::PipelineDesc ui_desc{
        .vertex_shader = ui_vertex.value().get(),
        .fragment_shader = ui_fragment.value().get(),
        .vertex_stride = sizeof(ui::Vertex),
        .attributes = k_ui_attributes,
        .blend = rhi::BlendMode::Alpha,
        .depth_test = false,
        .cull = rhi::CullMode::None,
    };
    const auto ui_pipeline = device->create_pipeline(ui_desc);
    if (!ui_pipeline.has_value()) {
        return ui_pipeline.error();
    }
    GpuPipeline ui(*device, ui_pipeline.value());
    ui_desc.blend = rhi::BlendMode::PremultipliedAlpha;
    const auto premultiplied_pipeline = device->create_pipeline(ui_desc);
    if (!premultiplied_pipeline.has_value()) {
        return premultiplied_pipeline.error();
    }
    GpuPipeline ui_premultiplied(*device, premultiplied_pipeline.value());

    std::optional<GpuShader> shadow_vertex;
    std::optional<GpuPipeline> shadow;
    if (settings.shadow_resolution > 0) {
        core::Result<GpuShader> loaded =
            load_shader(files, *device, "shadow.vert", rhi::ShaderStage::Vertex);
        if (!loaded.has_value()) {
            return loaded.error();
        }
        // Depth only. Both faces, since a model's faces needn't make a closed shape, with a bias
        // that grows with the slope, against surfaces shadowing themselves.
        const auto shadow_pipeline = device->create_pipeline(rhi::PipelineDesc{
            .vertex_shader = loaded.value().get(),
            .vertex_stride = sizeof(ModelVertex),
            .attributes = std::span(k_attributes).first(1),
            .cull = rhi::CullMode::None,
            .depth_bias = 1.0f,
            .slope_depth_bias = 1.5f,
            .depth_target = k_hdr_depth_format,
        });
        if (!shadow_pipeline.has_value()) {
            return shadow_pipeline.error();
        }
        shadow_vertex.emplace(std::move(loaded).value());
        shadow.emplace(*device, shadow_pipeline.value());
    }

    std::optional<GpuShader> tone_map_vertex;
    std::optional<GpuShader> tone_map_fragment;
    std::optional<GpuPipeline> tone_map;
    if (hdr.has_value()) {
        core::Result<GpuShader> loaded_vertex =
            load_shader(files, *device, "tonemap.vert", rhi::ShaderStage::Vertex);
        if (!loaded_vertex.has_value()) {
            return loaded_vertex.error();
        }
        core::Result<GpuShader> loaded_fragment =
            load_shader(files, *device, "tonemap.frag", rhi::ShaderStage::Fragment);
        if (!loaded_fragment.has_value()) {
            return loaded_fragment.error();
        }
        constexpr std::array k_screen_attributes{
            rhi::VertexAttribute{0, rhi::VertexFormat::Float2, offsetof(ScreenVertex, position)},
            rhi::VertexAttribute{1, rhi::VertexFormat::Float2, offsetof(ScreenVertex, uv)},
        };
        const auto tone_map_pipeline = device->create_pipeline(rhi::PipelineDesc{
            .vertex_shader = loaded_vertex.value().get(),
            .fragment_shader = loaded_fragment.value().get(),
            .vertex_stride = sizeof(ScreenVertex),
            .attributes = k_screen_attributes,
            .depth_test = false,
            .cull = rhi::CullMode::None,
        });
        if (!tone_map_pipeline.has_value()) {
            return tone_map_pipeline.error();
        }
        tone_map_vertex.emplace(std::move(loaded_vertex).value());
        tone_map_fragment.emplace(std::move(loaded_fragment).value());
        tone_map.emplace(*device, tone_map_pipeline.value());
    }
    return RendererPipelines{
        .vertex_shader = std::move(vertex).value(),
        .fragment_shader = std::move(fragment).value(),
        .pipeline = std::move(forward_pipeline),
        .line_vertex_shader = std::move(line_vertex).value(),
        .line_fragment_shader = std::move(line_fragment).value(),
        .line_pipeline = std::move(lines),
        .ui_vertex_shader = std::move(ui_vertex).value(),
        .ui_fragment_shader = std::move(ui_fragment).value(),
        .ui_pipeline = std::move(ui),
        .ui_premultiplied_pipeline = std::move(ui_premultiplied),
        .shadow_vertex_shader = std::move(shadow_vertex),
        .shadow_pipeline = std::move(shadow),
        .tone_map_vertex_shader = std::move(tone_map_vertex),
        .tone_map_fragment_shader = std::move(tone_map_fragment),
        .tone_map_pipeline = std::move(tone_map),
    };
}

core::Result<void> Renderer::initialize(App& app) {
    rhi::Device* const device = app.device();
    if (device == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "The renderer needs a graphics module in the project configuration"};
    }

    core::Result<RendererSettings> settings = RendererSettings::from_config(app.config(), *device);
    if (!settings.has_value()) {
        return settings.error();
    }
    m_settings = settings.value();
    m_device = device;
    if (m_settings.hdr_format.has_value()) {
        DREAM_LOG_INFO("The scene is drawn in HDR, into {}, with {} tone mapping",
                       texture_format_name(*m_settings.hdr_format),
                       m_settings.tone_mapping == ToneMapping::PbrNeutral ? "PBR Neutral" : "no");
    } else {
        DREAM_LOG_INFO("The scene is drawn straight to the window, without HDR");
    }
    if (m_settings.shadow_resolution > 0) {
        const core::Result<rhi::TextureHandle> shadow_map = device->create_texture(
            rhi::TextureDesc{.width = m_settings.shadow_resolution,
                             .height = m_settings.shadow_resolution,
                             .format = k_hdr_depth_format,
                             .render_target = true},
            {});
        if (!shadow_map.has_value()) {
            DREAM_LOG_WARNING("Could not make the shadow map, so there are no shadows: {}",
                              shadow_map.error().message);
            m_settings.shadow_resolution = 0;
        } else {
            m_shadow_map.emplace(*device, shadow_map.value());
            DREAM_LOG_INFO("Shadows: a {0}x{0} map, reaching {1} from the camera",
                           m_settings.shadow_resolution, m_settings.shadow_distance);
        }
    } else {
        DREAM_LOG_INFO("Shadows are off");
    }

    app.assets().register_loader<RendererPipelines>(
        [device, chosen = m_settings](AssetLoadContext& context) {
            return create_pipelines(context.files, *device, chosen);
        });
    core::Result<AssetRef<RendererPipelines>> pipelines =
        app.assets().load<RendererPipelines>("assets:/shaders/renderer");
    if (!pipelines.has_value()) {
        return pipelines.error();
    }
    m_pipelines = std::move(pipelines).value();
    m_ui_textures = std::make_unique<UiTextures>(app, *device);
    DREAM_LOG_INFO("Cooked textures load as {}",
                   texture_format_name(cooked_color_format(*device)));

    app.assets().register_loader<ModelAsset>(
        [device](AssetLoadContext& context) -> core::Result<ModelAsset> {
            const auto file = context.files.read(context.path);
            if (!file.has_value()) {
                return file.error();
            }
            // A model's texture files are beside it, and a cooked one is used in place of its
            // source when the build made one.
            const std::string_view model_path = context.path;
            const std::string_view folder = model_path.substr(0, model_path.rfind('/') + 1);
            const ModelFileReader read_file = [&context, folder](std::string_view relative)
                -> core::Result<std::vector<std::byte>> {
                const std::string path = std::string(folder) + std::string(relative);
                if (!relative.ends_with(".ktx2")) {
                    if (auto cooked = context.files.read(cooked_texture_path(path));
                        cooked.has_value()) {
                        return cooked;
                    }
                }
                return context.files.read(path);
            };
            const core::Result<ModelData> data = parse_glb(file.value(), read_file);
            if (!data.has_value()) {
                return data.error();
            }
            return create_model_asset(*device, data.value());
        });

    app.scenes().register_component("mesh", &read_mesh);
    app.scenes().register_component("camera", &read_camera);
    app.scenes().register_component("directional_light", &read_directional_light);
    return {};
}

void Renderer::draw_debug_lines(rhi::CommandList& commands) {
    if (m_pipelines == nullptr || m_debug_lines.empty()) {
        return;
    }
    commands.bind_pipeline(m_pipelines->line_pipeline.get());

    // Drawn in pieces that fit the frame's vertex memory, until it is used up.
    constexpr std::size_t k_vertices_per_draw = 8192;
    const std::span<const DebugVertex> vertices = m_debug_lines.vertices();
    for (std::size_t first = 0; first < vertices.size(); first += k_vertices_per_draw) {
        const std::span<const DebugVertex> piece =
            vertices.subspan(first, std::min(k_vertices_per_draw, vertices.size() - first));
        if (!commands.draw_vertices(std::as_bytes(piece), static_cast<std::uint32_t>(piece.size()))) {
            break;
        }
        m_last_line_count += static_cast<std::uint32_t>(piece.size() / 2);
    }
}

void Renderer::fit_hdr_targets(platform::SurfaceSize size) {
    if (size == m_hdr_size && m_hdr_color.has_value()) {
        return;
    }
    // Made between passes, before anything this frame reads the old ones.
    m_hdr_color.reset();
    m_hdr_depth.reset();
    m_hdr_size = {};
    if (size.width == 0 || size.height == 0 || m_device == nullptr) {
        return;
    }
    const auto make = [&](rhi::TextureFormat format) -> std::optional<GpuTexture> {
        const core::Result<rhi::TextureHandle> texture = m_device->create_texture(
            rhi::TextureDesc{.width = size.width, .height = size.height, .format = format,
                             .render_target = true},
            {});
        if (!texture.has_value()) {
            DREAM_LOG_ERROR("Could not make the renderer's HDR target: {}",
                            texture.error().message);
            return std::nullopt;
        }
        return GpuTexture(*m_device, texture.value());
    };
    if (std::optional<GpuTexture> color = make(*m_settings.hdr_format)) {
        m_hdr_color.emplace(std::move(*color));
    }
    if (std::optional<GpuTexture> depth = make(k_hdr_depth_format)) {
        m_hdr_depth.emplace(std::move(*depth));
    }
    if (!m_hdr_color.has_value() || !m_hdr_depth.has_value()) {
        m_hdr_color.reset();
        m_hdr_depth.reset();
        return;
    }
    m_hdr_size = size;
}

namespace {

// The sun's view for the shadow map: an orthographic box around the part of the camera's view
// within `distance`, reaching back toward the sun by as far again, so casters behind the camera
// still shadow what it sees. The box's size depends only on that part of the view, not on which
// way the camera turns, and it moves in whole texels of the map, so shadow edges hold still.
Mat4 fit_shadow_view(const CameraView& camera, Vec3 light_direction, float distance,
                     std::uint32_t resolution) {
    const std::optional<Mat4> clip_to_world = core::inverse(camera.view_projection);
    if (!clip_to_world.has_value()) {
        return Mat4{};
    }
    // The depth, in clip space, at `distance` in front of the camera, as perspective() maps it.
    const float near = camera.z_near;
    const float far = camera.z_far;
    const float reach = std::clamp(distance, near * 2.0f, far);
    const float far_depth = far * (reach - near) / (reach * (far - near));

    std::array<Vec3, 8> corners{};
    std::size_t corner = 0;
    for (const float depth : {0.0f, far_depth}) {
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
    const float back = radius + distance;
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

} // namespace

void Renderer::draw_meshes(const World& world, rhi::CommandList& commands, bool shadow_casters) {
    for (const auto [entity, transform, mesh] :
         world.registry().view<const WorldTransform, const MeshRenderer>().each()) {
        if (mesh.model == nullptr || (shadow_casters && !mesh.casts_shadows)) {
            continue;
        }
        const ModelAsset& model = *mesh.model;

        const std::optional<Mat4> inverted = core::inverse(transform.matrix);
        const Mat4 normals = inverted.has_value() ? core::transpose(*inverted) : transform.matrix;

        commands.bind_vertex_buffer(model.vertices.get());
        commands.bind_index_buffer(model.indices.get(), rhi::IndexFormat::UInt32);

        for (const ModelPart& part : model.parts) {
            const ModelMaterial& material = model.materials[part.material];
            const DrawConstants constants{
                .model = transform.matrix,
                .normal_x = normals.columns[0],
                .normal_y = normals.columns[1],
                .normal_z = normals.columns[2],
                .base_color = multiply(material.base_color, mesh.tint),
            };
            if (!shadow_casters) {
                commands.bind_texture(material.texture);
            }
            commands.push_constants(std::as_bytes(std::span(&constants, 1)));
            commands.draw_indexed_range(part.first_index, part.index_count);
            if (shadow_casters) {
                ++m_last_shadow_caster_count;
            } else {
                ++m_last_draw_count;
            }
        }
    }
}

void Renderer::render(const World& world, rhi::CommandList& commands, platform::SurfaceSize size,
                      rhi::ClearColor clear_color) {
    DREAM_PROFILE_ZONE("Draw scene");
    m_last_draw_count = 0;
    m_last_line_count = 0;
    m_last_ui_command_count = 0;
    m_last_shadow_caster_count = 0;

    // The frame's uniforms, which every pass reads, or nothing without a camera.
    std::optional<FrameUniforms> uniforms;
    bool shadows = false;
    if (const std::optional<CameraView> camera = camera_view(world, size);
        camera.has_value() && m_pipelines != nullptr) {
        FrameUniforms frame{
            .view_projection = camera->view_projection,
            .camera_position = extend(camera->eye, 1.0f),
            .direction_to_light = Vec4{0.0f, 1.0f, 0.0f, 0.0f},
            .light_color_and_ambient = Vec4{0.0f, 0.0f, 0.0f, 0.2f},
            .light_view_projection = Mat4{},
            .shadow_settings = Vec4{},
        };
        const auto lights = world.registry().view<const DirectionalLight>();
        if (const Entity light_entity = lights.front(); light_entity != k_no_entity) {
            const DirectionalLight& light = lights.get<const DirectionalLight>(light_entity);
            frame.direction_to_light = extend(core::normalized(-light.direction), 0.0f);
            frame.light_color_and_ambient = extend(light.color * light.intensity, light.ambient);
            shadows = light.casts_shadows && m_shadow_map.has_value() &&
                      m_pipelines->shadow_pipeline.has_value();
            if (shadows) {
                const std::uint32_t resolution = m_settings.shadow_resolution;
                frame.light_view_projection = fit_shadow_view(
                    *camera, light.direction, m_settings.shadow_distance, resolution);
                // The world's size of one texel sets how far a surface moves along its normal.
                const float texel = 1.0f / static_cast<float>(resolution);
                const Vec4 x_axis = frame.light_view_projection.columns[0];
                const float world_texel =
                    2.0f / (static_cast<float>(resolution) *
                            std::max(core::length(Vec3{x_axis.x, x_axis.y, x_axis.z}), 1e-6f));
                frame.shadow_settings = Vec4{1.0f, texel, 0.0005f, world_texel * 1.5f};
            }
        }
        uniforms = frame;
    }
    const auto uniform_bytes = [&] { return std::as_bytes(std::span(&*uniforms, 1)); };

    if (shadows) {
        DREAM_PROFILE_ZONE("Shadow map");
        commands.begin_render_pass(rhi::RenderTargets{.depth = m_shadow_map->get()});
        commands.bind_pipeline(m_pipelines->shadow_pipeline->get());
        commands.set_uniforms(uniform_bytes());
        draw_meshes(world, commands, true);
        commands.end_render_pass();
    }

    const bool hdr = m_settings.hdr_format.has_value() && m_pipelines != nullptr &&
                     m_pipelines->tone_map_pipeline.has_value();
    if (hdr) {
        fit_hdr_targets(size);
    }
    const auto draw_lit_scene = [&] {
        if (!uniforms.has_value()) {
            return;
        }
        commands.bind_pipeline(m_pipelines->pipeline.get());
        commands.set_uniforms(uniform_bytes());
        if (shadows) {
            commands.bind_texture(rhi::k_shadow_slot, m_shadow_map->get());
        }
        draw_meshes(world, commands, false);
    };
    if (hdr && m_hdr_color.has_value()) {
        // The scene in HDR, then tone-mapped onto the window, with the lines and interface over it.
        commands.begin_render_pass(rhi::RenderTargets{.color = m_hdr_color->get(),
                                                      .depth = m_hdr_depth->get(),
                                                      .clear_color = clear_color});
        draw_lit_scene();
        commands.end_render_pass();

        commands.begin_render_pass(rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
        {
            DREAM_PROFILE_ZONE("Tone mapping");
            const ToneMapConstants constants{.settings = Vec4{
                m_settings.exposure,
                m_settings.tone_mapping == ToneMapping::PbrNeutral ? 1.0f : 0.0f, 0.0f, 0.0f}};
            commands.bind_pipeline(m_pipelines->tone_map_pipeline->get());
            commands.bind_texture(1, m_hdr_color->get());
            commands.push_constants(std::as_bytes(std::span(&constants, 1)));
            commands.draw_vertices(std::as_bytes(std::span(k_screen_triangle)),
                                   static_cast<std::uint32_t>(k_screen_triangle.size()));
        }
        // A pass starts without the scene's uniforms on some backends, and the lines read them.
        if (uniforms.has_value()) {
            commands.set_uniforms(uniform_bytes());
        }
    } else {
        commands.begin_render_pass(clear_color);
        draw_lit_scene();
    }

    if (uniforms.has_value()) {
        draw_debug_lines(commands);
    }
    m_debug_lines.clear();
    draw_ui(commands, size);
    commands.end_render_pass();
    DREAM_PROFILE_VALUE("Draw calls", m_last_draw_count);
    DREAM_PROFILE_VALUE("Shadow casters", m_last_shadow_caster_count);
    DREAM_PROFILE_VALUE("Debug lines", m_last_line_count);
    DREAM_PROFILE_VALUE("UI draw commands", m_last_ui_command_count);
}

} // namespace dream::runtime
