#include "dream/runtime/renderer.h"

#include "dream/core/assert.h"
#include "dream/core/log.h"
#include "dream/core/profile.h"
#include "dream/runtime/animation.h"
#include "dream/runtime/app.h"
#include "dream/runtime/particles.h"
#include "dream/runtime/texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dream::runtime {
namespace {

using core::Mat4;
using core::Vec3;
using core::Vec4;

Vec4 multiply(Vec4 a, Vec4 b) {
    return Vec4{a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

Vec4 extend(Vec3 v, float w) {
    return Vec4{v.x, v.y, v.z, w};
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

// A point or spot light's color, intensity and range, which both share.
struct LightFields {
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
};

core::Result<LightFields> read_light_fields(const ComponentContext& context) {
    LightFields fields;
    const core::Result<Vec3> color = context.data.vec3("color", fields.color);
    const core::Result<float> intensity = context.data.number("intensity", fields.intensity);
    const core::Result<float> range = context.data.number("range", fields.range);
    if (!color.has_value()) {
        return color.error();
    }
    if (!intensity.has_value()) {
        return intensity.error();
    }
    if (!range.has_value()) {
        return range.error();
    }
    if (!(range.value() > 0.0f) || intensity.value() < 0.0f) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "A light's range must be above 0, and its intensity not below 0"};
    }
    return LightFields{color.value(), intensity.value(), range.value()};
}

core::Result<void> read_point_light(ComponentContext& context) {
    if (core::Result<void> known = context.data.check_keys({"color", "intensity", "range"});
        !known.has_value()) {
        return known;
    }
    const core::Result<LightFields> fields = read_light_fields(context);
    if (!fields.has_value()) {
        return fields.error();
    }
    context.world.registry().emplace_or_replace<PointLight>(
        context.entity, PointLight{.color = fields.value().color,
                                   .intensity = fields.value().intensity,
                                   .range = fields.value().range});
    return {};
}

core::Result<void> read_spot_light(ComponentContext& context) {
    if (core::Result<void> known = context.data.check_keys(
            {"color", "intensity", "range", "inner_angle", "outer_angle"});
        !known.has_value()) {
        return known;
    }
    const core::Result<LightFields> fields = read_light_fields(context);
    if (!fields.has_value()) {
        return fields.error();
    }
    SpotLight light{.color = fields.value().color,
                    .intensity = fields.value().intensity,
                    .range = fields.value().range};
    const core::Result<float> inner = context.data.number("inner_angle", light.inner_angle);
    const core::Result<float> outer = context.data.number("outer_angle", light.outer_angle);
    if (!inner.has_value()) {
        return inner.error();
    }
    if (!outer.has_value()) {
        return outer.error();
    }
    if (inner.value() < 0.0f || outer.value() <= inner.value() || outer.value() >= 90.0f) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "A spot light's angles must go 0 <= inner_angle < outer_angle < 90"};
    }
    light.inner_angle = inner.value();
    light.outer_angle = outer.value();
    context.world.registry().emplace_or_replace<SpotLight>(context.entity, light);
    return {};
}

// Normal maps need tangents, which neither the file nor a tangent generator gave these parts.
void warn_about_missing_tangents(const ModelData& data, std::string_view path) {
    const bool missing = std::ranges::any_of(data.parts, [&](const ModelPart& part) {
        if (part.material >= data.materials.size() ||
            data.materials[part.material].normal_image < 0 || part.index_count == 0 ||
            part.first_index >= data.indices.size()) {
            return false;
        }
        const std::uint32_t vertex = data.indices[part.first_index];
        return vertex < data.vertices.size() && data.vertices[vertex].tangent.w == 0.0f;
    });
    if (missing) {
        DREAM_LOG_WARNING("'{}' has normal maps but no tangents, so they are ignored; export it "
                          "with tangents, or list a tangent_generator module such as "
                          "\"mikktspace\"",
                          path);
    }
}

