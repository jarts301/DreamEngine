#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/renderer/scene_renderer.h"
#include "fake_device.h"

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_renderer_null(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::renderer;

TEST_CASE("The null renderer only clears the window") {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_renderer_null(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"null"};
    auto created = registry.create<SceneRenderer>(preference, config);
    REQUIRE(created.has_value());
    std::unique_ptr<SceneRenderer> renderer = std::move(created.value().instance);

    tests::FakeDeviceState state;
    tests::FakeDevice device(state);
    REQUIRE(renderer->start(device).has_value());
    // It has no shaders, so it reads no files.
    int reads = 0;
    REQUIRE(renderer
                ->load_shaders([&](std::string_view) -> core::Result<std::vector<std::byte>> {
                    ++reads;
                    return core::Error{core::ErrorCode::NotFound, "no files"};
                })
                .has_value());
    CHECK(reads == 0);
    CHECK_FALSE(renderer->describe().empty());

    const std::array meshes{MeshDraw{.index_count = 3}};
    rhi::CommandList* const commands = device.begin_frame().value();
    REQUIRE(commands != nullptr);
    const FrameStats stats = renderer->render(
        *commands, Frame{.size = {800, 600}, .view = View{}, .meshes = meshes});
    REQUIRE(device.end_frame().has_value());

    CHECK(stats.draws == 0);
    CHECK(state.calls == std::vector<std::string>{"begin_frame", "end_frame"});
}
