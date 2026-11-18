#include "dream/core/config.h"
#include "dream/core/module_registry.h"
#include "dream/rhi/rhi.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace dream::modules {
[[nodiscard]] core::Result<void> register_rhi_null(core::ModuleRegistry& registry);
}

using namespace dream;
using namespace dream::rhi;

namespace {

constexpr std::array<std::byte, 4> k_bytes{};
constexpr platform::SurfaceSize k_size{640, 480};

std::unique_ptr<Device> make_device() {
    core::ModuleRegistry registry;
    REQUIRE(modules::register_rhi_null(registry).has_value());

    const core::Config config = core::Config::parse("").value();
    const std::vector<std::string> preference{"null"};
    auto created = registry.create<Device>(preference, config);
    REQUIRE(created.has_value());
    return std::move(created.value().instance);
}

PipelineHandle make_pipeline(Device& device) {
    const ShaderHandle shader = device.create_shader(ShaderStage::Vertex, k_bytes).value();
    PipelineDesc desc;
    desc.vertex_shader = shader;
    desc.fragment_shader = shader;
    return device.create_pipeline(desc).value();
}

} // namespace

TEST_CASE("Null device has nothing to draw to without a surface") {
    const auto device = make_device();

    const auto frame = device->begin_frame();
    REQUIRE(frame.has_value());
    CHECK(frame.value() == nullptr);
}

TEST_CASE("Null device gives a frame only while a usable surface is attached") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());

    auto frame = device->begin_frame();
    REQUIRE(frame.has_value());
    REQUIRE(frame.value() != nullptr);
    REQUIRE(device->end_frame().has_value());

    device->resize_surface({0, 0});
    frame = device->begin_frame();
    CHECK(frame.value() == nullptr);

    device->resize_surface(k_size);
    device->detach_surface();
    frame = device->begin_frame();
    CHECK(frame.value() == nullptr);
}

TEST_CASE("Null device records a complete frame") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());
    const PipelineHandle pipeline = make_pipeline(*device);
    const BufferHandle buffer = device->create_buffer(BufferUsage::Vertex, k_bytes).value();

    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->begin_render_pass(ClearColor{});
    commands->bind_pipeline(pipeline);
    commands->bind_vertex_buffer(buffer);
    commands->bind_index_buffer(buffer, IndexFormat::UInt16);
    commands->push_constants(k_bytes);
    commands->draw_indexed(3);
    commands->end_render_pass();

    CHECK(device->end_frame().has_value());
}

TEST_CASE("Null device accepts textures and uniforms in a frame") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());
    const PipelineHandle pipeline = make_pipeline(*device);
    constexpr std::array<std::byte, 8> k_two_pixels{};
    const TextureHandle texture =
        device->create_texture(TextureDesc{.width = 2, .height = 1}, k_two_pixels).value();

    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->begin_render_pass(ClearColor{});
    commands->bind_pipeline(pipeline);
    commands->set_uniforms(k_bytes);
    commands->bind_texture(texture);
    commands->bind_texture(TextureHandle{});
    commands->draw_indexed_range(3, 6);
    commands->end_render_pass();
    CHECK(device->end_frame().has_value());

    device->destroy_texture(texture);
    device->destroy_texture(texture);
}

TEST_CASE("Null device limits the vertex data drawn in one frame") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());
    const PipelineHandle pipeline = make_pipeline(*device);
    const std::vector<std::byte> half(k_max_frame_vertex_bytes / 2);

    for (int frame = 0; frame < 2; ++frame) {
        CommandList* const commands = device->begin_frame().value();
        REQUIRE(commands != nullptr);
        commands->begin_render_pass(ClearColor{});
        commands->bind_pipeline(pipeline);
        CHECK(commands->draw_vertices(half, 2));
        CHECK(commands->draw_vertices(half, 2));
        CHECK_FALSE(commands->draw_vertices(k_bytes, 1));
        commands->end_render_pass();
        REQUIRE(device->end_frame().has_value());
    }
}

TEST_CASE("Null device rejects textures whose pixels don't match their size") {
    const auto device = make_device();

    CHECK_FALSE(device->create_texture(TextureDesc{.width = 2, .height = 2}, k_bytes).has_value());
    CHECK_FALSE(device->create_texture(TextureDesc{.width = 0, .height = 1}, {}).has_value());
}

TEST_CASE("Null device keeps pipelines across a reattached surface") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());
    const PipelineHandle pipeline = make_pipeline(*device);

    device->detach_surface();
    REQUIRE(device->attach_surface({}, k_size).has_value());

    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->bind_pipeline(pipeline);
    CHECK(device->end_frame().has_value());
}