// The [lods] table, which says how simpler versions of models are made.
core::Result<LodOptions> read_lod_options(const core::Config& config) {
    LodOptions options;
    const std::int64_t levels = config.get_int("lods.levels").value_or(options.levels);
    if (levels < 0 || levels > 8) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("Setting 'lods.levels' must be from 0 to 8, not {}", levels)};
    }
    options.levels = static_cast<std::uint32_t>(levels);
    const double reduction = config.get_float("lods.reduction").value_or(options.reduction);
    if (!(reduction >= 0.1 && reduction <= 0.9)) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("Setting 'lods.reduction' must be from 0.1 to 0.9, not {}",
                                       reduction)};
    }
    options.reduction = static_cast<float>(reduction);
    const std::int64_t min_triangles =
        config.get_int("lods.min_triangles").value_or(options.min_triangles);
    if (min_triangles < 1 || min_triangles > 1'000'000) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           std::format("Setting 'lods.min_triangles' must be from 1 to 1000000, "
                                       "not {}",
                                       min_triangles)};
    }
    options.min_triangles = static_cast<std::uint32_t>(min_triangles);
    return options;
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

core::Result<void> Renderer::initialize(App& app) {
    core::Result<std::unique_ptr<renderer::SceneRenderer>> backend =
        app.create_module<renderer::SceneRenderer>();
    if (!backend.has_value()) {
        return backend.error();
    }
    return initialize(app, std::move(backend).value());
}

core::Result<void> Renderer::initialize(App& app,
                                        std::unique_ptr<renderer::SceneRenderer> backend) {
    if (backend == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument, "The renderer needs a backend"};
    }
    rhi::Device* const device = app.device();
    if (device == nullptr) {
        return core::Error{core::ErrorCode::InvalidArgument,
                           "The renderer needs a graphics module in the project configuration"};
    }
    if (const core::Result<void> started = backend->start(*device); !started.has_value()) {
        return started.error();
    }
    DREAM_LOG_INFO("{}", backend->describe());

    // A reload keeps the same object and only swaps the backend's shaders, which it does between
    // frames.
    m_shaders = {};
    m_backend = std::move(backend);
    renderer::SceneRenderer* const drawing = m_backend.get();
    app.assets().register_loader<RendererShaders>(
        [drawing](AssetLoadContext& context) -> core::Result<RendererShaders> {
            const core::Result<void> loaded =
                drawing->load_shaders([&context](std::string_view path) {
                    return context.files.read(path);
                });
            if (!loaded.has_value()) {
                return loaded.error();
            }
            return RendererShaders{};
        });
    core::Result<AssetRef<RendererShaders>> shaders =
        app.assets().load<RendererShaders>("assets:/shaders/renderer");
    if (!shaders.has_value()) {
        m_backend.reset();
        return shaders.error();
    }
    m_shaders = std::move(shaders).value();
    m_ui_textures = std::make_unique<UiTextures>(app, *device);
    DREAM_LOG_INFO("Cooked textures load as {}",
                   texture_format_name(cooked_color_format(*device)));

    // Simpler versions of models, when the project lists a simplifier (ADR 0032).
    core::Result<LodOptions> lod_options = read_lod_options(app.config());
    if (!lod_options.has_value()) {
        return lod_options.error();
    }
    m_simplifier.reset();
    if (app.has_module<mesh_simplifier::MeshSimplifier>() && lod_options.value().levels > 0) {
        core::Result<std::unique_ptr<mesh_simplifier::MeshSimplifier>> simplifier =
            app.create_module<mesh_simplifier::MeshSimplifier>();
        if (!simplifier.has_value()) {
            return simplifier.error();
        }
        m_simplifier = std::move(simplifier).value();
        DREAM_LOG_INFO("Models get up to {} simpler versions each", lod_options.value().levels);
    }

    // Tangents for normal maps whose models have none, when the project lists a generator (ADR
    // 0033).
    m_tangents.reset();
    if (app.has_module<tangent_generator::TangentGenerator>()) {
        core::Result<std::unique_ptr<tangent_generator::TangentGenerator>> tangents =
            app.create_module<tangent_generator::TangentGenerator>();
        if (!tangents.has_value()) {
            return tangents.error();
        }
        m_tangents = std::move(tangents).value();
    }

    // Textures drawn in the world, such as particles': the cooked one when the build made it.
    app.assets().register_loader<TextureAsset>(
        [device](AssetLoadContext& context) -> core::Result<TextureAsset> {
            const std::string path(context.path);
            if (!path.ends_with(".ktx2")) {
                if (const auto cooked = context.files.read(cooked_texture_path(path));
                    cooked.has_value()) {
                    core::Result<TextureData> loaded = load_ktx2(cooked.value(), *device);
                    if (!loaded.has_value()) {
                        return loaded.error();
                    }
                    const core::Result<rhi::TextureHandle> texture =
                        device->create_texture(loaded.value().desc, loaded.value().pixels);
                    if (!texture.has_value()) {
                        return texture.error();
                    }
                    return TextureAsset{.texture = GpuTexture(*device, texture.value()),
                                        .width = loaded.value().desc.width,
                                        .height = loaded.value().desc.height};
                }
            }
            const auto file = context.files.read(path);
            if (!file.has_value()) {
                return file.error();
            }
            if (is_ktx2(file.value())) {
                core::Result<TextureData> loaded = load_ktx2(file.value(), *device);
                if (!loaded.has_value()) {
                    return loaded.error();
                }
                const core::Result<rhi::TextureHandle> texture =
                    device->create_texture(loaded.value().desc, loaded.value().pixels);
                if (!texture.has_value()) {
                    return texture.error();
                }
                return TextureAsset{.texture = GpuTexture(*device, texture.value()),
                                    .width = loaded.value().desc.width,
                                    .height = loaded.value().desc.height};
            }
            const core::Result<ImageData> image = decode_image(file.value());
            if (!image.has_value()) {
                return image.error();
            }
            const core::Result<rhi::TextureHandle> texture = device->create_texture(
                rhi::TextureDesc{.width = image.value().width,
                                 .height = image.value().height,
                                 .format = rhi::TextureFormat::Rgba8Srgb},
                image.value().pixels);
            if (!texture.has_value()) {
                return texture.error();
            }
            return TextureAsset{.texture = GpuTexture(*device, texture.value()),
                                .width = image.value().width,
                                .height = image.value().height};
        });

    app.assets().register_loader<ModelAsset>(
        [device, simplifier = m_simplifier.get(), tangents = m_tangents.get(),
         options = lod_options.value()](
            AssetLoadContext& context) -> core::Result<ModelAsset> {
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
            core::Result<ModelData> data = parse_glb(file.value(), read_file);
            if (!data.has_value()) {
                return data.error();
            }
            if (tangents != nullptr) {
                if (const core::Result<void> made = generate_tangents(data.value(), *tangents);
                    !made.has_value()) {
                    return core::Error{made.error().code,
                                       std::format("Making tangents for '{}': {}", context.path,
                                                   made.error().message)};
                }
            }
            warn_about_missing_tangents(data.value(), context.path);
            if (simplifier != nullptr) {
                if (const core::Result<void> made = generate_lods(data.value(), *simplifier, options);
                    !made.has_value()) {
                    return core::Error{made.error().code,
                                       std::format("Making simpler versions of '{}': {}",
                                                   context.path, made.error().message)};
                }
            }
            return create_model_asset(*device, data.value());
        });

    app.scenes().register_component("mesh", &read_mesh);
    app.scenes().register_component("camera", &read_camera);
    app.scenes().register_component("directional_light", &read_directional_light);
    app.scenes().register_component("point_light", &read_point_light);
    app.scenes().register_component("spot_light", &read_spot_light);
    return {};
}

