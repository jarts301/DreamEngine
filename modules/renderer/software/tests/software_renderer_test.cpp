#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "dream/core/config.h"
#include "dream/core/math/matrix.h"
#include "dream/core/module_registry.h"
#include "dream/renderer/scene_renderer.h"
#include "dream/rhi/software.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_renderer_software(core::ModuleRegistry& registry);
[[nodiscard]] core::Result<void> register_rhi_software(core::ModuleRegistry& registry);
} // namespace dream::modules

using namespace dream;
using namespace dream::renderer;
using core::Vec2;
using core::Vec3;
using core::Vec4;

namespace {

constexpr platform::SurfaceSize k_size{64, 48};

core::Config config_of(std::string_view text) {
    return core::Config::parse(text).value();
}

template <typename T>
std::unique_ptr<T> make(std::string_view backend, std::string_view config_text,
                        core::Result<void> (*register_module)(core::ModuleRegistry&)) {
    core::ModuleRegistry registry;
    REQUIRE(register_module(registry).has_value());
    const std::vector<std::string> preference{std::string(backend)};
    auto created = registry.create<T>(preference, config_of(config_text));
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

struct Rig {
    std::unique_ptr<rhi::Device> device =
        make<rhi::Device>("software", "", &modules::register_rhi_software);
    std::unique_ptr<SceneRenderer> renderer;

    explicit Rig(std::string_view config_text = "renderer.software.threads = 3") {
        renderer = make<SceneRenderer>("software", config_text, &modules::register_renderer_software);
        REQUIRE(device->attach_surface({}, k_size).has_value());
        REQUIRE(renderer->start(*device).has_value());
    }

    // Renders one frame and returns the window's pixels.
    std::vector<std::byte> draw(const Frame& frame, FrameStats* stats = nullptr) {
        REQUIRE(device->capture_next_frame());
        rhi::CommandList* const commands = device->begin_frame().value();
        REQUIRE(commands != nullptr);
        const FrameStats drawn = renderer->render(*commands, frame);
        if (stats != nullptr) {
            *stats = drawn;
        }
        REQUIRE(device->end_frame().has_value());
        std::optional<rhi::CapturedFrame> captured = device->take_captured_frame();
        REQUIRE(captured.has_value());
        return std::move(captured->pixels);
    }
};

std::array<int, 3> pixel_at(const std::vector<std::byte>& pixels, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * k_size.width + x) * 4;
    return {static_cast<int>(pixels[at]), static_cast<int>(pixels[at + 1]),
            static_cast<int>(pixels[at + 2])};
}

// A square two units across in the XY plane, facing +Z.
struct Square {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;

    explicit Square(rhi::Device& device) {
        const Vec3 normal{0.0f, 0.0f, 1.0f};
        const std::array<MeshVertex, 4> corners{
            MeshVertex{.position = {-1.0f, -1.0f, 0.0f}, .normal = normal, .uv = {0.0f, 1.0f}},
            MeshVertex{.position = {1.0f, -1.0f, 0.0f}, .normal = normal, .uv = {1.0f, 1.0f}},
            MeshVertex{.position = {1.0f, 1.0f, 0.0f}, .normal = normal, .uv = {1.0f, 0.0f}},
            MeshVertex{.position = {-1.0f, 1.0f, 0.0f}, .normal = normal, .uv = {0.0f, 0.0f}}};
        const std::array<std::uint32_t, 6> order{0, 1, 2, 0, 2, 3};
        vertices = device.create_buffer(rhi::BufferUsage::Vertex, std::as_bytes(std::span(corners)))
                       .value();
        indices =
            device.create_buffer(rhi::BufferUsage::Index, std::as_bytes(std::span(order))).value();
    }
};

View camera() {
    const Vec3 eye{0.0f, 0.0f, 4.0f};
    const core::Mat4 projection = core::perspective(core::radians(60.0f),
                                                    static_cast<float>(k_size.width) /
                                                        static_cast<float>(k_size.height),
                                                    0.1f, 100.0f);
    return View{.view_projection =
                    projection * core::look_at(eye, Vec3{}, Vec3{0.0f, 1.0f, 0.0f}),
                .eye = eye,
                .z_near = 0.1f,
                .z_far = 100.0f};
}

} // namespace

TEST_CASE("The software renderer needs a device that draws on the CPU") {
    const std::unique_ptr<SceneRenderer> renderer =
        make<SceneRenderer>("software", "", &modules::register_renderer_software);
    CHECK_FALSE(renderer->describe().empty());
    CHECK(renderer->load_shaders([](std::string_view) -> core::Result<std::vector<std::byte>> {
                      return core::Error{core::ErrorCode::NotFound, "no shaders here"};
                  })
              .has_value());
}

TEST_CASE("The software renderer rejects settings out of range") {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_renderer_software(registry).has_value());
    const std::vector<std::string> preference{"software"};
    CHECK_FALSE(registry.create<SceneRenderer>(preference, config_of("renderer.exposure = 0"))
                    .has_value());
    CHECK_FALSE(registry
                    .create<SceneRenderer>(preference,
                                           config_of("renderer.software.threads = 0"))
                    .has_value());
    CHECK_FALSE(registry
                    .create<SceneRenderer>(preference,
                                           config_of("renderer.tone_mapping = \"filmic\""))
                    .has_value());
}

TEST_CASE("Without a view, the software renderer draws the clear color") {
    Rig rig;
    const std::vector<std::byte> pixels =
        rig.draw(Frame{.size = k_size, .clear_color = rhi::ClearColor{1.0f, 0.0f, 0.0f, 1.0f}});
    CHECK(pixel_at(pixels, 10, 10) == std::array{255, 0, 0});
}