TEST_CASE("Null device rejects invalid resources") {
    const auto device = make_device();

    CHECK_FALSE(device->create_buffer(BufferUsage::Vertex, {}).has_value());
    CHECK_FALSE(device->create_shader(ShaderStage::Vertex, {}).has_value());

    SUBCASE("a pipeline before any surface") {
        const ShaderHandle shader = device->create_shader(ShaderStage::Vertex, k_bytes).value();
        PipelineDesc desc;
        desc.vertex_shader = shader;
        desc.fragment_shader = shader;
        CHECK_FALSE(device->create_pipeline(desc).has_value());
    }
    SUBCASE("a pipeline with a destroyed shader") {
        REQUIRE(device->attach_surface({}, k_size).has_value());
        const ShaderHandle shader = device->create_shader(ShaderStage::Vertex, k_bytes).value();
        device->destroy_shader(shader);
        PipelineDesc desc;
        desc.vertex_shader = shader;
        desc.fragment_shader = shader;
        CHECK_FALSE(device->create_pipeline(desc).has_value());
    }
}

TEST_CASE("Null device ignores destroying a handle twice") {
    const auto device = make_device();
    const BufferHandle buffer = device->create_buffer(BufferUsage::Vertex, k_bytes).value();

    device->destroy_buffer(buffer);
    device->destroy_buffer(buffer);
    device->destroy_buffer(BufferHandle{});
}

TEST_CASE("Null device takes per-frame indexed geometry and scissors inside a pass") {
    const auto device = make_device();
    REQUIRE(device->attach_surface({}, k_size).has_value());
    const PipelineHandle pipeline = make_pipeline(*device);
    const std::array<std::byte, 64> vertices{};
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 1, 3};
    const std::vector<std::byte> too_much(k_max_frame_vertex_bytes);

    CommandList* const commands = device->begin_frame().value();
    REQUIRE(commands != nullptr);
    commands->begin_render_pass(ClearColor{});
    commands->bind_pipeline(pipeline);
    commands->set_scissor(ScissorRect{.x = 10, .y = 20, .width = 30, .height = 40});
    CHECK(commands->bind_frame_geometry(vertices, indices));
    commands->draw_indexed_range(0, 6);
    commands->set_scissor(std::nullopt);
    CHECK_FALSE(commands->bind_frame_geometry(too_much, indices));
    commands->end_render_pass();
    CHECK(device->end_frame().has_value());
}

// The sizes every backend checks textures against, from rhi.h.
TEST_CASE("Texture sizes count every level, in pixels or 4x4 blocks") {
    static_assert(mip_size(16, 0) == 16 && mip_size(16, 4) == 1 && mip_size(16, 9) == 1);
    static_assert(mip_size(6, 1) == 3 && mip_size(6, 2) == 1);
    static_assert(full_mip_chain(1, 1) == 1 && full_mip_chain(16, 4) == 5);
    static_assert(full_mip_chain(5, 3) == 3);

    static_assert(texture_level_bytes(TextureFormat::Rgba8Srgb, 3, 2) == 24);
    static_assert(texture_level_bytes(TextureFormat::Bc7Srgb, 4, 4) == 16);
    // Levels smaller than a block still take a whole one.
    static_assert(texture_level_bytes(TextureFormat::Astc4x4Unorm, 1, 1) == 16);
    static_assert(texture_level_bytes(TextureFormat::Bc7Unorm, 8, 12) == 6 * 16);

    // 8x8 BC7 with all four levels: 4 blocks, then 1, 1 and 1.
    constexpr TextureDesc k_bc7{
        .width = 8, .height = 8, .format = TextureFormat::Bc7Srgb, .mip_levels = 4};
    static_assert(texture_bytes(k_bc7) == 7 * 16);
    CHECK_FALSE(texture_problem(k_bc7, 7 * 16).has_value());
    CHECK(texture_problem(k_bc7, 6 * 16).has_value());

    // Too many levels, and compressed sizes that aren't whole blocks, are refused.
    CHECK(texture_problem(TextureDesc{.width = 8, .height = 8, .mip_levels = 5}, 0).has_value());
    CHECK(texture_problem(TextureDesc{.width = 6, .height = 8, .format = TextureFormat::Bc7Srgb},
                          texture_level_bytes(TextureFormat::Bc7Srgb, 6, 8))
              .has_value());
    CHECK(texture_problem(TextureDesc{.width = 8, .height = 8, .mip_levels = 0}, 0).has_value());

    // Only one level of an uncompressed texture is made into a chain by the device.
    CHECK(generates_mipmaps(TextureDesc{.width = 8, .height = 8}));
    CHECK_FALSE(generates_mipmaps(TextureDesc{.width = 8, .height = 8, .mip_levels = 2}));
    CHECK_FALSE(
        generates_mipmaps(TextureDesc{.width = 8, .height = 8, .format = TextureFormat::Bc7Srgb}));
    CHECK_FALSE(generates_mipmaps(TextureDesc{.width = 1, .height = 1}));
}

TEST_CASE("Null device takes compressed textures with their levels") {
    const auto device = make_device();
    CHECK(device->supports_texture_format(TextureFormat::Astc4x4Srgb));
    // 2 blocks, then 1, 1 and 1.
    const std::vector<std::byte> blocks(5 * 16);
    const auto texture = device->create_texture(
        TextureDesc{.width = 8, .height = 4, .format = TextureFormat::Astc4x4Srgb, .mip_levels = 4},
        blocks);
    REQUIRE(texture.has_value());
    device->destroy_texture(texture.value());
}
