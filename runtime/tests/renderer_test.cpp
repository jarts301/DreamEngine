#include "dream/runtime/renderer.h"

#include "dream/runtime/app.h"
#include "fake_device.h"
#include "fake_platform.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;
using core::Vec3;
using doctest::Approx;

namespace {

using Calls = std::vector<std::string>;

constexpr platform::SurfaceSize k_size{800, 600};

const core::Config k_config = core::Config::parse("[app]\nmax_fps = 0").value();

std::vector<std::byte> fake_shader() {
    return std::vector<std::byte>(4);
}

// Runs a Renderer inside a real App, against a device that records what it is told.
struct RenderFixture {
    FakeState state;
    FakeDeviceState device_state;

    RenderFixture() {
        state.assets["shaders/forward.vert.fake"] = fake_shader();
        state.assets["shaders/forward.frag.fake"] = fake_shader();
        state.assets["shaders/debug_lines.vert.fake"] = fake_shader();
        state.assets["shaders/debug_lines.frag.fake"] = fake_shader();
    }

    // Calls `body` from inside a frame, with everything set up.
    template <typename Body> Calls in_frame(Body body) {
        struct TestGame final : Game {
            Renderer renderer;
            Body* run;
            core::Result<void> on_start(App& app) override {
                return renderer.initialize(app);
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
        game.run = &body;
        REQUIRE(app.run(game).has_value());
        return device_state.calls;
    }
};

ModelAsset make_model(rhi::Device& device, std::uint32_t parts, bool textured) {
    ModelData data;
    data.vertices.resize(3);
    data.indices = {0, 1, 2};
    if (textured) {
        data.images.push_back(ImageData{1, 1, std::vector<std::byte>(4)});
    }
    data.materials.push_back(ModelMaterialData{{1, 1, 1, 1}, textured ? 0 : -1});
    for (std::uint32_t part = 0; part < parts; ++part) {
        data.parts.push_back(ModelPart{part * 3, 3, 0});
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

Calls only_drawing(const Calls& calls) {
    Calls result;
    for (const std::string& call : calls) {
        if (call.starts_with("draw") || call.starts_with("bind_texture") || call == "uniforms") {
            result.push_back(call);
        }
    }
    return result;
}

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

TEST_CASE_FIXTURE(RenderFixture, "Renderer only clears the screen without a camera") {
    std::uint32_t draws = 99;
    World world;

    const Calls calls = in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_model(app, world, 1, false);
        world.update_transforms();
        renderer.render(world, commands, k_size);
        draws = renderer.last_draw_count();
        world.registry().clear();
    });

    CHECK(draws == 0);
    CHECK(only_drawing(calls).empty());
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer draws each part of each model") {
    std::uint32_t draws = 0;
    World world;

    const Calls calls = in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_camera(world);
        add_model(app, world, 2, false);
        add_model(app, world, 1, false);
        world.update_transforms();
        renderer.render(world, commands, k_size);
        draws = renderer.last_draw_count();
        world.registry().clear();
    });

    CHECK(draws == 3);

    // Uniforms are set once for the frame. The order of the models doesn't matter.
    Calls drawing = only_drawing(calls);
    REQUIRE(drawing.size() == 7);
    CHECK(drawing.front() == "uniforms");
    std::ranges::sort(drawing);
    CHECK(drawing == Calls{"bind_texture 0", "bind_texture 0", "bind_texture 0", "draw 0+3",
                           "draw 0+3", "draw 3+3", "uniforms"});
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer binds a model's texture, and white for untextured ones") {
    World world;

    const Calls calls = in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_camera(world);
        add_model(app, world, 1, true);
        world.update_transforms();
        renderer.render(world, commands, k_size);
        world.registry().clear();
    });

    CHECK(only_drawing(calls) == Calls{"uniforms", "bind_texture 1", "draw 0+3"});
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer skips drawing on a surface with no size") {
    std::uint32_t draws = 99;
    World world;

    in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        add_camera(world);
        add_model(app, world, 1, false);
        renderer.render(world, commands, platform::SurfaceSize{0, 0});
        draws = renderer.last_draw_count();
        world.registry().clear();
    });

    CHECK(draws == 0);
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer sends the camera position and the way to the light") {
    World world;

    in_frame([&](App& app, Renderer& renderer, rhi::CommandList& commands) {
        const Entity camera = add_camera(world);
        world.registry().get<Transform>(camera).position = Vec3{1.0f, 2.0f, 3.0f};
        const Entity sun = world.create_entity();
        world.registry().emplace<DirectionalLight>(
            sun, DirectionalLight{.direction = Vec3{0.0f, -2.0f, 0.0f}, .ambient = 0.5f});
        add_model(app, world, 1, false);
        world.update_transforms();
        renderer.render(world, commands, k_size);
        world.registry().clear();
    });

    // The layout of FrameUniforms: a matrix, then three vectors.
    REQUIRE(device_state.last_uniforms.size() >= 112);
    std::array<float, 12> vectors{};
    std::memcpy(vectors.data(), device_state.last_uniforms.data() + 64, sizeof(vectors));
    CHECK(vectors[0] == Approx(1.0f));
    CHECK(vectors[2] == Approx(3.0f));
    CHECK(vectors[5] == Approx(1.0f));
    CHECK(vectors[11] == Approx(0.5f));
}

TEST_CASE("Renderer explains what's wrong when its shaders are missing") {
    FakeState state;
    FakeDeviceState device_state;
    state.script = {k_end_of_frame};
    App app(std::make_unique<FakePlatform>(state), k_config,
            std::make_unique<FakeDevice>(device_state));

    struct TestGame final : Game {
        Renderer renderer;
        core::Result<void> on_start(App& started) override {
            return renderer.initialize(started);
        }
        void on_update(App&, const core::FrameTime&) override {}
    } game;

    const core::Result<void> result = app.run(game);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("RENDERER") != std::string::npos);
}

TEST_CASE("Renderer needs a graphics device") {
    FakeState state;
    state.script = {k_end_of_frame};
    App app(std::make_unique<FakePlatform>(state), k_config);

    struct TestGame final : Game {
        Renderer renderer;
        core::Result<void> on_start(App& started) override {
            return renderer.initialize(started);
        }
        void on_update(App&, const core::FrameTime&) override {}
    } game;

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

TEST_CASE_FIXTURE(RenderFixture, "Renderer draws debug lines once, after the models") {
    const Calls calls = in_frame([](App& app, Renderer& renderer, rhi::CommandList& commands) {
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

    const auto lines = std::find(calls.begin(), calls.end(), "vertices 26");
    REQUIRE(lines != calls.end());
    CHECK(std::count(calls.begin(), calls.end(), "vertices 26") == 1);
    const auto model = std::find(calls.begin(), calls.end(), "draw 0+3");
    CHECK(model < lines);
}

TEST_CASE_FIXTURE(RenderFixture, "Renderer drops debug lines when there is no camera") {
    const Calls calls = in_frame([](App&, Renderer& renderer, rhi::CommandList& commands) {
        World world;
        renderer.debug_lines().cross(Vec3{}, 1.0f, core::Vec4{1, 1, 1, 1});
        renderer.render(world, commands, k_size);
        CHECK(renderer.last_line_count() == 0);
        CHECK(renderer.debug_lines().empty());
    });
    CHECK(std::none_of(calls.begin(), calls.end(),
                       [](const std::string& call) { return call.starts_with("vertices"); }));
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