void Renderer::render(const World& world, rhi::CommandList& commands, platform::SurfaceSize size,
                      rhi::ClearColor clear_color) {
    DREAM_PROFILE_ZONE("Draw scene");
    m_last_stats = {};
    if (m_backend == nullptr) {
        m_debug_lines.clear();
        return;
    }

    renderer::Frame frame{.size = size, .clear_color = clear_color};
    m_meshes.clear();
    if (const std::optional<CameraView> camera = camera_view(world, size); camera.has_value()) {
        frame.view = renderer::View{.view_projection = camera->view_projection,
                                    .eye = camera->eye,
                                    .z_near = camera->z_near,
                                    .z_far = camera->z_far};

        const auto lights = world.registry().view<const DirectionalLight>();
        if (const Entity light_entity = lights.front(); light_entity != k_no_entity) {
            const DirectionalLight& light = lights.get<const DirectionalLight>(light_entity);
            frame.sun = renderer::Sunlight{.direction = light.direction,
                                           .color = light.color,
                                           .intensity = light.intensity,
                                           .ambient = light.ambient,
                                           .casts_shadows = light.casts_shadows};
        }

        for (const auto [entity, transform, mesh] :
             world.registry().view<const WorldTransform, const MeshRenderer>().each()) {
            if (mesh.model == nullptr) {
                continue;
            }
            const ModelAsset& model = *mesh.model;
            const Animator* const animator = world.registry().try_get<Animator>(entity);
            for (const ModelPart& part : model.parts) {
                renderer::MeshMaterial material = model.materials[part.material];
                material.base_color = multiply(material.base_color, mesh.tint);
                // A skinned part takes its animator's pose, or the rest pose, and a box around it
                // as posed, from its joints' boxes (ADR 0036).
                std::span<const Mat4> joints;
                core::Aabb bounds = part.bounds;
                if (part.skin >= 0 && static_cast<std::size_t>(part.skin) < model.skins.size()) {
                    const auto skin = static_cast<std::size_t>(part.skin);
                    joints = animator != nullptr && skin < animator->skinning.size() &&
                                     !animator->skinning[skin].empty()
                                 ? std::span<const Mat4>(animator->skinning[skin])
                                 : std::span<const Mat4>(model.rest_skinning[skin]);
                    if (!part.joint_bounds.empty()) {
                        bounds = {};
                        for (const JointBounds& joint : part.joint_bounds) {
                            if (joint.joint < joints.size()) {
                                const core::Aabb moved =
                                    core::transformed(joints[joint.joint], joint.bounds);
                                bounds.add(moved.min);
                                bounds.add(moved.max);
                            }
                        }
                    }
                }
                m_meshes.push_back(renderer::MeshDraw{
                    .vertices = model.vertices.get(),
                    .indices = model.indices.get(),
                    .first_index = part.first_index,
                    .index_count = part.index_count,
                    .model = transform.matrix,
                    .material = material,
                    .casts_shadows = mesh.casts_shadows,
                    .bounds = bounds,
                    .lods = part.lods,
                    .joint_matrices = joints,
                });
            }
        }
        frame.meshes = m_meshes;

        // Point and spot lights, where their entities are (ADR 0037).
        m_lights.clear();
        for (const auto [entity, transform, light] :
             world.registry().view<const WorldTransform, const PointLight>().each()) {
            m_lights.push_back(renderer::Light{
                .type = renderer::LightType::Point,
                .position = core::transform_point(transform.matrix, Vec3{}),
                .color = light.color,
                .intensity = light.intensity,
                .range = light.range});
        }
        for (const auto [entity, transform, light] :
             world.registry().view<const WorldTransform, const SpotLight>().each()) {
            const Vec3 forward =
                core::transform_direction(transform.matrix, Vec3{0.0f, 0.0f, -1.0f});
            m_lights.push_back(renderer::Light{
                .type = renderer::LightType::Spot,
                .position = core::transform_point(transform.matrix, Vec3{}),
                .direction = core::length(forward) > 0.0f ? core::normalized(forward)
                                                          : Vec3{0.0f, 0.0f, -1.0f},
                .color = light.color,
                .intensity = light.intensity,
                .range = light.range,
                .inner_angle = core::radians(light.inner_angle),
                .outer_angle = core::radians(light.outer_angle)});
        }
        frame.lights = m_lights;

        // Particles, as Particles::update left them (ADR 0038).
        m_particles.clear();
        for (const auto [entity, emitter] : world.registry().view<const ParticleEmitter>().each()) {
            if (!emitter.particles.empty()) {
                m_particles.push_back(renderer::ParticleBatch{
                    .particles = emitter.particles,
                    .texture = emitter.texture ? emitter.texture->texture.get()
                                               : rhi::TextureHandle{},
                    .blend = emitter.blend});
            }
        }
        frame.particles = m_particles;
        frame.lines = m_debug_lines.vertices();
    }

    std::array<renderer::UiLayer, static_cast<std::size_t>(UiLayer::Count)> layers{};
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
        layers[layer] = renderer::UiLayer{.list = &m_ui_layers[layer], .offset = m_ui_offsets[layer]};
    }
    frame.ui_layers = layers;
    frame.ui_texture = [this](ui::TextureId texture) {
        return m_ui_textures != nullptr ? m_ui_textures->find(texture) : rhi::TextureHandle{};
    };

    m_last_stats = m_backend->render(commands, frame);
    m_debug_lines.clear();
    DREAM_PROFILE_VALUE("Draw calls", m_last_stats.draws);
    DREAM_PROFILE_VALUE("Shadow casters", m_last_stats.shadow_casters);
    DREAM_PROFILE_VALUE("Culled parts", m_last_stats.culled);
    DREAM_PROFILE_VALUE("Debug lines", m_last_stats.lines);
    DREAM_PROFILE_VALUE("Particles", m_last_stats.particles);
    DREAM_PROFILE_VALUE("UI draw commands", m_last_stats.ui_commands);
}

} // namespace dream::runtime