TEST_CASE("The software renderer lights an opaque part by the sun and keeps the background") {
    Rig rig;
    const Square square(*rig.device);
    const std::array meshes{MeshDraw{
        .vertices = square.vertices,
        .indices = square.indices,
        .index_count = 6,
        .model = core::Mat4{},
        .material = MeshMaterial{.base_color = {1.0f, 1.0f, 1.0f, 1.0f}, .roughness = 1.0f},
        .bounds = core::Aabb{.min = {-1.0f, -1.0f, 0.0f}, .max = {1.0f, 1.0f, 0.0f}}}};
    Frame frame{.size = k_size, .clear_color = rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
                .view = camera(),
                .sun = Sunlight{.direction = {0.0f, 0.0f, -1.0f}, .intensity = 1.0f,
                                .ambient = 0.0f, .casts_shadows = false},
                .meshes = meshes};
    FrameStats stats;
    const std::vector<std::byte> pixels = rig.draw(frame, &stats);
    CHECK(stats.draws == 1);
    CHECK(stats.culled == 0);
    const std::array<int, 3> middle = pixel_at(pixels, k_size.width / 2, k_size.height / 2);
    CHECK(middle[0] > 150);
    CHECK(middle[0] == middle[1]);
    CHECK(pixel_at(pixels, 0, 0) == std::array{0, 0, 0});

    // Mirrored, the square turns its back to the camera, and a single-sided part isn't drawn then.
    std::array mirrored = meshes;
    mirrored[0].model = core::scaling({-1.0f, 1.0f, 1.0f});
    frame.meshes = mirrored;
    const std::vector<std::byte> behind = rig.draw(frame);
    CHECK(pixel_at(behind, k_size.width / 2, k_size.height / 2) == std::array{0, 0, 0});

    // Unless it is double-sided, and then its back is lit as its front turned around.
    mirrored[0].material.double_sided = true;
    const std::vector<std::byte> both = rig.draw(frame);
    CHECK(pixel_at(both, k_size.width / 2, k_size.height / 2)[0] > 150);
}

TEST_CASE("The software renderer skips parts out of view") {
    Rig rig;
    const Square square(*rig.device);
    const std::array meshes{MeshDraw{
        .vertices = square.vertices,
        .indices = square.indices,
        .index_count = 6,
        .model = core::translation({0.0f, 50.0f, 0.0f}),
        .bounds = core::Aabb{.min = {-1.0f, -1.0f, 0.0f}, .max = {1.0f, 1.0f, 0.0f}}}};
    FrameStats stats;
    (void)rig.draw(Frame{.size = k_size, .view = camera(), .meshes = meshes}, &stats);
    CHECK(stats.draws == 0);
    CHECK(stats.culled == 1);
}

TEST_CASE("The software renderer blends interface layers over the scene, with their textures") {
    Rig rig;
    const std::array<std::byte, 4> green{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
    const rhi::TextureHandle texture =
        rig.device
            ->create_texture(rhi::TextureDesc{.width = 1, .height = 1, .generate_mipmaps = false},
                             green)
            .value();
    ui::DrawList list;
    list.vertices = {ui::Vertex{.position = {0.0f, 0.0f}}, ui::Vertex{.position = {32.0f, 0.0f}},
                     ui::Vertex{.position = {32.0f, 48.0f}}, ui::Vertex{.position = {0.0f, 48.0f}}};
    list.indices = {0, 1, 2, 0, 2, 3};
    list.commands = {ui::DrawCommand{.first_index = 0, .index_count = 6, .texture = 7}};
    const std::array layers{UiLayer{.list = &list}};
    FrameStats stats;
    const std::vector<std::byte> pixels = rig.draw(
        Frame{.size = k_size, .clear_color = rhi::ClearColor{0.0f, 0.0f, 1.0f, 1.0f},
              .ui_layers = layers,
              .ui_texture = [&](ui::TextureId id) { return id == 7 ? texture : rhi::TextureHandle{}; }},
        &stats);
    CHECK(stats.ui_commands == 1);
    CHECK(pixel_at(pixels, 5, 5) == std::array{0, 255, 0});
    CHECK(pixel_at(pixels, 60, 5) == std::array{0, 0, 255});
}

TEST_CASE("The software renderer draws debug lines and particles over the scene") {
    Rig rig;
    const std::array lines{LineVertex{.position = {-3.0f, 0.0f, 0.0f}, .color = {1.0f, 0.0f, 0.0f, 1.0f}},
                           LineVertex{.position = {3.0f, 0.0f, 0.0f}, .color = {1.0f, 0.0f, 0.0f, 1.0f}}};
    const std::array quads{ParticleQuad{.position = {0.0f, 1.0f, 0.0f}, .size = 0.8f,
                                        .color = {0.0f, 1.0f, 0.0f, 1.0f}}};
    const std::array batches{ParticleBatch{.particles = quads, .blend = ParticleBlend::Additive}};
    FrameStats stats;
    const std::vector<std::byte> pixels = rig.draw(
        Frame{.size = k_size, .clear_color = rhi::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
              .view = camera(), .particles = batches, .lines = lines},
        &stats);
    CHECK(stats.lines == 1);
    CHECK(stats.particles == 1);
    CHECK(pixel_at(pixels, k_size.width / 2, k_size.height / 2)[0] > 200);
    // The particle sits a unit above the line.
    bool green_found = false;
    for (std::uint32_t y = 0; y < k_size.height / 2; ++y) {
        green_found = green_found || pixel_at(pixels, k_size.width / 2, y)[1] > 100;
    }
    CHECK(green_found);
}
