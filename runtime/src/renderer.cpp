#include "dream/runtime/renderer.h"

#include "dream/runtime/app.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <utility>

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

void Renderer::render(const World& world, rhi::CommandList& commands, platform::SurfaceSize size,
                      rhi::ClearColor clear_color) {
    m_last_draw_count = 0;
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
    }

    commands.end_render_pass();
}

} // namespace dream::runtime
