#include "dream/runtime/renderer.h"

#include "dream/core/assert.h"
#include "dream/runtime/app.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
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
};

struct DrawConstants {
    Mat4 model;
    Vec4 normal_x;
    Vec4 normal_y;
    Vec4 normal_z;
    Vec4 base_color;
};

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

core::Result<GpuShader> load_shader(App& app, rhi::Device& device, std::string_view name,
                                    rhi::ShaderStage stage) {
    const auto code =
        app.files().read(std::format("assets:/shaders/{}{}", name, device.shader_file_suffix()));
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
    context.world.registry().emplace<MeshRenderer>(context.entity, std::move(model).value(),
                                                   extend(tint.value(), 1.0f));
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
    context.world.registry().emplace<DirectionalLight>(
        context.entity, direction.value(), color.value(), intensity.value(), ambient.value());
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
    if (!m_ui_pipeline.has_value() || size.width == 0 || size.height == 0) {
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
        commands.bind_pipeline(list.premultiplied_alpha ? m_ui_premultiplied_pipeline->get()
                                                        : m_ui_pipeline->get());
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

core::Result<void> Renderer::initialize(App& app) {
    rhi::Device* const device = app.device();
    if (device == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "The renderer needs a graphics module in the project configuration"};
    }

    core::Result<GpuShader> vertex =
        load_shader(app, *device, "forward.vert", rhi::ShaderStage::Vertex);
    if (!vertex.has_value()) {
        return vertex.error();
    }
    core::Result<GpuShader> fragment =
        load_shader(app, *device, "forward.frag", rhi::ShaderStage::Fragment);
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
    });
    if (!pipeline.has_value()) {
        return pipeline.error();
    }

    m_vertex_shader.emplace(std::move(vertex).value());
    m_fragment_shader.emplace(std::move(fragment).value());
    m_pipeline.emplace(*device, pipeline.value());

    core::Result<GpuShader> line_vertex =
        load_shader(app, *device, "debug_lines.vert", rhi::ShaderStage::Vertex);
    if (!line_vertex.has_value()) {
        return line_vertex.error();
    }
    core::Result<GpuShader> line_fragment =
        load_shader(app, *device, "debug_lines.frag", rhi::ShaderStage::Fragment);
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
        .cull_back_faces = false,
    });
    if (!line_pipeline.has_value()) {
        return line_pipeline.error();
    }
    m_line_vertex_shader.emplace(std::move(line_vertex).value());
    m_line_fragment_shader.emplace(std::move(line_fragment).value());
    m_line_pipeline.emplace(*device, line_pipeline.value());

    core::Result<GpuShader> ui_vertex =
        load_shader(app, *device, "ui.vert", rhi::ShaderStage::Vertex);
    if (!ui_vertex.has_value()) {
        return ui_vertex.error();
    }
    core::Result<GpuShader> ui_fragment =
        load_shader(app, *device, "ui.frag", rhi::ShaderStage::Fragment);
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
        .cull_back_faces = false,
    };
    const auto ui_pipeline = device->create_pipeline(ui_desc);
    if (!ui_pipeline.has_value()) {
        return ui_pipeline.error();
    }
    m_ui_pipeline.emplace(*device, ui_pipeline.value());
    ui_desc.blend = rhi::BlendMode::PremultipliedAlpha;
    const auto premultiplied_pipeline = device->create_pipeline(ui_desc);
    if (!premultiplied_pipeline.has_value()) {
        return premultiplied_pipeline.error();
    }
    m_ui_premultiplied_pipeline.emplace(*device, premultiplied_pipeline.value());
    m_ui_vertex_shader.emplace(std::move(ui_vertex).value());
    m_ui_fragment_shader.emplace(std::move(ui_fragment).value());
    m_ui_textures = std::make_unique<UiTextures>(app, *device);

    app.assets().register_loader<ModelAsset>(
        [device](AssetLoadContext& context) -> core::Result<ModelAsset> {
            const auto file = context.files.read(context.path);
            if (!file.has_value()) {
                return file.error();
            }
            const core::Result<ModelData> data = parse_glb(file.value());
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
    if (!m_line_pipeline.has_value() || m_debug_lines.empty()) {
        return;
    }
    commands.bind_pipeline(m_line_pipeline->get());

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

void Renderer::render(const World& world, rhi::CommandList& commands, platform::SurfaceSize size,
                      rhi::ClearColor clear_color) {
    m_last_draw_count = 0;
    m_last_line_count = 0;
    m_last_ui_command_count = 0;
    commands.begin_render_pass(clear_color);

    const Registry& registry = world.registry();
    const auto cameras = registry.view<const Camera, const WorldTransform>();
    const Entity camera_entity = cameras.front();

    if (camera_entity != k_no_entity && size.width > 0 && size.height > 0 && m_pipeline.has_value()) {
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

        FrameUniforms uniforms{
            .view_projection = projection * view,
            .camera_position = extend(eye, 1.0f),
            .direction_to_light = Vec4{0.0f, 1.0f, 0.0f, 0.0f},
            .light_color_and_ambient = Vec4{0.0f, 0.0f, 0.0f, 0.2f},
        };
        const auto lights = registry.view<const DirectionalLight>();
        if (const Entity light_entity = lights.front(); light_entity != k_no_entity) {
            const DirectionalLight& light = lights.get<const DirectionalLight>(light_entity);
            uniforms.direction_to_light = extend(core::normalized(-light.direction), 0.0f);
            uniforms.light_color_and_ambient = extend(light.color * light.intensity, light.ambient);
        }

        commands.bind_pipeline(m_pipeline->get());
        commands.set_uniforms(std::as_bytes(std::span(&uniforms, 1)));

        for (const auto [entity, transform, mesh] :
             registry.view<const WorldTransform, const MeshRenderer>().each()) {
            if (mesh.model == nullptr) {
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
                commands.bind_texture(material.texture);
                commands.push_constants(std::as_bytes(std::span(&constants, 1)));
                commands.draw_indexed_range(part.first_index, part.index_count);
                ++m_last_draw_count;
            }
        }

        draw_debug_lines(commands);
    }

    m_debug_lines.clear();
    draw_ui(commands, size);
    commands.end_render_pass();
}

} // namespace dream::runtime
