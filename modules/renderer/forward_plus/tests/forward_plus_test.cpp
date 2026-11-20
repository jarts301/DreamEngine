#include "dream/core/config.h"
#include "dream/core/math/matrix.h"
#include "dream/core/module_registry.h"
#include "dream/renderer/scene_renderer.h"
#include "fake_device.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_renderer_forward_plus(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::renderer;
using namespace dream::renderer::tests;
using core::Vec3;

namespace {

constexpr platform::SurfaceSize k_size{800, 600};

struct Fixture {
    FakeDeviceState state;
    FakeDevice device{state};
    std::unique_ptr<SceneRenderer> renderer;

    Fixture() {
        core::ModuleRegistry registry;
        REQUIRE(modules::register_renderer_forward_plus(registry).has_value());
        const core::Config config = core::Config::parse("").value();
        const std::vector<std::string> preference{"forward_plus"};
        auto created = registry.create<SceneRenderer>(preference, config);
        REQUIRE(created.has_value());
        renderer = std::move(created.value().instance);
        REQUIRE(renderer->start(device).has_value());
        const FileReader files = [](std::string_view) -> core::Result<std::vector<std::byte>> {
            return std::vector<std::byte>(4);
        };
        REQUIRE(renderer->load_shaders(files).has_value());
    }

    FrameStats draw(std::span<const Light> lights) {
        const core::Mat4 projection =
            core::perspective(core::radians(60.0f), 800.0f / 600.0f, 0.1f, 100.0f);
        const View view{.view_projection = projection * core::look_at(Vec3{0.0f, 0.0f, 5.0f},
                                                                      Vec3{}, Vec3{0, 1, 0}),
                        .eye = Vec3{0.0f, 0.0f, 5.0f},
                        .z_near = 0.1f,
                        .z_far = 100.0f};
        rhi::CommandList* const commands = device.begin_frame().value();
        const FrameStats stats =
            renderer->render(*commands, Frame{.size = k_size, .view = view, .lights = lights});
        REQUIRE(device.end_frame().has_value());
        return stats;
    }

    std::vector<float> texels() const {
        std::vector<float> floats(state.last_texture_update.size() / sizeof(float));
        std::memcpy(floats.data(), state.last_texture_update.data(), floats.size() * sizeof(float));
        return floats;
    }

    // How many lights cluster (x, y, z) has.
    float lights_in(const std::vector<float>& data, int x, int y, int z) const {
        const auto start = static_cast<std::size_t>(data[6]);
        const int cluster = (z * 9 + y) * 16 + x;
        return data[(start + static_cast<std::size_t>(cluster / 2)) * 4 +
                    (cluster % 2 == 0 ? 1 : 3)];
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "forward_plus sorts lights into clusters of the view") {
    // A small light on the right of the view, and one on the left.
    const std::array lights{
        Light{.position = Vec3{2.5f, 0.0f, 0.0f}, .range = 0.5f},
        Light{.position = Vec3{-2.5f, 0.0f, 0.0f}, .range = 0.5f},
    };
    const FrameStats stats = draw(lights);
    CHECK(stats.lights == 2);

    const std::vector<float> data = texels();
    REQUIRE(data.size() >= 4 * 4);
    CHECK(data[1] == 16.0f);
    CHECK(data[2] == 9.0f);
    CHECK(data[3] == 24.0f);

    // Each light is in clusters on its own side of the screen, at its depth, and not in the
    // middle ones.
    const float depth_scale = data[5];
    const int slice = static_cast<int>(std::log(5.0f / 0.1f) * depth_scale);
    float right = 0.0f;
    float left = 0.0f;
    float middle = 0.0f;
    for (int y = 0; y < 9; ++y) {
        for (int x = 0; x < 16; ++x) {
            const float count = lights_in(data, x, y, slice);
            (x >= 11 ? right : x <= 4 ? left : middle) += count;
        }
    }
    CHECK(right > 0.0f);
    CHECK(left > 0.0f);
    CHECK(middle == 0.0f);
}

TEST_CASE_FIXTURE(Fixture, "forward_plus takes up to 256 lights") {
    std::vector<Light> lights;
    for (int index = 0; index < 300; ++index) {
        lights.push_back(Light{.position = Vec3{static_cast<float>(index % 10) - 5.0f,
                                                static_cast<float>(index / 10 % 6) - 3.0f,
                                                -static_cast<float>(index / 60)},
                               .range = 1.0f});
    }
    const FrameStats stats = draw(lights);
    CHECK(stats.lights == 256);
    CHECK(stats.lights_dropped == 44);
}
