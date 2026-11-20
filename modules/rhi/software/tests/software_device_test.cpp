#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/platform/web_canvas.h"
#include "dream/rhi/rhi.h"
#include "dream/rhi/software.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_rhi_software(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::rhi;

namespace {

constexpr platform::SurfaceSize k_size{4, 2};

std::unique_ptr<Device> make_device() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_rhi_software(registry).has_value());
    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"software"};
    auto created = registry.create<Device>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

class RecordingCanvas final : public platform::WebCanvas {
public:
    void present(platform::SurfaceSize size, std::span<const std::byte> rgba) override {
        ++presented;
        last_size = size;
        last_pixels.assign(rgba.begin(), rgba.end());
    }

    int presented = 0;
    platform::SurfaceSize last_size;
    std::vector<std::byte> last_pixels;
};

} // namespace

TEST_CASE("The software device keeps buffers' contents for a CPU renderer to read") {
    const std::unique_ptr<Device> device = make_device();
    SoftwareAccess* const access = device->software_access();
    REQUIRE(access != nullptr);

    const std::array<std::byte, 3> contents{std::byte{1}, std::byte{2}, std::byte{3}};
    const BufferHandle buffer = device->create_buffer(BufferUsage::Vertex, contents).value();
    REQUIRE(access->buffer_contents(buffer).size() == 3);
    CHECK(access->buffer_contents(buffer)[2] == std::byte{3});

    device->destroy_buffer(buffer);
    CHECK(access->buffer_contents(buffer).empty());
    CHECK_FALSE(device->create_buffer(BufferUsage::Index, {}).has_value());
}

TEST_CASE("The software device makes RGBA8 textures' mipmaps, averaging sRGB in linear light") {
    const std::unique_ptr<Device> device = make_device();
    // A 2x2 texture, black and white in a checkerboard.
    std::vector<std::byte> pixels(16, std::byte{0});
    for (const std::size_t pixel : {0, 3}) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            pixels[pixel * 4 + channel] = std::byte{255};
        }
    }
    for (const std::size_t pixel : {1, 2}) {
        pixels[pixel * 4 + 3] = std::byte{255};
    }
    const TextureHandle texture =
        device->create_texture(TextureDesc{.width = 2, .height = 2}, pixels).value();
    const std::optional<SoftwareTexture> contents =
        device->software_access()->texture_contents(texture);
    REQUIRE(contents.has_value());
    CHECK(contents->desc.mip_levels == 2);
    REQUIRE(contents->pixels.size() == 20);
    // Half of white in linear light is 188 in sRGB, not 128.
    CHECK(static_cast<int>(contents->pixels[16]) == 188);
    CHECK(static_cast<int>(contents->pixels[19]) == 255);

    CHECK_FALSE(device->create_texture(TextureDesc{.width = 4, .height = 4,
                                                   .format = TextureFormat::Bc7Srgb},
                                       std::vector<std::byte>(16))
                    .has_value());
}

TEST_CASE("The software device updates dynamic textures' rows") {
    const std::unique_ptr<Device> device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());
    const TextureHandle texture =
        device
            ->create_texture(TextureDesc{.width = 1, .height = 2, .format = TextureFormat::Rgba8Unorm,
                                         .generate_mipmaps = false, .dynamic = true},
                             std::vector<std::byte>(8))
            .value();
    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    const std::array<std::byte, 4> row{std::byte{9}, std::byte{9}, std::byte{9}, std::byte{9}};
    commands->update_texture(texture, 1, row);
    REQUIRE(device->end_frame().has_value());
    CHECK(device->software_access()->texture_contents(texture)->pixels[4] == std::byte{9});
}

TEST_CASE("The software device presents frames to a web canvas and captures them") {
    const std::unique_ptr<Device> device = make_device();
    RecordingCanvas canvas;
    REQUIRE(device->attach_surface(platform::WebWindow{&canvas}, k_size).has_value());
    CHECK(device->capture_next_frame());

    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->begin_render_pass(ClearColor{1.0f, 0.0f, 0.0f, 1.0f});
    commands->end_render_pass();
    const SoftwareTarget target = device->software_access()->window_target();
    CHECK(target.width == k_size.width);
    CHECK(target.height == k_size.height);
    REQUIRE(target.pixels.size() == 4 * 2 * 4);
    CHECK(target.pixels[0] == std::byte{255});
    CHECK(target.pixels[1] == std::byte{0});
    target.pixels[4] = std::byte{7}; // A renderer writes the second pixel's red.
    REQUIRE(device->end_frame().has_value());

    CHECK(canvas.presented == 1);
    CHECK(canvas.last_size == k_size);
    REQUIRE(canvas.last_pixels.size() == target.pixels.size());
    CHECK(canvas.last_pixels[4] == std::byte{7});

    const std::optional<CapturedFrame> captured = device->take_captured_frame();
    REQUIRE(captured.has_value());
    CHECK(captured->width == 4);
    CHECK(captured->pixels[4] == std::byte{7});
    CHECK(captured->pixels[3] == std::byte{255});
    CHECK_FALSE(device->take_captured_frame().has_value());
}

TEST_CASE("The software device skips frames with no surface") {
    const std::unique_ptr<Device> device = make_device();
    CHECK(device->begin_frame().value() == nullptr);
    REQUIRE(device->attach_surface({}, k_size).has_value());
    device->detach_surface();
    CHECK(device->begin_frame().value() == nullptr);
}
