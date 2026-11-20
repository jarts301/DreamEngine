#include "dream/runtime/renderer.h"

#include "dream/runtime/app.h"
#include "fake_device.h"
#include "fake_platform.h"
#include "fake_renderer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using core::Vec3;
using doctest::Approx;

namespace {

constexpr platform::SurfaceSize k_size{800, 600};

const core::Config k_config = core::Config::parse("[app]\nmax_fps = 0").value();

// Runs a Renderer inside a real App, with a renderer module that records what it is given.
struct RenderFixture {
    FakeState state;
    FakeDeviceState device_state;
    FakeRendererState renderer_state;

    RenderFixture() {
        state.assets["shaders/fake_renderer.txt"] = std::vector<std::byte>(4);
    }

    // Calls `body` from inside a frame, with everything set up.
    template <typename Body> void in_frame(Body body) {
        struct TestGame final : Game {
            Renderer renderer;
            FakeRendererState* renderer_state = nullptr;
            Body* run = nullptr;
            core::Result<void> on_start(App& app) override {
                return renderer.initialize(app, std::make_unique<FakeRenderer>(*renderer_state));
            }
            void on_update(App&, const core::FrameTime&) override {}
            void on_render(App& app, rhi::CommandList& commands) override {
                (*run)(app, renderer, commands);
            }
        };

        state.script = {k_end_of_frame};
        App app(std::make_unique<FakePlatform>(state), k_config,
                std::make_unique<FakeDevice>(device_state));
        TestGame game;
        game.renderer_state = &renderer_state;
        game.run = &body;
        REQUIRE(app.run(game).has_value());
    }

    [[nodiscard]] const FakeFrame& last_frame() const {
        REQUIRE_FALSE(renderer_state.frames.empty());
        return renderer_state.frames.back();
    }
};

ModelAsset make_model(rhi::Device& device, std::uint32_t parts, bool textured) {
    ModelData data;
    data.vertices.resize(3);
    data.indices = {0, 1, 2};
    if (textured) {
        data.images.push_back(ImageData{.width = 1, .height = 1, .pixels = std::vector<std::byte>(4)});
    }
    data.materials.push_back(ModelMaterialData{.image = textured ? 0 : -1});
    for (std::uint32_t part = 0; part < parts; ++part) {
        data.parts.push_back(
            ModelPart{.first_index = part * 3, .index_count = 3, .material = 0, .bounds = {}});
    }
    return create_model_asset(device, data).value();
}

Entity add_model(App& app, World& world, std::uint32_t parts, bool textured) {
    const Entity entity = world.create_entity();
    world.registry().emplace<MeshRenderer>(
        entity, std::make_shared<const ModelAsset>(make_model(*app.device(), parts, textured)));
    return entity;
}

Entity add_camera(World& world) {
    const Entity entity = world.create_entity();
    world.registry().emplace<Camera>(entity);
    return entity;
}

// A game that only initializes a renderer, with the given module or the configured one.
struct StartOnly final : Game {
    Renderer renderer;
    std::unique_ptr<renderer::SceneRenderer> backend;
    core::Result<void> on_start(App& app) override {
        return backend != nullptr ? renderer.initialize(app, std::move(backend))
                                  : renderer.initialize(app);
    }
    void on_update(App&, const core::FrameTime&) override {}
};

} // namespace

TEST_CASE("fitted_vertical_fov leaves landscape alone and widens portrait") {
    const float fov = core::radians(60.0f);

    CHECK(fitted_vertical_fov(fov, 16.0f / 9.0f) == Approx(fov));
    CHECK(fitted_vertical_fov(fov, 1.0f) == Approx(fov));
    CHECK(fitted_vertical_fov(fov, 0.0f) == Approx(fov));

    // The horizontal view in portrait equals the vertical view in landscape.
    const float aspect = 0.5f;
    const float fitted = fitted_vertical_fov(fov, aspect);
    CHECK(fitted > fov);
    CHECK(std::tan(fitted * 0.5f) * aspect == Approx(std::tan(fov * 0.5f)));
}

