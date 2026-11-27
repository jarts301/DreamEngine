#include "dream/core/config.h"
#include "dream/core/math/matrix.h"
#include "dream/core/module_registry.h"
#include "dream/renderer/scene_renderer.h"
#include "fake_device.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <memory>
#include <ostream> // Needed for doctest to print std::string.
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_renderer_sprites(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::renderer;
using namespace dream::renderer::tests;

namespace {

constexpr platform::SurfaceSize k_size{800, 600};

struct Fixture {
    FakeDeviceState state;
    FakeDevice device{state};
    std::unique_ptr<SceneRenderer> renderer;
    // The shaders it asked for.
    std::set<std::string> read;

    Fixture() {
        core::ModuleRegistry registry;
        REQUIRE(modules::register_renderer_sprites(registry).has_value());
        const core::Config config = core::Config::parse("").value();
        const std::vector<std::string> preference{"sprites"};
        auto created = registry.create<SceneRenderer>(preference, config);
        REQUIRE(created.has_value());
        renderer = std::move(created.value().instance);
        REQUIRE(renderer->start(device).has_value());
        // Only its own shaders are there, as the build ships only those.
        const FileReader files =
            [this](std::string_view path) -> core::Result<std::vector<std::byte>> {
            read.emplace(path);
            for (const std::string_view own : {"debug_lines", "sprites", "ui."}) {
                if (path.find(own) != std::string_view::npos) {
                    return std::vector<std::byte>(4);
                }
            }
            return core::Error{core::ErrorCode::NotFound, std::string(path)};
        };
        REQUIRE(renderer->load_shaders(files).has_value());
    }

    FrameStats draw(const Frame& frame) {
        rhi::CommandList* const commands = device.begin_frame().value();
        const FrameStats stats = renderer->render(*commands, frame);
        REQUIRE(device.end_frame().has_value());
        return stats;
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "The sprites renderer loads only its 2D shaders, and makes no 3D targets") {
    CHECK(read.size() == 6);
    CHECK(std::ranges::none_of(read, [](const std::string& path) {
        return path.find("forward") != std::string::npos || path.find("shadow") != std::string::npos;
    }));
    // No shadow map, HDR targets or lights' texture: only its square's buffers.
    CHECK(std::ranges::none_of(state.textures, [](const rhi::TextureDesc& desc) {
        return desc.render_target || desc.dynamic;
    }));
    CHECK(renderer->describe().find("Only 2D") != std::string_view::npos);
}

TEST_CASE_FIXTURE(Fixture, "The sprites renderer draws sprites, lines and the interface into the window") {
    const std::array sprites{SpriteQuad{.position = {1.0f, 2.0f}}, SpriteQuad{}};
    const std::array batches{SpriteBatch{.sprites = sprites}};
    const std::array<LineVertex, 4> lines{};
    const FrameStats stats = draw(Frame{.size = k_size,
                                        .sprite_view = core::orthographic(-5, 5, -5, 5, -1, 1),
                                        .sprites = batches,
                                        .lines = lines});
    CHECK(stats.sprites == 2);
    CHECK(stats.lines == 2);
    // Straight into the window: no pass into targets of its own.
    CHECK(std::ranges::count(state.calls, "render_targets") == 0);
    CHECK(std::ranges::find(state.calls, "draw 0+6 x2") != state.calls.end());
}

TEST_CASE_FIXTURE(Fixture, "The sprites renderer leaves out a 3D scene") {
    const std::array<MeshDraw, 1> meshes{};
    const View view{.view_projection = core::Mat4{}, .eye = core::Vec3{}};
    const FrameStats stats = draw(Frame{.size = k_size, .view = view, .meshes = meshes});
    CHECK(stats.draws == 0);
    CHECK(stats.shadow_casters == 0);
}