TEST_CASE("camera_ray goes from the eye through the pixel, as the camera draws it") {
    World world;
    const platform::SurfaceSize size{800, 600};
    CHECK_FALSE(camera_ray(world, size, {400.0f, 300.0f}).has_value());

    const Entity entity = add_camera(world);
    world.registry().get<Transform>(entity).position = Vec3{0.0f, 2.0f, 10.0f};
    world.registry().get<Camera>(entity).target = Vec3{0.0f, 2.0f, 0.0f};
    world.update_transforms();
    CHECK_FALSE(camera_ray(world, platform::SurfaceSize{0, 0}, {0.0f, 0.0f}).has_value());

    const std::optional<CameraRay> middle = camera_ray(world, size, {400.0f, 300.0f});
    REQUIRE(middle.has_value());
    CHECK(middle->origin.y == Approx(2.0f));
    CHECK(middle->origin.z == Approx(10.0f));
    CHECK(middle->direction.x == Approx(0.0f).epsilon(1e-4));
    CHECK(middle->direction.y == Approx(0.0f).epsilon(1e-4));
    CHECK(middle->direction.z == Approx(-1.0f));

    // The top edge is half the vertical field of view up, and the right edge is to the right.
    const std::optional<CameraRay> top = camera_ray(world, size, {400.0f, 0.0f});
    REQUIRE(top.has_value());
    const float half_fov = core::radians(world.registry().get<Camera>(entity).vertical_fov_degrees) *
                           0.5f;
    CHECK(std::atan2(top->direction.y, -top->direction.z) == Approx(half_fov));
    const std::optional<CameraRay> right = camera_ray(world, size, {800.0f, 300.0f});
    REQUIRE(right.has_value());
    CHECK(right->direction.x > 0.5f);
    CHECK(core::length(right->direction) == Approx(1.0f));

    // Without a target, it looks along its own -Z.
    world.registry().get<Camera>(entity).target.reset();
    const std::optional<CameraRay> ahead = camera_ray(world, size, {400.0f, 300.0f});
    REQUIRE(ahead.has_value());
    CHECK(ahead->direction.z == Approx(-1.0f));
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer starts its module and loads its shaders through the game's files") {
    in_frame([](App&, Renderer& renderer, rhi::CommandList&) {
        CHECK(renderer.backend() != nullptr);
    });
    CHECK(renderer_state.started);
    CHECK(renderer_state.shader_loads == 1);
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer gives only the clear color and interface without a camera") {
    std::uint32_t draws = 99;
    World world;

    in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_model(app, world, 1, false);
        world.update_transforms();
        renderer.render(world, commands, k_size, rhi::ClearColor{0.1f, 0.2f, 0.3f, 1.0f});
        draws = renderer.last_draw_count();
        world.registry().clear();
    });

    CHECK(draws == 0);
    const FakeFrame& frame = last_frame();
    CHECK_FALSE(frame.view.has_value());
    CHECK(frame.meshes.empty());
    CHECK(frame.clear_color.g == Approx(0.2f));
    CHECK(frame.size == k_size);
    CHECK(frame.ui_layers.size() == static_cast<std::size_t>(UiLayer::Count));
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer gives each part of each model") {
    std::uint32_t draws = 0;
    World world;

    in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_camera(world);
        const Entity first = add_model(app, world, 2, false);
        world.registry().get<MeshRenderer>(first).tint = core::Vec4{0.5f, 1.0f, 1.0f, 1.0f};
        world.registry().get<Transform>(first).position = Vec3{4.0f, 0.0f, 0.0f};
        add_model(app, world, 1, true);
        world.update_transforms();
        renderer.render(world, commands, k_size);
        draws = renderer.last_draw_count();
        world.registry().clear();
    });

    CHECK(draws == 3);
    const FakeFrame& frame = last_frame();
    REQUIRE(frame.meshes.size() == 3);
    // The order of the models doesn't matter, so they are found by what they are.
    const auto textured = std::ranges::count_if(
        frame.meshes, [](const renderer::MeshDraw& mesh) { return mesh.material.base_color_texture.is_valid(); });
    CHECK(textured == 1);
    const auto tinted = std::ranges::find_if(frame.meshes, [](const renderer::MeshDraw& mesh) {
        return mesh.first_index == 3;
    });
    REQUIRE(tinted != frame.meshes.end());
    CHECK(tinted->index_count == 3);
    CHECK(tinted->material.base_color.x == Approx(0.5f));
    CHECK(tinted->model.columns[3].x == Approx(4.0f));
    CHECK(tinted->vertices.is_valid());
    // Each part's box, which the renderer culls by, comes from the model's vertices. The test models
    // have indices for their first part only, so the others have nothing to put in a box.
    CHECK(std::ranges::count_if(frame.meshes, [](const renderer::MeshDraw& part) {
              return !part.bounds.empty();
          }) == 2);
    CHECK(tinted->bounds.empty());
    CHECK(tinted->indices.is_valid());
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer gives no view on a surface with no size") {
    std::uint32_t draws = 99;
    World world;

    in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_camera(world);
        add_model(app, world, 1, false);
        world.update_transforms();
        renderer.render(world, commands, platform::SurfaceSize{0, 0});
        draws = renderer.last_draw_count();
        world.registry().clear();
    });

    CHECK(draws == 0);
    CHECK_FALSE(last_frame().view.has_value());
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer gives the camera and the first light") {
    World world;

    in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        const Entity camera = add_camera(world);
        world.registry().get<Transform>(camera).position = Vec3{1.0f, 2.0f, 3.0f};
        const Entity sun = world.create_entity();
        world.registry().emplace<DirectionalLight>(
            sun, DirectionalLight{.direction = Vec3{0.0f, -2.0f, 0.0f},
                                  .ambient = 0.5f,
                                  .casts_shadows = false});
        add_model(app, world, 1, false);
        world.update_transforms();
        renderer.render(world, commands, k_size);
        world.registry().clear();
    });

    const FakeFrame& frame = last_frame();
    REQUIRE(frame.view.has_value());
    CHECK(frame.view->eye.x == Approx(1.0f));
    CHECK(frame.view->eye.z == Approx(3.0f));
    CHECK(frame.view->z_near == Approx(Camera{}.z_near));
    REQUIRE(frame.sun.has_value());
    CHECK(frame.sun->direction.y == Approx(-2.0f));
    CHECK(frame.sun->ambient == Approx(0.5f));
    CHECK_FALSE(frame.sun->casts_shadows);
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer tells which models cast shadows") {
    World world;
    std::uint32_t casters = 0;

    in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_camera(world);
        world.registry().emplace<DirectionalLight>(world.create_entity());
        add_model(app, world, 2, false);
        const Entity hidden = add_model(app, world, 1, false);
        world.registry().get<MeshRenderer>(hidden).casts_shadows = false;
        world.update_transforms();
        renderer.render(world, commands, k_size);
        casters = renderer.last_shadow_caster_count();
        world.registry().clear();
    });

    CHECK(casters == 2);
    CHECK(std::ranges::count_if(last_frame().meshes, [](const renderer::MeshDraw& mesh) {
              return !mesh.casts_shadows;
          }) == 1);
}

TEST_CASE("Renderer passes on why its module can't load its shaders") {
    FakeState state;
    FakeDeviceState device_state;
    FakeRendererState renderer_state;
    state.script = {k_end_of_frame};
    App app(std::make_unique<FakePlatform>(state), k_config,
            std::make_unique<FakeDevice>(device_state));

    StartOnly game;
    game.backend = std::make_unique<FakeRenderer>(renderer_state);
    const core::Result<void> result = app.run(game);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("fake_renderer.txt") != std::string::npos);
    CHECK(game.renderer.backend() == nullptr);
}

TEST_CASE("Renderer needs a graphics device") {
    FakeState state;
    FakeRendererState renderer_state;
    state.script = {k_end_of_frame};
    App app(std::make_unique<FakePlatform>(state), k_config);

    StartOnly game;
    game.backend = std::make_unique<FakeRenderer>(renderer_state);
    CHECK_FALSE(app.run(game).has_value());
    CHECK_FALSE(renderer_state.started);
}

TEST_CASE("Renderer needs a renderer module in the project configuration") {
    FakeState state;
    FakeDeviceState device_state;
    state.script = {k_end_of_frame};
    App app(std::make_unique<FakePlatform>(state), k_config,
            std::make_unique<FakeDevice>(device_state));

    StartOnly game;
    CHECK_FALSE(app.run(game).has_value());
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer adds its components to scene files") {
    const std::string_view scene = R"(
[entities.cam.camera]
vertical_fov = 40.0
target = [1.0, 2.0, 3.0]

[entities.sun.directional_light]
intensity = 7.0
)";

    Camera camera;
    DirectionalLight light;
    bool has_target = false;
    in_frame([&](App& app, Renderer&, rhi::CommandList&) {
        auto world = app.scenes().parse(scene);
        REQUIRE(world.has_value());
        Registry& registry = world.value()->registry();
        camera = registry.get<Camera>(world.value()->find("cam"));
        light = registry.get<DirectionalLight>(world.value()->find("sun"));
        has_target = camera.target.has_value();
    });

    CHECK(camera.vertical_fov_degrees == Approx(40.0f));
    REQUIRE(has_target);
    CHECK(camera.target->y == Approx(2.0f));
    CHECK(light.intensity == Approx(7.0f));
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer reports a mistake in a mesh component") {
    std::string message;
    in_frame([&](App& app, Renderer&, rhi::CommandList&) {
        const auto missing_model = app.scenes().parse("[entities.a.mesh]\ntint = [1, 1, 1]\n");
        REQUIRE_FALSE(missing_model.has_value());
        message = missing_model.error().message;
    });
    CHECK(message.find("'model'") != std::string::npos);
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer gives the debug lines once, then clears them") {
    in_frame([](App& app, Renderer& renderer, rhi::CommandList& commands) {
        World world;
        add_camera(world);
        add_model(app, world, 1, false);
        world.update_transforms();

        renderer.debug_lines().line(Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, core::Vec4{1, 0, 0, 1});
        renderer.debug_lines().box(Vec3{}, core::Quat{}, Vec3{1.0f, 1.0f, 1.0f},
                                   core::Vec4{0, 1, 0, 1});
        renderer.render(world, commands, k_size);
        CHECK(renderer.last_line_count() == 13);
        CHECK(renderer.debug_lines().empty());

        // Nothing is left over for the next frame.
        renderer.render(world, commands, k_size);
        CHECK(renderer.last_line_count() == 0);
    });

    REQUIRE(renderer_state.frames.size() == 2);
    CHECK(renderer_state.frames[0].lines.size() == 26);
    CHECK(renderer_state.frames[0].lines[1].position.x == Approx(1.0f));
    CHECK(renderer_state.frames[1].lines.empty());
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer drops debug lines when there is no camera") {
    in_frame([](App&, Renderer& renderer, rhi::CommandList& commands) {
        World world;
        renderer.debug_lines().cross(Vec3{}, 1.0f, core::Vec4{1, 1, 1, 1});
        renderer.render(world, commands, k_size);
        CHECK(renderer.last_line_count() == 0);
        CHECK(renderer.debug_lines().empty());
    });
    CHECK(last_frame().lines.empty());
}

TEST_CASE("DebugLines builds shapes out of line pairs") {
    DebugLines lines;
    lines.box(Vec3{}, core::Quat{}, Vec3{1.0f, 2.0f, 3.0f}, core::Vec4{});
    CHECK(lines.vertices().size() == 24);
    for (const DebugVertex& vertex : lines.vertices()) {
        CHECK(std::abs(vertex.position.x) == Approx(1.0f));
        CHECK(std::abs(vertex.position.y) == Approx(2.0f));
        CHECK(std::abs(vertex.position.z) == Approx(3.0f));
    }

    lines.clear();
    lines.circle(Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 2.0f, core::Vec4{}, 8);
    CHECK(lines.vertices().size() == 16);
    for (const DebugVertex& vertex : lines.vertices()) {
        CHECK(vertex.position.y == Approx(1.0f));
        CHECK(core::length(vertex.position - Vec3{0.0f, 1.0f, 0.0f}) == Approx(2.0f));
    }

    lines.clear();
    lines.sphere(Vec3{}, core::Quat{}, 1.0f, core::Vec4{});
    lines.capsule(Vec3{}, core::Quat{}, 1.0f, 0.5f, core::Vec4{});
    CHECK(lines.vertices().size() % 2 == 0);
    for (const DebugVertex& vertex : lines.vertices()) {
        CHECK(std::abs(vertex.position.y) <= 1.5f + 1e-4f);
    }
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer gives a skinned part its joints and its posed box") {
    World world;
    std::size_t joints = 0;
    float posed_bottom = 0.0f;

    in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_camera(world);
        // One joint, resting 2 above the model's origin, bound where the vertices are, so at rest
        // it lifts them by 2.
        ModelData data;
        data.vertices.resize(3);
        data.vertices[1].position = Vec3{1.0f, 0.0f, 0.0f};
        data.vertices[2].position = Vec3{0.0f, 1.0f, 0.0f};
        for (ModelVertex& vertex : data.vertices) {
            vertex.weights = {255, 0, 0, 0};
        }
        data.indices = {0, 1, 2};
        data.materials.emplace_back();
        data.parts.push_back(
            ModelPart{.first_index = 0, .index_count = 3, .material = 0, .bounds = {}, .skin = 0});
        data.skeleton.parents = {-1};
        data.skeleton.rest_pose.push_back(
            animation::JointTransform{.translation = Vec3{0.0f, 2.0f, 0.0f}});
        data.skeleton.names = {"root"};
        data.skins.push_back(ModelSkin{.joints = {0}, .inverse_bind = {core::Mat4{}}});
        const Entity entity = world.create_entity();
        world.registry().emplace<MeshRenderer>(
            entity, std::make_shared<const ModelAsset>(create_model_asset(*app.device(), data).value()));
        world.update_transforms();
        renderer.render(world, commands, k_size);

        const FakeFrame& frame = last_frame();
        REQUIRE(frame.meshes.size() == 1);
        joints = frame.meshes[0].joint_matrices.size();
        posed_bottom = frame.meshes[0].bounds.min.y;
        world.registry().clear();
    });

    // Without an animator, the rest pose: the box is the vertices' box, lifted by the joint.
    CHECK(joints == 1);
    CHECK(posed_bottom == Approx(2.0f));
}
